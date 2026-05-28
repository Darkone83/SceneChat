/*---------------------------------------------------------------------------
    screen_kb.cpp -- Controller-navigable on-screen keyboard overlay.

    Layout mirrors XBMC4Gamers: full-screen dark semi-transparent overlay,
    QWERTY grid at the bottom third of the screen, typed text shown above.
    All coordinates are in virtual 1280x720 space (ui.h scale system).
---------------------------------------------------------------------------*/

#include <xtl.h>
#include "screen_kb.h"
#include "ui.h"
#include "font.h"
#include "input.h"
#include <string.h>

/* ── Key layout ───────────────────────────────────────────────────────────── */

/* Three key sets per physical key position:
   [0] = lowercase / default
   [1] = uppercase
   [2] = symbols / numbers                                                    */

typedef struct {
    char sets[3];  /* character produced in each key set                     */
    int  wide;     /* 1 = normal key, 2 = wide key (space/backspace/done)    */
} Key;

/* Row definitions -- NULL char terminates a row                              */
/*
   Row 0: 1 2 3 4 5 6 7 8 9 0 - =
   Row 1: q w e r t y u i o p [ ]
   Row 2: a s d f g h j k l ; '
   Row 3: z x c v b n m , . /
   Row 4: [SPACE x4] [BKSP x2] [DONE x2] [CNCL x2]
*/

#define ROWS        5
#define MAX_KEYS_PER_ROW 13

/* Special key sentinel values stored in sets[0] */
#define SK_SPACE    '\x01'
#define SK_BKSP     '\x02'
#define SK_DONE     '\x03'
#define SK_CANCEL   '\x04'
#define SK_SHIFT    '\x05'

static const Key k_row0[MAX_KEYS_PER_ROW] = {
    {{'1','1','!'},1},{{'2','2','@'},1},{{'3','3','#'},1},{{'4','4','$'},1},
    {{'5','5','%'},1},{{'6','6','^'},1},{{'7','7','&'},1},{{'8','8','*'},1},
    {{'9','9','('},1},{{'0','0',')'},1},{{'-','-','_'},1},{{'=','=','+'},1},
    {{0,0,0},0}
};
static const Key k_row1[MAX_KEYS_PER_ROW] = {
    {{'q','Q','q'},1},{{'w','W','w'},1},{{'e','E','e'},1},{{'r','R','r'},1},
    {{'t','T','t'},1},{{'y','Y','y'},1},{{'u','U','u'},1},{{'i','I','i'},1},
    {{'o','O','o'},1},{{'p','P','p'},1},{{'[','[','{'},1},{{']',']','}'},1},
    {{0,0,0},0}
};
static const Key k_row2[MAX_KEYS_PER_ROW] = {
    {{'a','A','a'},1},{{'s','S','s'},1},{{'d','D','d'},1},{{'f','F','f'},1},
    {{'g','G','g'},1},{{'h','H','h'},1},{{'j','J','j'},1},{{'k','K','k'},1},
    {{'l','L','l'},1},{{';',';',':'},1},{{'\'','\'','"'},1},{{0,0,0},0}
};
static const Key k_row3[MAX_KEYS_PER_ROW] = {
    {{'z','Z','z'},1},{{'x','X','x'},1},{{'c','C','c'},1},{{'v','V','v'},1},
    {{'b','B','b'},1},{{'n','N','n'},1},{{'m','M','m'},1},{{',',',','<'},1},
    {{'.','.','>'},1},{{'/','/','?'},1},{{0,0,0},0}
};
/* Bottom action row: Space(4 wide), Backspace(2), Done(2), Cancel(2) */
static const Key k_row4[MAX_KEYS_PER_ROW] = {
    {{SK_SPACE, SK_SPACE, SK_SPACE},  4},
    {{SK_BKSP,  SK_BKSP,  SK_BKSP},  2},
    {{SK_DONE,  SK_DONE,  SK_DONE},   2},
    {{SK_CANCEL,SK_CANCEL,SK_CANCEL}, 2},
    {{0,0,0},0}
};

static const Key* const k_rows[ROWS] = {
    k_row0, k_row1, k_row2, k_row3, k_row4
};

/* Key counts per row (pre-counted for navigation) */
static const int k_row_count[ROWS] = { 12, 12, 11, 10, 4 };

/* ── Virtual layout constants ─────────────────────────────────────────────── */
/* Keyboard panel occupies the bottom portion of the virtual 1280x720 screen  */

#define KB_PANEL_Y      340.0f    /* top of keyboard panel                    */
#define KB_PANEL_H      (720.0f - KB_PANEL_Y)
#define KB_PANEL_X      40.0f
#define KB_PANEL_W      (1280.0f - KB_PANEL_X * 2.0f)

#define KB_KEY_H        52.0f
#define KB_KEY_GAP      6.0f
#define KB_ROW_GAP      6.0f

/* Text input area */
#define KB_TEXT_Y       270.0f
#define KB_TEXT_H       60.0f
#define KB_TEXT_X       KB_PANEL_X
#define KB_TEXT_W       KB_PANEL_W

/* ── Internal state ───────────────────────────────────────────────────────── */

static int   s_open = 0;
static int   s_keyset = 0;      /* 0=lower 1=upper 2=symbols             */
static int   s_cur_row = 1;      /* currently selected row                */
static int   s_cur_col = 0;      /* currently selected column             */
static int   s_max_len = SCREEN_KB_MAX_LEN;
static char  s_text[SCREEN_KB_MAX_LEN + 1];
static int   s_text_len = 0;
static int   s_cursor_blink = 0;     /* frame counter for cursor blink         */

/* Input repeat tracking */
static WORD  s_last_btn = 0;
static int   s_repeat_timer = 0;
#define REPEAT_INITIAL  18   /* frames before first repeat                    */
#define REPEAT_RATE     5    /* frames between repeats                        */

/* ── Navigation helpers ───────────────────────────────────────────────────── */

static int row_key_count(int row) {
    return k_row_count[row];
}

static void clamp_col(void) {
    int n = row_key_count(s_cur_row);
    if (s_cur_col >= n) s_cur_col = n - 1;
    if (s_cur_col < 0)  s_cur_col = 0;
}

/* ── Key geometry ─────────────────────────────────────────────────────────── */

/* Compute x position and pixel width of key [row][col] in virtual coords */
static void key_rect(int row, int col, float* out_x, float* out_w) {
    const Key* keys = k_rows[row];
    int n = row_key_count(row);
    float unit_w;
    float total_units = 0.0f;
    float cx;
    int i;

    /* Total unit-widths in this row */
    for (i = 0; i < n; i++) total_units += keys[i].wide;
    unit_w = (KB_PANEL_W - KB_KEY_GAP * (n - 1)) / total_units;

    cx = KB_PANEL_X;
    for (i = 0; i < col; i++) {
        cx += keys[i].wide * unit_w + KB_KEY_GAP;
    }
    *out_x = cx;
    *out_w = keys[col].wide * unit_w;
}

static float key_y(int row) {
    return KB_PANEL_Y + (float)row * (KB_KEY_H + KB_ROW_GAP);
}

/* ── Type a key at current cursor position ────────────────────────────────── */

static void do_type_key(int row, int col) {
    const Key* k = &k_rows[row][col];
    char ch = k->sets[s_keyset];

    switch (ch) {
    case SK_SPACE:
        if (s_text_len < s_max_len) {
            s_text[s_text_len++] = ' ';
            s_text[s_text_len] = 0;
        }
        break;
    case SK_BKSP:
        if (s_text_len > 0) {
            s_text_len--;
            s_text[s_text_len] = 0;
        }
        break;
    case SK_DONE:
        /* Handled in ScreenKB_Update return value */
        break;
    case SK_CANCEL:
        break;
    case SK_SHIFT:
        s_keyset = (s_keyset + 1) % 3;
        break;
    default:
        if (ch >= 0x20 && s_text_len < s_max_len) {
            s_text[s_text_len++] = ch;
            s_text[s_text_len] = 0;
            /* Auto-revert to lowercase after one uppercase char */
            if (s_keyset == 1) s_keyset = 0;
        }
        break;
    }
}

/* ── Public API ───────────────────────────────────────────────────────────── */

void ScreenKB_Open(const char* initial_text, int max_len) {
    int len;
    s_open = 1;
    s_keyset = 0;
    s_cur_row = 1;
    s_cur_col = 0;
    s_max_len = (max_len > SCREEN_KB_MAX_LEN) ? SCREEN_KB_MAX_LEN : max_len;
    s_last_btn = 0;
    s_repeat_timer = 0;
    s_cursor_blink = 0;

    if (initial_text) {
        len = (int)lstrlenA(initial_text);
        if (len > s_max_len) len = s_max_len;
        memcpy(s_text, initial_text, len);
        s_text[len] = 0;
        s_text_len = len;
    }
    else {
        s_text[0] = 0;
        s_text_len = 0;
    }
}

void ScreenKB_Close(void) { s_open = 0; }
int  ScreenKB_IsOpen(void) { return s_open; }

void ScreenKB_GetText(char* buf, int buflen) {
    int n = s_text_len < buflen - 1 ? s_text_len : buflen - 1;
    memcpy(buf, s_text, n);
    buf[n] = 0;
}

int ScreenKB_Update(WORD wPressed) {
    WORD held;
    int moved = 0;

    if (!s_open) return 0;

    s_cursor_blink++;

    /* --- Button-held repeat logic --- */
    held = GetButtons();  /* from input.h */

    if (held == s_last_btn && held != 0) {
        s_repeat_timer++;
        if (s_repeat_timer < REPEAT_INITIAL) {
            /* Suppress held repeats until delay expires */
            wPressed = 0;
        }
        else if ((s_repeat_timer - REPEAT_INITIAL) % REPEAT_RATE != 0) {
            wPressed = 0;
        }
    }
    else {
        s_repeat_timer = 0;
        s_last_btn = held;
    }

    /* --- D-pad / stick navigation --- */
    if (wPressed & BTN_DPAD_UP) {
        s_cur_row--;
        if (s_cur_row < 0) s_cur_row = ROWS - 1;
        clamp_col();
        moved = 1;
    }
    if (wPressed & BTN_DPAD_DOWN) {
        s_cur_row = (s_cur_row + 1) % ROWS;
        clamp_col();
        moved = 1;
    }
    if (wPressed & BTN_DPAD_LEFT) {
        s_cur_col--;
        if (s_cur_col < 0) s_cur_col = row_key_count(s_cur_row) - 1;
        moved = 1;
    }
    if (wPressed & BTN_DPAD_RIGHT) {
        s_cur_col = (s_cur_col + 1) % row_key_count(s_cur_row);
        moved = 1;
    }

    /* --- Action buttons --- */

    /* A or Left Trigger: select current key */
    if (wPressed & (BTN_A | BTN_LTRIG)) {
        char action = k_rows[s_cur_row][s_cur_col].sets[s_keyset];
        if (action == SK_DONE) { s_open = 0; return  1; }
        if (action == SK_CANCEL) { s_open = 0; return -1; }
        do_type_key(s_cur_row, s_cur_col);
    }

    /* B: backspace */
    if (wPressed & BTN_B) {
        if (s_text_len > 0) {
            s_text_len--;
            s_text[s_text_len] = 0;
        }
    }

    /* X: cycle key set */
    if (wPressed & BTN_X) {
        s_keyset = (s_keyset + 1) % 3;
    }

    /* Y: space */
    if (wPressed & BTN_Y) {
        if (s_text_len < s_max_len) {
            s_text[s_text_len++] = ' ';
            s_text[s_text_len] = 0;
        }
    }

    /* Start: confirm */
    if (wPressed & BTN_START) { s_open = 0; return 1; }

    /* Back: cancel */
    if (wPressed & BTN_BACK) { s_open = 0; return -1; }

    (void)moved;
    return 0;
}

/* ── Drawing ──────────────────────────────────────────────────────────────── */

void ScreenKB_Draw(IDirect3DDevice8* pDevice) {
    int row, col, n;
    float kx, ky, kw;
    char label[4];
    D3DCOLOR key_bg, key_fg, key_border;
    const char* set_label;

    if (!s_open) return;

    /* Full-screen dark overlay (semi-transparent) */
    UI_DrawRect(pDevice, 0, 0, (float)UI_VIRT_W, (float)UI_VIRT_H,
        0xCC000000);

    /* Text input area background */
    UI_DrawRect(pDevice,
        KB_TEXT_X, KB_TEXT_Y, KB_TEXT_W, KB_TEXT_H,
        UI_COL_INPUT_BG);
    UI_DrawRectOutline(pDevice,
        KB_TEXT_X, KB_TEXT_Y, KB_TEXT_W, KB_TEXT_H,
        UI_COL_DIVIDER);

    /* Typed text */
    {
        char display[SCREEN_KB_MAX_LEN + 4];
        /* Show cursor blink */
        if ((s_cursor_blink / 20) % 2 == 0) {
            lstrcpyA(display, s_text);
            lstrcatA(display, "_");
        }
        else {
            lstrcpyA(display, s_text);
        }
        Font_DrawText(pDevice,
            KB_TEXT_X + 12.0f,
            KB_TEXT_Y + (KB_TEXT_H - Font_GlyphHeight(FONT_SIZE_MEDIUM)) * 0.5f,
            display,
            FONT_SIZE_MEDIUM,
            FONT_WHITE,
            Ftoi(KB_TEXT_W - 24.0f));
    }

    /* Key set indicator (bottom right of text box) */
    switch (s_keyset) {
    case 0:  set_label = "abc"; break;
    case 1:  set_label = "ABC"; break;
    default: set_label = "123"; break;
    }
    Font_DrawTextRight(pDevice,
        KB_TEXT_X + KB_TEXT_W - 8.0f,
        KB_TEXT_Y + 4.0f,
        set_label,
        FONT_SIZE_SMALL,
        UI_COL_TEXT_SEC);

    /* Draw all keys */
    for (row = 0; row < ROWS; row++) {
        n = row_key_count(row);
        ky = key_y(row);

        for (col = 0; col < n; col++) {
            const Key* k = &k_rows[row][col];
            char ch = k->sets[s_keyset];
            int is_selected = (row == s_cur_row && col == s_cur_col);

            key_rect(row, col, &kx, &kw);

            /* Key colours */
            if (is_selected) {
                key_bg = UI_COL_GREEN;
                key_fg = UI_COL_BLACK;
                key_border = UI_COL_GREEN;
            }
            else {
                switch (ch) {
                case SK_DONE:
                    key_bg = 0xFF1A3A1A;
                    key_fg = UI_COL_GREEN;
                    break;
                case SK_CANCEL:
                    key_bg = 0xFF3A1A1A;
                    key_fg = UI_COL_RED;
                    break;
                case SK_BKSP:
                case SK_SPACE:
                    key_bg = 0xFF1E1E1E;
                    key_fg = UI_COL_TEXT_SEC;
                    break;
                default:
                    key_bg = 0xFF1A1A1A;
                    key_fg = UI_COL_TEXT_PRI;
                    break;
                }
                key_border = UI_COL_DIVIDER;
            }

            /* Key background */
            UI_DrawRoundRect(pDevice, kx, ky, kw, KB_KEY_H, 6.0f, key_bg);
            UI_DrawRectOutline(pDevice, kx, ky, kw, KB_KEY_H, key_border);

            /* Key label */
            switch (ch) {
            case SK_SPACE:  label[0] = 'S'; label[1] = 'P'; label[2] = 'C'; label[3] = 0; break;
            case SK_BKSP:   label[0] = '<'; label[1] = 'X'; label[2] = 0; break;
            case SK_DONE:   label[0] = 'O'; label[1] = 'K'; label[2] = 0; break;
            case SK_CANCEL: label[0] = 'X'; label[1] = 0; break;
            default:
                label[0] = ch; label[1] = 0;
                break;
            }

            Font_DrawTextCentered(pDevice,
                kx,
                ky + (KB_KEY_H - Font_GlyphHeight(FONT_SIZE_MEDIUM)) * 0.5f,
                kw,
                label,
                FONT_SIZE_MEDIUM,
                key_fg);
        }
    }

    /* Bottom hint bar */
    Font_DrawTextCentered(pDevice,
        0, 700.0f, (float)UI_VIRT_W,
        "A=Type  B=Delete  X=Case  Y=Space  Start=Done  Back=Cancel",
        FONT_SIZE_SMALL,
        UI_COL_TEXT_MUTED);
}