/*---------------------------------------------------------------------------
    debug_keyboard.cpp -- RXDK/XKbd debug keyboard backend.

    Per-port device tracking via XGetDeviceChanges + XInputOpen, matching
    the dvd2xbox / RXDK-SDL2x pattern. Even with SINGLE_KEYBOARD_ONLY
    (always defined on Xbox), opening the device per-port activates the
    debug keyboard routing for that device. XInputDebugGetKeystroke drains
    the global queue regardless of handle.

    Compile flag: #define SC_ENABLE_DEBUG_KEYBOARD 1
---------------------------------------------------------------------------*/

#define DEBUG_KEYBOARD
#include <xtl.h>
#include "debug_keyboard.h"
#include "sc_log.h"
#include <string.h>

#define DK_CHAR_QUEUE_SIZE    64
#define DK_SPECIAL_QUEUE_SIZE 32

static int    s_initialized = 0;
static int    s_present = 0;
static HANDLE s_handles[4] = { NULL, NULL, NULL, NULL };

static char   s_charQueue[DK_CHAR_QUEUE_SIZE];
static int    s_charHead = 0, s_charTail = 0;
static int    s_specialQueue[DK_SPECIAL_QUEUE_SIZE];
static int    s_specialHead = 0, s_specialTail = 0;

static void DK_PushChar(char c) {
    int next = (s_charTail + 1) % DK_CHAR_QUEUE_SIZE;
    if (next == s_charHead) return;
    s_charQueue[s_charTail] = c;
    s_charTail = next;
}

static void DK_PushSpecial(int key) {
    int next = (s_specialTail + 1) % DK_SPECIAL_QUEUE_SIZE;
    if (next == s_specialHead) return;
    s_specialQueue[s_specialTail] = key;
    s_specialTail = next;
}

static int DK_VKeyToSpecial(BYTE vk) {
    switch (vk) {
    case VK_RETURN:  return DK_KEY_ENTER;
    case VK_BACK:    return DK_KEY_BACKSPACE;
    case VK_ESCAPE:  return DK_KEY_ESCAPE;
    case VK_LEFT:    return DK_KEY_LEFT;
    case VK_RIGHT:   return DK_KEY_RIGHT;
    case VK_UP:      return DK_KEY_UP;
    case VK_DOWN:    return DK_KEY_DOWN;
    case VK_DELETE:  return DK_KEY_DELETE;
    case VK_TAB:     return DK_KEY_TAB;
    case VK_HOME:    return DK_KEY_HOME;
    case VK_END:     return DK_KEY_END;
    case VK_PRIOR:   return DK_KEY_PGUP;
    case VK_NEXT:    return DK_KEY_PGDN;
    default:         return DK_KEY_NONE;
    }
}

static void DK_UpdateDevices(void) {
    DWORD insertions = 0, removals = 0;
    DWORD i;
    XINPUT_POLLING_PARAMETERS poll;

    XGetDeviceChanges(XDEVICE_TYPE_DEBUG_KEYBOARD, &insertions, &removals);

    for (i = 0; i < 4; i++) {
        if (removals & (1 << i)) {
            if (s_handles[i]) {
                XInputClose(s_handles[i]);
                s_handles[i] = NULL;
                SC_Log_Int("DK", "keyboard removed port", (int)i);
            }
        }
        if (insertions & (1 << i)) {
            memset(&poll, 0, sizeof(poll));
            poll.fAutoPoll = TRUE;
            poll.fInterruptOut = TRUE;
            poll.bInputInterval = 32;
            poll.bOutputInterval = 32;
            s_handles[i] = XInputOpen(
                XDEVICE_TYPE_DEBUG_KEYBOARD,
                i, XDEVICE_NO_SLOT, &poll);
            SC_Log_Int("DK", "keyboard opened port", (int)i);
        }
    }
}

void DK_Init(void) {
    XINPUT_DEBUG_KEYQUEUE_PARAMETERS params;
    DWORD result;

    DK_Clear();
    s_initialized = 0;
    s_present = 0;
    memset(s_handles, 0, sizeof(s_handles));

    memset(&params, 0, sizeof(params));
    /* ONE_QUEUE is mandatory on RDXK: SINGLE_KEYBOARD_ONLY is always
       defined in XKbd.h, so XInputDebugGetKeystroke never takes a
       handle parameter. Without ONE_QUEUE there is no queue to drain. */
    params.dwFlags = XINPUT_DEBUG_KEYQUEUE_FLAG_KEYDOWN
        | XINPUT_DEBUG_KEYQUEUE_FLAG_KEYREPEAT
        | XINPUT_DEBUG_KEYQUEUE_FLAG_KEYUP
        | XINPUT_DEBUG_KEYQUEUE_FLAG_ONE_QUEUE;
    params.dwQueueSize = 32;
    params.dwRepeatDelay = 500;
    params.dwRepeatInterval = 50;

    result = XInputDebugInitKeyboardQueue(&params);
    SC_Log_Int("DK", "InitKeyboardQueue result", (int)result);

    if (result != ERROR_SUCCESS) return;
    s_initialized = 1;

    /* Initial device scan */
    DK_UpdateDevices();
}

void DK_Shutdown(void) {
    DWORD i;
    for (i = 0; i < 4; i++) {
        if (s_handles[i]) { XInputClose(s_handles[i]); s_handles[i] = NULL; }
    }
    DK_Clear();
    s_initialized = 0;
    s_present = 0;
}

void DK_Poll(void) {
    XINPUT_DEBUG_KEYSTROKE ks;
    DWORD result;
    int spec;

    if (!s_initialized) return;

    DK_UpdateDevices();

    for (;;) {
        memset(&ks, 0, sizeof(ks));
        result = XInputDebugGetKeystroke(&ks);
        if (result != ERROR_SUCCESS) break;

        s_present = 1;
        if (ks.Flags & XINPUT_DEBUG_KEYSTROKE_FLAG_KEYUP) continue;

        if (ks.Ascii >= 0x20 && ks.Ascii <= 0x7E) {
            DK_PushChar((char)ks.Ascii);
            continue;
        }

        spec = DK_VKeyToSpecial(ks.VirtualKey);
        if (spec != DK_KEY_NONE) DK_PushSpecial(spec);
    }
}

char DK_GetChar(void) {
    char c;
    if (s_charHead == s_charTail) return 0;
    c = s_charQueue[s_charHead];
    s_charHead = (s_charHead + 1) % DK_CHAR_QUEUE_SIZE;
    return c;
}

int DK_GetSpecial(void) {
    int k;
    if (s_specialHead == s_specialTail) return DK_KEY_NONE;
    k = s_specialQueue[s_specialHead];
    s_specialHead = (s_specialHead + 1) % DK_SPECIAL_QUEUE_SIZE;
    return k;
}

int  DK_IsPresent(void) { return s_present; }
void DK_Clear(void) {
    s_charHead = s_charTail = s_specialHead = s_specialTail = 0;
}