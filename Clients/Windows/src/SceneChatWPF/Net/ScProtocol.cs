// ScProtocol.cs -- SCCP packet type constants and wire format helpers.
// Identical to sc_net.h on the Xbox client.

using System.Text;

namespace SceneChatWPF.Net;

public static class ScProtocol
{
    // Packet types
    public const byte DH_INIT = 0x01;
    public const byte DH_RESPONSE = 0x02;
    public const byte REGISTER = 0x03;
    public const byte LOGIN = 0x04;
    public const byte AUTH_OK = 0x05;
    public const byte AUTH_FAIL = 0x06;
    public const byte ROOM_LIST = 0x07;
    public const byte JOIN_ROOM = 0x08;
    public const byte ROOM_INFO = 0x09;
    public const byte MESSAGE = 0x0A;
    public const byte MSG_RECV = 0x0B;
    public const byte HISTORY = 0x0C;
    public const byte ERROR = 0x0D;
    public const byte PING = 0x0E;
    public const byte PONG = 0x0F;
    public const byte DISCONNECT = 0x10;
    public const byte MSG_DELETE = 0x19;
    public const byte USER_LIST = 0x11;
    public const byte USER_JOIN = 0x12;
    public const byte USER_LEAVE = 0x13;
    public const byte DM_OPEN = 0x14;
    public const byte MAIL_LIST = 0x15;
    public const byte MAIL_SEND = 0x16;
    public const byte MAIL_READ = 0x17;
    public const byte MAIL_DELETE = 0x18;

    // ── String packing (matches server.py pack_string8/16) ──────────────────

    public static byte[] PackString8(string s)
    {
        var b = Encoding.UTF8.GetBytes(s);
        var len = Math.Min(b.Length, 255);
        var out_ = new byte[1 + len];
        out_[0] = (byte)len;
        Buffer.BlockCopy(b, 0, out_, 1, len);
        return out_;
    }

    public static byte[] PackString16(string s)
    {
        var b = Encoding.UTF8.GetBytes(s);
        var len = Math.Min(b.Length, 65535);
        var out_ = new byte[2 + len];
        out_[0] = (byte)(len >> 8);
        out_[1] = (byte)(len & 0xFF);
        Buffer.BlockCopy(b, 0, out_, 2, len);
        return out_;
    }

    public static (string value, int next) UnpackString8(byte[] data, int offset)
    {
        int len = data[offset];
        return (Encoding.UTF8.GetString(data, offset + 1, len), offset + 1 + len);
    }

    public static (string value, int next) UnpackString16(byte[] data, int offset)
    {
        int len = (data[offset] << 8) | data[offset + 1];
        return (Encoding.UTF8.GetString(data, offset + 2, len), offset + 2 + len);
    }

    // ── Packet framing ───────────────────────────────────────────────────────
    // Wire: [2B total_len BE][1B type][payload]

    public static byte[] FramePacket(byte type, byte[] payload)
    {
        int totalLen = 2 + 1 + payload.Length;
        var frame = new byte[totalLen];
        frame[0] = (byte)(totalLen >> 8);
        frame[1] = (byte)(totalLen & 0xFF);
        frame[2] = type;
        Buffer.BlockCopy(payload, 0, frame, 3, payload.Length);
        return frame;
    }

    public static byte[] Combine(params byte[][] parts)
    {
        int total = 0;
        foreach (var p in parts) total += p.Length;
        var result = new byte[total];
        int offset = 0;
        foreach (var p in parts)
        {
            Buffer.BlockCopy(p, 0, result, offset, p.Length);
            offset += p.Length;
        }
        return result;
    }
}