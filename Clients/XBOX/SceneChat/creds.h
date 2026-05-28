#ifndef SCENECHAT_CREDS_H
#define SCENECHAT_CREDS_H
/*---------------------------------------------------------------------------
    creds.h -- Credential persistence for SceneChat.

    Writes to D:\creds.dat (XBE root).
    On HDD: kernel remaps D:\ to the app directory -- write succeeds.
    On DVD: D:\ is the actual disc (read-only) -- write silently fails.
    No detection needed; CreateFile failure is the signal.

    TODO: migrate to TDATA when project matures.
---------------------------------------------------------------------------*/

#include <xtl.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CREDS_PATH     "D:\\creds.dat"
#define CREDS_MAX_USER 32
#define CREDS_MAX_PASS 64

    /* Try to load D:\creds.dat. Returns 1 if valid creds found, 0 otherwise. */
    int  Creds_Load(void);

    /* Save to D:\creds.dat. Silently does nothing on DVD (write fails). */
    void Creds_Save(const char* username, const char* password);

    /* Delete D:\creds.dat if it exists. */
    void Creds_Clear(void);

    /* 1 if Creds_Load() succeeded this session. */
    int  Creds_Have(void);

    const char* Creds_GetUsername(void);
    const char* Creds_GetPassword(void);

#ifdef __cplusplus
}
#endif

#endif /* SCENECHAT_CREDS_H */