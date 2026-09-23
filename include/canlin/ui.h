/* CanLin flow UI: the `ui` object of draw(ui), its layout and per-module view state.
 *
 * Contract: docs/superpowers/specs/2026-09-23-canlin-ui-design.md §1–§4, §8.
 * Owner-thread only. Nuklear types stay inside src/ui and src/render (spec §1).
 *
 * Three objects:
 *   canlin_ui         shared per theme load: the resolved theme and the font cache. It
 *                     holds no display metrics. Outlives every view using it.
 *   canlin_ui_output  one display surface (spec §2.1): stable platform id, px size, DPI
 *                     factor and UI scale step. Changed only through the C setters below
 *                     (trusted shell/settings); nothing reachable from Lua touches it.
 *   canlin_ui_view    one per module, bound to one output: its own nk_context (Nuklear
 *                     window/edit state never crosses modules; draws are per-module
 *                     scheduler deliveries, not one global frame), queued input, widget
 *                     state keyed by identity, the last frame's command list and layout
 *                     record.
 *
 * Font cache: one entry per distinct em px size (body plus heading sizes baked into one
 * atlas), baked on demand, at most CANLIN_UI_FONT_SIZES_MAX held. A view pins the entry
 * of its current em from binding until it moves to another em, ui or is freed (its last
 * command list points into that atlas). Unpinned entries stay baked and are released
 * least recently used first when a new size needs the slot. If every slot is pinned, the
 * frame that needs a new size fails with CANLIN_UI_EFONT; the view keeps its previous
 * frame and size, nothing is released under another view.
 *
 * Frame protocol (canlin_module does this around M.draw):
 *   canlin_ui_view_frame_begin(v)   apply queued input, start the Nuklear frame
 *   canlin_ui_push(L, v)            push the per-draw Lua `ui` object (inside the call)
 *   ... M.draw(ui) ...
 *   canlin_ui_view_frame_end(v)     finish the frame; the ui object is dead from now on
 *   canlin_ui_view_render(v, r)     rasterize the last finished frame
 *
 * Layout (spec §2): em = theme font size x output DPI factor x output UI scale, read
 * from the view's output at every frame_begin. The view is one vertical flow with a
 * core-owned scroll offset. Every widget rectangle is computed in float px, rounded
 * once to whole pixels (edges rounded, so neighbours abut), and interactive elements
 * are never below ceil(2.5 em) after rounding. ui:columns splits at >= 40 em of the
 * current column's content width, else stacks.
 */
#ifndef CANLIN_UI_H
#define CANLIN_UI_H

#include <stddef.h>
#include <stdint.h>

#include "canlin/render.h"
#include "canlin/theme.h"
#include "canlin/ui_limits.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct canlin_ui canlin_ui;
typedef struct canlin_ui_view canlin_ui_view;

/* Copies theme t; its blobs must outlive the canlin_ui (theme.h). Fonts are baked on
 * demand per view em (the built-in font until canlin#21). NULL on failure. */
canlin_ui *canlin_ui_new(const canlin_theme *t);
/* Every view using ui must be freed or switched first. */
void canlin_ui_free(canlin_ui *ui);
/* Font cache state (tests): entries held, and bakes performed since canlin_ui_new. */
void canlin_ui_font_stats(const canlin_ui *ui, unsigned *held, unsigned *bakes);

/* ---- outputs (spec §2.1) ---- */

typedef struct canlin_ui_output canlin_ui_output;

typedef struct canlin_ui_output_desc {
    const char *id;  /* stable platform identity, 1..CANLIN_UI_OUTPUT_ID_MAX-1 bytes, copied */
    int w, h;        /* px, > 0 */
    float dpi;       /* platform DPI factor, CANLIN_UI_DPI_MIN..CANLIN_UI_DPI_MAX */
    int scale;       /* UI scale step in percent: 75, 100, 125, 150 or 200 */
} canlin_ui_output_desc;

/* Returns 1 for a valid UI scale step (percent), else 0. No continuous scale. */
int canlin_ui_scale_valid(int percent);
/* NULL and *err = CANLIN_UI_EINVAL for an invalid description, CANLIN_UI_ENOMEM. */
canlin_ui_output *canlin_ui_output_new(const canlin_ui_output_desc *d, int *err);
/* CANLIN_UI_EBUSY (nothing freed) while views are still bound to o. */
int canlin_ui_output_free(canlin_ui_output *o);
const char *canlin_ui_output_id(const canlin_ui_output *o);
int canlin_ui_output_scale(const canlin_ui_output *o);
float canlin_ui_output_dpi(const canlin_ui_output *o);
void canlin_ui_output_size(const canlin_ui_output *o, int *w, int *h);
/* em in px for theme ui on output o. */
float canlin_ui_output_em(const canlin_ui *ui, const canlin_ui_output *o);

/* Trusted setters (shell / settings only). EINVAL for a value outside the contract (the
 * output is unchanged), EBUSY while a view on o has a frame open. A changed value marks
 * every view on o dirty (canlin_ui_view_dirty) and calls its dirty callback; the next
 * frame re-lays out with the new metrics. Widget state is untouched. Setting the current
 * value is a no-op. */
int canlin_ui_output_set_scale(canlin_ui_output *o, int percent);
int canlin_ui_output_set_dpi(canlin_ui_output *o, float dpi);
int canlin_ui_output_set_size(canlin_ui_output *o, int w, int h);

/* ---- normalised input (fed by the core; never delivered to Lua as events) ---- */

typedef enum canlin_ui_key {
    CANLIN_UI_KEY_BACKSPACE = 1,
    CANLIN_UI_KEY_DELETE,
    CANLIN_UI_KEY_ENTER,
    CANLIN_UI_KEY_TAB,
    CANLIN_UI_KEY_LEFT,
    CANLIN_UI_KEY_RIGHT,
    CANLIN_UI_KEY_UP,
    CANLIN_UI_KEY_DOWN,
    CANLIN_UI_KEY_HOME,
    CANLIN_UI_KEY_END,
    CANLIN_UI_KEY_SHIFT,
    CANLIN_UI_KEY_CTRL
} canlin_ui_key;

typedef enum canlin_ui_input_type {
    CANLIN_UI_IN_MOTION = 1, /* x, y: pointer position, screen px */
    CANLIN_UI_IN_BUTTON,     /* x, y, down: primary button / touch contact edge */
    CANLIN_UI_IN_KEY,        /* key, down */
    CANLIN_UI_IN_TEXT,       /* cp: one Unicode scalar value (no surrogates, <= 0x10FFFF) */
    CANLIN_UI_IN_SCROLL      /* dy: px, positive scrolls content up (towards the end) */
} canlin_ui_input_type;

typedef struct canlin_ui_input {
    canlin_ui_input_type type;
    int x, y;
    int down;
    int key;
    uint32_t cp;
    int dy;
} canlin_ui_input;

enum {
    CANLIN_UI_OK = 0,
    CANLIN_UI_EINVAL = -1,  /* malformed input event (dropped) or argument */
    CANLIN_UI_EFULL = -2,   /* queue full: the queue was reset, see below */
    CANLIN_UI_EFONT = -3,   /* every font cache entry is pinned by a live view */
    CANLIN_UI_ENOMEM = -4,
    CANLIN_UI_EBUSY = -5    /* a frame is open */
};

/* Queues one input event in order. At most one edge per button and per key is applied
 * per frame; later edges stay queued for the next frame (canlin_ui_view_pending), so a
 * press and release between two draws still produce a press frame. On overflow
 * (CANLIN_UI_INPUT_MAX) the queue is dropped and pressed buttons/keys are released at
 * the next frame (event runtime §6: overflow clears stuck interaction state). */
int canlin_ui_view_input(canlin_ui_view *v, const canlin_ui_input *in);
/* Queued events not yet applied: the host requests another draw while this is > 0. */
size_t canlin_ui_view_pending(const canlin_ui_view *v);

/* ---- view lifecycle ---- */

/* A view of theme ui on output o, pinning the font of o's current em. NULL on failure
 * (*err, if not NULL: EINVAL, ENOMEM or EFONT). */
canlin_ui_view *canlin_ui_view_new(canlin_ui *ui, canlin_ui_output *o, int *err);
void canlin_ui_view_free(canlin_ui_view *v);
/* Switch to another canlin_ui (theme reload) and/or output (the shell moves the view).
 * Widget state is kept, the view is marked dirty and its old command list dropped (it
 * pointed into the old atlas). EINVAL, EBUSY inside a frame, EFONT/ENOMEM (unchanged). */
int canlin_ui_view_bind(canlin_ui_view *v, canlin_ui *ui, canlin_ui_output *o);
/* Screen rectangle of the view in px (w, h > 0), in its output's coordinates. */
void canlin_ui_view_set_rect(canlin_ui_view *v, int x, int y, int w, int h);
/* Called (owner thread, outside any frame) when the view's output metrics change, so the
 * owner can schedule a draw. fn may be NULL. */
void canlin_ui_view_set_dirty_fn(canlin_ui_view *v, void (*fn)(void *ud), void *ud);
/* 1 if the view needs a re-layout (new output metrics or binding) since its last frame. */
int canlin_ui_view_dirty(const canlin_ui_view *v);
/* em in px the view lays out with: of the last frame, or of the binding before one. */
float canlin_ui_view_em(const canlin_ui_view *v);
/* The atlas of the view's current font (canlin_ui_view_render sets it itself). */
const canlin_render_font *canlin_ui_view_font(const canlin_ui_view *v);
/* Forget all widget state, queued input and scroll (new module generation). */
void canlin_ui_view_reset(canlin_ui_view *v);

/* ---- frames ---- */

/* Starts a frame with the output's current metrics (re-binding the font if em changed).
 * Returns 0, EBUSY if a frame is already open, EINVAL for NULL, EFONT if the new em
 * needs a font slot and all are pinned, ENOMEM if baking failed. On error no frame is
 * open and the previous frame (command list, record, font) is intact. */
int canlin_ui_view_frame_begin(canlin_ui_view *v);
/* Ends the open frame (no-op without one). Widget state not used in this frame is
 * dropped. Does not touch any Lua state. */
void canlin_ui_view_frame_end(canlin_ui_view *v);

struct lua_State;
/* Pushes the per-draw `ui` object (a table with protected metatable). Charges its
 * construction to the running callback. v may be NULL (headless module): every method
 * then raises. Must run inside the sandbox callback that runs M.draw, between
 * frame_begin and frame_end; the object raises when used outside that frame. */
int canlin_ui_push(struct lua_State *L, canlin_ui_view *v);

/* Rasterizes the last finished frame into r; sets r's atlas to the view's font. */
void canlin_ui_view_render(canlin_ui_view *v, canlin_render *r);

/* ---- layout record (tests, later hit testing/accessibility) ---- */

typedef enum canlin_ui_kind {
    CANLIN_UI_W_HEADING = 1, CANLIN_UI_W_TEXT, CANLIN_UI_W_BUBBLE, CANLIN_UI_W_FIELD,
    CANLIN_UI_W_AREA, CANLIN_UI_W_BUTTON, CANLIN_UI_W_TOGGLE, CANLIN_UI_W_CHOICE,
    CANLIN_UI_W_METER, CANLIN_UI_W_SEPARATOR, CANLIN_UI_W_COLUMNS, CANLIN_UI_W_SECTION
} canlin_ui_kind;

typedef enum canlin_ui_part {
    CANLIN_UI_P_BOX = 1,  /* the widget's interactive or visual box */
    CANLIN_UI_P_LABEL,    /* single-line label (ellipsized if too long) */
    CANLIN_UI_P_LINE,     /* one wrapped text line */
    CANLIN_UI_P_OPTION    /* one option row of an expanded choice */
} canlin_ui_part;

typedef struct canlin_ui_rect {
    unsigned char kind, part;
    unsigned char ellipsized;  /* LABEL: text was shortened with "..." */
    unsigned short widget;     /* ordinal of the widget in the draw, from 0 */
    int x, y, w, h;            /* screen px, after rounding and scrolling */
    uint32_t off, len;         /* LINE/LABEL: byte range of the source string shown */
} canlin_ui_rect;

/* Entries of the last finished frame, in emission order. At most CANLIN_UI_RECORD_MAX;
 * *overflow (if not NULL) is set when later entries were not recorded. */
const canlin_ui_rect *canlin_ui_view_record(const canlin_ui_view *v, size_t *n,
                                            int *overflow);
/* Content height of the last finished frame in px, and the current scroll offset. */
int canlin_ui_view_content_height(const canlin_ui_view *v);
int canlin_ui_view_scroll(const canlin_ui_view *v);

/* The Nuklear context of the view (src/ui, src/render and tests only). */
struct nk_context *canlin_ui_view_nk(canlin_ui_view *v);

/* Returns 1 if s[0..len) is valid UTF-8 (no overlongs, surrogates or > U+10FFFF). */
int canlin_ui_utf8_valid(const char *s, size_t len);

#ifdef __cplusplus
}
#endif

#endif
