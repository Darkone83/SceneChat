/*---------------------------------------------------------------------------
    chat.cpp -- Main chat screen for SceneChat.

    Layout uses virtual 1280x720 coords (ui.h scale system):
        Header bar   y=0..48
        Sidebar      x=0..240   y=48..720
        Chat area    x=240..1280 y=48..664
        Input bar    x=240..1280 y=664..720

    Navigation:
        Left stick         -- analog cursor
        Left trigger       -- click (join room / focus input)
        D-pad up/down      -- scroll messages (focus=chat) / select room (focus=sidebar)
        D-pad left/right   -- switch focus between sidebar and chat
        A                  -- same as left trigger
        B / Back           -- clear input / back
        Start              -- send message
        HID keyboard       -- types directly into input bar
---------------------------------------------------------------------------*/

#include <xtl.h>
#include "chat.h"
#include "emoji.h"
#include "sc_net.h"
#include "ui.h"
#include "font.h"
#include "hid.h"
#include "screen_kb.h"
#include "input.h"
#include "voice.h"
#include <string.h>
#include <math.h>

/*    Per-room message cache                                                   */

typedef struct {
    SC_Message  msgs[CHAT_MSG_BUF];
    int         count;
    int         scroll;    /* index of first visible message (from bottom=0) */
} RoomCache;

/*    Internal state                                                           */

static SC_Room      s_rooms[SC_MAX_ROOMS];
static int          s_room_count = 0;
static int          s_cur_room = -1;   /* index into s_rooms              */
static int          s_joined_id = 0xFF; /* room id currently joined        */
static int          s_joining = 0;    /* waiting for ROOM_INFO response  */
static int          s_listpending = 0;    /* waiting for ROOM_LIST response  */

/* Single message cache for the active room */
static RoomCache    s_msgcache;

static char         s_input[SC_MAX_MSGLEN + 1] = { 0 };
static int          s_input_len = 0;

static char         s_my_user[SC_MAX_USERNAME] = { 0 };

/* Focus: 0 = sidebar, 1 = chat/input */
static int          s_focus = 0;
static int          s_sidebar_sel = 0;   /* highlighted room in sidebar      */

/* Analog cursor (virtual coords) */
static float        s_cx = 120.0f;   /* starts in sidebar centre    */
static float        s_cy = 360.0f;

/* Ping keepalive */
static int          s_ping_timer = 0;

/* Notification: flash a room index when a message arrives in non-active room */
static int          s_notify_room = -1;
static int          s_notify_timer = 0;

/* Error / status message */
static char         s_status[96] = { 0 };
static int          s_status_timer = 0;

static int          s_frame = 0;

/* Voice state */
static char          s_server_ip[64] = { 0 };
static unsigned int  s_user_id_voice = 0;
static int           s_in_voice_room = 0;

/*    Helpers                                                                  */

static void set_status(const char* msg) {
    lstrcpynA(s_status, msg, sizeof(s_status));
    s_status_timer = 180;   /* show for ~3 seconds                           */
}

static void cache_append(const SC_Message* msg) {
    if (s_msgcache.count < CHAT_MSG_BUF) {
        s_msgcache.msgs[s_msgcache.count++] = *msg;
    }
    else {
        /* Shift buffer -- drop oldest */
        int i;
        for (i = 0; i < CHAT_MSG_BUF - 1; i++)
            s_msgcache.msgs[i] = s_msgcache.msgs[i + 1];
        s_msgcache.msgs[CHAT_MSG_BUF - 1] = *msg;
    }
    /* Auto-scroll to bottom if already at bottom */
    if (s_msgcache.scroll == 0) s_msgcache.scroll = 0;
}

static void cache_clear(void) {
    s_msgcache.count = 0;
    s_msgcache.scroll = 0;
}

/* Visible message area in virtual coords */
static float chat_area_y(void) { return (float)UI_HEADER_H; }
static float chat_area_h(void) {
    return (float)(UI_VIRT_H - UI_HEADER_H - UI_INPUT_H);
}

static int visible_msg_count(void) {
    return Ftoi(chat_area_h() / CHAT_MSG_LINE_H);
}

/* Room index in s_rooms by room_id -- returns -1 if not found */
static int room_idx_by_id(unsigned char id) {
    int i;
    for (i = 0; i < s_room_count; i++)
        if (s_rooms[i].id == id) return i;
    return -1;
}

/*    Join a room by s_rooms index                                             */

static void do_join_room(int idx) {
    if (idx < 0 || idx >= s_room_count) return;
    if (s_rooms[idx].id == s_joined_id) return;
    cache_clear();
    s_cur_room = idx;
    s_joining = 1;
    s_joined_id = s_rooms[idx].id;
    SC_Net_SendJoinRoom(s_rooms[idx].id);

    /* Voice room handling */
    if (s_in_voice_room) {
        Voice_Disconnect();
        s_in_voice_room = 0;
    }
    if (s_rooms[idx].type == 1) {
        /* Joining a voice room -- connect UDP voice */
        Voice_Connect(s_server_ip, s_user_id_voice, s_rooms[idx].id);
        s_in_voice_room = 1;
    }
}

/*    Send the current input buffer                                           */

static void do_send_message(void) {
    if (s_input_len == 0 || s_cur_room < 0) return;
    if (!SC_Net_IsReady()) { set_status("Not connected."); return; }

    SC_Net_SendMessage(s_rooms[s_cur_room].id, s_input);
    s_input[0] = 0;
    s_input_len = 0;
    HID_ClearQueue();
}

/*    Cursor hit-test helpers                                                  */

/* Returns sidebar room index under cursor, or -1 */
static int cursor_hit_room(void) {
    int i;
    float ry;
    if (s_cx > (float)UI_SIDEBAR_W) return -1;
    for (i = 0; i < s_room_count; i++) {
        ry = (float)(UI_HEADER_H + 32 + i * UI_SIDEBAR_ITEM_H);
        if (s_cy >= ry && s_cy < ry + UI_SIDEBAR_ITEM_H) return i;
    }
    return -1;
}

/* Returns 1 if cursor is over the input bar */
static int cursor_hit_input(void) {
    return (s_cx > (float)UI_SIDEBAR_W &&
        s_cy > (float)(UI_VIRT_H - UI_INPUT_H));
}

/*    Public API                                                               */

void Chat_Init(IDirect3DDevice8* pDevice, const char* username,
    const char* server_ip, unsigned int user_id) {
    (void)pDevice;
    lstrcpynA(s_my_user, username, sizeof(s_my_user));
    lstrcpynA(s_server_ip, server_ip ? server_ip : "", sizeof(s_server_ip));
    s_user_id_voice = user_id;
    s_in_voice_room = 0;
    s_room_count = 0;
    s_cur_room = -1;
    s_joined_id = 0xFF;
    s_joining = 0;
    s_listpending = 0;
    s_focus = 0;
    s_sidebar_sel = 0;
    s_cx = 120.0f;
    s_cy = 360.0f;
    s_ping_timer = 0;
    s_notify_room = -1;
    s_input[0] = 0;
    s_input_len = 0;
    s_status[0] = 0;
    s_frame = 0;
    cache_clear();

    HID_Init();

    /* Request room list immediately */
    SC_Net_SendRoomList();
    s_listpending = 1;
}

void Chat_Shutdown(void) {
    HID_Shutdown();
    ScreenKB_Close();
}

/*    Update                                                                   */

int Chat_Update(WORD wPressed) {
    SC_RoomInfo  room_info;
    SC_Message   msg;
    char         err[96];
    int          lx, ly, rx, ry_stick;
    int          hit_room, kb_result;
    char         ch;
    int          spec;

    s_frame++;

    /*    Voice frame update    */
    Voice_Update(wPressed);

    /*    On-screen keyboard overlay    */
    if (ScreenKB_IsOpen()) {
        kb_result = ScreenKB_Update(wPressed);
        if (kb_result != 0) {
            char tmp[SC_MAX_MSGLEN + 1];
            ScreenKB_GetText(tmp, sizeof(tmp));
            lstrcpynA(s_input, tmp, sizeof(s_input));
            s_input_len = (int)lstrlenA(s_input);
            if (kb_result == 1) do_send_message();
        }
        return 0;
    }

    /*    Network poll    */
    SC_Net_Poll();

    /* Ping keepalive */
    s_ping_timer++;
    if (s_ping_timer >= CHAT_PING_FRAMES) {
        SC_Net_SendPing();
        s_ping_timer = 0;
    }

    /* Connection lost */
    if (!SC_Net_IsReady()) {
        set_status("Connection lost.");
    }

    /* Consume room list */
    if (s_listpending) {
        if (SC_Net_RecvRoomList(s_rooms, &s_room_count)) {
            s_listpending = 0;
            /* Auto-join first text room */
            if (s_cur_room < 0 && s_room_count > 0) {
                int i;
                for (i = 0; i < s_room_count; i++) {
                    if (s_rooms[i].type == 0) { do_join_room(i); break; }
                }
            }
        }
    }

    /* Consume room info (join response + history) */
    if (s_joining) {
        if (SC_Net_RecvRoomInfo(&room_info)) {
            int i;
            s_joining = 0;
            cache_clear();
            for (i = 0; i < room_info.history_count; i++)
                cache_append(&room_info.history[i]);
        }
    }

    /* Consume incoming messages */
    while (SC_Net_RecvMessage(&msg)) {
        int ridx = room_idx_by_id(msg.room_id);
        if (ridx == s_cur_room) {
            cache_append(&msg);
        }
        else if (ridx >= 0) {
            /* Notification for other room */
            s_notify_room = ridx;
            s_notify_timer = 120;
        }
    }

    /* Consume error */
    if (SC_Net_RecvError(err, sizeof(err))) {
        set_status(err);
    }

    /* Status timer */
    if (s_status_timer > 0) s_status_timer--;

    /* Notification timer */
    if (s_notify_timer > 0) s_notify_timer--;
    else s_notify_room = -1;

    /*    Analog cursor movement    */
    GetSticks(lx, ly, rx, ry_stick);
    ly = -ly;        /* Xbox reports up=positive; invert for UI */
    ry_stick = -ry_stick;

    if (lx > CHAT_DEADZONE || lx < -CHAT_DEADZONE)
        s_cx += (float)lx / 32768.0f * CHAT_CURSOR_SPEED;
    if (ly > CHAT_DEADZONE || ly < -CHAT_DEADZONE)
        s_cy += (float)ly / 32768.0f * CHAT_CURSOR_SPEED;

    /* Clamp cursor to screen */
    if (s_cx < 0.0f)              s_cx = 0.0f;
    if (s_cx > (float)UI_VIRT_W)  s_cx = (float)UI_VIRT_W;
    if (s_cy < 0.0f)              s_cy = 0.0f;
    if (s_cy > (float)UI_VIRT_H)  s_cy = (float)UI_VIRT_H;

    /* Message scroll with right stick Y */
    if (ry_stick > CHAT_DEADZONE) {
        s_msgcache.scroll++;
        int max_scroll = s_msgcache.count - visible_msg_count();
        if (max_scroll < 0) max_scroll = 0;
        if (s_msgcache.scroll > max_scroll) s_msgcache.scroll = max_scroll;
    }
    else if (ry_stick < -CHAT_DEADZONE) {
        s_msgcache.scroll--;
        if (s_msgcache.scroll < 0) s_msgcache.scroll = 0;
    }

    /*    D-pad navigation    */
    if (wPressed & BTN_DPAD_LEFT)  s_focus = 0;
    if (wPressed & BTN_DPAD_RIGHT) s_focus = 1;

    if (s_focus == 0) {
        /* Sidebar focus: navigate rooms */
        if (wPressed & BTN_DPAD_UP) {
            s_sidebar_sel--;
            if (s_sidebar_sel < 0) s_sidebar_sel = 0;
        }
        if (wPressed & BTN_DPAD_DOWN) {
            s_sidebar_sel++;
            if (s_sidebar_sel >= s_room_count) s_sidebar_sel = s_room_count - 1;
        }
    }
    else {
        /* Chat focus: scroll messages */
        if (wPressed & BTN_DPAD_UP) {
            s_msgcache.scroll++;
            int max_scroll = s_msgcache.count - visible_msg_count();
            if (max_scroll < 0) max_scroll = 0;
            if (s_msgcache.scroll > max_scroll) s_msgcache.scroll = max_scroll;
        }
        if (wPressed & BTN_DPAD_DOWN) {
            s_msgcache.scroll--;
            if (s_msgcache.scroll < 0) s_msgcache.scroll = 0;
        }
    }

    /*    Click (Left Trigger or A)    */
    if (wPressed & (BTN_LTRIG | BTN_A)) {
        /* Cursor-based click */
        hit_room = cursor_hit_room();
        if (hit_room >= 0) {
            s_sidebar_sel = hit_room;
            do_join_room(hit_room);
            s_focus = 1;
        }
        else if (cursor_hit_input()) {
            s_focus = 1;
            if (!HID_IsPresent()) {
                ScreenKB_Open(s_input, SC_MAX_MSGLEN);
            }
        }
        else if (s_focus == 0 && s_room_count > 0) {
            /* D-pad mode: join highlighted room */
            do_join_room(s_sidebar_sel);
            s_focus = 1;
        }
    }

    /*    HID keyboard input -- poll unconditionally, IsPresent only
         gates the UI hint. Gating Poll on IsPresent was chicken-and-egg:
         presence is set on first keystroke, but poll never ran to get it. */
    HID_Poll();
    while ((ch = HID_GetChar()) != 0) {
        if (s_input_len < SC_MAX_MSGLEN) {
            s_input[s_input_len++] = ch;
            s_input[s_input_len] = 0;
        }
    }
    while ((spec = HID_GetSpecial()) != HID_KEY_NONE) {
        switch (spec) {
        case HID_KEY_BACKSPACE:
            if (s_input_len > 0) s_input[--s_input_len] = 0;
            break;
        case HID_KEY_ENTER:
            do_send_message();
            break;
        case HID_KEY_ESCAPE:
            s_input[0] = 0; s_input_len = 0;
            break;
        default: break;
        }
    }

    /*    Start: send message    */
    if (wPressed & BTN_START) {
        if (s_input_len > 0) do_send_message();
    }

    /*    B: clear input or back    */
    if (wPressed & BTN_B) {
        if (s_input_len > 0) {
            s_input[0] = 0; s_input_len = 0;
        }
    }

    /*    Back: logout / quit    */
    if (wPressed & BTN_BACK) return -1;

    /*    Y: open on-screen keyboard    */
    if (wPressed & BTN_Y) {
        if (!HID_IsPresent()) {
            ScreenKB_Open(s_input, SC_MAX_MSGLEN);
        }
    }

    return 0;
}

/*    Drawing                                                                  */

/* Measure + render a message line, wrapping content if needed.
   Returns total pixel height consumed. */
static float draw_message(IDirect3DDevice8* pDevice,
    float x, float y, float max_w,
    const SC_Message* msg) {
    char prefix[SC_MAX_USERNAME + 12];
    int prefix_w;
    D3DCOLOR user_col;
    float cy = y;

    /* Username colour: green for self, purple for others */
    user_col = (lstrcmpA(msg->username, s_my_user) == 0)
        ? UI_COL_GREEN : UI_COL_PURPLE;

    /* "[HH:MM] Username" prefix */
    wsprintf(prefix, "[%s] %s", msg->timestamp, msg->username);
    prefix_w = Font_MeasureText(prefix, FONT_SIZE_SMALL);

    Font_DrawText(pDevice, x, cy, prefix,
        FONT_SIZE_SMALL, user_col, 0);

    /* Message content -- may need to wrap */
    {
        const char* p = msg->content;
        float        cx = x + prefix_w + 6.0f;
        float        cw = max_w - (cx - x);
        int          line_w;
        char         line[SC_MAX_MSGLEN + 1];
        int          llen = 0;

        while (*p) {
            char c = *p++;
            line[llen++] = c;
            line[llen] = 0;

            line_w = Font_MeasureText(line, FONT_SIZE_SMALL);

            if (line_w > (int)cw || *p == 0) {
                /* If we overflowed, back up one char and emit */
                if (line_w > (int)cw && llen > 1) {
                    p--;
                    line[--llen] = 0;
                }
                Emoji_DrawMixed(pDevice, cx, cy, line,
                    FONT_SIZE_SMALL, UI_COL_TEXT_PRI,
                    (int)cw);
                cy += (float)CHAT_MSG_LINE_H;
                cx = x;
                cw = max_w;
                llen = 0;
                line[0] = 0;
            }
        }
        if (llen > 0) cy += (float)CHAT_MSG_LINE_H;
    }

    return cy - y;
}

void Chat_Draw(IDirect3DDevice8* pDevice) {
    int i;
    char room_label[SC_MAX_USERNAME + 4];
    float ry;
    D3DCOLOR item_bg, item_fg;
    int is_active, is_sel, is_notify;

    /*    Full background    */
    UI_DrawRect(pDevice, 0, 0, (float)UI_VIRT_W, (float)UI_VIRT_H,
        UI_COL_BG);

    /*    Header bar    */
    UI_DrawRect(pDevice, 0, 0, (float)UI_VIRT_W, (float)UI_HEADER_H,
        UI_COL_HEADER);
    UI_DrawLine(pDevice,
        0, (float)UI_HEADER_H,
        (float)UI_VIRT_W, (float)UI_HEADER_H,
        UI_COL_DIVIDER);

    /* Logo small in header */
    UI_DrawLogo(pDevice, 8.0f, 6.0f, 36.0f, 36.0f);

    /* App name */
    Font_DrawText(pDevice, 52.0f, 14.0f, "SceneChat",
        FONT_SIZE_MEDIUM, UI_COL_GREEN, 0);

    /* Logged-in username top right */
    Font_DrawTextRight(pDevice,
        (float)UI_VIRT_W - 12.0f,
        14.0f, s_my_user,
        FONT_SIZE_MEDIUM, UI_COL_TEXT_SEC);

    /* Connection status dot */
    {
        D3DCOLOR dot = SC_Net_IsReady() ? UI_COL_ONLINE : UI_COL_RED;
        UI_DrawCircle(pDevice,
            (float)UI_VIRT_W - Font_MeasureText(s_my_user, FONT_SIZE_MEDIUM) - 24.0f,
            24.0f, 5.0f, dot);
    }

    /*    Sidebar    */
    UI_DrawRect(pDevice,
        0, (float)UI_HEADER_H,
        (float)UI_SIDEBAR_W,
        (float)(UI_VIRT_H - UI_HEADER_H),
        UI_COL_SIDEBAR);
    UI_DrawLine(pDevice,
        (float)UI_SIDEBAR_W, (float)UI_HEADER_H,
        (float)UI_SIDEBAR_W, (float)UI_VIRT_H,
        UI_COL_DIVIDER);

    /* Room list section label */
    Font_DrawText(pDevice,
        (float)UI_PADDING,
        (float)(UI_HEADER_H + 10),
        "ROOMS",
        FONT_SIZE_SMALL, UI_COL_TEXT_MUTED, 0);

    /* Room list items */
    for (i = 0; i < s_room_count; i++) {
        ry = (float)(UI_HEADER_H + 32 + i * UI_SIDEBAR_ITEM_H);

        is_active = (i == s_cur_room);
        is_sel = (i == s_sidebar_sel && s_focus == 0);
        is_notify = (i == s_notify_room && (s_frame / 6) % 2 == 0);

        if (is_active) {
            UI_DrawRectAccent(pDevice,
                0, ry,
                (float)UI_SIDEBAR_W, (float)UI_SIDEBAR_ITEM_H,
                UI_COL_SELECTED, UI_COL_GREEN);
            item_fg = UI_COL_TEXT_PRI;
        }
        else if (is_sel) {
            UI_DrawRect(pDevice, 0, ry,
                (float)UI_SIDEBAR_W, (float)UI_SIDEBAR_ITEM_H,
                UI_COL_HOVER);
            item_fg = UI_COL_TEXT_PRI;
        }
        else {
            item_fg = is_notify ? UI_COL_GREEN : UI_COL_TEXT_SEC;
        }

        /* Room icon: # for text, speaker symbol approximation for voice */
        wsprintf(room_label, "%s%s",
            s_rooms[i].type == 1 ? "o " : "# ",
            s_rooms[i].name);

        Font_DrawText(pDevice,
            (float)(UI_PADDING + UI_ACTIVE_BORDER + 4),
            ry + (UI_SIDEBAR_ITEM_H - Font_GlyphHeight(FONT_SIZE_MEDIUM)) * 0.5f,
            room_label,
            FONT_SIZE_MEDIUM, item_fg,
            UI_SIDEBAR_W - UI_PADDING * 2);
    }

    /*    Chat area    */
    {
        float cx0 = (float)UI_SIDEBAR_W + 4.0f;
        float cy0 = (float)UI_HEADER_H + 4.0f;
        float cw = (float)(UI_VIRT_W - UI_SIDEBAR_W) - 8.0f;
        float ch = chat_area_h() - 8.0f;
        int   vis = visible_msg_count();
        int   start = s_msgcache.count - vis - s_msgcache.scroll;
        float my = cy0 + ch;   /* render from bottom up */
        int   n, idx;
        float line_h;

        if (start < 0) start = 0;
        n = s_msgcache.count - start;
        if (n > vis) n = vis;

        /* Render messages bottom-up so newest is at the bottom */
        for (idx = start + n - 1; idx >= start; idx--) {
            const SC_Message* m = &s_msgcache.msgs[idx];
            line_h = (float)CHAT_MSG_LINE_H;
            my -= line_h;
            if (my < cy0) break;
            draw_message(pDevice, cx0, my, cw, m);
        }

        /* Room name heading */
        if (s_cur_room >= 0) {
            char heading[SC_MAX_USERNAME + 4];
            wsprintf(heading, "%s%s",
                s_rooms[s_cur_room].type == 1 ? "o " : "# ",
                s_rooms[s_cur_room].name);
            Font_DrawText(pDevice,
                (float)(UI_SIDEBAR_W + UI_PADDING),
                (float)(UI_HEADER_H + 4),
                heading,
                FONT_SIZE_MEDIUM, UI_COL_TEXT_PRI, 0);
        }

        /* Scrollbar */
        if (s_msgcache.count > vis) {
            UI_DrawScrollbar(pDevice,
                (float)(UI_VIRT_W - UI_SCROLLBAR_W - 2),
                cy0, ch,
                s_msgcache.count, vis, s_msgcache.scroll);
        }

        /* Joining indicator */
        if (s_joining) {
            Font_DrawTextCentered(pDevice,
                (float)UI_SIDEBAR_W, cy0 + ch * 0.5f,
                (float)(UI_VIRT_W - UI_SIDEBAR_W),
                "Joining...",
                FONT_SIZE_MEDIUM, UI_COL_TEXT_SEC);
        }
    }

    /*    Input bar    */
    {
        float ix = UI_InputX();
        float iy = UI_InputY();
        float iw = UI_InputW();
        float ih = UI_InputH();
        D3DCOLOR input_border = (s_focus == 1) ? UI_COL_GREEN : UI_COL_DIVIDER;
        char display[SC_MAX_MSGLEN + 4];

        UI_DrawRect(pDevice, (float)UI_SIDEBAR_W, iy, (float)(UI_VIRT_W - UI_SIDEBAR_W), ih, UI_COL_HEADER);
        UI_DrawLine(pDevice, (float)UI_SIDEBAR_W, iy, (float)UI_VIRT_W, iy, UI_COL_DIVIDER);
        UI_DrawRect(pDevice, ix, iy + 8.0f, iw, ih - 16.0f, UI_COL_INPUT_BG);
        UI_DrawRectOutline(pDevice, ix, iy + 8.0f, iw, ih - 16.0f, input_border);

        /* Input text + cursor blink */
        lstrcpynA(display, s_input, sizeof(display));
        if ((s_frame / 20) % 2 == 0 && s_focus == 1) {
            int dl = (int)lstrlenA(display);
            if (dl < SC_MAX_MSGLEN) { display[dl] = '_'; display[dl + 1] = 0; }
        }

        if (s_input_len == 0 && s_focus != 1) {
            Font_DrawText(pDevice,
                ix + 12.0f,
                iy + 8.0f + (ih - 16.0f - Font_GlyphHeight(FONT_SIZE_MEDIUM)) * 0.5f,
                s_cur_room >= 0 ? "Message..." : "Join a room to chat",
                FONT_SIZE_MEDIUM, UI_COL_TEXT_MUTED, Ftoi(iw - 24.0f));
        }
        else {
            Font_DrawText(pDevice,
                ix + 12.0f,
                iy + 8.0f + (ih - 16.0f - Font_GlyphHeight(FONT_SIZE_MEDIUM)) * 0.5f,
                display,
                FONT_SIZE_MEDIUM, UI_COL_TEXT_PRI, Ftoi(iw - 24.0f));
        }

        /* Hint: sits at right edge of input box, not full-screen right */
        Font_DrawTextRight(pDevice,
            ix + iw - 8.0f,
            iy + 8.0f + (ih - 16.0f - Font_GlyphHeight(FONT_SIZE_SMALL)) * 0.5f,
            HID_IsPresent() ? "Enter=Send" : "Y=Keyboard  Start=Send",
            FONT_SIZE_SMALL, UI_COL_TEXT_MUTED);
    }

    /*    Status / error message    */
    if (s_status_timer > 0 && s_status[0]) {
        Font_DrawTextCentered(pDevice,
            (float)UI_SIDEBAR_W,
            (float)(UI_VIRT_H - UI_INPUT_H - 28),
            (float)(UI_VIRT_W - UI_SIDEBAR_W),
            s_status,
            FONT_SIZE_SMALL, UI_COL_RED);
    }

    /*    Analog cursor    */
    {
        /* Simple crosshair cursor */
        float cs = 8.0f;
        UI_DrawLine(pDevice, s_cx - cs, s_cy, s_cx + cs, s_cy, UI_COL_WHITE);
        UI_DrawLine(pDevice, s_cx, s_cy - cs, s_cx, s_cy + cs, UI_COL_WHITE);
        UI_DrawCircle(pDevice, s_cx, s_cy, 3.0f, UI_COL_GREEN);
    }

    /*    Voice HUD (mic/mute indicators)    */
    if (s_in_voice_room)
        Voice_DrawHUD(pDevice);

    /*    On-screen keyboard overlay    */
    if (ScreenKB_IsOpen()) {
        ScreenKB_Draw(pDevice);
    }
}