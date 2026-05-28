#ifndef SCENECHAT_EMOJI_H
#define SCENECHAT_EMOJI_H
/*---------------------------------------------------------------------------
    emoji.h -- Inline emoji rendering for SceneChat.

    Token format in message text:  :smile:  :angry:  :thumbs_up:  etc.

    Usage:
        Emoji_Init(pDevice)      -- load atlas texture (call once)
        Emoji_Shutdown()         -- release texture
        Emoji_Find(name)         -- returns index >= 0, or -1 if not found
        Emoji_DrawInline(...)    -- draw one emoji quad at (x,y), size px
        Emoji_MeasureToken(str)  -- parse one :token: from str, return length
                                    or 0 if not a valid token
---------------------------------------------------------------------------*/

#ifndef EMOJI_RENDER_SIZE
#define EMOJI_RENDER_SIZE 20    /* screen pixels, matches FONT_SIZE_MEDIUM line height */
#endif

#ifdef __cplusplus
extern "C" {
#endif

    int  Emoji_Init(IDirect3DDevice8* pDevice);
    void Emoji_Shutdown(void);

    int  Emoji_Find(const char* name);   /* -1 = not found */
    void Emoji_DrawInline(IDirect3DDevice8* pDevice, float x, float y,
        int emoji_idx, int size_px, unsigned int colour);

    /* Draw a string with inline :emoji: tokens. Returns width rendered. */
    float Emoji_DrawMixed(IDirect3DDevice8* pDevice, float x, float y,
        const char* str, int font_size,
        unsigned int colour, int max_w);

    /* Parse :token: at str[0]. Returns byte length of token (including colons)
       and fills name_out (up to name_max bytes). Returns 0 if not a token. */
    int  Emoji_ParseToken(const char* str, char* name_out, int name_max);

#ifdef __cplusplus
}
#endif

#endif /* SCENECHAT_EMOJI_H */