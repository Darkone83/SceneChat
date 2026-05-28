#ifndef DEBUG_KEYBOARD_H
#define DEBUG_KEYBOARD_H
/*---------------------------------------------------------------------------
    debug_keyboard.h -- RXDK/XKbd debug keyboard backend.
    NOT generic USB HID. Only works when the Xbox debug keyboard device
    path sees the attached keyboard.
---------------------------------------------------------------------------*/

#ifdef __cplusplus
extern "C" {
#endif

#define DK_KEY_NONE       0
#define DK_KEY_ENTER      1
#define DK_KEY_BACKSPACE  2
#define DK_KEY_ESCAPE     3
#define DK_KEY_LEFT       4
#define DK_KEY_RIGHT      5
#define DK_KEY_UP         6
#define DK_KEY_DOWN       7
#define DK_KEY_DELETE     8
#define DK_KEY_TAB        9
#define DK_KEY_HOME       10
#define DK_KEY_END        11
#define DK_KEY_PGUP       12
#define DK_KEY_PGDN       13

    void DK_Init(void);
    void DK_Shutdown(void);
    void DK_Poll(void);
    char DK_GetChar(void);
    int  DK_GetSpecial(void);
    int  DK_IsPresent(void);
    void DK_Clear(void);

#ifdef __cplusplus
}
#endif

#endif /* DEBUG_KEYBOARD_H */