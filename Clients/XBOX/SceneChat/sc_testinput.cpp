/*---------------------------------------------------------------------------
    sc_textinput.cpp -- Editable text buffer (C89/MSVC2003).
    Input source agnostic -- OSK, debug keyboard, and future HID all
    push into the same interface.
---------------------------------------------------------------------------*/

#include <xtl.h>
#include "sc_textinput.h"
#include <string.h>

static char s_text[SC_TEXT_MAX];
static int  s_len = 0;
static int  s_cursor = 0;
static int  s_active = 0;
static int  s_submitted = 0;

void SC_TextInput_Begin(const char* initial) {
    int i = 0;
    if (!initial) initial = "";
    while (initial[i] && i < SC_TEXT_MAX - 1) { s_text[i] = initial[i]; i++; }
    s_text[i] = 0;
    s_len = i;
    s_cursor = i;
    s_active = 1;
    s_submitted = 0;
}

void SC_TextInput_End(void) { s_active = 0; }

const char* SC_TextInput_GetText(void) { return s_text; }
int         SC_TextInput_GetCursor(void) { return s_cursor; }
int         SC_TextInput_IsActive(void) { return s_active; }

int SC_TextInput_WasSubmitted(void) {
    if (s_submitted) { s_submitted = 0; return 1; }
    return 0;
}

void SC_TextInput_PushChar(char c) {
    int i;
    if (!s_active) return;
    if (c < 0x20 || c > 0x7E) return;
    if (s_len >= SC_TEXT_MAX - 1) return;
    for (i = s_len; i >= s_cursor; i--) s_text[i + 1] = s_text[i];
    s_text[s_cursor] = c;
    s_cursor++;
    s_len++;
}

void SC_TextInput_Backspace(void) {
    int i;
    if (!s_active || s_cursor <= 0) return;
    for (i = s_cursor - 1; i < s_len; i++) s_text[i] = s_text[i + 1];
    s_cursor--;
    s_len--;
}

void SC_TextInput_Delete(void) {
    int i;
    if (!s_active || s_cursor >= s_len) return;
    for (i = s_cursor; i < s_len; i++) s_text[i] = s_text[i + 1];
    s_len--;
}

void SC_TextInput_Enter(void) {
    if (!s_active) return;
    s_submitted = 1;
    s_active = 0;
}

void SC_TextInput_Cancel(void) {
    if (!s_active) return;
    s_text[0] = 0; s_len = 0; s_cursor = 0;
    s_active = 0;
}

void SC_TextInput_MoveLeft(void) { if (s_active && s_cursor > 0)    s_cursor--; }
void SC_TextInput_MoveRight(void) { if (s_active && s_cursor < s_len) s_cursor++; }
void SC_TextInput_Home(void) { if (s_active) s_cursor = 0; }
void SC_TextInput_End_Key(void) { if (s_active) s_cursor = s_len; }