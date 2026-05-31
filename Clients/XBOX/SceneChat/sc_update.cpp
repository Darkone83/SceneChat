// sc_update.cpp
// SceneChat - OTA update downloader and extractor.
// Mirrors XbDiag Update.cpp exactly.
//
// RXDK constraints:
//   No sprintf/sscanf/strlen
//   No inline asm
//   File-scope statics only

#include "sc_update.h"
#include "xba.h"
#include <xtl.h>
#include <winsockx.h>

// ============================================================================
// Version constant -- bump each release
// ============================================================================
static const char* k_local_version = "1.3";

// ============================================================================
// Config
// ============================================================================
static const int   k_http_port = 8950;
static const char* k_xba_path = "/update/scenechat.xba";
static const char* k_ver_path = "/update/scenechat.ver";
static const char* k_xba_tmp = "D:\\sc_update.xba";

// ============================================================================
// Internal string helpers (RXDK-safe, no sprintf/strlen)
// ============================================================================
static int StrLen(const char* s)
{
    int n = 0; while (s && s[n]) n++; return n;
}
static void StrCopy(char* dst, int dstLen, const char* src)
{
    int i = 0;
    while (i < dstLen - 1 && src && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}
static void AppendStr(char* dst, int dstLen, const char* src)
{
    int dlen = StrLen(dst), slen = StrLen(src), space = dstLen - dlen - 1;
    for (int i = 0; i < slen && i < space; i++) dst[dlen + i] = src[i];
    dst[dlen + (slen < space ? slen : space)] = '\0';
}
static void IntToStr(int v, char* buf, int bufLen)
{
    if (bufLen < 2) return;
    if (v == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    char tmp[16]; int n = 0; int neg = (v < 0);
    if (neg) v = -v;
    while (v > 0 && n < 15) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    int di = 0;
    if (neg && di < bufLen - 1) buf[di++] = '-';
    for (int i = n - 1; i >= 0 && di < bufLen - 1; i--) buf[di++] = tmp[i];
    buf[di] = '\0';
}
static void StripWhitespace(char* buf)
{
    int len = StrLen(buf);
    while (len > 0 && (buf[len - 1] == '\r' || buf[len - 1] == '\n' || buf[len - 1] == ' ' || buf[len - 1] == '\t'))
        buf[--len] = '\0';
    int start = 0;
    while (buf[start] == ' ' || buf[start] == '\t') start++;
    if (start > 0) {
        int i = 0;
        while (buf[start + i]) { buf[i] = buf[start + i]; i++; }
        buf[i] = '\0';
    }
}

// ============================================================================
// HTTP helpers
// ============================================================================
static int FindHeaderEnd(const char* buf, int len)
{
    for (int i = 0; i < len - 3; i++)
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') return i + 4;
    return -1;
}
static int GetHttpStatus(const char* buf, int len)
{
    if (len < 12 || buf[0] != 'H' || buf[1] != 'T' || buf[2] != 'T' || buf[3] != 'P') return 0;
    int i = 0;
    while (i < len && buf[i] != ' ') i++;
    while (i < len && buf[i] == ' ') i++;
    int code = 0;
    for (int j = 0; j < 3 && buf[i + j] >= '0' && buf[i + j] <= '9'; j++) code = code * 10 + (int)(buf[i + j] - '0');
    return code;
}
static DWORD ParseContentLength(const char* buf, int bodyStart)
{
    const char* needle = "content-length:";
    for (int i = 0; i < bodyStart - 15; i++) {
        bool m = true;
        for (int k = 0; needle[k]; k++) {
            char hc = buf[i + k];
            if (hc >= 'A' && hc <= 'Z') hc = (char)(hc + 32);
            if (hc != needle[k]) { m = false; break; }
        }
        if (m) {
            const char* p = buf + i + 15;
            while (*p == ' ') p++;
            DWORD val = 0;
            while (*p >= '0' && *p <= '9') val = val * 10 + (DWORD)(*p++ - '0');
            return val;
        }
    }
    return 0;
}

// ============================================================================
// Version comparison
// ============================================================================
static void ParseVerParts(const char* s, int v[3])
{
    v[0] = v[1] = v[2] = 0; int field = 0;
    for (int i = 0; s[i] && field < 3; i++) {
        char c = s[i];
        if (c >= '0' && c <= '9') v[field] = v[field] * 10 + (int)(c - '0');
        else if (c == '.')    field++;
        else                break;
    }
}
static int VerCmp(const char* a, const char* b)
{
    int pa[3], pb[3];
    ParseVerParts(a, pa); ParseVerParts(b, pb);
    for (int i = 0; i < 3; i++) { if (pa[i] < pb[i]) return -1; if (pa[i] > pb[i]) return 1; }
    return 0;
}

// ============================================================================
// State
// ============================================================================
typedef enum SC_UpdState {
    SCUPD_IDLE = 0,
    SCUPD_NET_WAIT,
    SCUPD_DNS,
    SCUPD_CHECK_CONNECT,
    SCUPD_CHECK_RECV,
    SCUPD_CHECK_COMPARE,
    SCUPD_CHECK_DONE,
    SCUPD_DOWNLOADING,
    SCUPD_EXTRACTING,
    SCUPD_DONE,
    SCUPD_ERROR
} SC_UpdState;

static SC_UpdState  s_state = SCUPD_IDLE;
static int          s_available = 0;
static int          s_check_started = 0;
static char         s_remote_ver[32];
static char         s_local_ver[32];
static char         s_server_ip[64];
static char         s_install_dir[128];
static char         s_ver_dest[128];
static char         s_status[96];
static char         s_error[80];
static char         s_detail[128];
static SOCKET       s_sock = INVALID_SOCKET;
static char         s_recv_buf[4096];
static int          s_recv_len = 0;
static DWORD        s_total = 0;
static DWORD        s_received = 0;
static DWORD        s_net_wait_start = 0;
static XNDNS* s_dns = NULL;
static IN_ADDR      s_server_addr;

// ============================================================================
// XBA progress callback
// ============================================================================
static void XbaProgressCb(int filesDone, int filesTotal, DWORD bytesDone, DWORD bytesTotal)
{
    s_received = bytesDone; s_total = bytesTotal;
    (void)filesDone; (void)filesTotal;
}

// ============================================================================
// Path setup
// ============================================================================
static void DeriveInstallPaths(const char* install_dir)
{
    if (install_dir && install_dir[0]) {
        StrCopy(s_install_dir, sizeof(s_install_dir), install_dir);
        int len = StrLen(s_install_dir);
        if (len > 0 && s_install_dir[len - 1] != '\\') {
            s_install_dir[len] = '\\'; s_install_dir[len + 1] = '\0';
        }
    }
    else {
        StrCopy(s_install_dir, sizeof(s_install_dir), "D:\\");
    }
    StrCopy(s_ver_dest, sizeof(s_ver_dest), s_install_dir);
    AppendStr(s_ver_dest, sizeof(s_ver_dest), "scenechat.ver");
}

static void ReadLocalVer(void)
{
    ZeroMemory(s_local_ver, sizeof(s_local_ver));
    HANDLE hv = CreateFile(s_ver_dest, GENERIC_READ, FILE_SHARE_READ,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hv == INVALID_HANDLE_VALUE) return; /* no file = empty = always update */
    char buf[64]; DWORD rd = 0;
    if (ReadFile(hv, buf, sizeof(buf) - 1, &rd, NULL) && rd > 0) {
        buf[rd] = '\0'; StripWhitespace(buf);
        if (buf[0]) StrCopy(s_local_ver, sizeof(s_local_ver), buf);
    }
    CloseHandle(hv);
}

// ============================================================================
// Blocking DoDownload -- mirrors XbDiag exactly
// Uses blocking socket with SO_RCVTIMEO/SO_SNDTIMEO
// ============================================================================
static int DoDownload(const char* path, const char* dest)
{
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) return 0;

    /* Non-blocking connect with 5s select timeout -- mirrors XbDiag */
    u_long nb = 1; ioctlsocket(sock, FIONBIO, &nb);
    sockaddr_in sa; ZeroMemory(&sa, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((u_short)k_http_port);
    sa.sin_addr = s_server_addr;
    int cr = connect(sock, (sockaddr*)&sa, sizeof(sa));
    if (cr == SOCKET_ERROR) {
        if (WSAGetLastError() != WSAEWOULDBLOCK) { closesocket(sock); return 0; }
        fd_set wset; FD_ZERO(&wset); FD_SET(sock, &wset);
        TIMEVAL tv; tv.tv_sec = 5; tv.tv_usec = 0;
        if (select(0, NULL, &wset, NULL, &tv) <= 0) { closesocket(sock); return 0; }
    }

    /* Switch to blocking with 10s timeouts */
    nb = 0; ioctlsocket(sock, FIONBIO, &nb);
    int tmo = 10000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof(tmo));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tmo, sizeof(tmo));

    /* Send GET request */
    char req[256];
    StrCopy(req, sizeof(req), "GET "); AppendStr(req, sizeof(req), path);
    AppendStr(req, sizeof(req), " HTTP/1.0\r\nHost: "); AppendStr(req, sizeof(req), s_server_ip);
    AppendStr(req, sizeof(req), "\r\nConnection: close\r\n\r\n");
    if (send(sock, req, StrLen(req), 0) <= 0) { closesocket(sock); return 0; }

    /* Read header */
    char hdrBuf[2048]; int hdrLen = 0;
    while (hdrLen < (int)sizeof(hdrBuf) - 1) {
        int n = recv(sock, hdrBuf + hdrLen, (int)sizeof(hdrBuf) - 1 - hdrLen, 0);
        if (n <= 0) { closesocket(sock); return 0; }
        hdrLen += n; hdrBuf[hdrLen] = '\0';
        if (FindHeaderEnd(hdrBuf, hdrLen) >= 0) break;
    }
    int bodyStart = FindHeaderEnd(hdrBuf, hdrLen);
    if (bodyStart < 0 || GetHttpStatus(hdrBuf, hdrLen) != 200) { closesocket(sock); return 0; }

    DWORD cl = ParseContentLength(hdrBuf, bodyStart);
    s_total = cl; s_received = 0;

    /* Open destination file */
    HANDLE hf = CreateFile(dest, GENERIC_WRITE, 0, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) { closesocket(sock); return 0; }

    /* Write any body bytes already in header buffer */
    int overflow = hdrLen - bodyStart;
    DWORD totalRecv = 0;
    if (overflow > 0) {
        DWORD wr = 0; WriteFile(hf, hdrBuf + bodyStart, (DWORD)overflow, &wr, NULL);
        totalRecv += wr; s_received = totalRecv;
    }

    /* Download body */
    char dlBuf[4096];
    while (1) {
        int n = recv(sock, dlBuf, sizeof(dlBuf), 0);
        if (n <= 0) break;
        DWORD wr = 0; WriteFile(hf, dlBuf, (DWORD)n, &wr, NULL);
        totalRecv += wr; s_received = totalRecv;
        if (cl > 0 && totalRecv >= cl) break;
    }

    FlushFileBuffers(hf);
    CloseHandle(hf);
    closesocket(sock);

    if (totalRecv == 0) { DeleteFileA(dest); return 0; }
    return 1;
}

// ============================================================================
// Non-blocking helpers (version check only)
// ============================================================================
static void CloseSocket(void)
{
    if (s_sock != INVALID_SOCKET) { closesocket(s_sock); s_sock = INVALID_SOCKET; }
}
static void SetError(const char* msg)
{
    StrCopy(s_error, sizeof(s_error), msg);
    StrCopy(s_status, sizeof(s_status), "Update error: ");
    AppendStr(s_status, sizeof(s_status), msg);
    s_state = SCUPD_ERROR;
    CloseSocket();
    if (s_dns) { XNetDnsRelease(s_dns); s_dns = NULL; }
}
static int BeginCheckConnect(void)
{
    CloseSocket();
    s_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s_sock == INVALID_SOCKET) return 0;
    u_long nb = 1; ioctlsocket(s_sock, FIONBIO, &nb);
    sockaddr_in sa; ZeroMemory(&sa, sizeof(sa));
    sa.sin_family = AF_INET; sa.sin_port = htons((u_short)k_http_port);
    sa.sin_addr = s_server_addr;
    int r = connect(s_sock, (sockaddr*)&sa, sizeof(sa));
    if (r == 0 || WSAGetLastError() == WSAEWOULDBLOCK) return 1;
    CloseSocket(); return 0;
}
static int PollConnect(void)
{
    fd_set wfds, efds;
    FD_ZERO(&wfds); FD_SET(s_sock, &wfds);
    FD_ZERO(&efds); FD_SET(s_sock, &efds);
    TIMEVAL tv = { 0,0 };
    int r = select(0, NULL, &wfds, &efds, &tv);
    if (r == SOCKET_ERROR || FD_ISSET(s_sock, &efds)) { CloseSocket(); return -1; }
    return FD_ISSET(s_sock, &wfds) ? 1 : 0;
}
static int SendGet(const char* path)
{
    char req[256];
    StrCopy(req, sizeof(req), "GET "); AppendStr(req, sizeof(req), path);
    AppendStr(req, sizeof(req), " HTTP/1.0\r\nHost: "); AppendStr(req, sizeof(req), s_server_ip);
    AppendStr(req, sizeof(req), "\r\nConnection: close\r\n\r\n");
    int total = StrLen(req), sent = 0;
    while (sent < total) {
        int n = send(s_sock, req + sent, total - sent, 0);
        if (n == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAEWOULDBLOCK) continue;
            CloseSocket(); return 0;
        }
        sent += n;
    }
    return 1;
}

// ============================================================================
// Non-blocking update check tick (version check only)
// ============================================================================
static void CheckTick(void)
{
    switch (s_state)
    {
    case SCUPD_NET_WAIT:
    {
        XNADDR xna; ZeroMemory(&xna, sizeof(xna));
        DWORD st = XNetGetTitleXnAddr(&xna);
        if (st == XNET_GET_XNADDR_PENDING) {
            if (GetTickCount() - s_net_wait_start > 5000) s_state = SCUPD_CHECK_DONE;
            return;
        }
        if ((st & XNET_GET_XNADDR_NONE) || xna.ina.s_addr == 0) {
            s_state = SCUPD_CHECK_DONE; return;
        }
        int dr = XNetDnsLookup(s_server_ip, NULL, &s_dns);
        if (dr != 0 || !s_dns) { s_state = SCUPD_CHECK_DONE; return; }
        s_state = SCUPD_DNS; return;
    }
    case SCUPD_DNS:
    {
        if (!s_dns) { s_state = SCUPD_CHECK_DONE; return; }
        if (s_dns->iStatus == WSAEINPROGRESS) return;
        if (s_dns->iStatus != 0) {
            XNetDnsRelease(s_dns); s_dns = NULL;
            s_state = SCUPD_CHECK_DONE; return;
        }
        s_server_addr = s_dns->aina[0];
        XNetDnsRelease(s_dns); s_dns = NULL;
        if (!BeginCheckConnect()) { s_state = SCUPD_CHECK_DONE; return; }
        s_state = SCUPD_CHECK_CONNECT; return;
    }
    case SCUPD_CHECK_CONNECT:
    {
        int r = PollConnect();
        if (r == 0) return;
        if (r < 0) { s_state = SCUPD_CHECK_DONE; return; }
        s_recv_len = 0; ZeroMemory(s_recv_buf, sizeof(s_recv_buf));
        if (!SendGet(k_ver_path)) { s_state = SCUPD_CHECK_DONE; return; }
        s_state = SCUPD_CHECK_RECV; return;
    }
    case SCUPD_CHECK_RECV:
    {
        /* Mirror XbDiag UPST_RECV_VER -- accumulate only */
        int space = (int)sizeof(s_recv_buf) - s_recv_len - 1;
        if (space <= 0) { s_state = SCUPD_CHECK_COMPARE; return; }
        int n = recv(s_sock, s_recv_buf + s_recv_len, space, 0);
        if (n > 0) { s_recv_len += n; s_recv_buf[s_recv_len] = '\0'; }
        else if (n == 0) { CloseSocket(); s_state = SCUPD_CHECK_COMPARE; }
        else if (WSAGetLastError() != WSAEWOULDBLOCK) { CloseSocket(); s_state = SCUPD_CHECK_DONE; }
        return;
    }
    case SCUPD_CHECK_COMPARE:
    {
        /* Mirror XbDiag UPST_COMPARE */
        int status = GetHttpStatus(s_recv_buf, s_recv_len);
        if (status != 200) { s_state = SCUPD_CHECK_DONE; return; }
        int he = FindHeaderEnd(s_recv_buf, s_recv_len);
        if (he < 0) { s_state = SCUPD_CHECK_DONE; return; }
        DWORD cl = ParseContentLength(s_recv_buf, he);
        (void)cl;
        StrCopy(s_remote_ver, sizeof(s_remote_ver), s_recv_buf + he);
        StripWhitespace(s_remote_ver);
        if (s_remote_ver[0] == '\0') { s_state = SCUPD_CHECK_DONE; return; }
        s_available = (s_local_ver[0] == '\0' || VerCmp(s_remote_ver, s_local_ver) > 0) ? 1 : 0;
        if (s_available) {
            StrCopy(s_status, sizeof(s_status), "Update available: v");
            AppendStr(s_status, sizeof(s_status), s_remote_ver);
            AppendStr(s_status, sizeof(s_status), "  [A]=Update  [B]=Skip");
            s_state = SCUPD_IDLE; /* wait for user input */
        }
        else {
            s_state = SCUPD_CHECK_DONE;
        }
        return;
    }
    default: return;
    }
}

// ============================================================================
// Public API
// ============================================================================
void SC_Update_Init(const char* server_ip, const char* install_dir)
{
    StrCopy(s_server_ip, sizeof(s_server_ip), server_ip ? server_ip : "");
    DeriveInstallPaths(install_dir);
    ReadLocalVer();

    /* NetEnsure -- mirror XbDiag: init network stack before any socket use */
    {
        XNetStartupParams xnsp; WSADATA wsa;
        ZeroMemory(&xnsp, sizeof(xnsp));
        xnsp.cfgSizeOfStruct = sizeof(xnsp);
        xnsp.cfgFlags = XNET_STARTUP_BYPASS_SECURITY;
        XNetStartup(&xnsp);
        WSAStartup(MAKEWORD(2, 2), &wsa);
    }

    s_state = SCUPD_IDLE;
    s_available = 0;
    s_check_started = 0;
    s_net_wait_start = 0;
    ZeroMemory(&s_server_addr, sizeof(s_server_addr));
    ZeroMemory(s_remote_ver, sizeof(s_remote_ver));
    ZeroMemory(s_status, sizeof(s_status));
    ZeroMemory(s_error, sizeof(s_error));
}

void SC_Update_StartCheck(void)
{
    if (s_state != SCUPD_IDLE) return;
    s_check_started = 1;
    s_recv_len = 0;
    s_available = 0;
    ZeroMemory(s_remote_ver, sizeof(s_remote_ver));
    ZeroMemory(s_recv_buf, sizeof(s_recv_buf));
    StrCopy(s_status, sizeof(s_status), "Checking for updates...");
    s_net_wait_start = GetTickCount();
    s_state = SCUPD_NET_WAIT;
}

int SC_Update_IsCheckDone(void)
{
    if (!s_check_started) return 0;
    if (s_state == SCUPD_NET_WAIT)      return 0;
    if (s_state == SCUPD_DNS)           return 0;
    if (s_state == SCUPD_CHECK_CONNECT) return 0;
    if (s_state == SCUPD_CHECK_RECV)    return 0;
    if (s_state == SCUPD_CHECK_COMPARE) return 0;
    return 1;
}

void SC_Update_Tick(void)
{
    if (s_state == SCUPD_IDLE ||
        s_state == SCUPD_CHECK_DONE ||
        s_state == SCUPD_DONE ||
        s_state == SCUPD_ERROR ||
        s_state == SCUPD_DOWNLOADING ||
        s_state == SCUPD_EXTRACTING)
        return;
    CheckTick();
}

int         SC_Update_IsAvailable(void) { return s_available; }
const char* SC_Update_GetRemoteVersion(void) { return s_remote_ver; }
const char* SC_Update_GetLocalVersion(void) { return s_local_ver[0] ? s_local_ver : k_local_version; }
const char* SC_Update_GetStatus(void) { return s_status; }
int         SC_Update_IsComplete(void) { return s_state == SCUPD_DONE; }
int         SC_Update_HasError(void) { return s_state == SCUPD_ERROR; }

float SC_Update_GetProgress(void)
{
    if (s_state == SCUPD_DONE) return 1.0f;
    return (s_total > 0) ? (float)s_received / (float)s_total : 0.0f;
}

void SC_Update_StartDownload(void)
{
    /* Mirror XbDiag UPST_AVAIL handler exactly -- blocking download + extract */
    if (!s_available) return;

    s_state = SCUPD_DOWNLOADING;
    StrCopy(s_status, sizeof(s_status), "Downloading update...");
    s_total = 0; s_received = 0;

    /* Step 1: download scenechat.xba to temp */
    if (!DoDownload(k_xba_path, k_xba_tmp))
    {
        DeleteFileA(k_xba_tmp);
        SetError("download failed"); return;
    }

    /* Step 2: extract XBA to install directory */
    s_state = SCUPD_EXTRACTING;
    StrCopy(s_status, sizeof(s_status), "Extracting update...");
    s_total = 0; s_received = 0;

    ZeroMemory(s_detail, sizeof(s_detail));
    XbaResult xr = Xba_Extract(k_xba_tmp, s_install_dir,
        XbaProgressCb, s_detail, sizeof(s_detail));
    DeleteFileA(k_xba_tmp);

    if (xr != XBA_OK)
    {
        char msg[80]; StrCopy(msg, sizeof(msg), "extract failed: ");
        AppendStr(msg, sizeof(msg), Xba_ResultStr(xr));
        if (s_detail[0]) { AppendStr(msg, sizeof(msg), " ("); AppendStr(msg, sizeof(msg), s_detail); AppendStr(msg, sizeof(msg), ")"); }
        SetError(msg); return;
    }

    /* Step 3: 5-second settle wait -- mirrors XbDiag exactly */
    DWORD waitStart = GetTickCount();
    while (GetTickCount() - waitStart < 5000) {}

    /* Step 4: download scenechat.ver last to confirm success */
    StrCopy(s_status, sizeof(s_status), "Finalising...");
    if (!DoDownload(k_ver_path, s_ver_dest))
    {
        SetError("ver download failed"); return;
    }

    s_state = SCUPD_DONE;
    StrCopy(s_status, sizeof(s_status), "Update complete. Press A to relaunch.");
}

void SC_Update_Apply(void)
{
    /* Derive XBE path from install dir */
    char xbe_path[128];
    StrCopy(xbe_path, sizeof(xbe_path), s_install_dir);
    AppendStr(xbe_path, sizeof(xbe_path), "default.xbe");
    LAUNCH_DATA ld; ZeroMemory(&ld, sizeof(ld));
    XLaunchNewImage(xbe_path, &ld);
    while (1) {}
}

void SC_Update_Reset(void)
{
    CloseSocket();
    if (s_dns) { XNetDnsRelease(s_dns); s_dns = NULL; }
    DeleteFileA(k_xba_tmp);
    s_state = SCUPD_IDLE;
    s_available = 0;
    s_check_started = 0;
    s_net_wait_start = 0;
    ZeroMemory(s_remote_ver, sizeof(s_remote_ver));
    ZeroMemory(s_status, sizeof(s_status));
    ZeroMemory(s_error, sizeof(s_error));
}