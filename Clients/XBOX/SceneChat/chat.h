#ifndef SCENECHAT_CHAT_H
#define SCENECHAT_CHAT_H
/*---------------------------------------------------------------------------
    chat.h -- Main chat screen for SceneChat.

    Drives the full chat UI after successful login:
        - Sidebar room list (text + voice rooms)
        - Message history with scroll
        - Text input bar (HID keyboard or on-screen KB fallback)
        - Ping/pong keepalive
        - Analog cursor navigation (left stick + left trigger = click)

    Call order:
        Chat_Init(pDevice, username)
        -- per frame --
        Chat_Update(wPressed)
        Chat_Draw(pDevice)
        Chat_Shutdown()

    Message cache: held in memory only.
    D:\ cache available if needed later -- room messages can be written
    to D:\room_N.dat for persistence across sessions.
---------------------------------------------------------------------------*/

#ifndef DEBUG_KEYBOARD
#define DEBUG_KEYBOARD
#endif

#include <xtl.h>
#include <d3d8.h>

#ifdef __cplusplus
extern "C" {
#endif

    /* Per-room in-memory message cache */
#define CHAT_MSG_BUF        100   /* messages kept per room                  */
#define CHAT_PING_FRAMES    300   /* send ping every 300 frames (~5s @ 60fps)*/
#define CHAT_MSG_LINE_H     26    /* virtual pixels per message line         */
#define CHAT_CURSOR_SPEED   8.0f  /* virtual pixels per frame at full stick  */
#define CHAT_VOICE_SERVER_PORT 7800
#define CHAT_DEADZONE       3000  /* stick dead zone                         */

/* Initialise -- call once after Auth_Update returns AUTH_RESULT_OK.
   username : the logged-in username from Auth_GetUsername()
   server_ip: dotted-decimal server IP (same as used in Auth_Init)
   user_id  : from SC_AuthResult.user_id (Auth passes this through)       */
    void Chat_Init(IDirect3DDevice8* pDevice, const char* username,
        const char* server_ip, unsigned int user_id);
    void Chat_Shutdown(void);

    /* Per-frame update.  wPressed = freshly-pressed button mask from input.h.
       Returns 0 normally, -1 if user pressed Back (caller should quit/logout). */
    int  Chat_Update(WORD wPressed);

    /* Draw the full chat UI -- call inside render pass */
    void Chat_Draw(IDirect3DDevice8* pDevice);

#ifdef __cplusplus
}
#endif

#endif /* SCENECHAT_CHAT_H */