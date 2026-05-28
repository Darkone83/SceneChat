/*---------------------------------------------------------------------------
    auth.cpp -- Login and registration screen for SceneChat.

    State machine:
        CONNECTING  ->  LOGIN_FORM  <->  REGISTER_FORM
                            |                  |
                          WAITING            WAITING
                            |                  |
                        DONE / ERROR       DONE / ERROR
---------------------------------------------------------------------------*/

#include <xtl.h>
#include "auth.h"
#include "sc_log.h"
#include "sc_net.h"
#include "ui.h"
#include "font.h"
#include "hid.h"
#include "screen_kb.h"
#include "input.h"
#include "creds.h"
#include <string.h>

/*    States                                                                   */

#define ST_CONNECTING   0
#define ST_LOGIN        1
#define ST_REGISTER     2
#define ST_WAITING      3
#define ST_ERROR        4
#define ST_DONE         5

/*    Field indices                                                            */

#define FIELD_USER      0
#define FIELD_PASS      1
#define FIELD_PASS2     2   /* confirm password -- register only             */
#define FIELD_COUNT_LOGIN 2
#define FIELD_COUNT_REG   3

/*    Layout (virtual 1280x720)                                                */

/* Full-screen layout -- no card, use the whole display */
#define FORM_W      560.0f
#define FORM_X      ((1280.0f - FORM_W) * 0.5f)

#define LOGO_SIZE   96.0f
#define LOGO_X      ((1280.0f - LOGO_SIZE) * 0.5f)
#define LOGO_Y      60.0f

#define TITLE_Y     (LOGO_Y + LOGO_SIZE + 20.0f)

#define FIELD_H     48.0f
#define FIELD_W     FORM_W
#define FIELD_X     FORM_X
#define FIELD_TOP   (TITLE_Y + 44.0f)

#define BTN_H       50.0f
#define BTN_W       FORM_W
#define BTN_X       FORM_X

/* Alias CARD_* to form bounds for shared code */
#define CARD_X      FORM_X
#define CARD_W      FORM_W
#define CARD_Y      LOGO_Y
#define CARD_H      (720.0f - LOGO_Y)

/*    Internal state                                                           */

static int   s_state = ST_CONNECTING;
static int   s_is_register = 0;
static int   s_active_field = 0;
static int   s_field_count = FIELD_COUNT_LOGIN;
static int   s_connect_dots = 0;  /* animation counter                      */
static int   s_frame = 0;

static char  s_user[33] = { 0 };
static char  s_pass[65] = { 0 };
static char  s_pass2[65] = { 0 };
static char  s_error_msg[96] = { 0 };

static char         s_out_username[33] = { 0 };
static char         s_out_token[129] = { 0 };
static unsigned int s_out_user_id = 0;

/*    Field helpers                                                            */

static char* field_buf(int idx) {
    switch (idx) {
    case FIELD_USER:  return s_user;
    case FIELD_PASS:  return s_pass;
    case FIELD_PASS2: return s_pass2;
    default:          return s_user;
    }
}

static int field_maxlen(int idx) {
    return (idx == FIELD_USER) ? 32 : 64;
}

static float field_y(int idx) {
    return FIELD_TOP + (float)idx * (FIELD_H + 20.0f);
}

static float btn_y(void) {
    return field_y(s_field_count) + 14.0f;
}

/*    Open on-screen keyboard for the active field                             */

static void open_kb_for_field(void) {
    char* buf = field_buf(s_active_field);
    ScreenKB_Open(buf, field_maxlen(s_active_field));
}

/*    Submit login or register                                                 */

static void do_submit(void) {
    if (s_state != ST_LOGIN && s_state != ST_REGISTER) return;

    /* Basic validation */
    if (lstrlenA(s_user) < 3) {
        lstrcpyA(s_error_msg, "Username must be at least 3 characters.");
        s_state = ST_ERROR;
        return;
    }
    if (lstrlenA(s_pass) < 8) {
        lstrcpyA(s_error_msg, "Password must be at least 8 characters.");
        s_state = ST_ERROR;
        return;
    }
    if (s_is_register) {
        if (lstrcmpA(s_pass, s_pass2) != 0) {
            lstrcpyA(s_error_msg, "Passwords do not match.");
            s_state = ST_ERROR;
            return;
        }
        SC_Log("AUTH", "sending SCCP_REGISTER");
        SC_Net_SendRegister(s_user, s_pass);
    }
    else {
        SC_Log("AUTH", "sending SCCP_LOGIN");
        SC_Net_SendLogin(s_user, s_pass);
    }

    s_error_msg[0] = 0;
    s_state = ST_WAITING;
}

/*    Public API                                                               */

void Auth_Init(IDirect3DDevice8* pDevice, const char* server_ip) {
    (void)pDevice;
    s_state = ST_CONNECTING;
    s_is_register = 0;
    s_active_field = 0;
    s_field_count = FIELD_COUNT_LOGIN;
    s_connect_dots = 0;
    s_frame = 0;
    s_user[0] = 0;
    s_pass[0] = 0;
    s_pass2[0] = 0;
    s_error_msg[0] = 0;
    s_out_username[0] = 0;
    s_out_token[0] = 0;
    s_out_user_id = 0;

    HID_Init();

    /* Pre-fill from saved creds if present */
    if (Creds_Load()) {
        lstrcpynA(s_user, Creds_GetUsername(), sizeof(s_user));
        lstrcpynA(s_pass, Creds_GetPassword(), sizeof(s_pass));
    }
    SC_Net_Init();
    SC_Net_ConnectBegin(server_ip);
}

void Auth_Shutdown(void) {
    SC_Net_Disconnect();
    HID_Shutdown();
    ScreenKB_Close();
}

void Auth_InitLogout(IDirect3DDevice8* pDevice, const char* server_ip) {
    /* Called when returning from chat -- show login form without
       auto-connecting or auto-logging in. Pre-fill username from
       saved creds so the user only needs to press Start to quit
       via Back, or re-type password to get back in. */
    (void)pDevice;
    (void)server_ip;
    s_state = ST_LOGIN;
    s_is_register = 0;
    s_active_field = 0;
    s_field_count = FIELD_COUNT_LOGIN;
    s_connect_dots = 0;
    s_frame = 0;
    s_pass[0] = 0;
    s_pass2[0] = 0;
    s_error_msg[0] = 0;
    s_out_username[0] = 0;
    s_out_token[0] = 0;
    s_out_user_id = 0;

    /* Keep username pre-filled from saved creds if present */
    s_user[0] = 0;
    if (Creds_Load()) lstrcpynA(s_user, Creds_GetUsername(), sizeof(s_user));

    HID_Init();
}

const char* Auth_GetUsername(void) { return s_out_username; }
unsigned int Auth_GetUserID(void) { return s_out_user_id; }
const char* Auth_GetToken(void) { return s_out_token; }

/*    Update                                                                   */

int Auth_Update(WORD wPressed) {
    SC_AuthResult auth_res;
    char err_buf[96];
    int kb_result;
    int poll_result;
    char ch;
    int spec;

    s_frame++;

    /*    On-screen keyboard overlay active    */
    if (ScreenKB_IsOpen()) {
        kb_result = ScreenKB_Update(wPressed);
        if (kb_result != 0) {
            /* Keyboard closed -- copy result back into the field buffer     */
            char tmp[65];
            ScreenKB_GetText(tmp, sizeof(tmp));
            lstrcpynA(field_buf(s_active_field),
                tmp,
                field_maxlen(s_active_field) + 1);
        }
        return AUTH_RESULT_NONE;
    }

    /*    State machine    */
    switch (s_state) {

        /*    CONNECTING    */
    case ST_CONNECTING:
        s_connect_dots = (s_connect_dots + 1) % 120;
        poll_result = SC_Net_ConnectPoll();
        if (poll_result == 1) {
            /* Handshake complete -- auto-login if saved creds present       */
            if (Creds_Have() && s_user[0] != 0 && s_pass[0] != 0) {
                SC_Net_SendLogin(s_user, s_pass);
                SC_Log("AUTH", "connect ok, auto-login attempt");
                s_state = ST_WAITING;
            }
            else {
                SC_Log("AUTH", "connect ok, showing login form");
                s_state = ST_LOGIN;
                s_active_field = 0;
            }
        }
        else if (poll_result == -1) {
            SC_Log("AUTH", "connect failed -> ST_ERROR");
            SC_Net_GetError(s_error_msg, sizeof(s_error_msg));
            s_state = ST_ERROR;
        }

        if (wPressed & BTN_BACK) return AUTH_RESULT_QUIT;
        break;

        /*    LOGIN / REGISTER FORM    */
    case ST_LOGIN:
    case ST_REGISTER:
        /* HID_Poll already called above for all states */

        /* HID keyboard input into active field -- no IsPresent gate */
    {
        char* fbuf = field_buf(s_active_field);
        int   fmax = field_maxlen(s_active_field);
        int   flen = (int)lstrlenA(fbuf);

        while ((ch = HID_GetChar()) != 0) {
            if (flen < fmax) {
                fbuf[flen++] = ch;
                fbuf[flen] = 0;
            }
        }
        while ((spec = HID_GetSpecial()) != HID_KEY_NONE) {
            switch (spec) {
            case HID_KEY_BACKSPACE:
                if (flen > 0) fbuf[--flen] = 0;
                break;
            case HID_KEY_ENTER:
                if (s_active_field < s_field_count - 1)
                    s_active_field++;
                else
                    do_submit();
                break;
            case HID_KEY_TAB:
            case HID_KEY_DOWN:
                s_active_field = (s_active_field + 1) % s_field_count;
                break;
            case HID_KEY_UP:
                s_active_field = (s_active_field - 1 + s_field_count)
                    % s_field_count;
                break;
            case HID_KEY_ESCAPE:
                return AUTH_RESULT_QUIT;
            default: break;
            }
            flen = (int)lstrlenA(fbuf);
        }
    }

    /* Controller navigation */
    if (wPressed & BTN_DPAD_DOWN)
        s_active_field = (s_active_field + 1) % s_field_count;
    if (wPressed & BTN_DPAD_UP)
        s_active_field = (s_active_field - 1 + s_field_count)
        % s_field_count;

    /* A / Left Trigger: open on-screen keyboard */
    if (wPressed & (BTN_A | BTN_LTRIG)) {
        open_kb_for_field();
    }

    /* Start: submit form */
    if (wPressed & BTN_START) do_submit();

    /* Back: quit */
    if (wPressed & BTN_BACK) return AUTH_RESULT_QUIT;

    /* White: switch between login and register */
    if (wPressed & BTN_WHITE) {
        s_is_register = !s_is_register;
        s_state = s_is_register ? ST_REGISTER : ST_LOGIN;
        s_field_count = s_is_register ? FIELD_COUNT_REG : FIELD_COUNT_LOGIN;
        s_active_field = 0;
        s_error_msg[0] = 0;
    }
    break;

    /*    WAITING FOR SERVER RESPONSE    */
    case ST_WAITING:
        SC_Net_Poll();

        /* Check for auth ok */
        if (SC_Net_RecvAuthResult(&auth_res)) {
            if (s_is_register) {
                /* Registration successful -- send login, then treat next AUTH_OK as login */
                SC_Net_SendLogin(s_user, s_pass);
                s_is_register = 0;
            }
            else {
                lstrcpynA(s_out_username, auth_res.username,
                    sizeof(s_out_username));
                lstrcpynA(s_out_token, auth_res.token,
                    sizeof(s_out_token));
                s_out_user_id = auth_res.user_id;
                Creds_Save(s_user, s_pass);
                s_state = ST_DONE;
            }
        }

        /* Check for error */
        if (SC_Net_RecvError(err_buf, sizeof(err_buf))) {
            lstrcpynA(s_error_msg, err_buf, sizeof(s_error_msg));
            s_state = s_is_register ? ST_REGISTER : ST_LOGIN;
        }

        /* Connection lost */
        if (!SC_Net_IsReady()) {
            SC_Log("AUTH", "connection lost detected");
            lstrcpyA(s_error_msg, "Connection lost.");
            s_state = ST_ERROR;
        }
        break;

        /*    ERROR    */
    case ST_ERROR:
        if (wPressed & BTN_A) {
            /* Retry -- go back to login */
            s_state = ST_LOGIN;
            s_error_msg[0] = 0;
        }
        if (wPressed & BTN_BACK) return AUTH_RESULT_QUIT;
        break;

        /*    DONE    */
    case ST_DONE:
        return AUTH_RESULT_OK;
    }

    return AUTH_RESULT_NONE;
}

/*    Drawing helpers                                                          */

static void draw_field(IDirect3DDevice8* pDevice,
    float x, float y, float w,
    const char* label,
    const char* value,
    int is_password,
    int is_active) {
    char display[66];
    int len, i;
    D3DCOLOR border = is_active ? UI_COL_GREEN : UI_COL_DIVIDER;
    D3DCOLOR bg = is_active ? 0xFF1E1E1E : UI_COL_INPUT_BG;

    /* Label above field */
    Font_DrawText(pDevice, x, y - 20.0f, label,
        FONT_SIZE_SMALL, UI_COL_TEXT_SEC, 0);

    /* Field background */
    UI_DrawRect(pDevice, x, y, w, FIELD_H, bg);
    UI_DrawRectOutline(pDevice, x, y, w, FIELD_H, border);

    /* Value (masked if password) */
    len = (int)lstrlenA(value);
    if (is_password) {
        for (i = 0; i < len && i < 64; i++) display[i] = '*';
        display[i] = 0;
    }
    else {
        lstrcpynA(display, value, sizeof(display));
    }

    /* Cursor blink on active field */
    if (is_active && (s_frame / 20) % 2 == 0) {
        int dl = (int)lstrlenA(display);
        if (dl < 64) { display[dl] = '_'; display[dl + 1] = 0; }
    }

    Font_DrawText(pDevice,
        x + 12.0f,
        y + (FIELD_H - Font_GlyphHeight(FONT_SIZE_MEDIUM)) * 0.5f,
        display,
        FONT_SIZE_MEDIUM,
        UI_COL_TEXT_PRI,
        Ftoi(w - 24.0f));
}

static void draw_button(IDirect3DDevice8* pDevice,
    float x, float y, float w,
    const char* label,
    D3DCOLOR bg, D3DCOLOR fg) {
    UI_DrawRoundRect(pDevice, x, y, w, BTN_H, 6.0f, bg);
    Font_DrawTextCentered(pDevice,
        x,
        y + (BTN_H - Font_GlyphHeight(FONT_SIZE_MEDIUM)) * 0.5f,
        w, label, FONT_SIZE_MEDIUM, fg);
}

/*    Draw                                                                     */

void Auth_Draw(IDirect3DDevice8* pDevice) {
    char title_buf[64];
    float title_y, switch_y;

    /* Full background */
    UI_DrawRect(pDevice, 0, 0, (float)UI_VIRT_W, (float)UI_VIRT_H,
        UI_COL_BG);

    /* On-screen keyboard renders over everything if open */
    if (ScreenKB_IsOpen()) {
        ScreenKB_Draw(pDevice);
        return;
    }

    switch (s_state) {

        /*    CONNECTING    */
    case ST_CONNECTING: {
        int dots = (s_connect_dots / 20) % 4;
        char conn_str[32] = "Connecting";
        int i;
        for (i = 0; i < dots; i++) {
            int l = (int)lstrlenA(conn_str);
            conn_str[l] = '.'; conn_str[l + 1] = 0;
        }
        UI_DrawLogo(pDevice,
            (UI_VIRT_W - LOGO_SIZE * 2.0f) * 0.5f,
            720.0f * 0.3f,
            LOGO_SIZE * 2.0f, LOGO_SIZE * 2.0f);
        Font_DrawTextCentered(pDevice,
            0, 720.0f * 0.62f, (float)UI_VIRT_W,
            conn_str,
            FONT_SIZE_MEDIUM, UI_COL_TEXT_SEC);
        Font_DrawTextCentered(pDevice,
            0, 700.0f, (float)UI_VIRT_W,
            "Press Back to quit",
            FONT_SIZE_SMALL, UI_COL_TEXT_MUTED);
        break;
    }

                      /*    LOGIN / REGISTER FORM    */
    case ST_LOGIN:
    case ST_REGISTER: {
        /* Logo */
        UI_DrawLogo(pDevice, LOGO_X, LOGO_Y, LOGO_SIZE, LOGO_SIZE);

        /* Title */
        title_y = TITLE_Y;
        lstrcpyA(title_buf, s_is_register ? "Create Account" : "Welcome back!");
        Font_DrawTextCentered(pDevice,
            CARD_X, title_y, CARD_W,
            title_buf,
            FONT_SIZE_LARGE, UI_COL_TEXT_PRI);

        /* Fields */
        draw_field(pDevice,
            FIELD_X, field_y(FIELD_USER), FIELD_W,
            "USERNAME", s_user, 0,
            s_active_field == FIELD_USER);

        draw_field(pDevice,
            FIELD_X, field_y(FIELD_PASS), FIELD_W,
            "PASSWORD", s_pass, 1,
            s_active_field == FIELD_PASS);

        if (s_is_register) {
            draw_field(pDevice,
                FIELD_X, field_y(FIELD_PASS2), FIELD_W,
                "CONFIRM PASSWORD", s_pass2, 1,
                s_active_field == FIELD_PASS2);
        }

        /* Submit button */
        draw_button(pDevice,
            BTN_X, btn_y(), BTN_W,
            s_is_register ? "Register" : "Login",
            UI_COL_GREEN, UI_COL_BLACK);

        /* Switch mode hint */
        switch_y = btn_y() + BTN_H + 14.0f;
        Font_DrawTextCentered(pDevice,
            CARD_X, switch_y, CARD_W,
            s_is_register
            ? "Already have an account?  White = Login"
            : "No account?  White = Register",
            FONT_SIZE_SMALL, UI_COL_TEXT_MUTED);

        /* Error message */
        if (s_error_msg[0]) {
            Font_DrawTextCentered(pDevice,
                CARD_X, switch_y + 28.0f, CARD_W,
                s_error_msg,
                FONT_SIZE_SMALL, UI_COL_RED);
        }

        /* Hint bar */
        Font_DrawTextCentered(pDevice,
            0, 690.0f, 1280.0f,
            "D-Pad=Navigate  A=Keyboard  Start=Submit  Back=Quit",
            FONT_SIZE_SMALL, UI_COL_TEXT_MUTED);
        break;
    }

                    /*    WAITING    */
    case ST_WAITING: {
        int spinner_frame = (s_frame / 6) % 4;
        static const char* spinner[] = { "|", "/", "-", "\\" };

        UI_DrawLogo(pDevice, LOGO_X, LOGO_Y, LOGO_SIZE, LOGO_SIZE);

        Font_DrawTextCentered(pDevice,
            CARD_X,
            LOGO_Y + LOGO_SIZE + 40.0f,
            CARD_W,
            spinner[spinner_frame],
            FONT_SIZE_LARGE, UI_COL_GREEN);
        Font_DrawTextCentered(pDevice,
            CARD_X,
            LOGO_Y + LOGO_SIZE + 80.0f,
            CARD_W,
            "Please wait...",
            FONT_SIZE_MEDIUM, UI_COL_TEXT_SEC);
        break;
    }

                   /*    ERROR    */
    case ST_ERROR: {
        UI_DrawLogo(pDevice, LOGO_X, LOGO_Y, LOGO_SIZE, LOGO_SIZE);

        Font_DrawTextCentered(pDevice,
            CARD_X, LOGO_Y + LOGO_SIZE + 24.0f, CARD_W,
            "Connection Error",
            FONT_SIZE_LARGE, UI_COL_RED);
        Font_DrawTextCentered(pDevice,
            CARD_X, LOGO_Y + LOGO_SIZE + 60.0f, CARD_W,
            s_error_msg,
            FONT_SIZE_SMALL, UI_COL_TEXT_SEC);

        draw_button(pDevice,
            BTN_X, btn_y() - 60.0f, BTN_W,
            "Retry  (A)",
            UI_COL_INPUT_BG, UI_COL_GREEN);

        Font_DrawTextCentered(pDevice,
            0, 700.0f, (float)UI_VIRT_W,
            "A=Retry  Back=Quit",
            FONT_SIZE_SMALL, UI_COL_TEXT_MUTED);
        break;
    }

    default: break;
    }
}