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
 * frame and size, nothing is released under another view. When any entry of the same ui
 * becomes unpinned later (a view freed, re-bound or moved to another em), every view
 * whose last frame_begin failed with EFONT is marked dirty and its dirty callback runs,
 * so its owner retries the draw without an external invalidate (canlin#30).
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
    CANLIN_UI_KEY_CTRL,
    CANLIN_UI_KEY_ESCAPE    /* leaves the graph's keyboard mode (focus order, below) */
} canlin_ui_key;
#define CANLIN_UI_KEY_LAST CANLIN_UI_KEY_ESCAPE

/* Keyboard focus (UI spec §3, canlin#26). One widget of a view has the focus. Focusable,
 * in draw order: field, area, button, toggle, choice, section header, graph. Tab /
 * Shift-Tab move it to the next / previous focusable widget of the last finished frame's
 * draw order (wrapping); the widget then shows an accent focus mark (record part
 * P_FOCUS) until a pointer press. Enter or Space (text U+0020) activates a focused
 * button, toggle, section header or choice (expands/collapses); Up/Down step a focused
 * choice's index. A focused graph keeps Tab / Shift-Tab (node cycling) and Del for
 * itself; Esc leaves its keyboard mode, and the next Tab moves on from it. A pointer
 * press clears the focus of button, toggle, choice and section; fields and graphs take
 * the focus by press as before. Keys reserved by the shell never reach a view. */

/* Pointer buttons of CANLIN_UI_IN_BUTTON (canlin#32). Primary is 0, so a zeroed event is
 * a primary edge. Touch contact is primary; backends map a platform's context gesture to
 * SECONDARY only where the platform itself does (no long-press synthesis here). */
typedef enum canlin_ui_button {
    CANLIN_UI_BUTTON_PRIMARY = 0,
    CANLIN_UI_BUTTON_SECONDARY = 1,
    CANLIN_UI_BUTTON_MIDDLE = 2
} canlin_ui_button;

typedef enum canlin_ui_input_type {
    CANLIN_UI_IN_MOTION = 1, /* x, y: pointer position, screen px */
    CANLIN_UI_IN_BUTTON,     /* x, y, down, button: button / touch contact edge */
    CANLIN_UI_IN_KEY,        /* key, down */
    CANLIN_UI_IN_TEXT,       /* cp: one Unicode scalar value (no surrogates, <= 0x10FFFF) */
    CANLIN_UI_IN_SCROLL      /* dy: px, positive scrolls content up (towards the end) */
} canlin_ui_input_type;

typedef struct canlin_ui_input {
    canlin_ui_input_type type;
    int x, y;
    int down;
    int button;  /* BUTTON: canlin_ui_button; ignored for every other type */
    int key;
    uint32_t cp;
    int dy;
    uint64_t t;  /* monotonic ms of the event; canlin_module_post_input stamps edges with
                  * the scheduler clock at post time. Only differences matter (graph
                  * long-press/double-click). */
} canlin_ui_input;

enum {
    CANLIN_UI_OK = 0,
    CANLIN_UI_EINVAL = -1,  /* malformed input event (dropped) or argument */
    CANLIN_UI_EFULL = -2,   /* queue full: the queue was reset, see below */
    CANLIN_UI_EFONT = -3,   /* every font cache entry is pinned by a live view */
    CANLIN_UI_ENOMEM = -4,
    CANLIN_UI_EBUSY = -5    /* a frame is open */
};

/* 1 if in is a well-formed input event (known type, button and key in range, text a
 * Unicode scalar value other than 0), else 0. */
int canlin_ui_input_valid(const canlin_ui_input *in);

/* Queues one input event in order. At most one edge (of any button or key) is applied
 * per frame; later edges stay queued for the next frame (canlin_ui_view_pending), so a
 * press and release between two draws still produce a press frame. On overflow
 * (CANLIN_UI_INPUT_MAX) the view is reset as by canlin_ui_view_input_reset (event
 * runtime §5: overflow clears stuck interaction state) and EFULL returned. */
int canlin_ui_view_input(canlin_ui_view *v, const canlin_ui_input *in);
/* Input was lost (overflow outside or inside the view): drop the queue, release pressed
 * buttons (every button held) and keys at the next frame and cancel graph gestures without edits. No-op inside
 * a frame. */
void canlin_ui_view_input_reset(canlin_ui_view *v);
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
/* Called (owner thread, outside the view's own frame) when the view's output metrics
 * change or, after an EFONT skip, a font slot was unpinned, so the owner can schedule a
 * draw. It may run while another view's frame starts; it must only schedule. fn may be
 * NULL. */
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

/* Time-driven redraw (canlin#33). The owner passes the scheduler's monotonic time before
 * each frame_begin (same clock as canlin_ui_input.t); widgets that change look with time
 * alone (graph long-press feedback) read it. canlin_ui_view_wake returns 1 and *at when
 * the last finished frame needs another draw at monotonic ms *at even without input, else
 * 0. The owner arms a core-owned one-shot timer for it (never delivered to Lua) and cancels
 * it once a frame no longer asks. Without set_now the time is 0 (nothing is due). */
void canlin_ui_view_set_now(canlin_ui_view *v, uint64_t now);
int canlin_ui_view_wake(const canlin_ui_view *v, uint64_t *at);

/* Rasterizes the last finished frame into r; sets r's atlas to the view's font. */
void canlin_ui_view_render(canlin_ui_view *v, canlin_render *r);

/* ---- layout record (tests, later hit testing/accessibility) ---- */

typedef enum canlin_ui_kind {
    CANLIN_UI_W_HEADING = 1, CANLIN_UI_W_TEXT, CANLIN_UI_W_BUBBLE, CANLIN_UI_W_FIELD,
    CANLIN_UI_W_AREA, CANLIN_UI_W_BUTTON, CANLIN_UI_W_TOGGLE, CANLIN_UI_W_CHOICE,
    CANLIN_UI_W_METER, CANLIN_UI_W_SEPARATOR, CANLIN_UI_W_COLUMNS, CANLIN_UI_W_SECTION,
    CANLIN_UI_W_GRAPH
} canlin_ui_kind;

typedef enum canlin_ui_part {
    CANLIN_UI_P_BOX = 1,  /* the widget's interactive or visual box */
    CANLIN_UI_P_LABEL,    /* single-line label (ellipsized if too long) */
    CANLIN_UI_P_LINE,     /* one wrapped text line */
    CANLIN_UI_P_OPTION,   /* one option row of an expanded choice */
    CANLIN_UI_P_NODE,     /* graph: a visible node panel; off = node index (from 0) */
    CANLIN_UI_P_PORT,     /* graph: a visible port touch target; off = node index,
                           * len = port index, inputs first then outputs */
    CANLIN_UI_P_FOCUS,    /* keyboard focus mark around the focused widget (canlin#26) */
    CANLIN_UI_P_HOLD      /* graph: long-press feedback shown while a press is held past
                           * CANLIN_UI_LONG_PRESS_MS; off = node index, or
                           * CANLIN_UI_HOLD_CANVAS for a hold on empty canvas */
} canlin_ui_part;

#define CANLIN_UI_HOLD_CANVAS 0xffffffffu

typedef struct canlin_ui_rect {
    unsigned char kind, part;
    unsigned char ellipsized;  /* LABEL: text was shortened with U+2026 */
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

/* ==== shell support (canlin#35) ====
 * The resolved theme of ui (chrome is drawn with it; read-only, lives as long as ui). */
const canlin_theme *canlin_ui_theme(const canlin_ui *ui);
/* Frames started on v so far; changes whenever the view has a new command list. */
uint32_t canlin_ui_view_frames(const canlin_ui_view *v);

#ifdef __cplusplus
}
#endif

#endif
