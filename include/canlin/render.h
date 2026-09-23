/* CanLin software rasterizer: Nuklear command list -> RGB buffer in RAM.
 *
 * UI spec §9, system design §4 (render). Integer and float code only, no SIMD, no OS
 * calls, no allocation in the draw path. The caller owns every buffer. Backends (rawfb,
 * SDL2, offscreen tests) copy the rows reported dirty after canlin_render_end().
 *
 * Pixel format: 3 bytes per pixel, byte order R, G, B; rows are `pitch` bytes apart.
 *
 * Geometry conventions (all half-open, pixel centres sampled at +0.5):
 *   filled rect/rounded rect/circle  cover [x, x+w) x [y, y+h)
 *   stroked rect/rounded rect/circle the outermost `thickness` px inside that area
 *   line, polyline, triangle/polygon outline  Bresenham, both endpoints drawn,
 *                                    thickness > 1 stamps a square brush
 *   curve                            CANLIN_UI_GRAPH_CURVE_SEGMENTS straight segments
 *   text                             glyphs from the baked alpha8 atlas, alpha-blended
 *   image                            only the font atlas (handle.ptr == atlas pixels)
 * NK_COMMAND_ARC, NK_COMMAND_ARC_FILLED, NK_COMMAND_CUSTOM and images other than the
 * atlas are ignored in v1 (no widget CanLin uses emits them).
 * Colours with alpha 0 draw nothing, alpha 255 overwrite, anything else blends.
 *
 * Dirty rows: every pixel write flags its row. With a shadow buffer, canlin_render_end()
 * clears the flag of every row whose bytes equal the previous frame, so a frame that
 * redraws identical content reports zero dirty rows. Without one, "dirty" means
 * "written this frame".
 */
#ifndef CANLIN_RENDER_H
#define CANLIN_RENDER_H

#include <stddef.h>

#include "canlin/nk.h"
#include "canlin/render_limits.h"

#define CANLIN_RENDER_BPP 3

typedef struct canlin_render {
    unsigned char *px;           /* w*h RGB, caller-owned */
    int w, h;
    size_t pitch;
    unsigned char *dirty;        /* h flags, caller-owned, NULL = no tracking */
    unsigned char *shadow;       /* pitch*h previous frame, caller-owned, may be NULL */
    int full;                    /* next end() reports every row (first frame) */
    int cx0, cy0, cx1, cy1;      /* current scissor, half-open, inside the buffer */
    const unsigned char *atlas;  /* alpha8 font atlas, caller-owned, may be NULL */
    int atlas_w, atlas_h;
} canlin_render;

/* Binds the buffers. w, h > 0 and pitch >= 3*w, else returns -1. dirty (h bytes) and
 * shadow (pitch*h bytes) are optional; shadow requires dirty. The first end() after
 * init or canlin_render_invalidate() reports every row dirty. */
int canlin_render_init(canlin_render *r, unsigned char *px, int w, int h, size_t pitch,
                       unsigned char *dirty, unsigned char *shadow);

/* Font atlas used for NK_COMMAND_TEXT and atlas images (alpha8, w*h bytes, pitch w). */
void canlin_render_set_atlas(canlin_render *r, const unsigned char *alpha8, int w, int h);

/* Forces the next end() to report every row (e.g. after the backend lost its copy). */
void canlin_render_invalidate(canlin_render *r);

/* Starts a frame: clears the dirty flags and resets the scissor to the whole buffer. */
void canlin_render_begin(canlin_render *r);

/* Fills the whole buffer (ignores the scissor, ignores alpha). */
void canlin_render_clear(canlin_render *r, struct nk_color c);

/* Rasterizes one command. NK_COMMAND_SCISSOR changes the clip for later commands. */
void canlin_render_command(canlin_render *r, const struct nk_command *cmd);

/* Rasterizes the context's whole command list in order. Does not call nk_clear(); the
 * caller does, after it no longer needs the commands. */
void canlin_render_commands(canlin_render *r, struct nk_context *ctx);

/* Ends a frame and returns the number of dirty rows (0 if tracking is off). With a
 * shadow buffer it also copies the changed rows into the shadow. */
int canlin_render_end(canlin_render *r);

/* Baked font: the Nuklear atlas plus an owned copy of its alpha8 pixels (Nuklear frees
 * its own copy in nk_font_atlas_end). Allocates; call once per theme load, not per
 * frame. `font->handle` is the struct nk_user_font for nk_init / nk_style_set_font. */
typedef struct canlin_render_font {
    struct nk_font_atlas atlas;
    struct nk_font *font;
    unsigned char *alpha;
    int w, h;
} canlin_render_font;

/* Bakes Nuklear's embedded ProggyClean at px_height (> 0). Returns 0 or -1. The v1
 * built-in font; theme TTF blobs are not baked yet (stb_truetype is not safe on
 * untrusted fonts, see third_party/nuklear/VERSION). */
int canlin_render_font_default(canlin_render_font *f, float px_height);
void canlin_render_font_free(canlin_render_font *f);

#endif
