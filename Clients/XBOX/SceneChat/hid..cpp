/*---------------------------------------------------------------------------
    hid.cpp -- USB HID keyboard input using XInputDebugGetKeystroke (XKbd.h).

    The Xbox debug keyboard API is gated on #define DEBUG_KEYBOARD (set in
    hid.h before xtl.h is pulled in). It exposes:
        XInputDebugInitKeyboardQueue()   -- called once at init
        XInputDebugGetKeystroke()        -- polled each frame

    XINPUT_DEBUG_KEYSTROKE::Ascii already carries the translated printable
    character (Shift + CapsLock applied by the kernel), so no lookup tables
    are needed. We only inspect VirtualKey for special keys (arrows, Enter,
    Backspace, etc.) where Ascii is 0.

    Return codes from XInputDebugGetKeystroke:
        ERROR_SUCCESS      -- got a keystroke
        ERROR_HANDLE_EOF   -- queue empty (keyboard present but no new keys)
        other error        -- keyboard not present / init failure
---------------------------------------------------------------------------*/

#define DEBUG_KEYBOARD
#include <xtl.h>
#include "hid.h"
#include <string.h>

/* ── Internal state ───────────────────────────────────────────────────────── */

static int  s_initialised = 0;
static int  s_present = 0;
static int  s_shift = 0;
static int  s_ctrl = 0;
static int  s_caps = 0;

static char s_char_buf[HID_CHAR_QUEUE_SIZE];
static int  s_char_head = 0;
static int  s_char_tail = 0;

static int  s_spec_buf[HID_SPECIAL_QUEUE_SIZE];
static int  s_spec_head = 0;
static int  s_spec_tail = 0;

/* ── Queue helpers ────────────────────────────────────────────────────────── */

static void char_push(char c) {
    int next = (s_char_tail + 1) % HID_CHAR_QUEUE_SIZE;
    if (next == s_char_head) return;
    s_char_buf[s_char_tail] = c;
    s_char_tail = next;
}

static void spec_push(int key) {
    int next = (s_spec_tail + 1) % HID_SPECIAL_QUEUE_SIZE;
    if (next == s_spec_head) return;
    s_spec_buf[s_spec_tail] = key;
    s_spec_tail = next;
}

/* ── VK -> special key mapping ────────────────────────────────────────────── */
/* Only keys where Ascii == 0 need to be handled here.                        */

static int vk_to_special(BYTE vk) {
    switch (vk) {
    case VK_RETURN:  return HID_KEY_ENTER;
    case VK_BACK:    return HID_KEY_BACKSPACE;
    case VK_DELETE:  return HID_KEY_DELETE;
    case VK_LEFT:    return HID_KEY_LEFT;
    case VK_RIGHT:   return HID_KEY_RIGHT;
    case VK_UP:      return HID_KEY_UP;
    case VK_DOWN:    return HID_KEY_DOWN;
    case VK_HOME:    return HID_KEY_HOME;
    case VK_END:     return HID_KEY_END;
    case VK_ESCAPE:  return HID_KEY_ESCAPE;
    case VK_TAB:     return HID_KEY_TAB;
    case VK_PRIOR:   return HID_KEY_PGUP;
    case VK_NEXT:    return HID_KEY_PGDN;
    case VK_F1:      return HID_KEY_F1;
    case VK_F2:      return HID_KEY_F2;
    default:         return HID_KEY_NONE;
    }
}

/* ── Public API ───────────────────────────────────────────────────────────── */

void HID_Init(int keyboard_port) {
    XINPUT_DEBUG_KEYQUEUE_PARAMETERS params;
    DWORD result;

    (void)keyboard_port; /* SINGLE_KEYBOARD_ONLY -- port ignored by the API */

    s_char_head = s_char_tail = 0;
    s_spec_head = s_spec_tail = 0;
    s_shift = s_ctrl = s_caps = 0;
    s_present = 0;

    /* Initialise the keyboard queue with keydown + repeat events */
    memset(&params, 0, sizeof(params));
    params.dwFlags = XINPUT_DEBUG_KEYQUEUE_FLAG_KEYDOWN |
        XINPUT_DEBUG_KEYQUEUE_FLAG_KEYREPEAT |
        XINPUT_DEBUG_KEYQUEUE_FLAG_ONE_QUEUE;
    params.dwQueueSize = 32;
    params.dwRepeatDelay = 400;   /* ms before first repeat     */
    params.dwRepeatInterval = 80;    /* ms between repeats         */

    result = XInputDebugInitKeyboardQueue(&params);
    s_initialised = (result == ERROR_SUCCESS) ? 1 : 0;
}

void HID_Shutdown(void) {
    HID_ClearQueue();
    s_initialised = 0;
}

void HID_Poll(void) {
    XINPUT_DEBUG_KEYSTROKE ks;
    DWORD result;
    int spec;

    if (!s_initialised) return;

    for (;;) {
        memset(&ks, 0, sizeof(ks));
        result = XInputDebugGetKeystroke(&ks);

        if (result == ERROR_HANDLE_EOF) {
            /* Queue drained -- keyboard is present but no new events */
            s_present = 1;
            break;
        }
        if (result != ERROR_SUCCESS) {
            /* Keyboard disconnected or not present */
            s_present = 0;
            break;
        }

        s_present = 1;

        /* Skip key-up events -- we only act on keydown and repeat */
        if (ks.Flags & XINPUT_DEBUG_KEYSTROKE_FLAG_KEYUP) continue;

        /* Update modifier state from flags (kernel tracks these for us) */
        s_shift = (ks.Flags & XINPUT_DEBUG_KEYSTROKE_FLAG_SHIFT) ? 1 : 0;
        s_ctrl = (ks.Flags & XINPUT_DEBUG_KEYSTROKE_FLAG_CTRL) ? 1 : 0;
        s_caps = (ks.Flags & XINPUT_DEBUG_KEYSTROKE_FLAG_CAPSLOCK) ? 1 : 0;

        /* Skip Ctrl combinations for now (could add Ctrl+V paste later) */
        if (s_ctrl) continue;

        /* If Ascii is set the kernel already translated it -- use it */
        if (ks.Ascii != 0 && ks.Ascii >= 0x20 && ks.Ascii <= 0x7E) {
            char_push(ks.Ascii);
            continue;
        }

        /* Otherwise check if it maps to a special key */
        spec = vk_to_special(ks.VirtualKey);
        if (spec != HID_KEY_NONE) {
            spec_push(spec);
        }
    }
}

char HID_GetChar(void) {
    char c;
    if (s_char_head == s_char_tail) return 0;
    c = s_char_buf[s_char_head];
    s_char_head = (s_char_head + 1) % HID_CHAR_QUEUE_SIZE;
    return c;
}

int HID_GetSpecial(void) {
    int k;
    if (s_spec_head == s_spec_tail) return HID_KEY_NONE;
    k = s_spec_buf[s_spec_head];
    s_spec_head = (s_spec_head + 1) % HID_SPECIAL_QUEUE_SIZE;
    return k;
}

int  HID_IsShiftHeld(void) { return s_shift; }
int  HID_IsCtrlHeld(void) { return s_ctrl; }
int  HID_IsCapsLockOn(void) { return s_caps; }
int  HID_IsPresent(void) { return s_present; }

void HID_ClearQueue(void) {
    s_char_head = s_char_tail = 0;
    s_spec_head = s_spec_tail = 0;
}