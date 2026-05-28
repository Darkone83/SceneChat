/*---------------------------------------------------------------------------
    sc_dh.cpp -- Diffie-Hellman key exchange for SceneChat Xbox client.

    Implements SC_DH_ComputeSession():
      1. PEM decode + DER parse server DHParameter  -> p, g
      2. PEM decode + DER parse server SubjectPublicKeyInfo -> server_y
      3. Generate random 2048-bit private key x via XNetRandom
      4. Compute client_y = g^x mod p  (slow -- acceptable on connect screen)
      5. Build + PEM encode client SubjectPublicKeyInfo
      6. Compute shared = server_y^x mod p
      7. HKDF-SHA256(shared, info="scenechat-session") -> 32-byte session key

    All bignums are 128-byte big-endian arrays (1024 bits).
    Slow but correct schoolbook arithmetic -- fine for a one-time handshake.
---------------------------------------------------------------------------*/

#include <xtl.h>
#include <winsockx.h>
#include "sc_net.h"
#include "sc_log.h"
#include <string.h>

/* ===========================================================================
   S1  SHA-256 / HMAC-SHA256 / HKDF  (minimal, for session key only)
   =========================================================================== */

typedef struct { DWORD h[8]; unsigned char buf[64]; DWORD lo, hi; } dh_sha256_ctx;

static const DWORD dh_k256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

#define DH_ROR32(x,n) (((x)>>(n))|((x)<<(32-(n))))
#define DH_CH(x,y,z)  (((x)&(y))^(~(x)&(z)))
#define DH_MAJ(x,y,z) (((x)&(y))^((x)&(z))^((y)&(z)))
#define DH_S0(x) (DH_ROR32(x,2)^DH_ROR32(x,13)^DH_ROR32(x,22))
#define DH_S1(x) (DH_ROR32(x,6)^DH_ROR32(x,11)^DH_ROR32(x,25))
#define DH_G0(x) (DH_ROR32(x,7)^DH_ROR32(x,18)^((x)>>3))
#define DH_G1(x) (DH_ROR32(x,17)^DH_ROR32(x,19)^((x)>>10))

static void dh_sha256_init(dh_sha256_ctx* c) {
    c->h[0] = 0x6a09e667; c->h[1] = 0xbb67ae85; c->h[2] = 0x3c6ef372; c->h[3] = 0xa54ff53a;
    c->h[4] = 0x510e527f; c->h[5] = 0x9b05688c; c->h[6] = 0x1f83d9ab; c->h[7] = 0x5be0cd19;
    c->lo = c->hi = 0;
}

static void dh_sha256_compress(dh_sha256_ctx* c, const unsigned char* blk) {
    DWORD w[64], a, b, cc, d, e, f, g, h, t1, t2; int i;
    for (i = 0; i < 16; i++)
        w[i] = ((DWORD)blk[i * 4] << 24) | ((DWORD)blk[i * 4 + 1] << 16) | ((DWORD)blk[i * 4 + 2] << 8) | (DWORD)blk[i * 4 + 3];
    for (i = 16; i < 64; i++) w[i] = DH_G1(w[i - 2]) + w[i - 7] + DH_G0(w[i - 15]) + w[i - 16];
    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3]; e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];
    for (i = 0; i < 64; i++) {
        t1 = h + DH_S1(e) + DH_CH(e, f, g) + dh_k256[i] + w[i];
        t2 = DH_S0(a) + DH_MAJ(a, b, cc);
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
    c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

static void dh_sha256_update(dh_sha256_ctx* c, const unsigned char* data, int len) {
    DWORD save = c->lo; int fill, left;
    c->lo += (DWORD)len * 8; if (c->lo < save) c->hi++;
    c->hi += (DWORD)len >> 29;
    fill = (int)((save >> 3) & 63); left = 64 - fill;
    if (fill && len >= left) { memcpy(c->buf + fill, data, left); dh_sha256_compress(c, c->buf); data += left; len -= left; fill = 0; }
    while (len >= 64) { dh_sha256_compress(c, data); data += 64; len -= 64; }
    if (len > 0) memcpy(c->buf + fill, data, len);
}

static void dh_sha256_final(dh_sha256_ctx* c, unsigned char* out) {
    static const unsigned char pad[64] = { 0x80 };
    unsigned char ml[8]; DWORD lo = c->lo, hi = c->hi; int i, pl;
    ml[0] = (unsigned char)(hi >> 24); ml[1] = (unsigned char)(hi >> 16); ml[2] = (unsigned char)(hi >> 8); ml[3] = (unsigned char)(hi);
    ml[4] = (unsigned char)(lo >> 24); ml[5] = (unsigned char)(lo >> 16); ml[6] = (unsigned char)(lo >> 8); ml[7] = (unsigned char)(lo);
    pl = (int)(((lo >> 3) & 63) < 56 ? 56 - ((lo >> 3) & 63) : 120 - ((lo >> 3) & 63));
    dh_sha256_update(c, pad, pl); dh_sha256_update(c, ml, 8);
    for (i = 0; i < 8; i++) {
        out[i * 4] = (unsigned char)(c->h[i] >> 24); out[i * 4 + 1] = (unsigned char)(c->h[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(c->h[i] >> 8); out[i * 4 + 3] = (unsigned char)(c->h[i]);
    }
}

static void dh_sha256(const unsigned char* data, int len, unsigned char* out) {
    dh_sha256_ctx c; dh_sha256_init(&c); dh_sha256_update(&c, data, len); dh_sha256_final(&c, out);
}

static void dh_hmac_sha256(const unsigned char* key, int klen,
    const unsigned char* msg, int mlen, unsigned char* out) {
    dh_sha256_ctx c; unsigned char kpad[64], tmp[32]; int i;
    if (klen > 64) { dh_sha256(key, klen, kpad); klen = 32; }
    else memcpy(kpad, key, klen);
    for (i = klen; i < 64; i++) kpad[i] = 0;
    dh_sha256_init(&c);
    for (i = 0; i < 64; i++) { unsigned char b = (unsigned char)(kpad[i] ^ 0x36); dh_sha256_update(&c, &b, 1); }
    dh_sha256_update(&c, msg, mlen); dh_sha256_final(&c, tmp);
    dh_sha256_init(&c);
    for (i = 0; i < 64; i++) { unsigned char b = (unsigned char)(kpad[i] ^ 0x5c); dh_sha256_update(&c, &b, 1); }
    dh_sha256_update(&c, tmp, 32); dh_sha256_final(&c, out);
}

static void dh_hkdf(const unsigned char* ikm, int ikm_len,
    const unsigned char* info, int info_len,
    unsigned char* out) {
    static const unsigned char zero32[32] = { 0 };
    unsigned char prk[32], buf[256]; int blen = 0;
    dh_hmac_sha256(zero32, 32, ikm, ikm_len, prk);
    memcpy(buf, info, info_len); blen += info_len; buf[blen++] = 0x01;
    dh_hmac_sha256(prk, 32, buf, blen, out);
}

/* ===========================================================================
   S2  Base64
   =========================================================================== */

static const char b64c[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64val(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62; if (c == '/') return 63; return -1;
}

static int b64_decode(const unsigned char* in, int inlen, unsigned char* out, int outmax) {
    int i, v, outlen = 0, acc = 0, bits = 0;
    for (i = 0; i < inlen; i++) {
        v = b64val(in[i]); if (v < 0) continue;
        acc = (acc << 6) | v; bits += 6;
        if (bits >= 8) { bits -= 8; if (outlen < outmax) out[outlen++] = (unsigned char)(acc >> bits); acc &= (1 << bits) - 1; }
    }
    return outlen;
}

static int b64_encode(const unsigned char* in, int inlen, char* out, int outmax) {
    int i, outlen = 0, col = 0;
    for (i = 0; i < inlen; i += 3) {
        unsigned int b0 = in[i], b1 = (i + 1 < inlen) ? in[i + 1] : 0, b2 = (i + 2 < inlen) ? in[i + 2] : 0;
        unsigned int v = (b0 << 16) | (b1 << 8) | b2;
        int rem = inlen - i;
        if (outlen + 5 >= outmax) break;
        out[outlen++] = b64c[(v >> 18) & 63];
        out[outlen++] = b64c[(v >> 12) & 63];
        out[outlen++] = (rem > 1) ? b64c[(v >> 6) & 63] : '=';
        out[outlen++] = (rem > 2) ? b64c[v & 63] : '=';
        col += 4; if (col >= 64) { if (outlen < outmax) out[outlen++] = '\n'; col = 0; }
    }
    if (col > 0 && outlen < outmax) out[outlen++] = '\n';
    if (outlen < outmax) out[outlen] = 0;
    return outlen;
}

/* Strip PEM header/footer, base64 decode body */
static int pem_to_der(const unsigned char* pem, int pemlen,
    unsigned char* der, int dermax) {
    const unsigned char* p = pem, * end = pem + pemlen;
    const unsigned char* body;
    while (p < end && *p != '\n') p++;
    if (p < end) p++;
    body = p;
    while (p < end && *p != '-') p++;
    return b64_decode(body, (int)(p - body), der, dermax);
}

/* Write PEM with header/footer */
static int der_to_pem(const unsigned char* der, int derlen,
    char* pem, int pemmax, const char* hdr) {
    int pos = 0, b64len;
    int hdrlen = (int)lstrlenA(hdr);
    if (pos + 11 + hdrlen + 7 > pemmax) return 0;
    memcpy(pem + pos, "-----BEGIN ", 11); pos += 11;
    memcpy(pem + pos, hdr, hdrlen); pos += hdrlen;
    memcpy(pem + pos, "-----\n", 6); pos += 6;
    b64len = b64_encode(der, derlen, pem + pos, pemmax - pos);
    pos += b64len;
    if (pos + 9 + hdrlen + 7 > pemmax) return 0;
    memcpy(pem + pos, "-----END ", 9); pos += 9;
    memcpy(pem + pos, hdr, hdrlen); pos += hdrlen;
    memcpy(pem + pos, "-----\n", 6); pos += 6;
    pem[pos] = 0;
    return pos;
}

/* ===========================================================================
   S3  DER read helpers
   =========================================================================== */

   /* Read TLV at *pos. Returns tag, sets content pointer and length.
      Advances *pos past entire TLV. Returns 0 on error. */
static int der_read_tlv(const unsigned char** pos, const unsigned char* end,
    unsigned char* tag_out,
    const unsigned char** content, int* clen) {
    const unsigned char* p = *pos;
    int len;
    if (p >= end) return 0;
    if (tag_out) *tag_out = *p; p++;
    if (p >= end) return 0;
    if (*p < 0x80) { len = *p++; }
    else if (*p == 0x81) { if (p + 2 > end) return 0; len = p[1]; p += 2; }
    else if (*p == 0x82) { if (p + 3 > end) return 0; len = (p[1] << 8) | p[2]; p += 3; }
    else return 0;
    if (p + len > end) return 0;
    if (content) *content = p;
    if (clen) *clen = len;
    *pos = p + len;
    return 1;
}

/* Skip one TLV at *pos */
static int der_skip(const unsigned char** pos, const unsigned char* end) {
    return der_read_tlv(pos, end, NULL, NULL, NULL);
}

/* Read a DER INTEGER, strip leading 0x00, return value bytes */
static int der_read_integer(const unsigned char** pos, const unsigned char* end,
    const unsigned char** val, int* val_len) {
    unsigned char tag;
    if (!der_read_tlv(pos, end, &tag, val, val_len) || tag != 0x02) return 0;
    while (*val_len > 1 && **val == 0x00) { (*val)++; (*val_len)--; }
    return 1;
}

/* ===========================================================================
   S4  DER write helpers
   =========================================================================== */

static int der_wlen(unsigned char* buf, int len) {
    if (len < 128) { buf[0] = (unsigned char)len; return 1; }
    if (len < 256) { buf[0] = 0x81; buf[1] = (unsigned char)len; return 2; }
    buf[0] = 0x82; buf[1] = (unsigned char)(len >> 8); buf[2] = (unsigned char)len; return 3;
}

static int der_write_int(unsigned char* buf, const unsigned char* val, int vlen) {
    int pos = 0, pad;
    while (vlen > 1 && val[0] == 0x00) { val++; vlen--; }
    pad = (val[0] & 0x80) ? 1 : 0;
    buf[pos++] = 0x02;
    pos += der_wlen(buf + pos, vlen + pad);
    if (pad) buf[pos++] = 0x00;
    memcpy(buf + pos, val, vlen); pos += vlen;
    return pos;
}

static int der_write_seq(unsigned char* buf, const unsigned char* content, int clen) {
    int pos = 0;
    buf[pos++] = 0x30;
    pos += der_wlen(buf + pos, clen);
    memcpy(buf + pos, content, clen); pos += clen;
    return pos;
}

static int der_write_bitstr(unsigned char* buf, const unsigned char* content, int clen) {
    int pos = 0;
    buf[pos++] = 0x03;
    pos += der_wlen(buf + pos, clen + 1);
    buf[pos++] = 0x00;
    memcpy(buf + pos, content, clen); pos += clen;
    return pos;
}

/* ===========================================================================
   S5  Bignum  (256-byte big-endian)
   =========================================================================== */

#define BN_BYTES 128  /* 1024-bit DH -- ~15s on Xbox vs ~2min for 2048-bit */

typedef unsigned char BN[BN_BYTES];
typedef unsigned char BN2[BN_BYTES * 2];

static void bn_zero(unsigned char* a)
{
    memset(a, 0, BN_BYTES);
}

static void bn_from_bytes(unsigned char* a, const unsigned char* val, int vlen) {
    memset(a, 0, BN_BYTES);
    if (vlen > BN_BYTES) vlen = BN_BYTES;
    memcpy(a + (BN_BYTES - vlen), val, vlen);
}

/* Multiply two 256-byte bignums -> 512-byte result (schoolbook, big-endian) */
static void bn_mul(unsigned char* r512, const unsigned char* a, const unsigned char* b) {
    int i, j;
    memset(r512, 0, BN_BYTES * 2);
    for (i = BN_BYTES - 1; i >= 0; i--) {
        unsigned int carry = 0;
        for (j = BN_BYTES - 1; j >= 0; j--) {
            unsigned int tmp = (unsigned int)a[i] * b[j] + r512[i + j + 1] + carry;
            r512[i + j + 1] = (unsigned char)(tmp & 0xff);
            carry = tmp >> 8;
        }
        r512[i] += (unsigned char)carry;
    }
}

/* Reduce 512-byte a mod 256-byte m, result in r (256 bytes)
   Uses binary shift-subtract: 2048 iterations, correct and simple. */
static void bn_reduce(unsigned char* r, unsigned char* a512, const unsigned char* m) {
    unsigned char mshift[BN_BYTES * 2];
    int i, j, borrow;

    /* mshift = m in the top half of 512-byte buffer */
    memcpy(mshift, m, BN_BYTES);
    memset(mshift + BN_BYTES, 0, BN_BYTES);

    for (i = 0; i < BN_BYTES * 8; i++) {
        /* if a >= mshift: a -= mshift */
        if (memcmp(a512, mshift, BN_BYTES * 2) >= 0) {
            borrow = 0;
            for (j = BN_BYTES * 2 - 1; j >= 0; j--) {
                int diff = (int)a512[j] - mshift[j] - borrow;
                a512[j] = (unsigned char)(diff & 0xff);
                borrow = (diff < 0) ? 1 : 0;
            }
        }
        /* mshift >>= 1 (big-endian) */
        {
            unsigned char carry = 0, nc;
            for (j = 0; j < BN_BYTES * 2; j++) {
                nc = mshift[j] & 1;
                mshift[j] = (mshift[j] >> 1) | (carry << 7);
                carry = nc;
            }
        }
    }
    /* Final conditional subtract -- the loop checks BEFORE shifting,
       so after BN_BYTES*8 iterations mshift==m but the check at that
       value never ran. Without this, result is in [0,2m) not [0,m). */
    {
        int borrow = 0, cmp, j;
        cmp = memcmp(a512, mshift, BN_BYTES * 2);
        if (cmp >= 0) {
            for (j = BN_BYTES * 2 - 1; j >= 0; j--) {
                int diff = (int)a512[j] - mshift[j] - borrow;
                a512[j] = (unsigned char)(diff & 0xff);
                borrow = (diff < 0) ? 1 : 0;
            }
        }
    }
    memcpy(r, a512 + BN_BYTES, BN_BYTES);
}

/* r = (a * b) mod m */
static void bn_mulmod(unsigned char* r,
    const unsigned char* a,
    const unsigned char* b,
    const unsigned char* m) {
    BN2 product;
    bn_mul(product, a, b);
    bn_reduce(r, product, m);
}

/* r = base^exp mod m  (left-to-right binary, all 256-byte big-endian) */
static void bn_modexp(unsigned char* r,
    const unsigned char* base,
    const unsigned char* exp,
    const unsigned char* m) {
    BN result, b;
    int i, bit;
    bn_zero(result); result[BN_BYTES - 1] = 1; /* result = 1 */
    memcpy(b, base, BN_BYTES);
    for (i = 0; i < BN_BYTES; i++) {
        for (bit = 7; bit >= 0; bit--) {
            bn_mulmod(result, result, result, m);
            if ((exp[i] >> bit) & 1) bn_mulmod(result, result, b, m);
        }
    }
    memcpy(r, result, BN_BYTES);
}

/* ===========================================================================
   S6  Parse DHParameter PEM  ->  p, g
   =========================================================================== */

static int parse_dh_params(const unsigned char* pem, int pemlen,
    unsigned char* p_out, int* p_len,
    unsigned char* g_out, int* g_len) {
    unsigned char der[1024];
    int derlen;
    const unsigned char* p, * end, * seq_content, * val;
    unsigned char tag;
    int seq_len, val_len;

    derlen = pem_to_der(pem, pemlen, der, sizeof(der));
    if (derlen <= 0) return 0;

    p = der; end = der + derlen;

    /* SEQUENCE { INTEGER p, INTEGER g } */
    if (!der_read_tlv(&p, end, &tag, &seq_content, &seq_len) || tag != 0x30) return 0;
    p = seq_content; end = seq_content + seq_len;

    /* INTEGER p */
    if (!der_read_integer(&p, end, &val, &val_len)) return 0;
    if (val_len > BN_BYTES) val_len = BN_BYTES;
    memcpy(p_out, val, val_len); *p_len = val_len;

    /* INTEGER g */
    if (!der_read_integer(&p, end, &val, &val_len)) return 0;
    if (val_len > BN_BYTES) val_len = BN_BYTES;
    memcpy(g_out, val, val_len); *g_len = val_len;

    return 1;
}

/* ===========================================================================
   S7  Parse SubjectPublicKeyInfo PEM  ->  server public key y
       Structure:
         SEQUENCE {
           SEQUENCE { OID dhpublicnumber, SEQUENCE { INTEGER p, INTEGER g } }
           BIT STRING { 0x00, INTEGER y }
         }
   =========================================================================== */

static int parse_server_pub(const unsigned char* pem, int pemlen,
    unsigned char* y_out, int* y_len) {
    unsigned char der[2048];
    int derlen;
    const unsigned char* p, * end, * outer, * val;
    const unsigned char* bs_content;
    unsigned char tag;
    int outer_len, bs_len, val_len;

    derlen = pem_to_der(pem, pemlen, der, sizeof(der));
    if (derlen <= 0) return 0;
    p = der; end = der + derlen;

    /* Enter outer SEQUENCE */
    if (!der_read_tlv(&p, end, &tag, &outer, &outer_len) || tag != 0x30) return 0;
    p = outer; end = outer + outer_len;

    /* Skip AlgorithmIdentifier SEQUENCE */
    if (!der_skip(&p, end)) return 0;

    /* Read BIT STRING */
    if (!der_read_tlv(&p, end, &tag, &bs_content, &bs_len) || tag != 0x03) return 0;
    if (bs_len < 3) return 0;
    /* Skip unused-bits byte (0x00) */
    bs_content++; bs_len--;

    /* Read INTEGER y from inside BIT STRING */
    p = bs_content; end = bs_content + bs_len;
    if (!der_read_integer(&p, end, &val, &val_len)) return 0;
    if (val_len > BN_BYTES) val_len = BN_BYTES;
    memcpy(y_out, val, val_len); *y_len = val_len;
    return 1;
}

/* ===========================================================================
   S8  Build SubjectPublicKeyInfo DER for our client public key
       OID dhpublicnumber: 1.2.840.10046.2.1
   =========================================================================== */

static int build_spki(unsigned char* out, int outmax,
    const unsigned char* p_bytes, int p_len,
    const unsigned char* g_bytes, int g_len,
    const unsigned char* y_bytes, int y_len) {
    static const unsigned char oid_dh[11] = { 0x06,0x09,0x2a,0x86,0x48,0xce,0x3e,0x02,0x01 };
    unsigned char dh_param[600], algo_id[700];
    unsigned char pub_int[300], bit_str[350];
    unsigned char tmp[1300];
    int dh_len, algo_len, pub_len, bit_len, tpos, total;

    /* DHParameter: SEQUENCE { INTEGER p, INTEGER g } */
    {
        unsigned char t[600]; int tp = 0;
        tp += der_write_int(t + tp, p_bytes, p_len);
        tp += der_write_int(t + tp, g_bytes, g_len);
        dh_len = der_write_seq(dh_param, t, tp);
    }

    /* AlgorithmIdentifier: SEQUENCE { OID, DHParameter } */
    {
        unsigned char t[700]; int tp = 0;
        memcpy(t + tp, oid_dh, sizeof(oid_dh)); tp += sizeof(oid_dh);
        memcpy(t + tp, dh_param, dh_len); tp += dh_len;
        algo_len = der_write_seq(algo_id, t, tp);
    }

    /* Public key INTEGER */
    pub_len = der_write_int(pub_int, y_bytes, y_len);

    /* BIT STRING wrapping INTEGER */
    bit_len = der_write_bitstr(bit_str, pub_int, pub_len);

    /* Outer SEQUENCE */
    tpos = 0;
    memcpy(tmp + tpos, algo_id, algo_len); tpos += algo_len;
    memcpy(tmp + tpos, bit_str, bit_len); tpos += bit_len;
    total = der_write_seq(out, tmp, tpos);

    return (total <= outmax) ? total : 0;
}

/* ===========================================================================
   S9  SC_DH_ComputeSession  --  the public entry point
   =========================================================================== */

extern "C" int SC_DH_ComputeSession(
    const unsigned char* params_pem, int params_len,
    const unsigned char* server_pub_pem, int server_pub_len,
    unsigned char* client_pub_raw_out, int* client_pub_raw_len_out,
    unsigned char* session_key_out) {

    /* Raw value bytes from PEM (before bignum padding) */
    unsigned char p_raw[BN_BYTES], g_raw[BN_BYTES];
    unsigned char sy_raw[BN_BYTES];
    int p_rawlen, g_rawlen, sy_rawlen;

    /* Padded 256-byte bignums */
    BN p_bn, g_bn, sy_bn;
    BN x_bn;          /* private key */
    BN cy_bn;         /* client public key */
    BN shared_bn;     /* shared secret */

    unsigned char spki_der[2048];
    int spki_len;

    static const unsigned char info[] = "scenechat-session";

    /* Step 1: Parse DHParameter -> p, g */
    if (!parse_dh_params(params_pem, params_len, p_raw, &p_rawlen, g_raw, &g_rawlen))
        return 0;

    /* Step 2: Parse server SubjectPublicKeyInfo -> server_y */
    if (!parse_server_pub(server_pub_pem, server_pub_len, sy_raw, &sy_rawlen))
        return 0;

    /* Pad to BN_BYTES big-endian */
    bn_from_bytes(p_bn, p_raw, p_rawlen);
    bn_from_bytes(g_bn, g_raw, g_rawlen);
    bn_from_bytes(sy_bn, sy_raw, sy_rawlen);
    SC_Log_Int("DH", "p_rawlen", p_rawlen);
    SC_Log_Int("DH", "g_rawlen", g_rawlen);
    SC_Log_Int("DH", "sy_rawlen", sy_rawlen);
    SC_Log_Hex("DH-P", p_raw, 8);   /* first 8 bytes of modulus */
    SC_Log_Hex("DH-G", g_raw, g_rawlen < 8 ? g_rawlen : 8);
    SC_Log_Hex("DH-SY", sy_raw, 8);   /* first 8 bytes of server pub key */

    /* Step 3: Generate random 2048-bit private key x */
    XNetRandom(x_bn, BN_BYTES);
    /* Mask MSB to keep x < p (conservative -- p has top bit set so any 2048-bit x works) */
    x_bn[0] &= 0x7f;
    /* Ensure x > 1 */
    x_bn[BN_BYTES - 1] |= 0x02;

    /* Step 4: Compute client_y = g^x mod p  (slow -- ~3-5 seconds on Xbox) */
    bn_modexp(cy_bn, g_bn, x_bn, p_bn);

    /* Step 5: Output raw client public key bytes (big-endian, p_rawlen bytes)
       Server reconstructs DHPublicNumbers directly -- no PEM needed */
    memcpy(client_pub_raw_out, cy_bn + (BN_BYTES - p_rawlen), p_rawlen);
    *client_pub_raw_len_out = p_rawlen;
    SC_Log_Int("DH", "client_pub_raw_len", p_rawlen);

    SC_Log("DH", "DH_RESPONSE sent, computing shared secret");
    /* Step 6: Compute shared secret = server_y^x mod p */
    bn_modexp(shared_bn, sy_bn, x_bn, p_bn);
    SC_Log_Hex("DH-SHARED", shared_bn, 8);

    SC_Log("DH", "shared secret computed, deriving session key");
    /* Step 7: HKDF-SHA256(shared, info="scenechat-session") -> 32-byte session key
       Strip leading zeros to match Python cryptography exchange() output -- without
       this the HKDF input length differs and both sides derive different keys. */
    {
        const unsigned char* sp = shared_bn;
        int sl = BN_BYTES;
        while (sl > 1 && *sp == 0) { sp++; sl--; }
        dh_hkdf(sp, sl, info, (int)lstrlenA((const char*)info), session_key_out);
    }

    SC_Log("DH", "session key derived OK");
    SC_Log_Hex("DH-KEY", session_key_out, 8);
    /* Zero sensitive values */
    memset(x_bn, 0, BN_BYTES);
    memset(shared_bn, 0, BN_BYTES);

    return 1;
}