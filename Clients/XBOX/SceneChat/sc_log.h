#ifndef SC_LOG_H
#define SC_LOG_H
/*---------------------------------------------------------------------------
    sc_log.h -- SceneChat Xbox disk logger.
    Writes to E:\scenechat.log (HDD, always writable on xemu/real hardware).
    Call SC_Log_Init() once at startup, SC_Log() anywhere after.
---------------------------------------------------------------------------*/

#ifdef __cplusplus
extern "C" {
#endif

    void SC_Log_Init(void);
    void SC_Log(const char* tag, const char* msg);
    void SC_Log_Hex(const char* tag, const unsigned char* buf, int len);
    void SC_Log_Int(const char* tag, const char* label, int val);
    void SC_Log_Shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* SC_LOG_H */