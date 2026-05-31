// ChatClient.cs -- Async TCP client for SceneChat.
// Mirrors sc_net.cpp on the Xbox client and sc_net.py on the Python client.
// All events raised on the calling thread via TaskCompletionSource / callbacks.

using SceneChatWPF.Crypto;
using System.Buffers.Binary;
using System.IO;
using System.Net.Sockets;
using System.Numerics;
using System.Security.Cryptography;

namespace SceneChatWPF.Net;

// ── Event data ────────────────────────────────────────────────────────────────

public record RoomInfo(int Id, string Name, int Type, int PasswordFlag = 0);
public record ChatMessage(int RoomId, string Username, string Content, string Timestamp, int MsgId = 0);
public record UserInfo(int UserId, string Username, int RoomId);
public record HistoryMessage(string Username, string Content, string Timestamp, int MsgId = 0);
public record MailItem(int MailId, string Sender, string Body, string Timestamp);

// ── Client ────────────────────────────────────────────────────────────────────

public class ChatClient : IAsyncDisposable
{
    // ── Events ────────────────────────────────────────────────────────────────
    public event Action? OnConnected;
    public event Action<int, string, string>? OnAuthOk;          // user_id, username, token
    public event Action<string>? OnAuthFail;
    public event Action<List<RoomInfo>>? OnRoomList;
    public event Action<int, string, List<HistoryMessage>>? OnRoomJoined;  // id, name, history
    public event Action<ChatMessage>? OnMessage;
    public event Action<string>? OnDisconnected;
    public event Action<string>? OnError;
    public event Action<int, int>? OnMsgDelete;       // roomId, msgId
    public event Action<List<UserInfo>>? OnUserList;
    public event Action<UserInfo>? OnUserJoin;
    public event Action<int, string>? OnUserLeave;       // userId, username
    public event Action<string>? OnJoinFail;        // reason
    public event Action<int, string>? OnDmRoom;          // roomId, displayName (type==2)
    public event Action<List<MailItem>>? OnMailList;

    // ── State ─────────────────────────────────────────────────────────────────
    private TcpClient? _tcp;
    private bool _joiningRoom;
    private NetworkStream? _stream;
    private byte[]? _sessionKey;
    private CancellationTokenSource _cts = new();
    private bool _pendingRegister = false;
    private readonly SemaphoreSlim _writeLock = new(1, 1);
    private const int PingIntervalMs = 30_000;

    // ── Connect ───────────────────────────────────────────────────────────────

    public async Task ConnectAsync(string host, int port = 8943)
    {
        _cts = new CancellationTokenSource();
        _tcp = new TcpClient();
        await _tcp.ConnectAsync(host, port, _cts.Token);
        _stream = _tcp.GetStream();

        // DH handshake must complete before signalling ready
        _sessionKey = await DhHandshakeAsync();
        if (_sessionKey == null)
        {
            OnDisconnected?.Invoke("Handshake failed");
            return;
        }

        OnConnected?.Invoke();

        // Start receive loop + ping loop concurrently
        _ = Task.Run(ReceiveLoopAsync);
        _ = Task.Run(PingLoopAsync);
    }

    // ── DH Handshake ──────────────────────────────────────────────────────────

    private async Task<byte[]?> DhHandshakeAsync()
    {
        try
        {
            // Read DH_INIT
            var (type, payload) = await ReadPacketAsync();
            if (type != ScProtocol.DH_INIT || payload == null) return null;

            // Parse [params_len 2B][params_pem][pub_len 2B][pub_pem]
            int pos = 0;
            int paramsLen = BinaryPrimitives.ReadUInt16BigEndian(payload.AsSpan(pos)); pos += 2;
            var paramsPem = payload[pos..(pos + paramsLen)]; pos += paramsLen;
            int pubLen = BinaryPrimitives.ReadUInt16BigEndian(payload.AsSpan(pos)); pos += 2;
            var pubPem = payload[pos..(pos + pubLen)];

            // Import server public key and DH parameters
            // .NET doesn't have DH-from-PEM directly -- extract p, g, y via BigInteger
            var (p, g, serverY) = ParseDhPem(paramsPem, pubPem);

            // Generate client private key x in [2, p-2]
            var x = GenerateDhPrivate(p);
            var clientY = BigInteger.ModPow(g, x, p);

            // Shared secret
            var shared = BigInteger.ModPow(serverY, x, p);

            // Serialise client public key as big-endian bytes (128 bytes for 1024-bit)
            var clientYBytes = ToFixedBytes(clientY, 128);
            var respPayload = new byte[2 + 128];
            BinaryPrimitives.WriteUInt16BigEndian(respPayload, 128);
            Buffer.BlockCopy(clientYBytes, 0, respPayload, 2, 128);
            await WriteRawPacketAsync(ScProtocol.DH_RESPONSE, respPayload);

            // Derive session key
            var sharedBytes = ToUnsignedBytes(shared);
            return ScCrypto.DeriveSessionKey(sharedBytes);
        }
        catch (Exception ex)
        {
            OnError?.Invoke($"DH error: {ex.Message}");
            return null;
        }
    }

    // ── DH PEM parsing (minimal -- extracts raw BigIntegers from ASN.1 DER) ──

    private static (BigInteger p, BigInteger g, BigInteger y) ParseDhPem(
        byte[] paramsPem, byte[] pubPem)
    {
        // Strip PEM headers and decode base64
        static byte[] PemToDer(byte[] pem)
        {
            var s = System.Text.Encoding.ASCII.GetString(pem);
            var lines = s.Split('\n');
            var b64 = string.Concat(lines
                .Where(l => !l.StartsWith("-----"))
                .Select(l => l.Trim()));
            return Convert.FromBase64String(b64);
        }

        var paramsDer = PemToDer(paramsPem);
        var pubDer = PemToDer(pubPem);

        // Parse DHParameter SEQUENCE { p INTEGER, g INTEGER }
        var (p, g) = ParseDhParameter(paramsDer);

        // Parse SubjectPublicKeyInfo -- the public key is buried in a BIT STRING
        // containing a DER INTEGER (the y value)
        var y = ParseDhPublicKey(pubDer, p);

        return (p, g, y);
    }

    private static (BigInteger p, BigInteger g) ParseDhParameter(byte[] der)
    {
        int pos = 0;
        pos = SkipTag(der, pos, 0x30); // SEQUENCE
        var p = ReadDerInteger(der, ref pos);
        var g = ReadDerInteger(der, ref pos);
        return (p, g);
    }

    private static BigInteger ParseDhPublicKey(byte[] der, BigInteger p)
    {
        // SubjectPublicKeyInfo SEQUENCE { AlgorithmIdentifier, BIT STRING }
        int pos = 0;
        pos = SkipTag(der, pos, 0x30);    // outer SEQUENCE
        pos = SkipTlv(der, pos);          // AlgorithmIdentifier
        pos = SkipTag(der, pos, 0x03);    // BIT STRING -- pos now at content
        pos++;                            // skip unused-bits byte (0x00)
        // Inner INTEGER (y value)
        var intRef = pos;
        return ReadDerInteger(der, ref intRef);
    }

    private static int SkipTag(byte[] der, int pos, byte expectedTag)
    {
        if (der[pos] != expectedTag)
            throw new InvalidDataException($"Expected tag 0x{expectedTag:X2} got 0x{der[pos]:X2}");
        pos++;
        ReadDerLength(der, ref pos);
        return pos;
    }

    private static int SkipTlv(byte[] der, int pos)
    {
        pos++; // tag
        int len = ReadDerLength(der, ref pos);
        return pos + len;
    }

    private static int ReadDerLength(byte[] der, ref int pos)
    {
        int b = der[pos++];
        if ((b & 0x80) == 0) return b;
        int numBytes = b & 0x7F;
        int len = 0;
        for (int i = 0; i < numBytes; i++) len = (len << 8) | der[pos++];
        return len;
    }

    private static BigInteger ReadDerInteger(byte[] der, ref int pos)
    {
        if (der[pos++] != 0x02) throw new InvalidDataException("Expected INTEGER");
        int len = ReadDerLength(der, ref pos);
        var bytes = der[pos..(pos + len)];
        pos += len;
        // BigInteger from big-endian unsigned bytes
        // Prepend 0x00 to ensure positive interpretation
        var unsigned = new byte[bytes.Length + 1];
        Buffer.BlockCopy(bytes, 0, unsigned, 1, bytes.Length);
        return new BigInteger(unsigned, isBigEndian: true);
    }

    // ── DH key generation ─────────────────────────────────────────────────────

    private static BigInteger GenerateDhPrivate(BigInteger p)
    {
        // Generate random x in range [2, p-2]
        int byteLen = (int)Math.Ceiling(BigInteger.Log(p, 256));
        while (true)
        {
            var bytes = RandomNumberGenerator.GetBytes(byteLen);
            var unsigned = new byte[bytes.Length + 1];
            Buffer.BlockCopy(bytes, 0, unsigned, 1, bytes.Length);
            var x = new BigInteger(unsigned, isBigEndian: true);
            if (x >= 2 && x <= p - 2) return x;
        }
    }

    private static byte[] ToFixedBytes(BigInteger n, int len)
    {
        var bytes = n.ToByteArray(isUnsigned: true, isBigEndian: true);
        if (bytes.Length == len) return bytes;
        var result = new byte[len];
        int offset = len - bytes.Length;
        if (offset < 0) Buffer.BlockCopy(bytes, -offset, result, 0, len);
        else Buffer.BlockCopy(bytes, 0, result, offset, bytes.Length);
        return result;
    }

    private static byte[] ToUnsignedBytes(BigInteger n)
        => n.ToByteArray(isUnsigned: true, isBigEndian: true);

    // ── Packet I/O ────────────────────────────────────────────────────────────

    private async Task<(byte type, byte[]? payload)> ReadPacketAsync()
    {
        var header = new byte[2];
        await ReadExactAsync(header, 2);
        int totalLen = BinaryPrimitives.ReadUInt16BigEndian(header);
        if (totalLen < 3) return (0, null);
        var rest = new byte[totalLen - 2];
        await ReadExactAsync(rest, rest.Length);
        return (rest[0], rest[1..]);
    }

    private async Task ReadExactAsync(byte[] buf, int count)
    {
        int received = 0;
        while (received < count)
        {
            int n = await _stream!.ReadAsync(buf, received, count - received, _cts.Token);
            if (n == 0) throw new IOException("Connection closed");
            received += n;
        }
    }

    private async Task WriteRawPacketAsync(byte type, byte[] payload)
    {
        var frame = ScProtocol.FramePacket(type, payload);
        await _writeLock.WaitAsync();
        try { await _stream!.WriteAsync(frame, _cts.Token); }
        finally { _writeLock.Release(); }
    }

    private async Task WriteEncryptedAsync(byte type, byte[] payload)
    {
        var encrypted = ScCrypto.Encrypt(_sessionKey!, payload);
        await WriteRawPacketAsync(type, encrypted);
    }

    // ── Receive loop ──────────────────────────────────────────────────────────

    private async Task ReceiveLoopAsync()
    {
        try
        {
            while (!_cts.IsCancellationRequested)
            {
                var (type, payload) = await ReadPacketAsync();
                if (payload == null) break;

                if (payload.Length < 12) continue; // too short for encrypted
                var plain = ScCrypto.Decrypt(_sessionKey!, payload);
                if (plain == null) continue;        // tag mismatch

                await DispatchAsync(type, plain);
            }
        }
        catch (OperationCanceledException) { }
        catch (Exception ex)
        {
            OnDisconnected?.Invoke(ex.Message);
            return;
        }
        OnDisconnected?.Invoke("Connection closed");
    }

    // ── Packet dispatch ───────────────────────────────────────────────────────

    private Task DispatchAsync(byte type, byte[] payload)
    {
        switch (type)
        {
            case ScProtocol.AUTH_OK: HandleAuthOk(payload); break;
            case ScProtocol.AUTH_FAIL: HandleAuthFail(payload); break;
            case ScProtocol.ERROR: HandleAuthFail(payload); break;
            case ScProtocol.ROOM_LIST: HandleRoomList(payload); break;
            case ScProtocol.ROOM_INFO: HandleRoomInfo(payload); break;
            case ScProtocol.MSG_RECV: HandleMsgRecv(payload); break;
            case ScProtocol.MSG_DELETE: HandleMsgDelete(payload); break;
            case ScProtocol.USER_LIST: HandleUserList(payload); break;
            case ScProtocol.USER_JOIN: HandleUserJoin(payload); break;
            case ScProtocol.USER_LEAVE: HandleUserLeave(payload); break;
            case ScProtocol.MAIL_LIST: HandleMailList(payload); break;
            case ScProtocol.PONG: break;
        }
        return Task.CompletedTask;
    }

    private void HandleAuthOk(byte[] payload)
    {
        // Use the flag set by SendRegisterAsync/SendLoginAsync to distinguish
        // registration response [0x00][string8] from login response [user_id 4B][token][username]
        // Checking payload[0]==0x00 is unreliable -- login user_id bytes start with 0x00 too
        if (_pendingRegister)
        {
            _pendingRegister = false;
            OnAuthOk?.Invoke(0, "", "");
            return;
        }

        if (payload.Length < 5) { OnAuthOk?.Invoke(0, "", ""); return; }
        int pos = 0;
        int userId = (int)BinaryPrimitives.ReadUInt32BigEndian(payload.AsSpan(pos)); pos += 4;
        var (token, p2) = ScProtocol.UnpackString8(payload, pos); pos = p2;
        var (username, _) = ScProtocol.UnpackString8(payload, pos);
        OnAuthOk?.Invoke(userId, username, token);
        _ = RequestRoomListAsync();
    }

    private void HandleRoomList(byte[] payload)
    {
        int count = payload[0];
        int pos = 1;
        var rooms = new List<RoomInfo>(count);
        for (int i = 0; i < count; i++)
        {
            int id = payload[pos++];
            int type = payload[pos++];
            int passwordFlag = payload[pos++];
            var (name, next) = ScProtocol.UnpackString8(payload, pos);
            pos = next;
            rooms.Add(new RoomInfo(id, name, type, passwordFlag));
        }
        OnRoomList?.Invoke(rooms);
    }

    private void HandleAuthFail(byte[] payload)
    {
        var (msg, _) = ScProtocol.UnpackString8(payload, 0);
        if (_joiningRoom)
        {
            _joiningRoom = false;
            OnJoinFail?.Invoke(msg);
        }
        else
        {
            OnAuthFail?.Invoke(msg);
        }
    }

    private void HandleRoomInfo(byte[] payload)
    {
        _joiningRoom = false;
        int pos = 0;
        int roomId = payload[pos++];
        int roomType = payload[pos++];
        var (name, p2) = ScProtocol.UnpackString8(payload, pos); pos = p2;
        int count = payload[pos++];
        var history = new List<HistoryMessage>(count);
        for (int i = 0; i < count; i++)
        {
            int msgId = (payload[pos] << 24) | (payload[pos + 1] << 16) |
                        (payload[pos + 2] << 8) | payload[pos + 3]; pos += 4;
            var (user, p3) = ScProtocol.UnpackString8(payload, pos); pos = p3;
            var (content, p4) = ScProtocol.UnpackString16(payload, pos); pos = p4;
            var (ts, p5) = ScProtocol.UnpackString8(payload, pos); pos = p5;
            history.Add(new HistoryMessage(user, content, ts, msgId));
        }
        if (roomType == 2)
            OnDmRoom?.Invoke(roomId, name);
        OnRoomJoined?.Invoke(roomId, name, history);
    }

    private void HandleMailList(byte[] payload)
    {
        int pos = 0;
        int count = (payload[pos] << 8) | payload[pos + 1]; pos += 2;
        var mails = new List<MailItem>(count);
        for (int i = 0; i < count; i++)
        {
            int mailId = (payload[pos] << 24) | (payload[pos + 1] << 16) |
                         (payload[pos + 2] << 8) | payload[pos + 3]; pos += 4;
            var (sender, p1) = ScProtocol.UnpackString8(payload, pos); pos = p1;
            var (body, p2) = ScProtocol.UnpackString16(payload, pos); pos = p2;
            var (ts, p3) = ScProtocol.UnpackString8(payload, pos); pos = p3;
            mails.Add(new MailItem(mailId, sender, body, ts));
        }
        OnMailList?.Invoke(mails);
    }

    private void HandleMsgRecv(byte[] payload)
    {
        int pos = 0;
        int roomId = payload[pos++];
        int msgId = (payload[pos] << 24) | (payload[pos + 1] << 16) |
                     (payload[pos + 2] << 8) | payload[pos + 3]; pos += 4;
        var (username, p2) = ScProtocol.UnpackString8(payload, pos); pos = p2;
        var (content, p3) = ScProtocol.UnpackString16(payload, pos); pos = p3;
        var (ts, _) = ScProtocol.UnpackString8(payload, pos);
        OnMessage?.Invoke(new ChatMessage(roomId, username, content, ts, msgId));
    }

    private void HandleMsgDelete(byte[] payload)
    {
        if (payload.Length < 5) return;
        int roomId = payload[0];
        int msgId = (payload[1] << 24) | (payload[2] << 16) |
                     (payload[3] << 8) | payload[4];
        OnMsgDelete?.Invoke(roomId, msgId);
    }

    // ── Public send methods ───────────────────────────────────────────────────

    public async Task SendLoginAsync(string username, string password)
    {
        _pendingRegister = false;
        await WriteEncryptedAsync(ScProtocol.LOGIN,
            ScProtocol.Combine(ScProtocol.PackString8(username), ScProtocol.PackString8(password)));
    }

    public async Task SendRegisterAsync(string username, string password)
    {
        _pendingRegister = true;
        await WriteEncryptedAsync(ScProtocol.REGISTER,
            ScProtocol.Combine(ScProtocol.PackString8(username), ScProtocol.PackString8(password)));
    }

    public Task JoinRoomAsync(int roomId, string password = "")
    {
        _joiningRoom = true;
        var pwBytes = System.Text.Encoding.UTF8.GetBytes(password ?? "");
        var payload = new byte[2 + pwBytes.Length];
        payload[0] = (byte)roomId;
        payload[1] = (byte)pwBytes.Length;
        Array.Copy(pwBytes, 0, payload, 2, pwBytes.Length);
        return WriteEncryptedAsync(ScProtocol.JOIN_ROOM, payload);
    }

    public Task SendMessageAsync(int roomId, string content)
        => WriteEncryptedAsync(ScProtocol.MESSAGE,
            ScProtocol.Combine([(byte)roomId], ScProtocol.PackString16(content)));

    public Task SendDmOpenAsync(int targetUserId)
    {
        _joiningRoom = true;  // DM open returns a ROOM_INFO we want to join
        var payload = new byte[4];
        payload[0] = (byte)((targetUserId >> 24) & 0xFF);
        payload[1] = (byte)((targetUserId >> 16) & 0xFF);
        payload[2] = (byte)((targetUserId >> 8) & 0xFF);
        payload[3] = (byte)(targetUserId & 0xFF);
        return WriteEncryptedAsync(ScProtocol.DM_OPEN, payload);
    }

    public Task SendMailAsync(int recipientId, string body)
    {
        var idBytes = new byte[4];
        idBytes[0] = (byte)((recipientId >> 24) & 0xFF);
        idBytes[1] = (byte)((recipientId >> 16) & 0xFF);
        idBytes[2] = (byte)((recipientId >> 8) & 0xFF);
        idBytes[3] = (byte)(recipientId & 0xFF);
        return WriteEncryptedAsync(ScProtocol.MAIL_SEND,
            ScProtocol.Combine(idBytes, ScProtocol.PackString16(body)));
    }

    public Task SendMailReadAsync(int mailId)
    {
        var b = new byte[4];
        b[0] = (byte)((mailId >> 24) & 0xFF);
        b[1] = (byte)((mailId >> 16) & 0xFF);
        b[2] = (byte)((mailId >> 8) & 0xFF);
        b[3] = (byte)(mailId & 0xFF);
        return WriteEncryptedAsync(ScProtocol.MAIL_READ, b);
    }

    public Task SendMailDeleteAsync(int mailId)
    {
        var b = new byte[4];
        b[0] = (byte)((mailId >> 24) & 0xFF);
        b[1] = (byte)((mailId >> 16) & 0xFF);
        b[2] = (byte)((mailId >> 8) & 0xFF);
        b[3] = (byte)(mailId & 0xFF);
        return WriteEncryptedAsync(ScProtocol.MAIL_DELETE, b);
    }

    public Task RequestRoomListAsync()
        => WriteEncryptedAsync(ScProtocol.ROOM_LIST, []);

    private void HandleUserList(byte[] payload)
    {
        int count = payload[0]; int pos = 1;
        var users = new List<UserInfo>(count);
        for (int i = 0; i < count; i++)
        {
            int userId = (payload[pos] << 24) | (payload[pos + 1] << 16) |
                         (payload[pos + 2] << 8) | payload[pos + 3]; pos += 4;
            var (username, next) = ScProtocol.UnpackString8(payload, pos); pos = next;
            int roomId = payload[pos++];
            users.Add(new UserInfo(userId, username, roomId));
        }
        OnUserList?.Invoke(users);
    }

    private void HandleUserJoin(byte[] payload)
    {
        int pos = 0;
        int userId = (payload[pos] << 24) | (payload[pos + 1] << 16) |
                     (payload[pos + 2] << 8) | payload[pos + 3]; pos += 4;
        var (username, next) = ScProtocol.UnpackString8(payload, pos); pos = next;
        int roomId = pos < payload.Length ? payload[pos] : 0;
        OnUserJoin?.Invoke(new UserInfo(userId, username, roomId));
    }

    private void HandleUserLeave(byte[] payload)
    {
        int pos = 0;
        int userId = (payload[pos] << 24) | (payload[pos + 1] << 16) |
                     (payload[pos + 2] << 8) | payload[pos + 3]; pos += 4;
        var (username, _) = ScProtocol.UnpackString8(payload, pos);
        OnUserLeave?.Invoke(userId, username);
    }

    public async Task DisconnectAsync()
    {
        try { await WriteEncryptedAsync(ScProtocol.DISCONNECT, []); } catch { }
        _cts.Cancel();
    }

    // ── Ping loop ─────────────────────────────────────────────────────────────

    private async Task PingLoopAsync()
    {
        try
        {
            while (!_cts.IsCancellationRequested)
            {
                await Task.Delay(PingIntervalMs, _cts.Token);
                await WriteEncryptedAsync(ScProtocol.PING, []);
            }
        }
        catch (OperationCanceledException) { }
    }

    // ── Dispose ───────────────────────────────────────────────────────────────

    public async ValueTask DisposeAsync()
    {
        _cts.Cancel();
        _stream?.Dispose();
        _tcp?.Dispose();
        await Task.CompletedTask;
    }
}