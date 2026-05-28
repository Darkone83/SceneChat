// ScCrypto.cs -- DH handshake, HKDF, ChaCha20-Poly1305
// Mirrors sc_net.cpp / sc_dh.cpp on the Xbox client.
// Uses .NET 8 BCL -- no third-party crypto dependencies.

using System.Security.Cryptography;
using System.Text;

namespace SceneChatWPF.Crypto;

public static class ScCrypto
{
    private const int KeyLen   = 32;
    private const int NonceLen = 12;

    // ── HKDF  (info="scenechat-session", salt=32 zero bytes) ────────────────
    public static byte[] DeriveSessionKey(byte[] sharedSecret)
    {
        var salt = new byte[32];
        var info = Encoding.UTF8.GetBytes("scenechat-session");
        return HKDF.DeriveKey(HashAlgorithmName.SHA256, sharedSecret, KeyLen, salt, info);
    }

    // ── Encrypt  →  nonce(12) | ciphertext | tag(16) ────────────────────────
    public static byte[] Encrypt(byte[] key, byte[] plaintext)
    {
        var nonce      = RandomNumberGenerator.GetBytes(NonceLen);
        var ciphertext = new byte[plaintext.Length];
        var tag        = new byte[16];
        using var c    = new ChaCha20Poly1305(key);
        c.Encrypt(nonce, plaintext, ciphertext, tag);

        var result = new byte[NonceLen + ciphertext.Length + 16];
        Buffer.BlockCopy(nonce,      0, result, 0,                          NonceLen);
        Buffer.BlockCopy(ciphertext, 0, result, NonceLen,                   ciphertext.Length);
        Buffer.BlockCopy(tag,        0, result, NonceLen + ciphertext.Length, 16);
        return result;
    }

    // ── Decrypt  ─────────────────────────────────────────────────────────────
    public static byte[]? Decrypt(byte[] key, byte[] data)
    {
        if (data.Length < NonceLen + 16) return null;
        var nonce      = data[..NonceLen];
        var tag        = data[^16..];
        var ciphertext = data[NonceLen..^16];
        var plaintext  = new byte[ciphertext.Length];
        try
        {
            using var c = new ChaCha20Poly1305(key);
            c.Decrypt(nonce, ciphertext, tag, plaintext);
            return plaintext;
        }
        catch (AuthenticationTagMismatchException) { return null; }
    }
}
