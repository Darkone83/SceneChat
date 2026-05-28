#ifndef SCENECHAT_UI_H
#define SCENECHAT_UI_H
/*---------------------------------------------------------------------------
    ui.h -- SD/HD aware UI renderer for SceneChat.

    Designed at virtual 1280x720. Scale factors computed at init and applied
    transparently so all callers use virtual coordinates.

    SD (480p)  : scale 0.50 x, 0.667 y  ->  640x480
    HD (720p)  : scale 1.00 x, 1.000 y  -> 1280x720
    HD (1080i) : scale 1.50 x, 1.500 y  -> 1920x1080 (if ever needed)

    Colour constants match SceneChat branding:
        dark background, sidebar, green/purple accents, Discord-like layout.

    Call order:
        UI_Init(pDevice, screen_w, screen_h)
        UI_LoadLogo(pDevice)
        -- per frame --
        UI_DrawRect / UI_DrawLine / UI_DrawLogo / UI_DrawRoundRect ...
        UI_Shutdown()
---------------------------------------------------------------------------*/

#include <xtl.h>
#include <d3d8.h>

#ifdef __cplusplus
extern "C" {
#endif

    /* ── Float-to-int  (avoids __ftol2_sse on Xbox MSVC 2003) ────────────────── */
    static __inline int Ftoi(float f)
    {
        int r;
        __asm fld  f
        __asm fistp dword ptr r
        return r;
    }

    /* ── Colour palette  (D3DCOLOR = ARGB) ───────────────────────────────────── */
#define UI_COL_BG           0xFF0A0A0A   /* main background                  */
#define UI_COL_SIDEBAR      0xFF111111   /* left sidebar                     */
#define UI_COL_HEADER       0xFF111111   /* top header bar                   */
#define UI_COL_INPUT_BG     0xFF1A1A1A   /* message input box                */
#define UI_COL_DIVIDER      0xFF222222   /* separator lines                  */
#define UI_COL_HOVER        0xFF1A1A1A   /* list item hover                  */
#define UI_COL_SELECTED     0xFF1E1E1E   /* active room highlight            */
#define UI_COL_GREEN        0xFF39FF14   /* SceneChat accent green           */
#define UI_COL_PURPLE       0xFF8B5CF6   /* SceneChat accent purple          */
#define UI_COL_TEXT_PRI     0xFFE0E0E0   /* primary text                     */
#define UI_COL_TEXT_SEC     0xFF888888   /* secondary / timestamp text       */
#define UI_COL_TEXT_MUTED   0xFF555555   /* muted / placeholder text         */
#define UI_COL_WHITE        0xFFFFFFFF
#define UI_COL_BLACK        0xFF000000
#define UI_COL_RED          0xFFDC3232   /* error / banned indicator         */
#define UI_COL_ONLINE       0xFF3BA55C   /* online dot (Discord green)       */

/* ── Virtual layout constants (1280x720 base) ────────────────────────────── */
#define UI_VIRT_W           1280
#define UI_VIRT_H           720

#define UI_HEADER_H         48           /* top bar height                   */
#define UI_SIDEBAR_W        240          /* left room list panel             */
#define UI_INPUT_H          56           /* message input bar height         */
#define UI_SIDEBAR_ITEM_H   44           /* height of each room list entry   */
#define UI_ACTIVE_BORDER    3            /* left accent border on active item*/
#define UI_PADDING          16           /* general inner padding            */
#define UI_AVATAR_SIZE      36           /* username avatar circle diameter  */
#define UI_SCROLLBAR_W      6            /* thin scrollbar                   */

/* ── Init / shutdown ─────────────────────────────────────────────────────── */
    int  UI_Init(IDirect3DDevice8* pDevice, int screen_w, int screen_h);
    int  UI_LoadLogo(IDirect3DDevice8* pDevice);   /* uploads logo.h texture   */
    void UI_Shutdown(void);

    /* ── Scale helpers ───────────────────────────────────────────────────────── */
    /* Convert virtual coords -> real screen pixels */
    float UI_Sx(float virt_x);  /* scale X */
    float UI_Sy(float virt_y);  /* scale Y */
    int   UI_ScreenW(void);
    int   UI_ScreenH(void);
    int   UI_IsHD(void);      /* 1 if 720p or above                         */

    /* ── Primitive drawing (all coords are VIRTUAL 1280x720) ─────────────────── */

    /* Filled rectangle */
    void UI_DrawRect(IDirect3DDevice8* pDevice,
        float x, float y, float w, float h,
        D3DCOLOR colour);

    /* Rectangle outline (1 pixel border scaled) */
    void UI_DrawRectOutline(IDirect3DDevice8* pDevice,
        float x, float y, float w, float h,
        D3DCOLOR colour);

    /* Filled rectangle with a coloured left-side accent border (for active items) */
    void UI_DrawRectAccent(IDirect3DDevice8* pDevice,
        float x, float y, float w, float h,
        D3DCOLOR bg_colour, D3DCOLOR accent_colour);

    /* Horizontal or vertical line */
    void UI_DrawLine(IDirect3DDevice8* pDevice,
        float x1, float y1, float x2, float y2,
        D3DCOLOR colour);

    /* Approximated rounded rectangle (corner segments, 4-step arc) */
    void UI_DrawRoundRect(IDirect3DDevice8* pDevice,
        float x, float y, float w, float h,
        float radius, D3DCOLOR colour);

    /* Small filled circle (for online indicator dots, avatars) */
    void UI_DrawCircle(IDirect3DDevice8* pDevice,
        float cx, float cy, float r,
        D3DCOLOR colour);

    /* ── Logo rendering ──────────────────────────────────────────────────────── */
    /* Draws the SceneChat logo texture at virtual coords, scaled to fit w x h   */
    void UI_DrawLogo(IDirect3DDevice8* pDevice,
        float x, float y, float w, float h);

    /* ── Scrollbar ───────────────────────────────────────────────────────────── */
    /* Draws a thin right-edge scrollbar.
       total_items   = total number of items in the list
       visible_items = how many fit on screen
       scroll_pos    = current top item index */
    void UI_DrawScrollbar(IDirect3DDevice8* pDevice,
        float x, float y, float h,
        int total_items, int visible_items, int scroll_pos);

    /* ── Layout helpers ──────────────────────────────────────────────────────── */
    /* Main chat area origin (right of sidebar, below header) */
    float UI_ChatX(void);
    float UI_ChatY(void);
    float UI_ChatW(void);
    float UI_ChatH(void);   /* full height minus header and input bar        */

    /* Input bar rect */
    float UI_InputX(void);
    float UI_InputY(void);
    float UI_InputW(void);
    float UI_InputH(void);

#ifdef __cplusplus
}
#endif

#endif /* SCENECHAT_UI_H */