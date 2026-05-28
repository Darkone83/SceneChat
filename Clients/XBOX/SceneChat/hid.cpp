/*---------------------------------------------------------------------------
    hid.cpp -- Thin adapter: exposes the existing HID_* API used by
    auth.cpp and chat.cpp, backed by debug_keyboard.cpp.

    Future USB HID backend slots in here without touching callers.
---------------------------------------------------------------------------*/

#include <xtl.h>
#include "hid.h"
#include "debug_keyboard.h"

void HID_Init(void) { DK_Init(); }
void HID_Shutdown(void) { DK_Shutdown(); }
void HID_Poll(void) { DK_Poll(); }
char HID_GetChar(void) { return DK_GetChar(); }
int  HID_GetSpecial(void) { return DK_GetSpecial(); }
int  HID_IsPresent(void) { return DK_IsPresent(); }
void HID_ClearQueue(void) { DK_Clear(); }