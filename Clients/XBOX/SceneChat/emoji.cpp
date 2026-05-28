/*---------------------------------------------------------------------------
    emoji.cpp -- Inline emoji rendering (C89/MSVC2003)
    Loads a pre-baked 256x256 BGRA atlas from emoji_atlas.h.
    Renders emoji as textured quads inline with chat text.
---------------------------------------------------------------------------*/

#include <xtl.h>
#include <xgraphics.h>
#include "emoji.h"
#include "emoji_atlas.h"
#include "ui.h"
/* Ftoi defined in ui.h -- required for float-to-int on Xbox (no __ftol2_sse) */
#include <string.h>

/* ── FVF ────────────────────────────────────────────────────────────────── */
#define EMOJI_FVF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1)

typedef struct {
    float x, y, z, rhw;
    unsigned int colour;
    float u, v;
} EmojiVert;

/* ── State ──────────────────────────────────────────────────────────────── */
static IDirect3DTexture8* s_tex = NULL;

/* ── Init / Shutdown ────────────────────────────────────────────────────── */

int Emoji_Init(IDirect3DDevice8* pDevice) {
    D3DLOCKED_RECT lr;
    unsigned char* swiz;
    DWORD sz;

    if (s_tex) return 1;

    if (FAILED(pDevice->CreateTexture(
        EMOJI_ATLAS_W, EMOJI_ATLAS_H, 1,
        0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &s_tex)))
        return 0;

    sz = EMOJI_ATLAS_W * EMOJI_ATLAS_H * 4;
    swiz = (unsigned char*)malloc(sz);
    if (!swiz) { s_tex->Release(); s_tex = NULL; return 0; }

    XGSwizzleRect(g_emojiAtlasData, 0, NULL, swiz, EMOJI_ATLAS_W, EMOJI_ATLAS_H, NULL, 4);

    s_tex->LockRect(0, &lr, NULL, 0);
    memcpy(lr.pBits, swiz, sz);
    s_tex->UnlockRect(0);
    free(swiz);
    return 1;
}

void Emoji_Shutdown(void) {
    if (s_tex) { s_tex->Release(); s_tex = NULL; }
}

/* ── Lookup ─────────────────────────────────────────────────────────────── */

int Emoji_Find(const char* name) {
    int i, j;
    for (i = 0; i < EMOJI_COUNT; i++) {
        const char* n = g_emojiNames[i];
        j = 0;
        while (name[j] && n[j] && name[j] == n[j]) j++;
        if (!name[j] && !n[j]) return i;
    }
    return -1;
}

/* ── Token parser ───────────────────────────────────────────────────────── */

int Emoji_ParseToken(const char* str, char* name_out, int name_max) {
    int i;
    if (!str || str[0] != ':') return 0;
    /* scan for closing colon */
    for (i = 1; str[i] && str[i] != ':' && i < name_max + 1; i++);
    if (str[i] != ':' || i <= 1) return 0;
    /* copy name */
    {
        int len = i - 1;
        int j;
        for (j = 0; j < len && j < name_max - 1; j++)
            name_out[j] = str[1 + j];
        name_out[j] = 0;
    }
    return i + 1;  /* length including both colons */
}

/* ── Draw ───────────────────────────────────────────────────────────────── */

void Emoji_DrawInline(IDirect3DDevice8* pDevice, float x, float y,
    int emoji_idx, int size_px, unsigned int colour) {
    EmojiVert verts[4];
    float u0, v0, u1, v1;
    float sx, sy, sw, sh;

    if (!s_tex || emoji_idx < 0 || emoji_idx >= EMOJI_COUNT) return;

    u0 = (float)g_emojiUV[emoji_idx][0] / 65536.0f;
    v0 = (float)g_emojiUV[emoji_idx][1] / 65536.0f;
    u1 = (float)g_emojiUV[emoji_idx][2] / 65536.0f;
    v1 = (float)g_emojiUV[emoji_idx][3] / 65536.0f;

    sx = UI_Sx(x);
    sy = UI_Sy(y);
    sw = UI_Sx((float)size_px);
    sh = UI_Sy((float)size_px);

    verts[0].x = sx;    verts[0].y = sy;    verts[0].z = 0; verts[0].rhw = 1; verts[0].colour = colour; verts[0].u = u0; verts[0].v = v0;
    verts[1].x = sx + sw; verts[1].y = sy;    verts[1].z = 0; verts[1].rhw = 1; verts[1].colour = colour; verts[1].u = u1; verts[1].v = v0;
    verts[2].x = sx;    verts[2].y = sy + sh; verts[2].z = 0; verts[2].rhw = 1; verts[2].colour = colour; verts[2].u = u0; verts[2].v = v1;
    verts[3].x = sx + sw; verts[3].y = sy + sh; verts[3].z = 0; verts[3].rhw = 1; verts[3].colour = colour; verts[3].u = u1; verts[3].v = v1;

    pDevice->SetTexture(0, s_tex);
    pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    pDevice->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    pDevice->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    pDevice->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    pDevice->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
    pDevice->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
    pDevice->SetTextureStageState(0, D3DTSS_MIPFILTER, D3DTEXF_NONE);
    pDevice->SetTextureStageState(0, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
    pDevice->SetTextureStageState(0, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);
    pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    pDevice->SetVertexShader(EMOJI_FVF);
    pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, verts, sizeof(EmojiVert));
    pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    pDevice->SetTexture(0, NULL);
}

/* Forward declarations from font.cpp */
extern "C" void Font_DrawText(IDirect3DDevice8*, float, float, const char*, int, unsigned int, int);
extern "C" int  Font_MeasureText(const char*, int);

/* ── Mixed text+emoji line renderer ────────────────────────────────────── */
/* Draws a string that may contain :token: sequences.
   Returns the total pixel width rendered (virtual coords). */

float Emoji_DrawMixed(IDirect3DDevice8* pDevice,
    float x, float y,
    const char* str,
    int font_size,
    unsigned int colour,
    int max_w) {
    char seg[256];
    char tok[64];
    float cx = x;
    float end_x = (max_w > 0) ? (x + (float)max_w) : 1e9f;
    int emoji_sz = EMOJI_RENDER_SIZE;
    int idx;
    int tok_len;


    while (*str) {
        if (*str == ':') {
            tok_len = Emoji_ParseToken(str, tok, sizeof(tok));
            if (tok_len > 0) {
                idx = Emoji_Find(tok);
                if (idx >= 0) {
                    if (cx + emoji_sz > end_x) break;
                    Emoji_DrawInline(pDevice, cx, y, idx, emoji_sz, colour);
                    cx += (float)emoji_sz + 2.0f;
                    str += tok_len;
                    continue;
                }
            }
        }

        /* Accumulate plain text up to next colon or end */
        {
            int slen = 0;
            while (str[slen] && str[slen] != ':' && slen < (int)sizeof(seg) - 1)
                seg[slen++] = str[slen];
            seg[slen] = 0;
            if (slen > 0) {
                int seg_w = Font_MeasureText(seg, font_size);
                if (cx + seg_w > end_x) {
                    /* Draw as much as fits */
                    Font_DrawText(pDevice, cx, y, seg, font_size, colour,
                        Ftoi(end_x - cx));
                    cx = end_x;
                    break;
                }
                Font_DrawText(pDevice, cx, y, seg, font_size, colour, 0);
                cx += (float)seg_w;
                str += slen;
            }
            else {
                str++;   /* lone colon with no matching close -- skip */
            }
        }
    }
    return cx - x;
}