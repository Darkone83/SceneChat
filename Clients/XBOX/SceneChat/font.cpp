/*---------------------------------------------------------------------------
    font.cpp -- Font atlas texture loader and glyph renderer.

    Loads the pre-baked font_atlas.h (RGBA byte array + GlyphMetrics tables)
    into a D3D8 texture, then blits individual glyphs via textured quads.
    Uses alpha-blending so glyphs composite correctly over any background.
---------------------------------------------------------------------------*/

#include <xtl.h>
#include "font.h"
#include "ui.h"
#include "font_atlas.h"
#include <xgraphics.h>
#include <string.h>

/*    Internal state                                                           */

static IDirect3DTexture8* s_atlas_tex = NULL;

/* Map FONT_SIZE_* -> pointer into the three GlyphMetrics tables */
static const GlyphMetrics* s_metrics[3] = {
    g_glyphsSmall,
    g_glyphsMedium,
    g_glyphsLarge
};

/* Glyph heights per size (filled during init from metrics) */
static int s_glyph_h[3] = { FONT_SMALL_SIZE, FONT_MEDIUM_SIZE, FONT_LARGE_SIZE };
/* Max ascender per size -- largest -bear_y across all glyphs */
static int s_max_ascender[3] = { 0, 0, 0 };

/* Atlas dimensions (from font_atlas.h defines) */
#define ATLAS_W  FONT_ATLAS_WIDTH
#define ATLAS_H  FONT_ATLAS_HEIGHT

/*    Vertex format                                                            */

#define FONT_FVF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1)

typedef struct {
    float x, y, z, rhw;
    DWORD colour;
    float u, v;
} FontVert;

/*    Init / Shutdown                                                          */

int Font_Init(IDirect3DDevice8* pDevice) {
    IDirect3DTexture8* tex = NULL;
    D3DLOCKED_RECT lr;
    HRESULT hr;
    int level, sz, i;

    if (s_atlas_tex) return 1; /* already initialised */

    /* D3DFMT_A8R8G8B8 swizzled + XGSwizzleRect -- confirmed pattern from
       XbDiag DiagLoadDDS, ScorchedEarthXB, OpenJazzXB.
       D3DFMT_LIN_* causes GPU lock on NV2A when sampled.
       gen_atlas.py outputs BGRA which is the correct byte order for this format. */
    hr = pDevice->CreateTexture(
        ATLAS_W, ATLAS_H, 1,
        0,
        D3DFMT_A8R8G8B8,
        D3DPOOL_MANAGED,
        &tex);
    if (FAILED(hr)) return 0;

    hr = tex->LockRect(0, &lr, NULL, 0);
    if (FAILED(hr)) { tex->Release(); return 0; }

    XGSwizzleRect(g_fontAtlasData, ATLAS_W * 4, NULL,
        lr.pBits, ATLAS_W, ATLAS_H, NULL, 4);

    tex->UnlockRect(0);
    s_atlas_tex = tex;

    /* Compute glyph heights and max ascender per size */
    for (sz = 0; sz < 3; sz++) {
        int maxh = 0, maxasc = 0;
        for (i = 0; i < 95; i++) {
            int asc;
            if (s_metrics[sz][i].h > maxh) maxh = s_metrics[sz][i].h;
            asc = -s_metrics[sz][i].bear_y;
            if (asc > maxasc) maxasc = asc;
        }
        if (maxh > 0) s_glyph_h[sz] = maxh;
        if (maxasc > 0) s_max_ascender[sz] = maxasc;
    }

    return 1;
}

void Font_Shutdown(void) {
    if (s_atlas_tex) { s_atlas_tex->Release(); s_atlas_tex = NULL; }
}

/*    Measurement                                                              */

int Font_MeasureText(const char* str, int size) {
    const GlyphMetrics* metrics;
    int width = 0;
    unsigned char ch;

    if (!str || size < 0 || size > 2) return 0;
    metrics = s_metrics[size];

    while ((ch = (unsigned char)*str++) != 0) {
        if (ch < 32 || ch > 126) continue;
        width += metrics[ch - 32].advance;
    }
    return width;
}

int Font_GlyphHeight(int size) {
    if (size < 0 || size > 2) return 16;
    return s_glyph_h[size];
}

/*    Drawing                                                                  */

void Font_DrawText(IDirect3DDevice8* pDevice,
    float x, float y,
    const char* str,
    int size,
    DWORD colour,
    int max_w) {
    const GlyphMetrics* metrics;
    FontVert verts[4];
    float sx = UI_Sx(x);
    float sy = UI_Sy(y);
    float cur_x = sx;
    float end_x = (max_w > 0) ? (sx + UI_Sx((float)max_w)) : 1e9f;
    unsigned char ch;
    const GlyphMetrics* gm;
    float u0, v0, u1, v1;
    float gx, gy, gw, gh;

    if (!s_atlas_tex || !str || size < 0 || size > 2) return;
    metrics = s_metrics[size];

    /* Set render states for alpha-blended text */
    pDevice->SetTexture(0, s_atlas_tex);
    pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    pDevice->SetRenderState(D3DRS_ZENABLE, FALSE);
    pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);
    pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    pDevice->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    pDevice->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    pDevice->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    /* Point filter + clamp address -- Xbox NV2A values from D3D8Types.h */
    pDevice->SetTextureStageState(0, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
    pDevice->SetTextureStageState(0, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);
    pDevice->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTEXF_POINT);
    pDevice->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTEXF_POINT);
    pDevice->SetTextureStageState(0, D3DTSS_MIPFILTER, D3DTEXF_NONE);
    pDevice->SetVertexShader(FONT_FVF);

    while ((ch = (unsigned char)*str++) != 0) {
        if (ch < 32 || ch > 126) continue;
        gm = &metrics[ch - 32];
        if (gm->w == 0) { cur_x += UI_Sx((float)gm->advance); continue; }
        if (max_w > 0 && cur_x + UI_Sx((float)gm->w) > end_x) break;

        /* UV coordinates -- no half-texel offset, NV2A does not need it */
        u0 = (float)gm->x / ATLAS_W;
        v0 = (float)gm->y / ATLAS_H;
        u1 = (float)(gm->x + gm->w) / ATLAS_W;
        v1 = (float)(gm->y + gm->h) / ATLAS_H;

        gx = cur_x;
        /* bear_y offsets glyph from baseline: y is line top,
           baseline = y + max_ascender, glyph top = baseline + bear_y */
        gy = sy + UI_Sy((float)(s_max_ascender[size] + gm->bear_y));
        gw = UI_Sx((float)gm->w);
        gh = UI_Sy((float)gm->h);

        /* Two triangles as a quad (screen-space, XYZRHW) */
        verts[0].x = gx;    verts[0].y = gy;    verts[0].z = 0; verts[0].rhw = 1; verts[0].colour = colour; verts[0].u = u0; verts[0].v = v0;
        verts[1].x = gx + gw; verts[1].y = gy;    verts[1].z = 0; verts[1].rhw = 1; verts[1].colour = colour; verts[1].u = u1; verts[1].v = v0;
        verts[2].x = gx;    verts[2].y = gy + gh; verts[2].z = 0; verts[2].rhw = 1; verts[2].colour = colour; verts[2].u = u0; verts[2].v = v1;
        verts[3].x = gx + gw; verts[3].y = gy + gh; verts[3].z = 0; verts[3].rhw = 1; verts[3].colour = colour; verts[3].u = u1; verts[3].v = v1;

        pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, verts, sizeof(FontVert));

        cur_x += UI_Sx((float)gm->advance);
    }

    /* Restore states */
    pDevice->SetTexture(0, NULL);
    pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    pDevice->SetRenderState(D3DRS_ZENABLE, TRUE);
}

void Font_DrawTextCentered(IDirect3DDevice8* pDevice,
    float cx, float y,
    float width,
    const char* str,
    int size,
    DWORD colour) {
    int tw = Font_MeasureText(str, size);
    float x = cx + (width - tw) * 0.5f;
    Font_DrawText(pDevice, x, y, str, size, colour, 0);
}

void Font_DrawTextRight(IDirect3DDevice8* pDevice,
    float x, float y,
    const char* str,
    int size,
    DWORD colour) {
    int tw = Font_MeasureText(str, size);
    Font_DrawText(pDevice, x - tw, y, str, size, colour, 0);
}