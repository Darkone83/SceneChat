/*---------------------------------------------------------------------------
    sc_net.cpp -- SceneChat network layer for Xbox (C89/MSVC2003)

    Follows xb_net pattern exactly.
    Async connect -> DH handshake -> ChaCha20 encrypted binary packets.
---------------------------------------------------------------------------*/

#include <xtl.h>
#include <winsockx.h>
#include "sc_net.h"
#include "sc_log.h"
#include <string.h>

/* ===========================================================================
   S1  Internal state
   =========================================================================== */

static SOCKET        s_sock = INVALID_SOCKET;
static int           s_state = SC_STATE_IDLE;
static void* s_dns = NULL;  /* cast to XNDNS* at use-site */
static DWORD         s_serverAddr = 0;     /* resolved IP, network byte order */
static char          s_serverHost[64] = { 0 };
static DWORD         s_stateStart = 0;
static int           s_netUp = 0;
static unsigned char s_session_key[SC_CHACHA_KEY_LEN];
static char          s_error[128] = { 0 };

static unsigned char s_recv_buf[SC_MAX_PACKET];
static int           s_recv_have = 0;
static int           s_recv_need = 0;

static SC_Packet     s_queue[SC_PACKET_QUEUE_SIZE];
static int           s_queue_head = 0;
static int           s_queue_tail = 0;

/* ===========================================================================
   S2  SHA-256  (FIPS 180-4)
   =========================================================================== */

typedef struct { DWORD h[8]; unsigned char buf[64]; DWORD lo, hi; } sha256_ctx;

static const DWORD k256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,
    0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,
    0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,
    0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
    0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,
    0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,
    0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

#define ROR32(x,n) (((x)>>(n))|((x)<<(32-(n))))
#define ROL32(x,n) (((x)<<(n))|((x)>>(32-(n))))
#define S_CH(x,y,z)  (((x)&(y))^(~(x)&(z)))
#define S_MAJ(x,y,z) (((x)&(y))^((x)&(z))^((y)&(z)))
#define S_S0(x) (ROR32(x,2)^ROR32(x,13)^ROR32(x,22))
#define S_S1(x) (ROR32(x,6)^ROR32(x,11)^ROR32(x,25))
#define S_G0(x) (ROR32(x,7)^ROR32(x,18)^((x)>>3))
#define S_G1(x) (ROR32(x,17)^ROR32(x,19)^((x)>>10))

static void sha256_init(sha256_ctx* c) {
    c->h[0] = 0x6a09e667; c->h[1] = 0xbb67ae85;
    c->h[2] = 0x3c6ef372; c->h[3] = 0xa54ff53a;
    c->h[4] = 0x510e527f; c->h[5] = 0x9b05688c;
    c->h[6] = 0x1f83d9ab; c->h[7] = 0x5be0cd19;
    c->lo = c->hi = 0;
}

static void sha256_compress(sha256_ctx* c, const unsigned char* blk) {
    DWORD w[64], a, b, cc, d, e, f, g, h, t1, t2;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = ((DWORD)blk[i * 4] << 24) | ((DWORD)blk[i * 4 + 1] << 16) |
        ((DWORD)blk[i * 4 + 2] << 8) | (DWORD)blk[i * 4 + 3];
    for (i = 16; i < 64; i++)
        w[i] = S_G1(w[i - 2]) + w[i - 7] + S_G0(w[i - 15]) + w[i - 16];
    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3];
    e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];
    for (i = 0; i < 64; i++) {
        t1 = h + S_S1(e) + S_CH(e, f, g) + k256[i] + w[i];
        t2 = S_S0(a) + S_MAJ(a, b, cc);
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
    c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

static void sha256_update(sha256_ctx* c, const unsigned char* data, int len) {
    DWORD save = c->lo;
    int fill, left;
    c->lo += (DWORD)len * 8;
    if (c->lo < save) c->hi++;
    c->hi += (DWORD)len >> 29;
    fill = (int)((save >> 3) & 63);
    left = 64 - fill;
    if (fill && len >= left) {
        memcpy(c->buf + fill, data, left);
        sha256_compress(c, c->buf);
        data += left; len -= left; fill = 0;
    }
    while (len >= 64) { sha256_compress(c, data); data += 64; len -= 64; }
    if (len > 0) memcpy(c->buf + fill, data, len);
}

static void sha256_final(sha256_ctx* c, unsigned char* out) {
    static const unsigned char pad[64] = { 0x80 };
    unsigned char ml[8];
    DWORD lo = c->lo, hi = c->hi;
    int i, pl;
    ml[0] = (unsigned char)(hi >> 24); ml[1] = (unsigned char)(hi >> 16);
    ml[2] = (unsigned char)(hi >> 8); ml[3] = (unsigned char)(hi);
    ml[4] = (unsigned char)(lo >> 24); ml[5] = (unsigned char)(lo >> 16);
    ml[6] = (unsigned char)(lo >> 8); ml[7] = (unsigned char)(lo);
    pl = (int)(((lo >> 3) & 63) < 56 ? 56 - ((lo >> 3) & 63) : 120 - ((lo >> 3) & 63));
    sha256_update(c, pad, pl);
    sha256_update(c, ml, 8);
    for (i = 0; i < 8; i++) {
        out[i * 4] = (unsigned char)(c->h[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(c->h[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(c->h[i] >> 8);
        out[i * 4 + 3] = (unsigned char)(c->h[i]);
    }
}

static void sha256_hash(const unsigned char* data, int len, unsigned char* out) {
    sha256_ctx c; sha256_init(&c); sha256_update(&c, data, len); sha256_final(&c, out);
}

static void hmac_sha256(const unsigned char* key, int klen,
    const unsigned char* msg, int mlen,
    unsigned char* out) {
    sha256_ctx c;
    unsigned char kpad[64], tmp[32];
    int i;
    if (klen > 64) { sha256_hash(key, klen, kpad); klen = 32; }
    else memcpy(kpad, key, klen);
    for (i = klen; i < 64; i++) kpad[i] = 0;
    sha256_init(&c);
    for (i = 0; i < 64; i++) { unsigned char b = (unsigned char)(kpad[i] ^ 0x36); sha256_update(&c, &b, 1); }
    sha256_update(&c, msg, mlen);
    sha256_final(&c, tmp);
    sha256_init(&c);
    for (i = 0; i < 64; i++) { unsigned char b = (unsigned char)(kpad[i] ^ 0x5c); sha256_update(&c, &b, 1); }
    sha256_update(&c, tmp, 32);
    sha256_final(&c, out);
}

static void hkdf_sha256(const unsigned char* ikm, int ikm_len,
    const unsigned char* info, int info_len,
    unsigned char* out) {
    static const unsigned char zero32[32] = { 0 };
    unsigned char prk[32], buf[256];
    int blen = 0;
    hmac_sha256(zero32, 32, ikm, ikm_len, prk);
    memcpy(buf, info, info_len); blen += info_len;
    buf[blen++] = 0x01;
    hmac_sha256(prk, 32, buf, blen, out);
}

/* ===========================================================================
   S3  ChaCha20  (RFC 7539)
   =========================================================================== */

#define QR(a,b,c,d) \
    a+=b;d^=a;d=ROL32(d,16); \
    c+=d;b^=c;b=ROL32(b,12); \
    a+=b;d^=a;d=ROL32(d, 8); \
    c+=d;b^=c;b=ROL32(b, 7)

static void chacha20_block(DWORD out[16], const DWORD in[16]) {
    DWORD x[16]; int i;
    memcpy(x, in, 64);
    for (i = 0; i < 10; i++) {
        QR(x[0], x[4], x[8], x[12]); QR(x[1], x[5], x[9], x[13]);
        QR(x[2], x[6], x[10], x[14]); QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]); QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[8], x[13]); QR(x[3], x[4], x[9], x[14]);
    }
    for (i = 0; i < 16; i++) out[i] = x[i] + in[i];
}

static void chacha20_xor(const unsigned char key[32], const unsigned char nonce[12],
    DWORD ctr, const unsigned char* in, unsigned char* out, int len) {
    DWORD st[16], blk[16];
    unsigned char kb[64];
    int i, pos = 0, take;
    st[0] = 0x61707865; st[1] = 0x3320646e; st[2] = 0x79622d32; st[3] = 0x6b206574;
    for (i = 0; i < 8; i++)
        st[4 + i] = ((DWORD)key[i * 4]) | ((DWORD)key[i * 4 + 1] << 8) |
        ((DWORD)key[i * 4 + 2] << 16) | ((DWORD)key[i * 4 + 3] << 24);
    st[12] = ctr;
    st[13] = ((DWORD)nonce[0]) | ((DWORD)nonce[1] << 8) | ((DWORD)nonce[2] << 16) | ((DWORD)nonce[3] << 24);
    st[14] = ((DWORD)nonce[4]) | ((DWORD)nonce[5] << 8) | ((DWORD)nonce[6] << 16) | ((DWORD)nonce[7] << 24);
    st[15] = ((DWORD)nonce[8]) | ((DWORD)nonce[9] << 8) | ((DWORD)nonce[10] << 16) | ((DWORD)nonce[11] << 24);
    while (pos < len) {
        chacha20_block(blk, st); st[12]++;
        for (i = 0; i < 16; i++) {
            kb[i * 4] = (unsigned char)(blk[i]);
            kb[i * 4 + 1] = (unsigned char)(blk[i] >> 8);
            kb[i * 4 + 2] = (unsigned char)(blk[i] >> 16);
            kb[i * 4 + 3] = (unsigned char)(blk[i] >> 24);
        }
        take = len - pos; if (take > 64) take = 64;
        for (i = 0; i < take; i++) out[pos + i] = in[pos + i] ^ kb[i];
        pos += take;
    }
}

static void chacha20_poly_key(const unsigned char key[32], const unsigned char nonce[12],
    unsigned char poly_key[32]) {
    unsigned char zero32[32] = { 0 };
    chacha20_xor(key, nonce, 0, zero32, poly_key, 32);
}

/* ===========================================================================
   S4  Poly1305  (RFC 7539)
   =========================================================================== */

typedef struct {
    DWORD r[5], h[5], pad[4];
    unsigned char buf[16];
    int buf_len;
} poly_ctx;

static void poly_init(poly_ctx* p, const unsigned char key[32]) {
    DWORD t0, t1, t2, t3;
    t0 = ((DWORD)key[0]) | ((DWORD)key[1] << 8) | ((DWORD)key[2] << 16) | ((DWORD)key[3] << 24);
    t1 = ((DWORD)key[4]) | ((DWORD)key[5] << 8) | ((DWORD)key[6] << 16) | ((DWORD)key[7] << 24);
    t2 = ((DWORD)key[8]) | ((DWORD)key[9] << 8) | ((DWORD)key[10] << 16) | ((DWORD)key[11] << 24);
    t3 = ((DWORD)key[12]) | ((DWORD)key[13] << 8) | ((DWORD)key[14] << 16) | ((DWORD)key[15] << 24);
    p->r[0] = t0 & 0x3ffffff;
    p->r[1] = ((t0 >> 26) | (t1 << 6)) & 0x3ffff03;
    p->r[2] = ((t1 >> 20) | (t2 << 12)) & 0x3ffc0ff;
    p->r[3] = ((t2 >> 14) | (t3 << 18)) & 0x3f03fff;
    p->r[4] = (t3 >> 8) & 0x00fffff;
    p->h[0] = p->h[1] = p->h[2] = p->h[3] = p->h[4] = 0;
    p->pad[0] = ((DWORD)key[16]) | ((DWORD)key[17] << 8) | ((DWORD)key[18] << 16) | ((DWORD)key[19] << 24);
    p->pad[1] = ((DWORD)key[20]) | ((DWORD)key[21] << 8) | ((DWORD)key[22] << 16) | ((DWORD)key[23] << 24);
    p->pad[2] = ((DWORD)key[24]) | ((DWORD)key[25] << 8) | ((DWORD)key[26] << 16) | ((DWORD)key[27] << 24);
    p->pad[3] = ((DWORD)key[28]) | ((DWORD)key[29] << 8) | ((DWORD)key[30] << 16) | ((DWORD)key[31] << 24);
    p->buf_len = 0;
}

static void poly_block(poly_ctx* p, const unsigned char m[16], int hibit) {
    DWORD r0, r1, r2, r3, r4, s1, s2, s3, s4;
    DWORD h0, h1, h2, h3, h4, c;
    unsigned __int64 d0, d1, d2, d3, d4;
    DWORD t0, t1, t2, t3;
    t0 = ((DWORD)m[0]) | ((DWORD)m[1] << 8) | ((DWORD)m[2] << 16) | ((DWORD)m[3] << 24);
    t1 = ((DWORD)m[4]) | ((DWORD)m[5] << 8) | ((DWORD)m[6] << 16) | ((DWORD)m[7] << 24);
    t2 = ((DWORD)m[8]) | ((DWORD)m[9] << 8) | ((DWORD)m[10] << 16) | ((DWORD)m[11] << 24);
    t3 = ((DWORD)m[12]) | ((DWORD)m[13] << 8) | ((DWORD)m[14] << 16) | ((DWORD)m[15] << 24);
    h0 = p->h[0] + (t0 & 0x3ffffff);
    h1 = p->h[1] + ((t0 >> 26 | (t1 << 6)) & 0x3ffffff);
    h2 = p->h[2] + ((t1 >> 20 | (t2 << 12)) & 0x3ffffff);
    h3 = p->h[3] + ((t2 >> 14 | (t3 << 18)) & 0x3ffffff);
    h4 = p->h[4] + (t3 >> 8) + (hibit ? (1u << 24) : 0);
    r0 = p->r[0]; r1 = p->r[1]; r2 = p->r[2]; r3 = p->r[3]; r4 = p->r[4];
    s1 = r1 * 5; s2 = r2 * 5; s3 = r3 * 5; s4 = r4 * 5;
    d0 = (unsigned __int64)h0 * r0 + (unsigned __int64)h1 * s4 + (unsigned __int64)h2 * s3 + (unsigned __int64)h3 * s2 + (unsigned __int64)h4 * s1;
    d1 = (unsigned __int64)h0 * r1 + (unsigned __int64)h1 * r0 + (unsigned __int64)h2 * s4 + (unsigned __int64)h3 * s3 + (unsigned __int64)h4 * s2;
    d2 = (unsigned __int64)h0 * r2 + (unsigned __int64)h1 * r1 + (unsigned __int64)h2 * r0 + (unsigned __int64)h3 * s4 + (unsigned __int64)h4 * s3;
    d3 = (unsigned __int64)h0 * r3 + (unsigned __int64)h1 * r2 + (unsigned __int64)h2 * r1 + (unsigned __int64)h3 * r0 + (unsigned __int64)h4 * s4;
    d4 = (unsigned __int64)h0 * r4 + (unsigned __int64)h1 * r3 + (unsigned __int64)h2 * r2 + (unsigned __int64)h3 * r1 + (unsigned __int64)h4 * r0;
    c = (DWORD)(d0 >> 26); h0 = (DWORD)d0 & 0x3ffffff; d1 += c;
    c = (DWORD)(d1 >> 26); h1 = (DWORD)d1 & 0x3ffffff; d2 += c;
    c = (DWORD)(d2 >> 26); h2 = (DWORD)d2 & 0x3ffffff; d3 += c;
    c = (DWORD)(d3 >> 26); h3 = (DWORD)d3 & 0x3ffffff; d4 += c;
    c = (DWORD)(d4 >> 26); h4 = (DWORD)d4 & 0x3ffffff; h0 += c * 5;
    c = h0 >> 26; h0 &= 0x3ffffff; h1 += c;
    p->h[0] = h0; p->h[1] = h1; p->h[2] = h2; p->h[3] = h3; p->h[4] = h4;
}

static void poly_update(poly_ctx* p, const unsigned char* data, int len) {
    int have = p->buf_len, need;
    while (len > 0) {
        need = 16 - have;
        if (len < need) { memcpy(p->buf + have, data, len); p->buf_len += len; return; }
        memcpy(p->buf + have, data, need);
        poly_block(p, p->buf, 1);
        data += need; len -= need; have = 0; p->buf_len = 0;
    }
}

static void poly_finish(poly_ctx* p, unsigned char tag[16]) {
    DWORD h0, h1, h2, h3, h4, c, g0, g1, g2, g3, g4, mask;
    unsigned __int64 f;
    if (p->buf_len) {
        unsigned char tmp[16] = { 0 };
        memcpy(tmp, p->buf, p->buf_len);
        tmp[p->buf_len] = 1;
        poly_block(p, tmp, 0);
    }
    h0 = p->h[0]; h1 = p->h[1]; h2 = p->h[2]; h3 = p->h[3]; h4 = p->h[4];
    c = h1 >> 26; h1 &= 0x3ffffff; h2 += c;
    c = h2 >> 26; h2 &= 0x3ffffff; h3 += c;
    c = h3 >> 26; h3 &= 0x3ffffff; h4 += c;
    c = h4 >> 26; h4 &= 0x3ffffff; h0 += c * 5;
    c = h0 >> 26; h0 &= 0x3ffffff; h1 += c;
    g0 = h0 + 5; c = g0 >> 26; g0 &= 0x3ffffff;
    g1 = h1 + c; c = g1 >> 26; g1 &= 0x3ffffff;
    g2 = h2 + c; c = g2 >> 26; g2 &= 0x3ffffff;
    g3 = h3 + c; c = g3 >> 26; g3 &= 0x3ffffff;
    g4 = h4 + c - ((DWORD)1 << 26);
    mask = (g4 >> 31) - 1;
    g0 &= mask; g1 &= mask; g2 &= mask; g3 &= mask; g4 &= mask;
    mask = ~mask;
    h0 = (h0 & mask) | g0; h1 = (h1 & mask) | g1;
    h2 = (h2 & mask) | g2; h3 = (h3 & mask) | g3; h4 = (h4 & mask) | g4;
    /* Reconstruct h as 4 LE 32-bit words using natural DWORD overflow --
       no explicit carry needed between limbs. Then add pad separately
       with 64-bit carry propagation. The old code fed f>>=32 (which
       already contained h1>>6) back into the next step that also added
       h1>>6, double-counting it and producing a wrong tag. */
    {
        DWORD hb0, hb1, hb2, hb3;
        hb0 = h0 | (h1 << 26);
        hb1 = (h1 >> 6) | (h2 << 20);
        hb2 = (h2 >> 12) | (h3 << 14);
        hb3 = (h3 >> 18) | (h4 << 8);
        f = (unsigned __int64)hb0 + p->pad[0];
        tag[0] = (unsigned char)f; tag[1] = (unsigned char)(f >> 8);
        tag[2] = (unsigned char)(f >> 16); tag[3] = (unsigned char)(f >> 24); f >>= 32;
        f += (unsigned __int64)hb1 + p->pad[1];
        tag[4] = (unsigned char)f; tag[5] = (unsigned char)(f >> 8);
        tag[6] = (unsigned char)(f >> 16); tag[7] = (unsigned char)(f >> 24); f >>= 32;
        f += (unsigned __int64)hb2 + p->pad[2];
        tag[8] = (unsigned char)f; tag[9] = (unsigned char)(f >> 8);
        tag[10] = (unsigned char)(f >> 16); tag[11] = (unsigned char)(f >> 24); f >>= 32;
        f += (unsigned __int64)hb3 + p->pad[3];
        tag[12] = (unsigned char)f; tag[13] = (unsigned char)(f >> 8);
        tag[14] = (unsigned char)(f >> 16); tag[15] = (unsigned char)(f >> 24);
    }
}

/* ===========================================================================
   S5  ChaCha20-Poly1305 AEAD  (RFC 7539)
   =========================================================================== */

int SC_Crypto_Encrypt(const unsigned char* key, const unsigned char* nonce,
    const unsigned char* plain, int plain_len,
    unsigned char* out, int* out_len) {
    poly_ctx mac;
    unsigned char poly_key[32];
    unsigned char len_buf[8];
    *out_len = plain_len + SC_POLY_TAG_LEN;
    /* RFC 7539 ChaCha20-Poly1305: empty AAD.
       MAC input = ciphertext | pad16(ciphertext) | u64_LE(0) | u64_LE(ct_len) */
    chacha20_poly_key(key, nonce, poly_key);
    chacha20_xor(key, nonce, 1, plain, out, plain_len);
    poly_init(&mac, poly_key);
    poly_update(&mac, out, plain_len);
    if (plain_len & 15) {
        unsigned char pad[16] = { 0 };
        poly_update(&mac, pad, 16 - (plain_len & 15));
    }
    /* aad_len = 0 (8 bytes LE) */
    memset(len_buf, 0, 8);
    poly_update(&mac, len_buf, 8);
    /* ciphertext_len (8 bytes LE) */
    len_buf[0] = (unsigned char)plain_len;
    len_buf[1] = (unsigned char)(plain_len >> 8);
    len_buf[2] = (unsigned char)(plain_len >> 16);
    memset(len_buf + 3, 0, 5);
    poly_update(&mac, len_buf, 8);
    poly_finish(&mac, out + plain_len);
    return 1;
}

int SC_Crypto_Decrypt(const unsigned char* key, const unsigned char* nonce,
    const unsigned char* cipher, int cipher_len,
    unsigned char* out, int* out_len) {
    poly_ctx mac;
    unsigned char poly_key[32];
    unsigned char computed_tag[16];
    unsigned char len_buf[8];
    int plain_len = cipher_len - SC_POLY_TAG_LEN;
    if (plain_len < 0) return 0;
    chacha20_poly_key(key, nonce, poly_key);
    poly_init(&mac, poly_key);
    poly_update(&mac, cipher, plain_len);
    if (plain_len & 15) {
        unsigned char pad[16] = { 0 };
        poly_update(&mac, pad, 16 - (plain_len & 15));
    }
    memset(len_buf, 0, 8);
    poly_update(&mac, len_buf, 8);
    len_buf[0] = (unsigned char)plain_len;
    len_buf[1] = (unsigned char)(plain_len >> 8);
    len_buf[2] = (unsigned char)(plain_len >> 16);
    memset(len_buf + 3, 0, 5);
    poly_update(&mac, len_buf, 8);
    poly_finish(&mac, computed_tag);
    if (memcmp(computed_tag, cipher + plain_len, 16) != 0) return 0;
    chacha20_xor(key, nonce, 1, cipher, out, plain_len);
    *out_len = plain_len;
    return 1;
}

void SC_Crypto_RandNonce(unsigned char* nonce) {
    XNetRandom(nonce, SC_CHACHA_NONCE_LEN);
}

/* ===========================================================================
   S6  Blocking recv helper  (used during handshake only)
   =========================================================================== */

static int recv_exact(unsigned char* buf, int len) {
    int got = 0, n;
    while (got < len) {
        n = recv(s_sock, (char*)buf + got, len - got, 0);
        if (n <= 0) return 0;
        got += n;
    }
    return 1;
}

/* ===========================================================================
   S7  Packet send helpers
   =========================================================================== */

static int net_send_raw(const unsigned char* buf, int len) {
    int sent = 0, n;
    while (sent < len) {
        n = send(s_sock, (const char*)buf + sent, len - sent, 0);
        if (n == SOCKET_ERROR) return 0;
        sent += n;
    }
    return 1;
}

static int net_send_plain(unsigned char type, const unsigned char* payload, int plen) {
    unsigned char hdr[3];
    int total = plen + 3;
    hdr[0] = (unsigned char)(total >> 8);
    hdr[1] = (unsigned char)(total);
    hdr[2] = type;
    if (!net_send_raw(hdr, 3)) return 0;
    if (plen > 0 && !net_send_raw(payload, plen)) return 0;
    return 1;
}

static int net_send_enc(unsigned char type, const unsigned char* payload, int plen) {
    unsigned char nonce[SC_CHACHA_NONCE_LEN];
    unsigned char cipher[SC_MAX_PACKET];
    unsigned char hdr[3];
    int clen, total;
    SC_Crypto_RandNonce(nonce);
    if (!SC_Crypto_Encrypt(s_session_key, nonce, payload ? payload : (const unsigned char*)"", plen, cipher, &clen)) return 0;
    total = 3 + SC_CHACHA_NONCE_LEN + clen;
    hdr[0] = (unsigned char)(total >> 8);
    hdr[1] = (unsigned char)(total);
    hdr[2] = type;
    if (!net_send_raw(hdr, 3)) return 0;
    if (!net_send_raw(nonce, SC_CHACHA_NONCE_LEN)) return 0;
    if (!net_send_raw(cipher, clen)) return 0;
    return 1;
}

/* ===========================================================================
   S8  Packet queue
   =========================================================================== */

static void queue_push(unsigned char type, const unsigned char* data, int len) {
    SC_Packet* p;
    int next = (s_queue_tail + 1) % SC_PACKET_QUEUE_SIZE;
    if (next == s_queue_head) return;
    p = &s_queue[s_queue_tail];
    p->type = type;
    if (len > SC_MAX_PACKET) len = SC_MAX_PACKET;
    memcpy(p->data, data, len);
    p->len = len;
    s_queue_tail = next;
}

static int queue_peek_type(unsigned char type) {
    int i = s_queue_head;
    while (i != s_queue_tail) {
        if (s_queue[i].type == type) return i;
        i = (i + 1) % SC_PACKET_QUEUE_SIZE;
    }
    return -1;
}

static int queue_pop(int idx, unsigned char* data, int* len) {
    SC_Packet* p = &s_queue[idx];
    int i, next;
    *len = p->len;
    memcpy(data, p->data, p->len);
    i = idx;
    while (1) {
        next = (i + 1) % SC_PACKET_QUEUE_SIZE;
        if (next == s_queue_tail) { s_queue_tail = i; break; }
        s_queue[i] = s_queue[next];
        i = next;
    }
    return 1;
}

/* ===========================================================================
   S9  DH handshake
   =========================================================================== */

extern "C" int SC_DH_ComputeSession(
    const unsigned char* params_pem, int params_len,
    const unsigned char* server_pub_pem, int server_pub_len,
    unsigned char* client_pub_pem_out, int* client_pub_len_out,
    unsigned char* session_key_out);

static int do_dh_handshake(void) {
    unsigned char buf[4096];
    unsigned char params_pem[2048], server_pub_pem[2048];
    unsigned char client_pub_raw[128];  /* max 1024-bit = 128 bytes */
    unsigned char hdr[2];
    int params_len, server_pub_len, client_pub_raw_len;
    unsigned char payload[4096];
    int total, body, plen, ok;

    /* Read 2-byte length header */
    if (!recv_exact(hdr, 2)) { lstrcpyA(s_error, "DH: header recv failed"); return 0; }
    total = ((int)hdr[0] << 8) | hdr[1];
    body = total - 2;
    if (body<5 || body>(int)sizeof(buf)) { lstrcpyA(s_error, "DH: packet too large"); return 0; }
    if (!recv_exact(buf, body)) { lstrcpyA(s_error, "DH: body recv failed"); return 0; }
    if (buf[0] != SCCP_DH_INIT) { lstrcpyA(s_error, "DH: expected DH_INIT"); return 0; }

    {
        const unsigned char* p = buf + 1;
        params_len = ((int)p[0] << 8) | p[1]; p += 2;
        if (params_len<1 || params_len>(int)sizeof(params_pem)) { lstrcpyA(s_error, "DH: params too large"); return 0; }
        memcpy(params_pem, p, params_len); p += params_len;
        server_pub_len = ((int)p[0] << 8) | p[1]; p += 2;
        if (server_pub_len<1 || server_pub_len>(int)sizeof(server_pub_pem)) { lstrcpyA(s_error, "DH: pub too large"); return 0; }
        memcpy(server_pub_pem, p, server_pub_len);
    }

    client_pub_raw_len = 0;
    ok = SC_DH_ComputeSession(
        params_pem, params_len,
        server_pub_pem, server_pub_len,
        client_pub_raw, &client_pub_raw_len,
        s_session_key);
    if (!ok) { lstrcpyA(s_error, "DH: computation failed"); return 0; }

    /* Send raw bignum bytes -- server reconstructs DHPublicNumbers directly */
    plen = 0;
    payload[plen++] = (unsigned char)(client_pub_raw_len >> 8);
    payload[plen++] = (unsigned char)(client_pub_raw_len);
    memcpy(payload + plen, client_pub_raw, client_pub_raw_len);
    plen += client_pub_raw_len;
    if (!net_send_plain(SCCP_DH_RESPONSE, payload, plen)) {
        lstrcpyA(s_error, "DH: send failed"); return 0;
    }
    return 1;
}

/* ===========================================================================
   S10  Pack / unpack helpers
   =========================================================================== */

static int pack_str8(unsigned char* buf, int pos, const char* s) {
    int len = (int)lstrlenA(s); if (len > 255) len = 255;
    buf[pos++] = (unsigned char)len;
    memcpy(buf + pos, s, len);
    return pos + len;
}

static int pack_str16(unsigned char* buf, int pos, const char* s) {
    int len = (int)lstrlenA(s); if (len > SC_MAX_MSGLEN) len = SC_MAX_MSGLEN;
    buf[pos++] = (unsigned char)(len >> 8);
    buf[pos++] = (unsigned char)(len);
    memcpy(buf + pos, s, len);
    return pos + len;
}

static int unpack_str8(const unsigned char* buf, int pos, char* out, int outsz) {
    int len = buf[pos++];
    if (len >= outsz) len = outsz - 1;
    memcpy(out, buf + pos, len); out[len] = 0;
    return pos + len;
}

static int unpack_str16(const unsigned char* buf, int pos, char* out, int outsz) {
    int len = ((int)buf[pos] << 8) | buf[pos + 1]; pos += 2;
    if (len >= outsz) len = outsz - 1;
    memcpy(out, buf + pos, len); out[len] = 0;
    return pos + len;
}

/* ===========================================================================
   S11  Public API
   =========================================================================== */

void SC_Net_Init(void) {
    s_sock = INVALID_SOCKET;
    s_state = SC_STATE_IDLE;
    s_recv_have = 0;
    s_recv_need = 0;
    s_queue_head = 0;
    s_queue_tail = 0;
    s_error[0] = 0;
    memset(s_session_key, 0, sizeof(s_session_key));
}

void SC_Net_Shutdown(void) {
    if (s_dns) { XNetDnsRelease((XNDNS*)s_dns); s_dns = NULL; }
    if (s_sock != INVALID_SOCKET) { closesocket(s_sock); s_sock = INVALID_SOCKET; }
    s_state = SC_STATE_IDLE;
}

int SC_Net_ConnectBegin(const char* server_ip) {
    if (s_state != SC_STATE_IDLE) SC_Net_Shutdown();
    if (s_dns) { XNetDnsRelease((XNDNS*)s_dns); s_dns = NULL; }

    { int _i = 0; while (server_ip[_i] && _i < (int)sizeof(s_serverHost) - 1) { s_serverHost[_i] = server_ip[_i]; _i++; } s_serverHost[_i] = 0; }
    s_error[0] = 0;

    /* NetEnsure -- ref-counted, safe to call repeatedly (xb_net.cpp pattern) */
    if (!s_netUp) {
        XNetStartupParams xnsp;
        WSADATA wsa;
        ZeroMemory(&xnsp, sizeof(xnsp));
        xnsp.cfgSizeOfStruct = sizeof(xnsp);
        xnsp.cfgFlags = XNET_STARTUP_BYPASS_SECURITY;
        XNetStartup(&xnsp);
        WSAStartup(MAKEWORD(2, 2), &wsa);
        s_netUp = 1;
    }

    s_stateStart = GetTickCount();
    s_state = SC_STATE_LINK;
    SC_Log("NET", "ConnectBegin -> LINK");
    return 0;
}

int SC_Net_ConnectPoll(void) {
    switch (s_state) {

    case SC_STATE_IDLE:   return  0;
    case SC_STATE_READY:  return  1;
    case SC_STATE_ERROR:  return -1;

    case SC_STATE_LINK: {
        XNADDR xna;
        DWORD  st;
        ZeroMemory(&xna, sizeof(xna));
        st = XNetGetTitleXnAddr(&xna);
        if (st == XNET_GET_XNADDR_PENDING) {
            if (GetTickCount() - s_stateStart > SC_TIMEOUT_LINK)
            {
                lstrcpyA(s_error, "No network link"); s_state = SC_STATE_ERROR;
            }
            return 0;
        }
        if ((st & XNET_GET_XNADDR_NONE) || xna.ina.s_addr == 0)
        {
            lstrcpyA(s_error, "No network link"); SC_Log("NET", "ERR: No network link"); s_state = SC_STATE_ERROR; return -1;
        }
        if (XNetDnsLookup(s_serverHost, NULL, (XNDNS**)&s_dns) != 0 || !s_dns)
        {
            lstrcpyA(s_error, "DNS start failed"); SC_Log("NET", "ERR: DNS start failed"); s_state = SC_STATE_ERROR; return -1;
        }
        SC_Log("NET", "LINK ok -> DNS");
        s_stateStart = GetTickCount();
        s_state = SC_STATE_DNS;
        return 0;
    }

    case SC_STATE_DNS: {
        if (!s_dns) { lstrcpyA(s_error, "DNS handle null"); s_state = SC_STATE_ERROR; return -1; }
        if (((XNDNS*)s_dns)->iStatus == WSAEINPROGRESS) {
            if (GetTickCount() - s_stateStart > SC_TIMEOUT_DNS)
            {
                XNetDnsRelease((XNDNS*)s_dns); s_dns = NULL; lstrcpyA(s_error, "DNS timeout"); SC_Log("NET", "ERR: DNS timeout"); s_state = SC_STATE_ERROR;
            }
            return 0;
        }
        if (((XNDNS*)s_dns)->iStatus != 0)
        {
            XNetDnsRelease((XNDNS*)s_dns); s_dns = NULL; lstrcpyA(s_error, "DNS failed"); SC_Log("NET", "ERR: DNS failed"); s_state = SC_STATE_ERROR; return -1;
        }

        s_serverAddr = ((XNDNS*)s_dns)->aina[0].s_addr;
        XNetDnsRelease((XNDNS*)s_dns); s_dns = NULL;

        s_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s_sock == INVALID_SOCKET)
        {
            lstrcpyA(s_error, "socket() failed"); s_state = SC_STATE_ERROR; return -1;
        }
        {
            unsigned long nb = 1;
            ioctlsocket(s_sock, FIONBIO, &nb);
        }
        {
            struct sockaddr_in sa;
            ZeroMemory(&sa, sizeof(sa));
            sa.sin_family = AF_INET;
            sa.sin_port = htons(SC_SERVER_PORT);
            sa.sin_addr.s_addr = s_serverAddr;
            if (connect(s_sock, (struct sockaddr*)&sa, sizeof(sa)) != 0 &&
                WSAGetLastError() != WSAEWOULDBLOCK)
            {
                lstrcpyA(s_error, "connect() failed"); closesocket(s_sock); s_sock = INVALID_SOCKET; s_state = SC_STATE_ERROR; return -1;
            }
        }
        SC_Log("NET", "DNS ok -> CONNECTING");
        s_stateStart = GetTickCount();
        s_state = SC_STATE_CONNECTING;
        return 0;
    }

    case SC_STATE_CONNECTING: {
        fd_set wfds, efds;
        TIMEVAL tv = { 0,0 };
        int r;
        if (GetTickCount() - s_stateStart > SC_TIMEOUT_TCP)
        {
            lstrcpyA(s_error, "Connect timeout"); SC_Log("NET", "ERR: connect timeout"); s_state = SC_STATE_ERROR; return -1;
        }
        FD_ZERO(&wfds); FD_SET(s_sock, &wfds);
        FD_ZERO(&efds); FD_SET(s_sock, &efds);
        r = select(0, NULL, &wfds, &efds, &tv);
        if (r == SOCKET_ERROR) { lstrcpyA(s_error, "select() failed");  s_state = SC_STATE_ERROR; return -1; }
        if (FD_ISSET(s_sock, &efds)) { lstrcpyA(s_error, "Connect refused");  s_state = SC_STATE_ERROR; return -1; }
        if (!FD_ISSET(s_sock, &wfds))    return 0;

        SC_Log("NET", "TCP ok -> HANDSHAKE");
        s_state = SC_STATE_HANDSHAKE;
        { unsigned long block = 0; ioctlsocket(s_sock, FIONBIO, &block); }
        { DWORD tv = 10000; setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv)); }
        if (!do_dh_handshake()) { s_state = SC_STATE_ERROR; return -1; }
        /* Clear RCVTIMEO before restoring non-blocking -- prevents timeout errors in poll */
        { DWORD tv = 0; setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv)); }
        { unsigned long nb = 1; ioctlsocket(s_sock, FIONBIO, &nb); }
        SC_Log("NET", "HANDSHAKE ok -> READY");
        s_state = SC_STATE_READY; return 1;
    }

    default: return 0;
    }
}

void SC_Net_Disconnect(void) { SC_Net_Shutdown(); }
int  SC_Net_IsReady(void) { return s_state == SC_STATE_READY ? 1 : 0; }
int  SC_Net_GetState(void) { return s_state; }
void SC_Net_GetError(char* buf, int bufLen) {
    int i = 0;
    while (s_error[i] && i < bufLen - 1) { buf[i] = s_error[i]; i++; }
    buf[i] = 0;
}

/* ===========================================================================
   S12  Per-frame poll
   =========================================================================== */

void SC_Net_Poll(void) {
    unsigned char plain[SC_MAX_PACKET];
    int n, plain_len;

    if (s_state != SC_STATE_READY) return;

    for (;;) {
        if (s_recv_need == 0) {
            n = recv(s_sock, (char*)s_recv_buf + s_recv_have, 2 - s_recv_have, 0);
            if (n == SOCKET_ERROR) { if (WSAGetLastError() == WSAEWOULDBLOCK) return; goto disc; }
            if (n == 0) goto disc;
            s_recv_have += n;
            if (s_recv_have < 2) return;
            s_recv_need = (((int)s_recv_buf[0] << 8) | s_recv_buf[1]) - 2;
            if (s_recv_need<1 || s_recv_need>SC_MAX_PACKET) goto disc;
            s_recv_have = 0;
        }

        n = recv(s_sock, (char*)s_recv_buf + s_recv_have, s_recv_need - s_recv_have, 0);
        if (n == SOCKET_ERROR) { if (WSAGetLastError() == WSAEWOULDBLOCK) return; goto disc; }
        if (n == 0) goto disc;
        s_recv_have += n;
        if (s_recv_have < s_recv_need) return;

        {
            unsigned char ptype = s_recv_buf[0];
            const unsigned char* nonce = s_recv_buf + 1;
            const unsigned char* cipher = s_recv_buf + 1 + SC_CHACHA_NONCE_LEN;
            int clen = s_recv_need - 1 - SC_CHACHA_NONCE_LEN;
            if (clen > 0 && SC_Crypto_Decrypt(s_session_key, nonce, cipher, clen, plain, &plain_len))
                queue_push(ptype, plain, plain_len);
        }
        s_recv_have = 0;
        s_recv_need = 0;
    }
disc:
    s_state = SC_STATE_ERROR;
    lstrcpyA(s_error, "disconnected");
}

/* ===========================================================================
   S13  Send API
   =========================================================================== */

int SC_Net_SendRegister(const char* username, const char* password) {
    unsigned char buf[SC_MAX_USERNAME + SC_MAX_PASSWORD + 4];
    int pos = 0;
    pos = pack_str8(buf, pos, username);
    pos = pack_str8(buf, pos, password);
    return net_send_enc(SCCP_REGISTER, buf, pos);
}

int SC_Net_SendLogin(const char* username, const char* password) {
    unsigned char buf[SC_MAX_USERNAME + SC_MAX_PASSWORD + 4];
    int pos = 0;
    pos = pack_str8(buf, pos, username);
    pos = pack_str8(buf, pos, password);
    return net_send_enc(SCCP_LOGIN, buf, pos);
}

int SC_Net_SendRoomList(void) {
    return net_send_enc(SCCP_ROOM_LIST, NULL, 0);
}

int SC_Net_SendJoinRoom(unsigned char room_id) {
    unsigned char buf[1];
    buf[0] = room_id;
    return net_send_enc(SCCP_JOIN_ROOM, buf, 1);
}

int SC_Net_SendMessage(unsigned char room_id, const char* content) {
    unsigned char buf[SC_MAX_MSGLEN + 4];
    int pos = 0;
    buf[pos++] = room_id;
    pos = pack_str16(buf, pos, content);
    return net_send_enc(SCCP_MESSAGE, buf, pos);
}

int SC_Net_SendPing(void) {
    return net_send_enc(SCCP_PING, NULL, 0);
}

/* ===========================================================================
   S14  Receive API
   =========================================================================== */

int SC_Net_RecvAuthResult(SC_AuthResult* pOut) {
    unsigned char data[SC_MAX_PACKET];
    int len, idx, pos;
    idx = queue_peek_type(SCCP_AUTH_OK);
    if (idx < 0) return 0;
    queue_pop(idx, data, &len);
    pOut->user_id = ((DWORD)data[0] << 24) | ((DWORD)data[1] << 16) |
        ((DWORD)data[2] << 8) | (DWORD)data[3];
    pos = 4;
    pos = unpack_str8(data, pos, pOut->token, sizeof(pOut->token));
    pos = unpack_str8(data, pos, pOut->username, sizeof(pOut->username));
    return 1;
}

int SC_Net_RecvRoomList(SC_Room* rooms, int* count) {
    unsigned char data[SC_MAX_PACKET];
    int len, idx, i, pos;
    idx = queue_peek_type(SCCP_ROOM_LIST);
    if (idx < 0) return 0;
    queue_pop(idx, data, &len);
    *count = data[0]; pos = 1;
    if (*count > SC_MAX_ROOMS) *count = SC_MAX_ROOMS;
    for (i = 0; i < *count; i++) {
        rooms[i].id = data[pos++];
        rooms[i].type = data[pos++];
        rooms[i].password_flag = data[pos++] ? 1 : 0;
        pos = unpack_str8(data, pos, rooms[i].name, sizeof(rooms[i].name));
    }
    return 1;
}

int SC_Net_RecvRoomInfo(SC_RoomInfo* pOut) {
    unsigned char data[SC_MAX_PACKET];
    int len, idx, i, pos;
    idx = queue_peek_type(SCCP_ROOM_INFO);
    if (idx < 0) return 0;
    queue_pop(idx, data, &len);
    pOut->room_id = data[0];
    pOut->room_type = data[1];
    pos = 2;
    pos = unpack_str8(data, pos, pOut->room_name, sizeof(pOut->room_name));
    pOut->history_count = data[pos++];
    if (pOut->history_count > SC_MAX_HISTORY) pOut->history_count = SC_MAX_HISTORY;
    for (i = 0; i < pOut->history_count; i++) {
        pos += 4; /* skip msg_id (4 bytes) -- protocol v1.1 */
        pos = unpack_str8(data, pos, pOut->history[i].username, sizeof(pOut->history[i].username));
        pos = unpack_str16(data, pos, pOut->history[i].content, sizeof(pOut->history[i].content));
        pos = unpack_str8(data, pos, pOut->history[i].timestamp, sizeof(pOut->history[i].timestamp));
    }
    return 1;
}

int SC_Net_RecvMessage(SC_Message* pOut) {
    unsigned char data[SC_MAX_PACKET];
    int len, idx, pos;
    idx = queue_peek_type(SCCP_MSG_RECV);
    if (idx < 0) return 0;
    queue_pop(idx, data, &len);
    pOut->room_id = data[0];
    pos = 1;
    pos = unpack_str8(data, pos, pOut->username, sizeof(pOut->username));
    pos = unpack_str16(data, pos, pOut->content, sizeof(pOut->content));
    pos = unpack_str8(data, pos, pOut->timestamp, sizeof(pOut->timestamp));
    return 1;
}

int SC_Net_RecvMessageEx(SC_Message* pOut, unsigned int* pMsgId) {
    unsigned char data[SC_MAX_PACKET];
    int len, idx, pos;
    idx = queue_peek_type(SCCP_MSG_RECV);
    if (idx < 0) return 0;
    queue_pop(idx, data, &len);
    pOut->room_id = data[0];
    pos = 1;
    *pMsgId = ((unsigned int)data[pos] << 24) |
        ((unsigned int)data[pos + 1] << 16) |
        ((unsigned int)data[pos + 2] << 8) |
        (unsigned int)data[pos + 3];
    pos += 4;
    pos = unpack_str8(data, pos, pOut->username, sizeof(pOut->username));
    pos = unpack_str16(data, pos, pOut->content, sizeof(pOut->content));
    pos = unpack_str8(data, pos, pOut->timestamp, sizeof(pOut->timestamp));
    return 1;
}

int SC_Net_RecvMsgDelete(unsigned char* pRoomId, unsigned int* pMsgId) {
    (void)pRoomId; (void)pMsgId; return 0;
}

int SC_Net_RecvUserList(SC_User* users, int* count) {
    unsigned char data[SC_MAX_PACKET];
    int len, idx, i, pos;
    idx = queue_peek_type(SCCP_USER_LIST);
    if (idx < 0) return 0;
    queue_pop(idx, data, &len);
    *count = data[0]; pos = 1;
    if (*count > SC_MAX_USERS) *count = SC_MAX_USERS;
    for (i = 0; i < *count; i++) {
        users[i].user_id = ((unsigned int)data[pos] << 24) |
            ((unsigned int)data[pos + 1] << 16) |
            ((unsigned int)data[pos + 2] << 8) |
            (unsigned int)data[pos + 3]; pos += 4;
        pos = unpack_str8(data, pos, users[i].username, sizeof(users[i].username));
        users[i].room_id = data[pos++];
    }
    return 1;
}

int SC_Net_RecvUserJoin(SC_User* pOut) {
    unsigned char data[SC_MAX_PACKET];
    int len, idx, pos;
    idx = queue_peek_type(SCCP_USER_JOIN);
    if (idx < 0) return 0;
    queue_pop(idx, data, &len);
    pos = 0;
    pOut->user_id = ((unsigned int)data[pos] << 24) |
        ((unsigned int)data[pos + 1] << 16) |
        ((unsigned int)data[pos + 2] << 8) |
        (unsigned int)data[pos + 3]; pos += 4;
    pos = unpack_str8(data, pos, pOut->username, sizeof(pOut->username));
    pOut->room_id = (pos < len) ? data[pos] : 0;
    return 1;
}

int SC_Net_RecvUserLeave(unsigned int* pUserId, char* username, int bufLen) {
    unsigned char data[SC_MAX_PACKET];
    int len, idx, pos;
    idx = queue_peek_type(SCCP_USER_LEAVE);
    if (idx < 0) return 0;
    queue_pop(idx, data, &len);
    pos = 0;
    *pUserId = ((unsigned int)data[pos] << 24) |
        ((unsigned int)data[pos + 1] << 16) |
        ((unsigned int)data[pos + 2] << 8) |
        (unsigned int)data[pos + 3]; pos += 4;
    unpack_str8(data, pos, username, bufLen);
    return 1;
}

int SC_Net_SendJoinRoom(unsigned char room_id, const char* password) {
    unsigned char buf[SC_MAX_PASSWORD + 4];
    int pos = 0;
    int pass_len = 0;
    buf[pos++] = room_id;
    if (password && password[0]) {
        const char* p = password;
        while (*p && pass_len < SC_MAX_PASSWORD) { pass_len++; p++; }
    }
    buf[pos++] = (unsigned char)pass_len;
    if (pass_len > 0) {
        int k;
        for (k = 0; k < pass_len; k++) buf[pos++] = (unsigned char)password[k];
    }
    return net_send_enc(SCCP_JOIN_ROOM, buf, pos);
}

int SC_Net_RecvJoinFail(char* reason, int bufLen) {
    unsigned char data[SC_MAX_PACKET];
    int len, idx;
    idx = queue_peek_type(SCCP_AUTH_FAIL);
    if (idx < 0) return 0;
    queue_pop(idx, data, &len);
    unpack_str8(data, 0, reason, bufLen);
    return 1;
}

int SC_Net_RecvError(char* buf, int bufLen) {
    unsigned char data[SC_MAX_PACKET];
    int len, idx;
    idx = queue_peek_type(SCCP_ERROR);
    if (idx < 0) return 0;
    queue_pop(idx, data, &len);
    unpack_str8(data, 0, buf, bufLen);
    return 1;
}