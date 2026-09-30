/* CanLin minimal runtime assembly (canlin#72 "first light"): scheduler, theme/ui, shell,
 * one output with its software frame, and the built-in demo module, wired for the native
 * loop. Backend-neutral: a backend (Android now; rawfb canlin#64 and SDL2 canlin#65 later)
 * supplies the clock, the output's metrics and input class, a present hook, and its own
 * wait and sources for canlin_loop; it feeds normalised input through canlin_app_input.
 *
 * Not a framework: no storage, no LLM, no network, no module loading from bundles. The
 * demo module (heading, text, field, button) is compiled in and exists to prove present,
 * touch and the on-screen keyboard on a device.
 *
 * Owner thread only; none of these may be called from inside a loop hook's frame (the
 * present hook runs inside one: it may only copy pixels out).
 */
#ifndef CANLIN_APP_H
#define CANLIN_APP_H

#include <stdint.h>

#include "canlin/ev.h"
#include "canlin/loop.h"
#include "canlin/module.h"
#include "canlin/render.h"
#include "canlin/shell.h"
#include "canlin/ui.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    CANLIN_APP_OK = 0,
    CANLIN_APP_EINVAL = -1,
    CANLIN_APP_ENOMEM = -2,
    CANLIN_APP_ESETUP = -3   /* a subsystem refused (ev, ui, shell, module) */
};

enum { CANLIN_APP_LOG_INFO = 0, CANLIN_APP_LOG_WARN, CANLIN_APP_LOG_ERROR };

typedef struct canlin_app_config {
    canlin_ev_clock clock;          /* the scheduler clock (canlin_loop_clock_monotonic) */
    const char *output_id;          /* stable platform identity, e.g. "android:main" */
    int w, h;                       /* output px, > 0 */
    float dpi;                      /* platform DPI factor */
    int scale;                      /* UI scale step, 0 = 100 */
    unsigned input;                 /* CANLIN_SHELL_IN_* of the output */
    canlin_shell_clock_fn wall;     /* status bar clock, NULL = none (canlin_app_local_clock) */
    void *wall_ud;
    /* Called after each frame of the output with canlin_render_end()'s dirty row count
     * (loop.h canlin_loop_shell_output). Copies pixels out, nothing else. May be NULL. */
    void (*present)(void *ctx, canlin_render *r, int rows);
    void *present_ctx;
    /* One line without newline: module print(), theme warnings, module faults. */
    void (*log)(void *ud, int level, const char *line);
    void *log_ud;
} canlin_app_config;

typedef struct canlin_app canlin_app;

canlin_app *canlin_app_new(const canlin_app_config *cfg, int *err);
/* Stops the module and frees everything; the loop using it must be freed first. */
void canlin_app_free(canlin_app *a);

/* Sets lc->ev and lc->display; the backend adds wait, pump and sources. */
void canlin_app_loop_config(canlin_app *a, canlin_loop_config *lc);

/* New output size: reallocates the frame (the next frame is drawn and reported whole) and
 * relayouts. Same size: nothing. CANLIN_APP_ENOMEM keeps the old size and frame. */
int canlin_app_resize(canlin_app *a, int w, int h);
/* New DPI factor (clamped by the output setter's range: EINVAL outside it). */
int canlin_app_set_dpi(canlin_app *a, float dpi);
/* Routes one normalised input of the output (canlin_shell_input). */
int canlin_app_input(canlin_app *a, const canlin_ui_input *in);

/* The output's current frame (full RGB, always the latest rendered state). */
canlin_render *canlin_app_frame(canlin_app *a);
/* For tests: the demo module and the output. */
canlin_module *canlin_app_module(canlin_app *a);
canlin_ui_output *canlin_app_output(canlin_app *a);

/* Local wall clock for canlin_app_config.wall: seconds since local midnight
 * (localtime_r). */
int64_t canlin_app_local_clock(void *ud);

#ifdef __cplusplus
}
#endif

#endif
