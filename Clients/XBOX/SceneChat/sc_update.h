#pragma once
// sc_update.h
// SceneChat - OTA update downloader and extractor.
//
// Adapted from XbDiag Update.cpp/h.  Key differences:
//   - Version is delivered via SCCP_UPDATE_AVAIL (0x1A) after AUTH_OK,
//     not fetched from GitHub.  sc_net.cpp stores the remote version string.
//   - Download URL is http://<server_ip>:8950/update/scenechat.xba
//   - No DNS required -- server IP already known from the chat connection.
//   - No separate state screen -- integrates as a chat UI overlay/notification.
//   - Install path derived from XeImageFileName->Buffer (same as XbDiag).
//   - xba.h / xba.cpp are drop-in; Xba_Extract() is used without modification.
//
// RXDK constraints:
//   - No sprintf / sscanf / strlen
//   - Ftoi() for float-to-int
//   - File-scope statics only
//   - One _emit per line for inline asm (none used here)
//
// Call order:
//   SC_Update_Init(server_ip)       -- call once after successful login
//   SC_Update_IsAvailable()         -- returns true if SCCP_UPDATE_AVAIL received
//   SC_Update_GetRemoteVersion()    -- version string from server
//   SC_Update_StartDownload()       -- begin HTTP download of XBA
//   SC_Update_Tick()                -- call every frame during download
//   SC_Update_GetProgress()         -- 0.0f - 1.0f for progress bar
//   SC_Update_GetStatus()           -- status string for UI
//   SC_Update_IsComplete()          -- true when extraction is done
//   SC_Update_Apply()               -- XLaunchNewImage to relaunch updated XBE
//   SC_Update_Reset()               -- reset on logout/disconnect

#include <xtl.h>

#ifdef __cplusplus
extern "C" {
#endif

    /* Initialise with the server IP and the directory containing the running
       XBE (e.g. "D:\\").  Call once after successful login.
       install_dir is the same value passed to Update_SetPaths in XbDiag --
       derive it from XeImageFileName->Buffer in main.cpp or auth.cpp. */
    void SC_Update_Init(const char* server_ip, const char* install_dir);

    /* Call each frame between Init and IsComplete to drive the download state
       machine.  Safe to call when no update is pending (no-op). */
    void SC_Update_Tick(void);

    /* Returns 1 if SCCP_UPDATE_AVAIL was received and a newer version is
       available.  Populated by sc_net.cpp via SC_Update_NotifyAvailable(). */
    int SC_Update_IsAvailable(void);

    /* Called by sc_net.cpp when SCCP_UPDATE_AVAIL (0x1A) is received.
       Stores the remote version string. */
    void SC_Update_NotifyAvailable(const char* remote_version);

    /* Returns the remote version string set by NotifyAvailable, or "". */
    const char* SC_Update_GetRemoteVersion(void);

    /* Returns the local version string read from scenechat.ver, or k_localVer. */
    const char* SC_Update_GetLocalVersion(void);

    /* Start a background HTTP version check.
       Call immediately after SC_Update_Init(), before SC_Net_ConnectBegin().
       Drive with SC_Update_Tick() each frame.
       When SC_Update_IsCheckDone() returns 1:
         - SC_Update_IsAvailable() == 1  => show update prompt
         - SC_Update_IsAvailable() == 0  => proceed with SC_Net_ConnectBegin() */
    void SC_Update_StartCheck(void);

    /* Returns 1 when the version check HTTP request has completed
       (success, no update found, or network error). */
    int  SC_Update_IsCheckDone(void);

    /* Begin downloading and extracting the update XBA.
       Call after the user confirms the update prompt. */
    void SC_Update_StartDownload(void);

    /* 0.0f - 1.0f progress fraction.  Covers download + extraction. */
    float SC_Update_GetProgress(void);

    /* Short status string for the chat UI status bar, e.g.
       "Downloading update... 42%"  or  "Update complete. Press A to relaunch." */
    const char* SC_Update_GetStatus(void);

    /* Returns 1 when extraction is complete and relaunch is ready. */
    int SC_Update_IsComplete(void);

    /* Returns 1 if an error occurred during download or extraction. */
    int SC_Update_HasError(void);

    /* XLaunchNewImage to the newly installed XBE.  Call when user confirms
       relaunch.  Does not return. */
    void SC_Update_Apply(void);

    /* Reset all state.  Call on logout or disconnect. */
    void SC_Update_Reset(void);

#ifdef __cplusplus
}
#endif