#ifndef SC_TEXTINPUT_H
#define SC_TEXTINPUT_H
/*---------------------------------------------------------------------------
    sc_textinput.h -- Editable text buffer for SceneChat.
    Input source agnostic: fed by OSK, debug keyboard, or future HID.
---------------------------------------------------------------------------*/

#define SC_TEXT_MAX 256

#ifdef __cplusplus
extern "C" {
#endif

    void        SC_TextInput_Begin(const char* initial);
    void        SC_TextInput_End(void);
    const char* SC_TextInput_GetText(void);
    int         SC_TextInput_GetCursor(void);
    int         SC_TextInput_IsActive(void);
    int         SC_TextInput_WasSubmitted(void);   /* 1 once after Enter, then clears */

    void SC_TextInput_PushChar(char c);
    void SC_TextInput_Backspace(void);
    void SC_TextInput_Delete(void);
    void SC_TextInput_Enter(void);
    void SC_TextInput_Cancel(void);
    void SC_TextInput_MoveLeft(void);
    void SC_TextInput_MoveRight(void);
    void SC_TextInput_Home(void);
    void SC_TextInput_End_Key(void);

#ifdef __cplusplus
}
#endif

#endif /* SC_TEXTINPUT_H */