/*---------------------------------------------------------------------------
    creds.cpp -- Credential persistence for SceneChat.

    D:\creds.dat -- kernel remaps D:\ to app dir on HDD.
    On DVD D:\ is read-only so CreateFile fails silently.

    File format (binary):
        [4]  magic "SC01"
        [1]  username length
        [33] username (null padded)
        [1]  password length
        [65] password XOR-obfuscated (null padded)
---------------------------------------------------------------------------*/

#include <xtl.h>
#include "creds.h"
#include <string.h>

/* ── XOR obfuscation key -- not cryptographic, just avoids plaintext ──────── */
static const unsigned char k_xor[] = {
    0x4B,0x72,0x61,0x74,0x6F,0x73,0x53,0x43,
    0x78,0x62,0x44,0x61,0x72,0x6B,0x6F,0x6E
};
#define XOR_LEN 16

static void xor_buf(unsigned char* buf, int len) {
    int i;
    for (i = 0; i < len; i++)
        buf[i] ^= k_xor[i % XOR_LEN];
}

/* ── File layout ──────────────────────────────────────────────────────────── */
#pragma pack(push,1)
typedef struct {
    char          magic[4];          /* "SC01"          */
    unsigned char user_len;
    char          username[33];
    unsigned char pass_len;
    unsigned char password[65];      /* XOR obfuscated  */
} CredsFile;
#pragma pack(pop)

/* ── Internal state ───────────────────────────────────────────────────────── */
static int  s_have = 0;
static char s_username[33] = { 0 };
static char s_password[65] = { 0 };

/* ── Public API ───────────────────────────────────────────────────────────── */

int Creds_Load(void) {
    HANDLE hf;
    CredsFile cf;
    DWORD read;
    int ulen, plen;

    s_have = 0;
    s_username[0] = 0;
    s_password[0] = 0;

    hf = CreateFileA(CREDS_PATH,
        GENERIC_READ, FILE_SHARE_READ,
        NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) return 0;

    ReadFile(hf, &cf, sizeof(cf), &read, NULL);
    CloseHandle(hf);

    if (read != sizeof(cf))               return 0;
    if (memcmp(cf.magic, "SC01", 4) != 0) return 0;

    ulen = cf.user_len;
    plen = cf.pass_len;
    if (ulen < 1 || ulen > CREDS_MAX_USER) return 0;
    if (plen < 1 || plen > CREDS_MAX_PASS) return 0;

    /* Deobfuscate password */
    xor_buf(cf.password, plen);

    memcpy(s_username, cf.username, ulen); s_username[ulen] = 0;
    memcpy(s_password, cf.password, plen); s_password[plen] = 0;

    s_have = 1;
    return 1;
}

void Creds_Save(const char* username, const char* password) {
    HANDLE hf;
    CredsFile cf;
    DWORD written;
    int ulen, plen;

    if (!username || !password) return;

    ulen = (int)lstrlenA(username);
    plen = (int)lstrlenA(password);
    if (ulen < 1 || ulen > CREDS_MAX_USER) return;
    if (plen < 1 || plen > CREDS_MAX_PASS) return;

    memset(&cf, 0, sizeof(cf));
    memcpy(cf.magic, "SC01", 4);
    cf.user_len = (unsigned char)ulen;
    cf.pass_len = (unsigned char)plen;
    memcpy(cf.username, username, ulen);
    memcpy(cf.password, password, plen);
    xor_buf(cf.password, plen);

    /* Silently fails if D:\ is read-only (DVD) */
    hf = CreateFileA(CREDS_PATH,
        GENERIC_WRITE, 0,
        NULL, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) return;

    WriteFile(hf, &cf, sizeof(cf), &written, NULL);
    CloseHandle(hf);
}

void Creds_Clear(void) {
    DeleteFileA(CREDS_PATH);
    s_have = 0;
    s_username[0] = 0;
    s_password[0] = 0;
}

int         Creds_Have(void) { return s_have; }
const char* Creds_GetUsername(void) { return s_username; }
const char* Creds_GetPassword(void) { return s_password; }