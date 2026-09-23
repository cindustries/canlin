/* CanLin shell: arrangement of module views per output, trusted chrome and focus.
 *
 * Contract: docs/superpowers/specs/2026-09-23-canlin-shell-design.md (whole), UI spec
 * §2.1 and §4, core API §5.3 and §6. Owner thread only, and never from inside a scheduler
 * delivery (the shell rebinds views and reloads modules, which a module callback may not).
 *
 * Modes per output (§2): full-screen always; tiled when the output is >= 60 em wide and
 * its tiling fits (every pane >= 30 em in the split direction). Without a user choice
 * the output is tiled from 80 em. A user choice (canlin_shell_set_mode) is held in memory
 * per output here; persisting it in system/local/ is canlin#31. When the tiling no longer
 * fits the output shows its current pane full-screen and keeps the tiling, which returns
 * when space does.
 *
 * Every output has a tiling of 2-3 slots (panes); full-screen shows the slot `cur`.
 * A module sits in at most one slot of all outputs (switcher assignment swaps), so one
 * view is shown in at most one place (§1).
 *
 * Coordinates: pointer input and every rect are px of the output. A view's rectangle is
 * set in its output's coordinates (ui.h canlin_ui_view_set_rect), so output px are the
 * view's input space: the shell forwards them unchanged to the view under the point.
 *
 * Drawing (§5): the host calls canlin_shell_update() after its scheduler turns, then for
 * each output with canlin_shell_dirty() != 0: canlin_render_begin, canlin_shell_render,
 * canlin_render_end. Only dirty regions are rasterized: the bar (state change, clock at
 * most once per minute), a pane's chrome and view (rect, focus or state changed), a view
 * alone (its module finished a new frame), the switcher overlay.
 */
#ifndef CANLIN_SHELL_H
#define CANLIN_SHELL_H

#include <stdint.h>

#include "canlin/module.h"
#include "canlin/render.h"
#include "canlin/shell_limits.h"
#include "canlin/ui.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct canlin_shell canlin_shell;

enum {
    CANLIN_SHELL_OK = 0,
    CANLIN_SHELL_EINVAL = -1,
    CANLIN_SHELL_ENOMEM = -2,
    CANLIN_SHELL_EFULL = -3,   /* CANLIN_SHELL_OUTPUTS_MAX / _MODULES_MAX reached */
    CANLIN_SHELL_ESTATE = -4,  /* e.g. restart of a module that is not faulted */
    CANLIN_SHELL_EMODULE = -5  /* the module layer refused (reload/set_ui failed) */
};

/* Wall clock for the status bar: local time in seconds (the platform applies the time
 * zone). The bar shows HH:MM of (seconds mod 86400). */
typedef int64_t (*canlin_shell_clock_fn)(void *ud);

typedef struct canlin_shell_config {
    canlin_ui *ui;                /* theme and font cache of every view; outlives the shell */
    canlin_shell_clock_fn clock;  /* may be NULL: no clock in the bar */
    void *clock_ud;
} canlin_shell_config;

canlin_shell *canlin_shell_new(const canlin_shell_config *cfg, int *err);
/* Forgets outputs and modules; frees neither. */
void canlin_shell_free(canlin_shell *sh);

/* ---- outputs ---- */

/* Input classes of an output (flags); touch enables the swipe from the bar.
 * canlin_shell_add_output hands them to the output (canlin_ui_output_set_input, same
 * values): TOUCH without KEYBOARD gives focused fields the on-screen keyboard. */
enum {
    CANLIN_SHELL_IN_POINTER = 1,
    CANLIN_SHELL_IN_TOUCH = 2,
    CANLIN_SHELL_IN_KEYBOARD = 4
};

/* o outlives the shell or is removed first. Default: user mode AUTO, tiling of two
 * panes in a row, split evenly. Sets o's input class from `input` (EINVAL while a view
 * on o is inside its frame). */
int canlin_shell_add_output(canlin_shell *sh, canlin_ui_output *o, unsigned input);

typedef enum canlin_shell_mode {
    CANLIN_SHELL_AUTO = 0,     /* user choice only: follow the §2 defaults */
    CANLIN_SHELL_FULL,
    CANLIN_SHELL_TILED
} canlin_shell_mode;

/* User choice per output (trusted settings only). TILED is kept when the output is too
 * small for it; the effective mode then falls back to FULL until space returns. */
int canlin_shell_set_mode(canlin_shell *sh, canlin_ui_output *o, canlin_shell_mode mode);
/* Effective mode: FULL or TILED. */
canlin_shell_mode canlin_shell_mode_of(canlin_shell *sh, canlin_ui_output *o);
/* 1 if tiled could be chosen on o now (>= CANLIN_SHELL_TILED_MIN em and the tiling fits). */
int canlin_shell_tiled_available(canlin_shell *sh, canlin_ui_output *o);

typedef enum canlin_shell_orient {
    CANLIN_SHELL_ROW = 0,      /* panes side by side */
    CANLIN_SHELL_COLUMN        /* panes stacked */
} canlin_shell_orient;

/* 2..CANLIN_SHELL_PANES_MAX panes, split evenly. New slots take modules shown nowhere;
 * modules of dropped slots are shown nowhere (still in the switcher). */
int canlin_shell_set_tiling(canlin_shell *sh, canlin_ui_output *o, canlin_shell_orient orient,
                            int panes);

/* ---- modules ---- */

/* src is kept (caller-owned, must outlive the module's entry) for the restart action.
 * The module is placed in an empty slot if there is one, preferring the focused output's
 * current slot; the shell gives it a view with canlin_module_set_ui when it is shown. */
int canlin_shell_add_module(canlin_shell *sh, canlin_module *m, const canlin_module_source *src);
int canlin_shell_remove_module(canlin_shell *sh, canlin_module *m);
/* Mark m's view as built by the GUI tool: its pane gets the labelled model-view frame
 * (UI spec §4). The GUI tool itself is a later ticket. */
int canlin_shell_set_model_view(canlin_shell *sh, canlin_module *m, int on);
/* The restart action (core API §5.3): only for a FAULTED module, reloads it from its
 * source as a new generation. Also reachable as the faulted pane's button. */
int canlin_shell_restart(canlin_shell *sh, canlin_module *m);
/* Show m in the focused slot (swapping with wherever m was), as the switcher does. */
int canlin_shell_assign(canlin_shell *sh, canlin_module *m);

/* ---- status bar state (set by the platform/subsystems) ---- */

enum { CANLIN_SHELL_NET_UNKNOWN = 0, CANLIN_SHELL_NET_ONLINE, CANLIN_SHELL_NET_OFFLINE };
enum { CANLIN_SHELL_STORE_PERSISTENT = 0, CANLIN_SHELL_STORE_VOLATILE, CANLIN_SHELL_STORE_ERROR };

typedef struct canlin_shell_status {
    int net;      /* CANLIN_SHELL_NET_*; UNKNOWN shows nothing */
    int storage;  /* CANLIN_SHELL_STORE_*; volatile and error are never dropped from the bar */
    int mic;      /* nonzero while any capture runs; never dropped from the bar */
} canlin_shell_status;

/* Marks the bars dirty if anything changed. */
void canlin_shell_set_status(canlin_shell *sh, const canlin_shell_status *st);

/* ---- input (§4) ---- */

/* Keys reserved for the shell. They are outside the canlin_ui_key range, so no module
 * can receive or bind them; backends map a platform key to them (e.g. Super/Menu to
 * SWITCHER, F6 to NEXT). The chord Ctrl+Tab is NEXT as well: that Tab never reaches a
 * module (Ctrl itself is forwarded). */
#define CANLIN_SHELL_KEY_SWITCHER 0x100  /* open/close the switcher of the focused output */
#define CANLIN_SHELL_KEY_NEXT     0x101  /* focus the next pane, across outputs */

/* Where an input went. */
enum {
    CANLIN_SHELL_R_DROPPED = 0, /* nothing under it, or the module refused it */
    CANLIN_SHELL_R_CHROME,      /* consumed by the shell */
    CANLIN_SHELL_R_VIEW         /* posted with canlin_module_post_input */
};

/* Routes one normalised input event of output o. Chrome first (switcher, bar, divider,
 * restart), then the view under the point for pointer input (a press focuses it and is
 * delivered in the same call), the focused view for keys and text. A pointer press
 * delivered to a view captures motion and release until release. Returns R_* or EINVAL. */
int canlin_shell_input(canlin_shell *sh, canlin_ui_output *o, const canlin_ui_input *in);

/* The focused module (NULL if the focused pane is empty) and its output/pane. */
canlin_module *canlin_shell_focused(canlin_shell *sh, canlin_ui_output **o, int *pane);
/* 1 if the switcher of o is open. */
int canlin_shell_switcher_open(canlin_shell *sh, canlin_ui_output *o);

/* ---- drawing (§5) ---- */

/* Polls output metrics, module states and the clock, re-lays out, rebinds changed views.
 * Returns 1 if any output needs canlin_shell_render. */
int canlin_shell_update(canlin_shell *sh);
/* 1 if o has a dirty region (including a view with a new frame). */
int canlin_shell_dirty(canlin_shell *sh, canlin_ui_output *o);
/* Rasterizes the dirty regions of o into r (between canlin_render_begin/_end). The first
 * render of an output, and every re-layout, draw it whole. */
void canlin_shell_render(canlin_shell *sh, canlin_ui_output *o, canlin_render *r);
/* ms until the clock text changes (the host's wake-up bound), -1 without a clock. */
int64_t canlin_shell_next_tick_ms(canlin_shell *sh);

/* ---- geometry (tests, accessibility) ---- */

typedef struct canlin_shell_rect {
    int x, y, w, h;
} canlin_shell_rect;

typedef enum canlin_shell_part {
    CANLIN_SHELL_P_BAR = 1,
    CANLIN_SHELL_P_SWITCH_BUTTON,
    CANLIN_SHELL_P_CLOCK,
    CANLIN_SHELL_P_NET,
    CANLIN_SHELL_P_STORAGE,
    CANLIN_SHELL_P_MIC,
    CANLIN_SHELL_P_FAULTED,
    CANLIN_SHELL_P_PANE,          /* idx = pane */
    CANLIN_SHELL_P_VIEW,          /* idx = pane: the module view's rect */
    CANLIN_SHELL_P_DIVIDER,       /* idx = divider (between pane idx and idx+1): hit area */
    CANLIN_SHELL_P_RESTART,       /* idx = pane, only while its module is faulted */
    CANLIN_SHELL_P_MODEL_LABEL,   /* idx = pane, only for a model view */
    CANLIN_SHELL_P_SWITCHER,      /* only while open */
    CANLIN_SHELL_P_SWITCHER_ROW   /* idx = row, only while open */
} canlin_shell_part;

/* 1 and *r if the part is shown on o now, else 0. Panes are the visible ones: one in
 * full-screen mode (idx 0), the tiling's otherwise. */
int canlin_shell_part_rect(canlin_shell *sh, canlin_ui_output *o, canlin_shell_part part,
                           int idx, canlin_shell_rect *r);
/* Visible panes of o (1 in full-screen) and the module shown in pane idx (NULL: empty). */
int canlin_shell_panes(canlin_shell *sh, canlin_ui_output *o);
canlin_module *canlin_shell_pane_module(canlin_shell *sh, canlin_ui_output *o, int idx);
/* Module of switcher row idx (NULL past the end). */
canlin_module *canlin_shell_switcher_module(canlin_shell *sh, canlin_ui_output *o, int idx);

#ifdef __cplusplus
}
#endif

#endif
