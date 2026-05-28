/*---------------------------------------------------------------------------
    voice.cpp -- Xbox Communicator voice chat for SceneChat.
    Patched from ScorchedXB voice.cpp:
      - Removed xb_net.h / render.h dependencies
      - XbNet_SendData / XbNet_RecvVoice replaced with UDP socket
      - Voice_DrawHUD rewritten for SceneChat font.h / ui.h
      - g_pDS extern'd from main.cpp (defined there alongside D3D device)
---------------------------------------------------------------------------*/

#include <xtl.h>
#include <winsockx.h>
#include <dsound.h>
#include <xvoice.h>
#include "voice.h"
#include "font.h"
#include "ui.h"
#include "input.h"

/* g_pDS defined in main.cpp */
extern IDirectSound* g_pDS;

/* ── Constants ───────────────────────────────────────────────────────── */
#define VOICE_SAMPLE_RATE       8000
#define VOICE_FRAME_MS          20
#define VOICE_SAMPLES_PER_FRAME ( VOICE_SAMPLE_RATE * VOICE_FRAME_MS / 1000 )
#define VOICE_PCM_BYTES         ( VOICE_SAMPLES_PER_FRAME * 2 )
#define VOICE_MAX_CODEC_BYTES   64
#define VOICE_DS_BUF_BYTES      ( VOICE_SAMPLE_RATE * 2 * 400 / 1000 )
#define VOICE_SILENCE_THRESHOLD 400
#define VOICE_MAX_PACKETS       4
#define VOICE_GAIN              3
#define VOICE_UDP_PORT          7800

/* UDP header layout:
   [user_id 4B BE][room_id 1B][slot 1B][adpcm payload]  -- send
   [slot 1B][adpcm payload]                              -- recv relay  */
#define VOICE_HDR_SEND    6
#define VOICE_PKT_SEND    ( VOICE_HDR_SEND + VOICE_MAX_CODEC_BYTES )
#define VOICE_PKT_RECV    ( 1 + VOICE_MAX_CODEC_BYTES )

   /* ── Hardware state ──────────────────────────────────────────────────── */
static int                 s_bActive = 0;
static int                 s_mySlot = 0;
static int                 s_bLocalMuted = 0;
static int                 s_bIncomingMuted = 0;
static int                 s_nCodecBytes = 20;

static LPXMEDIAOBJECT      s_pMicObj = NULL;
static LPXMEDIAOBJECT      s_pHpObj = NULL;
static LPXVOICEENCODER     s_pEncoder = NULL;
static LPXVOICEDECODER     s_pDecoder = NULL;
static IDirectSoundBuffer* s_pPlayBuf = NULL;
static DWORD               s_dwPlayPos = 0;

static int                 s_bHasMic[VOICE_MAX_PLAYERS];
static int                 s_bTalking[VOICE_MAX_PLAYERS];

#define CAP_QUEUE  2
static short s_capBuf[CAP_QUEUE][VOICE_SAMPLES_PER_FRAME];
static DWORD s_capGot[CAP_QUEUE];
static DWORD s_capStat[CAP_QUEUE];
static int   s_capInited = 0;

#define HP_QUEUE   4
static short s_hpBuf[HP_QUEUE][VOICE_SAMPLES_PER_FRAME];
static DWORD s_hpStat[HP_QUEUE];
static int   s_hpHead = 0;

static short         s_pcmOut[VOICE_SAMPLES_PER_FRAME];
static unsigned char s_codec[VOICE_MAX_CODEC_BYTES];

/* ── UDP voice state ─────────────────────────────────────────────────── */
static SOCKET              s_udp = INVALID_SOCKET;
static struct sockaddr_in  s_srv_addr;
static unsigned int        s_user_id = 0;
static unsigned char       s_voice_room_id = 0;
static int                 s_bConnected = 0;

/* ── Audio helpers ───────────────────────────────────────────────────── */
static int IsSilent(const short* p, int n) {
    int i;
    for (i = 0; i < n; i++)
        if (p[i] > VOICE_SILENCE_THRESHOLD || p[i] < -VOICE_SILENCE_THRESHOLD)
            return 0;
    return 1;
}

static void ApplyGain(short* p, int n) {
    int i;
    for (i = 0; i < n; i++) {
        int v = (int)p[i] * VOICE_GAIN;
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        p[i] = (short)v;
    }
}

static void DsWrite(const short* pcm, DWORD bytes) {
    void* p1 = NULL, * p2 = NULL; DWORD b1 = 0, b2 = 0;
    DWORD dwPlay = 0, dwWrite = 0;
    if (!s_pPlayBuf) return;
    s_pPlayBuf->GetCurrentPosition(&dwPlay, &dwWrite);
    if (s_dwPlayPos == dwPlay)
        s_dwPlayPos = (dwWrite + 32) % VOICE_DS_BUF_BYTES;
    if (SUCCEEDED(s_pPlayBuf->Lock(s_dwPlayPos, bytes,
        &p1, &b1, &p2, &b2, 0))) {
        if (p1) CopyMemory(p1, pcm, b1);
        if (p2) CopyMemory(p2, (const BYTE*)pcm + b1, b2);
        s_pPlayBuf->Unlock(p1, b1, p2, b2);
        s_dwPlayPos = (s_dwPlayPos + bytes) % VOICE_DS_BUF_BYTES;
    }
}

static void HpSubmit(const short* pcm, DWORD bytes) {
    int q, tries;
    if (!s_pHpObj) return;
    for (tries = 0; tries < HP_QUEUE; tries++) {
        q = s_hpHead;
        if (s_hpStat[q] != XMEDIAPACKET_STATUS_PENDING) {
            XMEDIAPACKET pkt;
            CopyMemory(s_hpBuf[q], pcm, bytes);
            s_hpStat[q] = XMEDIAPACKET_STATUS_PENDING;
            ZeroMemory(&pkt, sizeof(pkt));
            pkt.pvBuffer = s_hpBuf[q];
            pkt.dwMaxSize = bytes;
            pkt.pdwStatus = &s_hpStat[q];
            s_pHpObj->Process(&pkt, NULL);
            s_hpHead = (s_hpHead + 1) % HP_QUEUE;
            return;
        }
        s_hpHead = (s_hpHead + 1) % HP_QUEUE;
    }
}

/* ── UDP helpers ─────────────────────────────────────────────────────── */

static void VoiceUDP_Send(const unsigned char* adpcm, int adpcm_len) {
    unsigned char pkt[VOICE_PKT_SEND];
    int k;
    if (s_udp == INVALID_SOCKET || !s_bConnected) return;
    /* Header: [user_id 4B BE][room_id 1B][slot 1B] */
    pkt[0] = (unsigned char)(s_user_id >> 24);
    pkt[1] = (unsigned char)(s_user_id >> 16);
    pkt[2] = (unsigned char)(s_user_id >> 8);
    pkt[3] = (unsigned char)(s_user_id);
    pkt[4] = s_voice_room_id;
    pkt[5] = (unsigned char)s_mySlot;
    if (adpcm_len > VOICE_MAX_CODEC_BYTES) adpcm_len = VOICE_MAX_CODEC_BYTES;
    for (k = 0; k < adpcm_len; k++) pkt[VOICE_HDR_SEND + k] = adpcm[k];
    sendto(s_udp, (const char*)pkt, VOICE_HDR_SEND + adpcm_len, 0,
        (struct sockaddr*)&s_srv_addr, sizeof(s_srv_addr));
}

static int VoiceUDP_Recv(unsigned char* adpcm_out, int* adpcm_len, int* slot_out) {
    unsigned char pkt[VOICE_PKT_RECV];
    int n;
    struct sockaddr_in from;
    int from_len = sizeof(from);
    if (s_udp == INVALID_SOCKET) return 0;
    n = recvfrom(s_udp, (char*)pkt, sizeof(pkt), 0,
        (struct sockaddr*)&from, &from_len);
    if (n < 2) return 0;
    *slot_out = (int)pkt[0];
    *adpcm_len = n - 1;
    CopyMemory(adpcm_out, pkt + 1, *adpcm_len);
    return 1;
}

static void VoiceUDP_SendJoin(void) {
    unsigned char pkt[VOICE_HDR_SEND];
    if (s_udp == INVALID_SOCKET) return;
    pkt[0] = (unsigned char)(s_user_id >> 24);
    pkt[1] = (unsigned char)(s_user_id >> 16);
    pkt[2] = (unsigned char)(s_user_id >> 8);
    pkt[3] = (unsigned char)(s_user_id);
    pkt[4] = s_voice_room_id;
    pkt[5] = (unsigned char)s_mySlot;
    sendto(s_udp, (const char*)pkt, VOICE_HDR_SEND, 0,
        (struct sockaddr*)&s_srv_addr, sizeof(s_srv_addr));
}

/* ── Init / Shutdown ─────────────────────────────────────────────────── */

void Voice_Init(int mySlot) {
    HRESULT      hr;
    WAVEFORMATEX wfx;
    DSBUFFERDESC desc;
    int          i, q, port;

    if (mySlot < 0 || mySlot >= VOICE_MAX_PLAYERS) return;

    Voice_Shutdown();

    s_mySlot = mySlot;
    s_bLocalMuted = 0;
    s_bIncomingMuted = 0;
    s_bActive = 0;
    s_dwPlayPos = 0;
    s_capInited = 0;
    s_hpHead = 0;

    for (i = 0; i < VOICE_MAX_PLAYERS; i++) {
        s_bHasMic[i] = 0;
        s_bTalking[i] = 0;
    }

    if (!g_pDS) return;

    /* PCM format: 8kHz 16-bit mono */
    ZeroMemory(&wfx, sizeof(wfx));
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = 1;
    wfx.nSamplesPerSec = VOICE_SAMPLE_RATE;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = 2;
    wfx.nAvgBytesPerSec = VOICE_SAMPLE_RATE * 2;

    /* DS ring buffer (TV speaker) */
    ZeroMemory(&desc, sizeof(desc));
    desc.dwSize = sizeof(desc);
    desc.dwBufferBytes = VOICE_DS_BUF_BYTES;
    desc.lpwfxFormat = &wfx;

    hr = g_pDS->CreateSoundBuffer(&desc, &s_pPlayBuf, NULL);
    if (FAILED(hr)) return;

    {
        void* p1 = NULL, * p2 = NULL; DWORD b1 = 0, b2 = 0;
        if (SUCCEEDED(s_pPlayBuf->Lock(0, VOICE_DS_BUF_BYTES,
            &p1, &b1, &p2, &b2, 0))) {
            ZeroMemory(p1, b1);
            if (p2) ZeroMemory(p2, b2);
            s_pPlayBuf->Unlock(p1, b1, p2, b2);
        }
    }
    s_pPlayBuf->Play(0, 0, DSBPLAY_LOOPING);
    s_dwPlayPos = (VOICE_SAMPLE_RATE * 2 * 200 / 1000) % VOICE_DS_BUF_BYTES;

    /* Encoder */
    hr = XVoiceCreateOneToOneEncoder(&s_pEncoder);
    if (FAILED(hr)) { Voice_Shutdown(); return; }

    {
        DWORD dwSize = 0;
        if (SUCCEEDED(IXVoiceEncoder_GetCodecBufferSize(
            s_pEncoder, VOICE_PCM_BYTES, &dwSize)) && dwSize > 0) {
            s_nCodecBytes = (int)dwSize;
            if (s_nCodecBytes > VOICE_MAX_CODEC_BYTES)
                s_nCodecBytes = VOICE_MAX_CODEC_BYTES;
        }
    }

    /* Decoder */
    hr = XVoiceCreateOneToOneDecoder(&s_pDecoder);
    if (FAILED(hr)) { Voice_Shutdown(); return; }

    /* Communicator detection */
    for (port = 0; port < 4; port++) {
        hr = XVoiceCreateMediaObject(XDEVICE_TYPE_VOICE_MICROPHONE,
            (DWORD)port, VOICE_MAX_PACKETS,
            &wfx, &s_pMicObj);
        if (SUCCEEDED(hr)) {
            XVoiceCreateMediaObject(XDEVICE_TYPE_VOICE_HEADPHONE,
                (DWORD)port, VOICE_MAX_PACKETS,
                &wfx, &s_pHpObj);
            s_bHasMic[s_mySlot] = 1;

            for (q = 0; q < CAP_QUEUE; q++) {
                XMEDIAPACKET pkt;
                s_capGot[q] = 0;
                s_capStat[q] = XMEDIAPACKET_STATUS_PENDING;
                ZeroMemory(&pkt, sizeof(pkt));
                pkt.pvBuffer = s_capBuf[q];
                pkt.dwMaxSize = VOICE_PCM_BYTES;
                pkt.pdwCompletedSize = &s_capGot[q];
                pkt.pdwStatus = &s_capStat[q];
                s_pMicObj->Process(NULL, &pkt);
            }
            s_capInited = 1;

            s_hpHead = 0;
            for (q = 0; q < HP_QUEUE; q++) {
                XMEDIAPACKET pkt;
                s_hpStat[q] = XMEDIAPACKET_STATUS_PENDING;
                ZeroMemory(s_hpBuf[q], VOICE_PCM_BYTES);
                ZeroMemory(&pkt, sizeof(pkt));
                pkt.pvBuffer = s_hpBuf[q];
                pkt.dwMaxSize = VOICE_PCM_BYTES;
                pkt.pdwStatus = &s_hpStat[q];
                s_pHpObj->Process(&pkt, NULL);
            }
            break;
        }
        if (s_pMicObj) { s_pMicObj->Release(); s_pMicObj = NULL; }
    }

    s_bActive = 1;
}

void Voice_Shutdown(void) {
    Voice_Disconnect();
    if (s_pMicObj) { s_pMicObj->Release();   s_pMicObj = NULL; }
    if (s_pHpObj) { s_pHpObj->Release();    s_pHpObj = NULL; }
    if (s_pDecoder) { s_pDecoder->Release();  s_pDecoder = NULL; }
    if (s_pEncoder) { s_pEncoder->Release();  s_pEncoder = NULL; }
    if (s_pPlayBuf) { s_pPlayBuf->Stop(); s_pPlayBuf->Release(); s_pPlayBuf = NULL; }
    s_bActive = 0;
    s_capInited = 0;
}

/* ── UDP connect / disconnect ────────────────────────────────────────── */

void Voice_Connect(const char* server_ip, unsigned int user_id,
    unsigned char room_id) {
    u_long nonblock = 1;

    Voice_Disconnect();

    s_user_id = user_id;
    s_voice_room_id = room_id;

    s_udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_udp == INVALID_SOCKET) return;

    ioctlsocket(s_udp, FIONBIO, &nonblock);

    ZeroMemory(&s_srv_addr, sizeof(s_srv_addr));
    s_srv_addr.sin_family = AF_INET;
    s_srv_addr.sin_port = htons(VOICE_UDP_PORT);
    s_srv_addr.sin_addr.s_addr = inet_addr(server_ip);

    s_bConnected = 1;

    /* Send join packet to register with the server */
    VoiceUDP_SendJoin();
}

void Voice_Disconnect(void) {
    if (s_udp != INVALID_SOCKET) {
        closesocket(s_udp);
        s_udp = INVALID_SOCKET;
    }
    s_bConnected = 0;
}

/* ── Per-frame update ────────────────────────────────────────────────── */

void Voice_Update(WORD wPressed) {
    int i;

    if (wPressed & BTN_LTHUMB) s_bLocalMuted = !s_bLocalMuted;
    if (wPressed & BTN_RTHUMB) s_bIncomingMuted = !s_bIncomingMuted;

    for (i = 0; i < VOICE_MAX_PLAYERS; i++) s_bTalking[i] = 0;

    if (!s_bActive) return;

    /* ── Capture + encode + transmit ────────────────────────────── */
    if (!s_bLocalMuted && s_pMicObj && s_pEncoder && s_capInited) {
        int q;
        for (q = 0; q < CAP_QUEUE; q++) {
            XMEDIAPACKET pkt;

            if (s_capStat[q] != XMEDIAPACKET_STATUS_SUCCESS) continue;

            if (s_capGot[q] >= (DWORD)VOICE_PCM_BYTES &&
                !IsSilent(s_capBuf[q], VOICE_SAMPLES_PER_FRAME)) {

                XMEDIAPACKET encIn, encOut;
                DWORD        dwEncGot = 0;

                ZeroMemory(&encIn, sizeof(encIn));
                ZeroMemory(&encOut, sizeof(encOut));
                encIn.pvBuffer = s_capBuf[q];
                encIn.dwMaxSize = VOICE_PCM_BYTES;
                encOut.pvBuffer = s_codec;
                encOut.dwMaxSize = (DWORD)s_nCodecBytes;
                encOut.pdwCompletedSize = &dwEncGot;

                IXVoiceEncoder_ProcessMultiple(s_pEncoder, 1, &encIn, 1, &encOut);

                if (dwEncGot > 0) {
                    /* Send to server via UDP */
                    if (s_bConnected)
                        VoiceUDP_Send(s_codec, (int)dwEncGot);

                    /* Local loopback to catch own voice */
                    if (!s_bIncomingMuted && s_pDecoder) {
                        XMEDIAPACKET decIn, decOut;
                        DWORD        dwDecGot = 0;
                        ZeroMemory(&decIn, sizeof(decIn));
                        ZeroMemory(&decOut, sizeof(decOut));
                        decIn.pvBuffer = s_codec;
                        decIn.dwMaxSize = dwEncGot;
                        decOut.pvBuffer = s_pcmOut;
                        decOut.dwMaxSize = VOICE_PCM_BYTES;
                        decOut.pdwCompletedSize = &dwDecGot;
                        IXVoiceDecoder_ProcessMultiple(s_pDecoder, 1, &decIn, 1, &decOut);
                        if (dwDecGot > 0) {
                            ApplyGain(s_pcmOut, (int)(dwDecGot / 2));
                            if (!s_pHpObj) DsWrite(s_pcmOut, dwDecGot);
                            HpSubmit(s_pcmOut, dwDecGot);
                        }
                    }

                    s_bTalking[s_mySlot] = 1;
                }
            }

            /* Resubmit capture slot */
            s_capGot[q] = 0;
            s_capStat[q] = XMEDIAPACKET_STATUS_PENDING;
            ZeroMemory(&pkt, sizeof(pkt));
            pkt.pvBuffer = s_capBuf[q];
            pkt.dwMaxSize = VOICE_PCM_BYTES;
            pkt.pdwCompletedSize = &s_capGot[q];
            pkt.pdwStatus = &s_capStat[q];
            s_pMicObj->Process(NULL, &pkt);
        }
    }

    /* ── Receive + decode + play ─────────────────────────────────── */
    if (!s_bIncomingMuted && s_pDecoder && s_pPlayBuf && s_bConnected) {
        unsigned char adpcm[VOICE_MAX_CODEC_BYTES];
        int           adpcm_len, recv_slot;

        while (VoiceUDP_Recv(adpcm, &adpcm_len, &recv_slot)) {
            XMEDIAPACKET decIn, decOut;
            DWORD        dwDecGot = 0;

            if (recv_slot < 0 || recv_slot >= VOICE_MAX_PLAYERS) continue;
            if (recv_slot == s_mySlot) continue;
            if (adpcm_len <= 0) continue;

            s_bHasMic[recv_slot] = 1;
            s_bTalking[recv_slot] = 1;

            ZeroMemory(&decIn, sizeof(decIn));
            ZeroMemory(&decOut, sizeof(decOut));
            decIn.pvBuffer = adpcm;
            decIn.dwMaxSize = (DWORD)adpcm_len;
            decOut.pvBuffer = s_pcmOut;
            decOut.dwMaxSize = VOICE_PCM_BYTES;
            decOut.pdwCompletedSize = &dwDecGot;

            IXVoiceDecoder_ProcessMultiple(s_pDecoder, 1, &decIn, 1, &decOut);

            if (dwDecGot > 0) {
                ApplyGain(s_pcmOut, (int)(dwDecGot / 2));
                DsWrite(s_pcmOut, dwDecGot);
                HpSubmit(s_pcmOut, dwDecGot);
            }
        }
    }
}

/* ── HUD ─────────────────────────────────────────────────────────────── */

void Voice_DrawHUD(IDirect3DDevice8* pDevice) {
    int   i;
    float virt_w = (float)UI_VIRT_W;
    float bar_w = virt_w / (float)VOICE_MAX_PLAYERS - 6.0f;
    float y = (float)(UI_HEADER_H + 4);

    if (!s_bActive && !s_bConnected) return;

    for (i = 0; i < VOICE_MAX_PLAYERS; i++) {
        float     bx = 3.0f + (float)i * (bar_w + 6.0f);
        D3DCOLOR  col;
        char      label[16];

        if (!s_bHasMic[i]) continue;

        if (i == s_mySlot && s_bLocalMuted)
            col = UI_COL_RED;
        else if (s_bTalking[i])
            col = UI_COL_ONLINE;
        else
            col = UI_COL_TEXT_MUTED;

        wsprintf(label, "[%d]", i);
        Font_DrawText(pDevice, bx, y, "[MIC]",
            FONT_SIZE_SMALL, col, 0);
    }

    /* Mute status for local user */
    if (s_bLocalMuted) {
        Font_DrawText(pDevice,
            virt_w - 120.0f, y,
            "MIC MUTED",
            FONT_SIZE_SMALL, UI_COL_RED, 0);
    }
    if (s_bIncomingMuted) {
        Font_DrawText(pDevice,
            virt_w - 120.0f, y + 18.0f,
            "AUDIO MUTED",
            FONT_SIZE_SMALL, UI_COL_RED, 0);
    }
}

/* ── Queries ─────────────────────────────────────────────────────────── */

int Voice_IsLocalMuted(void) { return s_bLocalMuted; }
int Voice_IsIncomingMuted(void) { return s_bIncomingMuted; }
int Voice_IsActive(void) { return s_bActive; }
int Voice_IsConnected(void) { return s_bConnected; }
int Voice_SlotHasMic(int s) { return (s >= 0 && s < VOICE_MAX_PLAYERS) ? s_bHasMic[s] : 0; }
int Voice_SlotTalking(int s) { return (s >= 0 && s < VOICE_MAX_PLAYERS) ? s_bTalking[s] : 0; }