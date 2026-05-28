#ifndef SCENECHAT_HID_H
#define SCENECHAT_HID_H
/*---------------------------------------------------------------------------
    hid.h -- RXDK debug keyboard adapter (NOT generic USB HID).

    Uses XInputDebugInitKeyboardQueue / XInputDebugGetKeystroke with
    ONE_QUEUE (SINGLE_KEYBOARD_ONLY is always defined on Xbox, so there
    is no per-port handle path). Modern USB keyboards are NOT supported
    by this path -- use the on-screen keyboard for those.

    Enable at compile time with:
        #define SC_ENABLE_DEBUG_KEYBOARD 1

    Public API is intentionally identical to what a future real HID
    backend would expose, so callers do not change.
---------------------------------------------------------------------------*/

#define SC_ENABLE_DEBUG_KEYBOARD 1

#define HID_KEY_NONE        0x00
#define HID_KEY_ENTER       0x01
#define HID_KEY_BACKSPACE   0x02
#define HID_KEY_DELETE      0x03
#define HID_KEY_LEFT        0x04
#define HID_KEY_RIGHT       0x05
#define HID_KEY_UP          0x06
#define HID_KEY_DOWN        0x07
#define HID_KEY_HOME        0x08
#define HID_KEY_END         0x09
#define HID_KEY_ESCAPE      0x0A
#define HID_KEY_TAB         0x0B
#define HID_KEY_PGUP        0x0C
#define HID_KEY_PGDN        0x0D
#define HID_KEY_F1          0x0E
#define HID_KEY_F2          0x0F

#define HID_CHAR_QUEUE_SIZE    32
#define HID_SPECIAL_QUEUE_SIZE 16

#ifdef __cplusplus
extern "C" {
#endif

    /* keyboard_port is accepted for API compatibility but ignored --
       ONE_QUEUE aggregates all ports and is the only valid path here. */
    void HID_Init(void);
    void HID_Shutdown(void);
    void HID_Poll(void);
    char HID_GetChar(void);
    int  HID_GetSpecial(void);
    int  HID_IsPresent(void);
    void HID_ClearQueue(void);

#ifdef __cplusplus
}
#endif

#endif /* SCENECHAT_HID_H */