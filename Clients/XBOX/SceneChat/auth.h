#ifndef SCENECHAT_AUTH_H
#define SCENECHAT_AUTH_H
/*---------------------------------------------------------------------------
    auth.h -- Login and registration screen for SceneChat.

    Manages the full auth flow:
        Connecting -> Login / Register form -> Waiting -> Done / Error

    Uses sc_net.h for server communication, ui.h + font.h for rendering,
    hid.h for keyboard input, screen_kb.h as fallback.

    Call order:
        Auth_Init(pDevice, server_ip)
        -- per frame --
        Auth_Update(wPressed)   returns AUTH_RESULT_*
        Auth_Draw(pDevice)

    On AUTH_RESULT_OK the session is established and sc_net is READY.
    Caller should then transition to the chat screen.
---------------------------------------------------------------------------*/

#ifndef DEBUG_KEYBOARD
#define DEBUG_KEYBOARD
#endif

#include <xtl.h>
#include <d3d8.h>

#ifdef __cplusplus
extern "C" {
#endif

    /* Return values from Auth_Update() */
#define AUTH_RESULT_NONE     0   /* still in progress                        */
#define AUTH_RESULT_OK       1   /* logged in successfully                   */
#define AUTH_RESULT_QUIT    -1   /* user pressed Back to quit                */

/* Initialise auth state and kick off the server connection.
   server_ip: dotted-decimal string e.g. "192.168.1.100"                    */
    void Auth_Init(IDirect3DDevice8* pDevice, const char* server_ip);
    void Auth_InitLogout(IDirect3DDevice8* pDevice, const char* server_ip);
    void Auth_Shutdown(void);

    /* Process one frame of input + network.
       wPressed: freshly-pressed button mask from input.h GetButtons().
       Returns AUTH_RESULT_*.                                                    */
    int  Auth_Update(WORD wPressed);

    /* Draw the auth screen -- call inside your render pass                      */
    void Auth_Draw(IDirect3DDevice8* pDevice);

    /* After AUTH_RESULT_OK these are populated                                  */
    const char* Auth_GetUsername(void);
    const char* Auth_GetToken(void);
    unsigned int Auth_GetUserID(void);

#ifdef __cplusplus
}
#endif

#endif /* SCENECHAT_AUTH_H */