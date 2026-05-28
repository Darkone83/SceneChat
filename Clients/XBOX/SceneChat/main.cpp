/*---------------------------------------------------------------------------
    main.cpp -- SceneChat XBE entry point.

    Init order:
        XNet  ->  D3D8  ->  DirectSound  ->  Font  ->  UI  ->  Auth  ->  Chat

    State machine:
        STATE_AUTH  -- login / registration screen
        STATE_CHAT  -- main chat screen
        STATE_QUIT  -- clean shutdown and dashboard return

    Resolution detection via XGetVideoFlags():
        XC_VIDEO_FLAGS_HDTV_720p  -> 1280x720  progressive widescreen
        XC_VIDEO_FLAGS_HDTV_480p  -> 640x480   progressive
        default                   -> 640x480   interlaced

    g_pDS is defined here and extern'd by voice.cpp.
    g_pDevice is defined here; all modules receive it as a parameter.

    Server IP is hardcoded as SCENECHAT_SERVER_IP -- change before build.
---------------------------------------------------------------------------*/

#include <xtl.h>
#include <dsound.h>
#include <d3d8.h>
#include "auth.h"
#include "sc_net.h"
#include "sc_log.h"
#include "chat.h"
#include "voice.h"
#include "ui.h"
#include "font.h"
#include "emoji.h"
#include "input.h"
#include "hid.h"

/*  Server IP  */
#ifndef SCENECHAT_SERVER_IP
#define SCENECHAT_SERVER_IP  "darkone83.myddns.me"
#endif

/*  Globals (extern'd by voice.cpp and other modules)  */
IDirect3D8* g_pD3D = NULL;
IDirect3DDevice8* g_pDevice = NULL;
IDirectSound* g_pDS = NULL;

static DWORD g_dwDisplayW = 640;
static DWORD g_dwDisplayH = 480;

/*  App states  */
#define STATE_AUTH  0
#define STATE_CHAT  1
#define STATE_QUIT  2

static int s_state = STATE_AUTH;

/* Auth result carries through to Chat_Init */
static unsigned int s_user_id = 0;

/*  Display mode setup  */
static DWORD DetectDisplayMode(void) {
    DWORD flags = XGetVideoFlags();

    if (flags & XC_VIDEO_FLAGS_HDTV_720p) {
        g_dwDisplayW = 1280;
        g_dwDisplayH = 720;
        return D3DPRESENTFLAG_PROGRESSIVE | D3DPRESENTFLAG_WIDESCREEN;
    }
    if (flags & XC_VIDEO_FLAGS_HDTV_480p) {
        g_dwDisplayW = 640;
        g_dwDisplayH = 480;
        return D3DPRESENTFLAG_PROGRESSIVE;
    }
    /* Default: 480i */
    g_dwDisplayW = 640;
    g_dwDisplayH = 480;
    return 0;
}

/*  D3D8 init  */
static int InitD3D(void) {
    D3DPRESENT_PARAMETERS pp;
    DWORD                 display_flags;
    HRESULT               hr;

    g_pD3D = Direct3DCreate8(D3D_SDK_VERSION);
    if (!g_pD3D) return 0;

    display_flags = DetectDisplayMode();

    ZeroMemory(&pp, sizeof(pp));
    pp.BackBufferWidth = g_dwDisplayW;
    pp.BackBufferHeight = g_dwDisplayH;
    pp.BackBufferFormat = D3DFMT_A8R8G8B8;
    pp.BackBufferCount = 1;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.FullScreen_RefreshRateInHz = 60;
    pp.FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    pp.Flags = display_flags;

    hr = g_pD3D->CreateDevice(
        0, D3DDEVTYPE_HAL, NULL,
        D3DCREATE_HARDWARE_VERTEXPROCESSING,
        &pp, &g_pDevice);

    if (FAILED(hr)) {
        g_pD3D->Release();
        g_pD3D = NULL;
        return 0;
    }

    /* Default render states */
    g_pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);
    g_pDevice->SetRenderState(D3DRS_ZENABLE, FALSE);
    g_pDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);

    return 1;
}

/*  DirectSound init  */
static int InitDS(void) {
    HRESULT hr = DirectSoundCreate(NULL, &g_pDS, NULL);
    return SUCCEEDED(hr) ? 1 : 0;
}

/*  Full shutdown  */
static void Shutdown(void) {
    Voice_Shutdown();
    Font_Shutdown();
    UI_Shutdown();

    if (g_pDS) { g_pDS->Release();     g_pDS = NULL; }
    if (g_pDevice) { g_pDevice->Release(); g_pDevice = NULL; }
    if (g_pD3D) { g_pD3D->Release();    g_pD3D = NULL; }
}

/*  Return to dashboard  */
static void ExitToDashboard(void) {
    Shutdown();
    XLaunchNewImage(NULL, NULL);
}

/*  XBE entry point  */
void __cdecl main(void) {
    DWORD wLast = 0, wCur = 0, wPressed = 0;
    int   auth_result;
    int   chat_result;

    /*  Logging -- must be first  */
    SC_Log_Init();
    SC_Log("MAIN", "startup");

    /*  Input  */
    InitInput();

    /*  D3D8  */
    if (!InitD3D()) {
        XLaunchNewImage(NULL, NULL);
        return;
    }

    /*  DirectSound  */
    InitDS();

    /*  UI scale  */
    UI_Init(g_pDevice, (int)g_dwDisplayW, (int)g_dwDisplayH);
    UI_LoadLogo(g_pDevice);

    /*  Font atlas  */
    Font_Init(g_pDevice);
    Emoji_Init(g_pDevice);

    /*  Voice hardware init (slot 0 for local user)  */
    Voice_Init(0);

    /*  HID keyboard  */

    /*  Auth screen -- link wait handled inside SC_Net_ConnectPoll  */
    s_state = STATE_AUTH;
    Auth_Init(g_pDevice, SCENECHAT_SERVER_IP);

    /*  Main loop  */
    for (;;) {
        /* Input */
        PumpInput();
        wCur = GetButtons();
        wPressed = wCur & ~wLast;
        wLast = wCur;

        switch (s_state) {

        case STATE_AUTH:
            auth_result = Auth_Update((WORD)wPressed);
            if (auth_result == AUTH_RESULT_OK) {
                /* Hand the live connection to chat -- do NOT call
                   Auth_Shutdown() here because that disconnects the
                   socket. Chat reuses the same authenticated session. */
                Chat_Init(g_pDevice,
                    Auth_GetUsername(),
                    SCENECHAT_SERVER_IP,
                    Auth_GetUserID());
                s_state = STATE_CHAT;
            }
            else if (auth_result == AUTH_RESULT_QUIT) {
                s_state = STATE_QUIT;
            }
            break;

        case STATE_CHAT:
            chat_result = Chat_Update((WORD)wPressed);
            if (chat_result == -1) {
                /* Logged out -- back to auth */
                Chat_Shutdown();
                SC_Net_Disconnect();
                Auth_InitLogout(g_pDevice, SCENECHAT_SERVER_IP);
                s_state = STATE_AUTH;
            }
            break;

        case STATE_QUIT:
            ExitToDashboard();
            return;
        }

        /*  Render  */
        g_pDevice->Clear(0, NULL,
            D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
            0xFF0A0A0A, 1.0f, 0);
        g_pDevice->BeginScene();

        switch (s_state) {
        case STATE_AUTH: Auth_Draw(g_pDevice); break;
        case STATE_CHAT: Chat_Draw(g_pDevice); break;
        default:         break;
        }

        g_pDevice->EndScene();
        g_pDevice->Present(NULL, NULL, NULL, NULL);
    }
}