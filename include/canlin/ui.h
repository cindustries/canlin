/* CanLin flow UI: the `ui` object of draw(ui), its layout and per-module view state.
 *
 * Contract: docs/superpowers/specs/2026-09-23-canlin-ui-design.md §1–§4, §8.
 * Owner-thread only. Nuklear types stay inside src/ui and src/render (spec §1).
 *
 * Two objects:
 *   canlin_ui       shared per theme load: resolved theme, DPI, em and the baked fonts
 *                   (body plus heading sizes in one atlas). Outlives every view using it.
 *   canlin_ui_view  one per module: its own nk_context (Nuklear window/edit state never
 *                   crosses modules; draws are per-module scheduler deliveries, not one
 *                   global frame), queued input, widget state keyed by identity, the
 *                   last frame's command list and layout record.
 *
 * Frame protocol (canlin_module does this around M.draw):
 *   canlin_ui_view_frame_begin(v)   apply queued input, start the Nuklear frame
 *   canlin_ui_push(L, v)            push the per-draw Lua `ui` object (inside the call)
 *   ... M.draw(ui) ...
 *   canlin_ui_view_frame_end(v)     finish the frame; the ui object is dead from now on
 *   canlin_ui_view_render(v, r)     rasterize the last finished frame
 *
 * Layout (spec §2): em = theme font size x DPI. The view is one vertical flow with a
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

/* Bakes the fonts for theme t at DPI dpi (the built-in font until canlin#21). The theme
 * is copied; its blobs must outlive the canlin_ui (theme.h). NULL on failure. */
canlin_ui *canlin_ui_new(const canlin_theme *t, float dpi);
void canlin_ui_free(canlin_ui *ui);
float canlin_ui_em(const canlin_ui *ui);
/* The baked atlas, for canlin_render_set_atlas(). */
const canlin_render_font *canlin_ui_font(const canlin_ui *ui);

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
    CANLIN_UI_EINVAL = -1,  /* malformed input event: dropped */
    CANLIN_UI_EFULL = -2    /* queue full: the queue was reset, see below */
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

canlin_ui_view *canlin_ui_view_new(canlin_ui *ui);
void canlin_ui_view_free(canlin_ui_view *v);
/* Switch to another canlin_ui (theme reload). Widget state is kept. */
void canlin_ui_view_set_ui(canlin_ui_view *v, canlin_ui *ui);
/* Screen rectangle of the view in px (w, h > 0). */
void canlin_ui_view_set_rect(canlin_ui_view *v, int x, int y, int w, int h);
/* Forget all widget state, queued input and scroll (new module generation). */
void canlin_ui_view_reset(canlin_ui_view *v);

/* ---- frames ---- */

/* Starts a frame. Returns 0, or -1 if a frame is already open or v has no ui. */
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

/* Rasterizes the last finished frame into r (the atlas of canlin_ui_font must be set). */
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
