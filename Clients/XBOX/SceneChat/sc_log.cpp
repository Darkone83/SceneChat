/*---------------------------------------------------------------------------
    sc_log.cpp -- SceneChat Xbox disk logger (C89/MSVC2003)
    Matches XbDiag SysInfo.cpp log pattern exactly:
      CreateFileA + WriteFile, D:\scenechat.log
---------------------------------------------------------------------------*/

#include <xtl.h>
#include "sc_log.h"

#define LOG_PATH  "D:\\scenechat.log"

static HANDLE s_hf = INVALID_HANDLE_VALUE;
static DWORD  s_t0 = 0;

/* -------------------------------------------------------------------------
   Internal helpers -- no sprintf, no CRT, matches XbDiag WriteLine exactly
   ------------------------------------------------------------------------- */

static void WriteRaw(const char* buf, DWORD len) {
    DWORD w;
    if (s_hf == INVALID_HANDLE_VALUE) return;
    WriteFile(s_hf, buf, len, &w, NULL);
}

static void WriteStr(const char* s) {
    DWORD len = 0;
    while (s[len]) len++;
    WriteRaw(s, len);
}

static void WriteHex32(DWORD v) {
    static const char h[] = "0123456789ABCDEF";
    char buf[8];
    int i;
    for (i = 7; i >= 0; i--) { buf[i] = h[v & 0xF]; v >>= 4; }
    WriteRaw(buf, 8);
}

static void WriteHex8(unsigned char b) {
    static const char h[] = "0123456789ABCDEF";
    char buf[2];
    buf[0] = h[b >> 4];
    buf[1] = h[b & 0xF];
    WriteRaw(buf, 2);
}

static void WriteInt(int v) {
    char tmp[12]; int i = 0, j; int neg = 0;
    if (v < 0) { neg = 1; v = -v; }
    if (v == 0) { WriteRaw("0", 1); return; }
    while (v > 0) { tmp[i++] = (char)('0' + v % 10); v /= 10; }
    if (neg) tmp[i++] = '-';
    for (j = 0; j < i / 2; j++) {
        char t = tmp[j]; tmp[j] = tmp[i - 1 - j]; tmp[i - 1 - j] = t;
    }
    WriteRaw(tmp, (DWORD)i);
}

/* Pad tag to 12 chars */
static void WriteTag(const char* tag) {
    int i = 0;
    while (tag[i] && i < 12) { WriteRaw(tag + i, 1); i++; }
    while (i < 12) { WriteRaw(" ", 1); i++; }
}

static void WritePrefix(const char* tag) {
    DWORD t = GetTickCount() - s_t0;
    WriteStr("[T+");
    WriteHex32(t);
    WriteStr("] [");
    WriteTag(tag);
    WriteStr("] ");
}

/* -------------------------------------------------------------------------
   Public API
   ------------------------------------------------------------------------- */

void SC_Log_Init(void) {
    s_hf = CreateFileA(
        LOG_PATH,
        GENERIC_WRITE,
        FILE_SHARE_READ,
        NULL,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );
    if (s_hf == INVALID_HANDLE_VALUE) return;

    /* Append -- seek to end */
    SetFilePointer(s_hf, 0, NULL, FILE_END);

    s_t0 = GetTickCount();
    WriteStr("\r\n=====================================================\r\n");
    WriteStr("  SceneChat log  T0=");
    WriteHex32(s_t0);
    WriteStr("\r\n=====================================================\r\n");
}

void SC_Log(const char* tag, const char* msg) {
    WritePrefix(tag);
    WriteStr(msg);
    WriteStr("\r\n");
}

void SC_Log_Int(const char* tag, const char* label, int val) {
    WritePrefix(tag);
    WriteStr(label);
    WriteStr("=");
    WriteInt(val);
    WriteStr("\r\n");
}

void SC_Log_Hex(const char* tag, const unsigned char* buf, int len) {
    int i;
    if (len > 64) len = 64;
    WritePrefix(tag);
    for (i = 0; i < len; i++) {
        WriteHex8(buf[i]);
        WriteRaw(" ", 1);
        if ((i & 15) == 15 && i < len - 1) WriteStr("| ");
    }
    WriteStr("\r\n");
}

void SC_Log_Shutdown(void) {
    if (s_hf == INVALID_HANDLE_VALUE) return;
    WriteStr("[SC_Log] shutdown\r\n");
    CloseHandle(s_hf);
    s_hf = INVALID_HANDLE_VALUE;
}