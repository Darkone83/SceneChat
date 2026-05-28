#ifndef SCENECHAT_VOICE_H
#define SCENECHAT_VOICE_H
/*---------------------------------------------------------------------------
    voice.h -- Xbox Communicator voice chat for SceneChat.

    XVoice ADPCM pipeline (hardware mic/headphone + TV speaker via DS).
    Audio relayed through SceneChat UDP voice server on port 7800.

    UDP packet format (SceneChat protocol):
        [user_id 4B BE][room_id 1B][slot 1B][adpcm payload]
    Join packet: same with empty payload (6 bytes).
    Relay from server: [slot 1B][adpcm payload].

    L3 = toggle local mute (stop transmitting)
    R3 = toggle incoming mute (stop playing received audio)

    Call order:
        Voice_Init(mySlot)                             -- after DS init
        Voice_Connect(server_ip, user_id, room_id)     -- when joining VC room
        -- per frame --
        Voice_Update(wPressed)
        Voice_DrawHUD(pDevice)                         -- after scene render
        Voice_Disconnect()                             -- when leaving VC room
        Voice_Shutdown()                               -- on exit
---------------------------------------------------------------------------*/

#include <xtl.h>
#include <d3d8.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VOICE_MAX_PLAYERS  4

    /* Hardware + codec init -- call once after DirectSound is ready.
       mySlot = local player's slot index (0-3).                        */
    void Voice_Init(int mySlot);
    void Voice_Shutdown(void);

    /* UDP voice session -- call when entering / leaving a voice room.  */
    void Voice_Connect(const char* server_ip,
        unsigned int user_id,
        unsigned char room_id);
    void Voice_Disconnect(void);

    /* Per-frame: capture, encode, transmit, receive, decode, play.
       wPressed = freshly-pressed button mask from input.h              */
    void Voice_Update(WORD wPressed);

    /* Draw mic/mute/talking indicators -- call after scene render      */
    void Voice_DrawHUD(IDirect3DDevice8* pDevice);

    /* State queries */
    int  Voice_IsLocalMuted(void);
    int  Voice_IsIncomingMuted(void);
    int  Voice_IsActive(void);   /* 1 if headset detected        */
    int  Voice_IsConnected(void);   /* 1 if UDP session active      */
    int  Voice_SlotHasMic(int slot);
    int  Voice_SlotTalking(int slot);

#ifdef __cplusplus
}
#endif

#endif /* SCENECHAT_VOICE_H */