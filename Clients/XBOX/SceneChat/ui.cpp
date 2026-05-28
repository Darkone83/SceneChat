/*---------------------------------------------------------------------------
    ui.cpp -- SD/HD aware UI renderer for SceneChat.

    All drawing uses D3DPT_TRIANGLELIST / TRIANGLESTRIP with XYZRHW vertices
    (pre-transformed screen space -- no projection matrix needed).
    Virtual 1280x720 coords are scaled to real screen pixels at draw time.
---------------------------------------------------------------------------*/

#include <xtl.h>
#include "ui.h"
#include "logo.h"
#include <math.h>
#include <string.h>
#include <XGraphics.h>

/* ── Internal state ───────────────────────────────────────────────────────── */

static int   s_screen_w = 1280;
static int   s_screen_h = 720;
static float s_scale_x = 1.0f;
static float s_scale_y = 1.0f;
static int   s_is_hd = 1;

static IDirect3DTexture8* s_logo_tex = NULL;

/* ── Vertex format ────────────────────────────────────────────────────────── */

#define UI_FVF_COL  (D3DFVF_XYZRHW | D3DFVF_DIFFUSE)
#define UI_FVF_TEX  (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1)

typedef struct { float x, y, z, rhw; DWORD col; }           UIVert;
typedef struct { float x, y, z, rhw; DWORD col; float u, v; } UIVertTex;

/* ── Math helpers ─────────────────────────────────────────────────────────── */

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

/* Fast integer cos/sin approximation not needed -- use CRT floats */

/* ── Init / Shutdown ──────────────────────────────────────────────────────── */

int UI_Init(IDirect3DDevice8* pDevice, int screen_w, int screen_h) {
    s_screen_w = screen_w;
    s_screen_h = screen_h;
    s_scale_x = (float)screen_w / (float)UI_VIRT_W;
    s_scale_y = (float)screen_h / (float)UI_VIRT_H;
    s_is_hd = (screen_h >= 720) ? 1 : 0;
    (void)pDevice;
    return 1;
}

int UI_LoadLogo(IDirect3DDevice8* pDevice) {
    IDirect3DTexture8* tex = NULL;
    D3DLOCKED_RECT lr;
    HRESULT hr;

    if (s_logo_tex) return 1;

    /* D3DFMT_A8R8G8B8 swizzled + XGSwizzleRect -- same pattern as XbDiag/ScorchedEarthXB.
       gen_logo.py outputs BGRA, correct byte order for XGSwizzleRect. */
    hr = pDevice->CreateTexture(
        LOGO_WIDTH, LOGO_HEIGHT, 1,
        0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex);
    if (FAILED(hr)) return 0;

    hr = tex->LockRect(0, &lr, NULL, 0);
    if (FAILED(hr)) { tex->Release(); return 0; }

    XGSwizzleRect(g_logoData, LOGO_WIDTH * 4, NULL,
        lr.pBits, LOGO_WIDTH, LOGO_HEIGHT, NULL, 4);

    tex->UnlockRect(0);
    s_logo_tex = tex;
    return 1;
}

void UI_Shutdown(void) {
    if (s_logo_tex) { s_logo_tex->Release(); s_logo_tex = NULL; }
}

/* ── Scale helpers ────────────────────────────────────────────────────────── */

float UI_Sx(float vx) { return vx * s_scale_x; }
float UI_Sy(float vy) { return vy * s_scale_y; }
int   UI_ScreenW(void) { return s_screen_w; }
int   UI_ScreenH(void) { return s_screen_h; }
int   UI_IsHD(void) { return s_is_hd; }

/* ── Render state helpers ─────────────────────────────────────────────────── */

static void set_colour_states(IDirect3DDevice8* pDevice) {
    pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    pDevice->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    pDevice->SetTexture(0, NULL);
    pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    pDevice->SetRenderState(D3DRS_ZENABLE, FALSE);
    pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);
    pDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    pDevice->SetVertexShader(UI_FVF_COL);
}

static void restore_states(IDirect3DDevice8* pDevice) {
    pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    pDevice->SetRenderState(D3DRS_ZENABLE, TRUE);
}

/* ── UIVert helper ────────────────────────────────────────────────────────── */

static void uiv(UIVert* v, float x, float y, DWORD col) {
    v->x = x; v->y = y; v->z = 0.0f; v->rhw = 1.0f; v->col = col;
}

/* ── Primitive drawing ────────────────────────────────────────────────────── */

void UI_DrawRect(IDirect3DDevice8* pDevice,
    float x, float y, float w, float h,
    D3DCOLOR colour) {
    UIVert v[4];
    float rx = UI_Sx(x), ry = UI_Sy(y);
    float rw = UI_Sx(x + w) - rx, rh = UI_Sy(y + h) - ry;

    set_colour_states(pDevice);
    uiv(&v[0], rx, ry, colour);
    uiv(&v[1], rx + rw, ry, colour);
    uiv(&v[2], rx, ry + rh, colour);
    uiv(&v[3], rx + rw, ry + rh, colour);
    pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(UIVert));
    restore_states(pDevice);
}

void UI_DrawLine(IDirect3DDevice8* pDevice,
    float x1, float y1, float x2, float y2,
    D3DCOLOR colour) {
    UIVert v[2];
    set_colour_states(pDevice);
    uiv(&v[0], UI_Sx(x1), UI_Sy(y1), colour);
    uiv(&v[1], UI_Sx(x2), UI_Sy(y2), colour);
    pDevice->DrawPrimitiveUP(D3DPT_LINELIST, 1, v, sizeof(UIVert));
    restore_states(pDevice);
}

void UI_DrawRectOutline(IDirect3DDevice8* pDevice,
    float x, float y, float w, float h,
    D3DCOLOR colour) {
    float t = (s_is_hd) ? 1.0f : 0.5f;
    /* Draw as 4 filled thin rects for clean scaling */
    UI_DrawRect(pDevice, x, y, w, t, colour); /* top    */
    UI_DrawRect(pDevice, x, y + h - t, w, t, colour); /* bottom */
    UI_DrawRect(pDevice, x, y, t, h, colour); /* left   */
    UI_DrawRect(pDevice, x + w - t, y, t, h, colour); /* right  */
}

void UI_DrawRectAccent(IDirect3DDevice8* pDevice,
    float x, float y, float w, float h,
    D3DCOLOR bg_colour, D3DCOLOR accent_colour) {
    float border = (float)UI_ACTIVE_BORDER;
    UI_DrawRect(pDevice, x, y, w, h, bg_colour);
    UI_DrawRect(pDevice, x, y, border, h, accent_colour);
}

void UI_DrawRoundRect(IDirect3DDevice8* pDevice,
    float x, float y, float w, float h,
    float radius, D3DCOLOR colour) {
    /*
       Approximated with 9 filled rects:
       centre body + 4 edge strips + 4 corner discs (each a small square
       that approximates the arc at this scale).
       For a chat UI at 720p a 4-step arc is invisible -- plain rects suffice.
       Keep it simple: just draw 3 overlapping rects forming a cross shape.
    */
    float r = radius;
    /* Horizontal bar */
    UI_DrawRect(pDevice, x + r, y, w - r * 2, h, colour);
    /* Left/right vertical fills (minus corners) */
    UI_DrawRect(pDevice, x, y + r, r, h - r * 2, colour);
    UI_DrawRect(pDevice, x + w - r, y + r, r, h - r * 2, colour);
    /* Four corner approximations (small circles would need tris --
       at our scale a square corner offset is fine) */
    UI_DrawCircle(pDevice, x + r, y + r, r, colour);
    UI_DrawCircle(pDevice, x + w - r, y + r, r, colour);
    UI_DrawCircle(pDevice, x + r, y + h - r, r, colour);
    UI_DrawCircle(pDevice, x + w - r, y + h - r, r, colour);
}

void UI_DrawCircle(IDirect3DDevice8* pDevice,
    float cx, float cy, float r,
    D3DCOLOR colour) {
    /* 16-segment fan -- enough for small UI circles */
#define CIRCLE_SEGS 16
    UIVert verts[CIRCLE_SEGS + 2];
    int i;
    float rcx = UI_Sx(cx), rcy = UI_Sy(cy), rr = UI_Sx(r);

    set_colour_states(pDevice);
    uiv(&verts[0], rcx, rcy, colour);
    for (i = 0; i <= CIRCLE_SEGS; i++) {
        float angle = (float)i / CIRCLE_SEGS * 2.0f * (float)M_PI;
        uiv(&verts[i + 1],
            rcx + (float)cos(angle) * rr,
            rcy + (float)sin(angle) * rr,
            colour);
    }
    pDevice->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, CIRCLE_SEGS, verts, sizeof(UIVert));
    restore_states(pDevice);
#undef CIRCLE_SEGS
}

/* ── Logo ─────────────────────────────────────────────────────────────────── */

void UI_DrawLogo(IDirect3DDevice8* pDevice,
    float x, float y, float w, float h) {
    UIVertTex v[4];
    float rx = UI_Sx(x), ry = UI_Sy(y);
    float rw = UI_Sx(x + w) - rx, rh = UI_Sy(y + h) - ry;

    if (!s_logo_tex) return;

    pDevice->SetTexture(0, s_logo_tex);
    pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    pDevice->SetRenderState(D3DRS_ZENABLE, FALSE);
    pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);
    pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    pDevice->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    pDevice->SetVertexShader(UI_FVF_TEX);

    v[0].x = rx;    v[0].y = ry;    v[0].z = 0; v[0].rhw = 1; v[0].col = 0xFFFFFFFF; v[0].u = 0; v[0].v = 0;
    v[1].x = rx + rw; v[1].y = ry;    v[1].z = 0; v[1].rhw = 1; v[1].col = 0xFFFFFFFF; v[1].u = 1; v[1].v = 0;
    v[2].x = rx;    v[2].y = ry + rh; v[2].z = 0; v[2].rhw = 1; v[2].col = 0xFFFFFFFF; v[2].u = 0; v[2].v = 1;
    v[3].x = rx + rw; v[3].y = ry + rh; v[3].z = 0; v[3].rhw = 1; v[3].col = 0xFFFFFFFF; v[3].u = 1; v[3].v = 1;
    pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(UIVertTex));

    pDevice->SetTexture(0, NULL);
    pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    pDevice->SetRenderState(D3DRS_ZENABLE, TRUE);
}

/* ── Scrollbar ────────────────────────────────────────────────────────────── */

void UI_DrawScrollbar(IDirect3DDevice8* pDevice,
    float x, float y, float h,
    int total_items, int visible_items, int scroll_pos) {
    float track_h, thumb_h, thumb_y, ratio;

    if (total_items <= visible_items) return;

    track_h = h;
    ratio = (float)visible_items / (float)total_items;
    thumb_h = track_h * ratio;
    if (thumb_h < 20) thumb_h = 20;

    thumb_y = y + (track_h - thumb_h) *
        ((float)scroll_pos / (float)(total_items - visible_items));

    /* Track */
    UI_DrawRect(pDevice, x, y, (float)UI_SCROLLBAR_W, h, UI_COL_DIVIDER);
    /* Thumb */
    UI_DrawRect(pDevice, x, thumb_y, (float)UI_SCROLLBAR_W, thumb_h, UI_COL_TEXT_MUTED);
}

/* ── Layout helpers ───────────────────────────────────────────────────────── */

float UI_ChatX(void) { return (float)(UI_SIDEBAR_W); }
float UI_ChatY(void) { return (float)(UI_HEADER_H); }
float UI_ChatW(void) { return (float)(UI_VIRT_W - UI_SIDEBAR_W); }
float UI_ChatH(void) { return (float)(UI_VIRT_H - UI_HEADER_H - UI_INPUT_H); }

float UI_InputX(void) { return (float)(UI_SIDEBAR_W + UI_PADDING); }
float UI_InputY(void) { return (float)(UI_VIRT_H - UI_INPUT_H); }
float UI_InputW(void) { return (float)(UI_VIRT_W - UI_SIDEBAR_W - UI_PADDING * 2); }
float UI_InputH(void) { return (float)(UI_INPUT_H); }