#pragma once
#ifndef SCENECHAT_SCREEN_KB_H
#define SCENECHAT_SCREEN_KB_H
/*---------------------------------------------------------------------------
    screen_kb.h -- Controller-navigable on-screen keyboard overlay.

    Renders a semi-transparent QWERTY overlay over the live scene, similar
    to the XBMC4Gamers keyboard.  Used as fallback when no USB HID keyboard
    is detected (HID_IsPresent() == 0).

    Three key sets cycle with the X button:
        SET 0 -- lowercase  a-z + symbols
        SET 1 -- uppercase  A-Z + symbols
        SET 2 -- numbers / extended symbols

    Controller mapping:
        D-pad / left stick  -- move key cursor
        A / Left Trigger    -- select key (type character)
        B                   -- backspace
        X                   -- cycle key set (lower / upper / symbols)
        Y                   -- space
        Start               -- confirm / close keyboard (returns 1)
        Back                -- cancel / close keyboard (returns -1)

    Call order:
        ScreenKB_Open(initial_text, max_len)   -- show overlay
        ScreenKB_Update(buttons_pressed)        -- call every frame, returns:
                                                     0  = still open
                                                     1  = confirmed (text in buf)
                                                    -1  = cancelled
        ScreenKB_Draw(pDevice)                  -- call during render pass
        ScreenKB_GetText(buf, buflen)           -- retrieve typed text
        ScreenKB_Close()                        -- hide overlay
---------------------------------------------------------------------------*/

#ifndef DEBUG_KEYBOARD
#define DEBUG_KEYBOARD
#endif

#include <xtl.h>
#include <d3d8.h>

#ifdef __cplusplus
extern "C" {
#endif

    /* Maximum text length the keyboard can hold */
#define SCREEN_KB_MAX_LEN   128

/* Open the keyboard overlay.
   initial_text  -- pre-fill buffer (pass "" for empty)
   max_len       -- maximum characters allowed (capped at SCREEN_KB_MAX_LEN) */
    void ScreenKB_Open(const char* initial_text, int max_len);

    /* Close / hide the keyboard */
    void ScreenKB_Close(void);

    /* Returns 1 if the overlay is currently visible */
    int  ScreenKB_IsOpen(void);

    /* Process controller input for this frame.
       Pass the freshly-pressed button mask from input.h (GetButtons() result).
       Returns:
           0   -- keyboard still open, no action yet
           1   -- user confirmed (Start / Done key)
          -1   -- user cancelled (Back)                                         */
    int  ScreenKB_Update(WORD wPressed);

    /* Draw the keyboard overlay -- call during the render pass AFTER the scene */
    void ScreenKB_Draw(IDirect3DDevice8* pDevice);

    /* Copy the current text into buf (null terminated) */
    void ScreenKB_GetText(char* buf, int buflen);

#ifdef __cplusplus
}
#endif

#endif /* SCENECHAT_SCREEN_KB_H */