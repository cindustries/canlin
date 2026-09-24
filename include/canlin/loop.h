/* CanLin native main loop (canlin#61): one owner-thread loop shared by the rawfb /
 * native POSIX and SDL2 backends.
 *
 * Contract: docs/superpowers/specs/2026-09-24-canlin-native-loop-design.md, event runtime
 * spec §3, HTTP transport spec §3. The loop core knows only canlin_ev; the LLM pump, the
 * sources (canlin_http, later evdev/MQTT/audio) and the display (canlin_shell) come in as
 * hooks. The adapters at the end of this header bind the real subsystems; each lives in
 * its own object, so linking the core pulls in none of them.
 *
 * One iteration (spec §3):
 *   1. canlin_ev_turn() until !more or CANLIN_LOOP_TURNS_MAX turns
 *   2. pump hook (canlin_llm_pump)
 *   3. every source's step, once
 *   4. display: update, then for each dirty output one frame (begin / shell_render / end,
 *      present) — at most one per output per iteration
 *   5. unless quit was requested: wait. Timeout 0 if work remains (canlin_ev_peek() says
 *      more, a source's deadline has passed, the descriptors did not fit); else the
 *      nearest of the scheduler, source and display deadlines; else infinite.
 * The timeout is computed after steps 1–4 and from canlin_ev_peek(), so work enqueued by
 * the pump, a source callback or the display update is never missed. A producer on
 * another thread must call canlin_loop_wake() after making its work visible to its
 * owner-thread source.
 *
 * Every function except canlin_loop_wake() and canlin_loop_quit() is owner-thread only.
 * canlin_loop_iterate()/_run() are refused from inside any hook (and a delivery).
 */
#ifndef CANLIN_LOOP_H
#define CANLIN_LOOP_H

#include <poll.h>
#include <stddef.h>
#include <stdint.h>

#include "canlin/ev.h"
#include "canlin/loop_limits.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    CANLIN_LOOP_OK = 0,
    CANLIN_LOOP_EINVAL = -1,
    CANLIN_LOOP_ENOMEM = -2,
    CANLIN_LOOP_EFULL = -3,     /* CANLIN_LOOP_SOURCES_MAX reached */
    CANLIN_LOOP_ESTATE = -4,    /* add_source after the first iteration */
    CANLIN_LOOP_EREENTRY = -5,  /* iterate/run from inside a hook or a delivery */
    CANLIN_LOOP_ESYS = -6       /* wake pipe could not be created; wait failed */
};

/* A source deadline meaning "none" (equals CANLIN_HTTP_NO_DEADLINE). */
#define CANLIN_LOOP_NO_DEADLINE UINT64_MAX

/* A source (spec §2): descriptors and/or a deadline, plus bounded owner-thread work.
 * pollfds writes at most `max` entries and returns how many it wants; *deadline is
 * preset to CANLIN_LOOP_NO_DEADLINE, and a deadline <= now means "step me without
 * waiting" (unfinished buffered work). step is called once per iteration whether or not
 * a descriptor became ready; it must not block. Either hook may be NULL. */
typedef struct canlin_loop_source {
    size_t (*pollfds)(void *ctx, struct pollfd *out, size_t max, canlin_ev_time *deadline);
    void (*step)(void *ctx, canlin_ev_time now);
    void *ctx;
} canlin_loop_source;

/* Display hooks (step 4). update (optional) runs first (canlin_shell_update); then for
 * output i < outputs(): if dirty(i), frame(i) once. next_tick_ms (optional) is a
 * relative wake-up bound (canlin_shell_next_tick_ms), < 0 = none. A zero struct means
 * no display. */
typedef struct canlin_loop_display {
    void (*update)(void *ctx);
    size_t (*outputs)(void *ctx);
    int (*dirty)(void *ctx, size_t i);
    void (*frame)(void *ctx, size_t i);
    int64_t (*next_tick_ms)(void *ctx);
    void *ctx;
} canlin_loop_display;

/* Wait (step 5): like poll(). fds[0] is the loop's wake pipe; timeout_ms < 0 waits
 * without a timeout. Returns the number of ready entries (revents set), 0 on timeout or
 * interruption, < 0 on failure (the iteration returns ESYS). */
typedef int (*canlin_loop_wait_fn)(void *ctx, struct pollfd *fds, size_t n, int timeout_ms);

/* The native POSIX wait: poll(), EINTR counts as a timeout. ctx is unused. */
int canlin_loop_poll_wait(void *ctx, struct pollfd *fds, size_t n, int timeout_ms);

typedef struct canlin_loop_config {
    canlin_ev *ev;                  /* required; its clock is the loop's clock */
    void (*pump)(void *ctx);        /* step 2, optional (canlin_loop_llm_pump) */
    void *pump_ctx;
    canlin_loop_display display;    /* step 4, optional */
    canlin_loop_wait_fn wait;       /* NULL = canlin_loop_poll_wait */
    void *wait_ctx;
} canlin_loop_config;

typedef struct canlin_loop canlin_loop;

/* Creates the wake pipe (nonblocking, close-on-exec). NULL with *err on failure. */
canlin_loop *canlin_loop_new(const canlin_loop_config *cfg, int *err);
/* Closes the wake pipe. No other thread may still call canlin_loop_wake(). */
void canlin_loop_free(canlin_loop *l);

/* Before the first iteration only; the source struct is copied, ctx must outlive the
 * loop. EFULL past CANLIN_LOOP_SOURCES_MAX. */
int canlin_loop_add_source(canlin_loop *l, const canlin_loop_source *src);

/* One iteration (steps 1–5; no wait when quit is pending). */
int canlin_loop_iterate(canlin_loop *l);
/* Iterates until canlin_loop_quit(); clears the request and returns OK, or the first
 * error of an iteration. */
int canlin_loop_run(canlin_loop *l);
/* Request the end of canlin_loop_run() after the current iteration, which then does not
 * wait. Owner thread (hooks and deliveries included) or a signal handler. */
void canlin_loop_quit(canlin_loop *l);
/* 1 while a quit request is pending. */
int canlin_loop_quitting(const canlin_loop *l);
/* End the current or next wait. Any thread and async-signal-safe (one write(2) to the
 * wake pipe; a full pipe already holds a wakeup). */
void canlin_loop_wake(canlin_loop *l);

typedef struct canlin_loop_stats {
    uint64_t iterations;
    uint64_t waits;          /* wait calls (iterations minus those ended by quit) */
    uint64_t wakeups;        /* waits that ended with the wake pipe readable */
    uint64_t frames;         /* display frame() calls */
    uint64_t fd_overflow;    /* iterations whose descriptors did not fit */
    unsigned last_turns;     /* scheduler turns of the last iteration */
    int last_timeout;        /* timeout of the last wait, -1 = infinite */
} canlin_loop_stats;

void canlin_loop_get_stats(const canlin_loop *l, canlin_loop_stats *out);

/* Monotonic platform clock (CLOCK_MONOTONIC, ms) for canlin_ev_new on native builds. On
 * Linux it does not count while the system is suspended: timers are delayed by the
 * suspended time (ev.h, clock interface). */
canlin_ev_clock canlin_loop_clock_monotonic(void);

/* ---- test wait (spec §4): fake clock, scripted readiness ---- */

typedef struct canlin_loop_fake_wait {
    canlin_ev_fake_clock *clock;
    /* Script for the next wait: if ready_fd is among the descriptors (and != -1), report
     * ready_events on it and return 1 without advancing the clock; the script is then
     * cleared (ready_fd = -1). Otherwise advance the clock by the timeout (not at all for
     * an infinite one) and return 0. */
    int ready_fd;
    short ready_events;
    /* Recorded by every wait. */
    unsigned waits;
    int last_timeout;
    size_t last_nfds;
} canlin_loop_fake_wait;

/* Initialises fw (ready_fd = -1) bound to fc. */
void canlin_loop_fake_wait_init(canlin_loop_fake_wait *fw, canlin_ev_fake_clock *fc);
/* canlin_loop_wait_fn; ctx is a canlin_loop_fake_wait. */
int canlin_loop_fake_wait_poll(void *ctx, struct pollfd *fds, size_t n, int timeout_ms);

/* ---- adapters (each in its own object file) ---- */

struct canlin_http;
struct canlin_llm;
struct canlin_shell;
struct canlin_ui_output;
struct canlin_render;

/* src/loop/loop_http.c: canlin_http_pollfds / canlin_http_step as a source. */
canlin_loop_source canlin_loop_http_source(struct canlin_http *h);

/* src/loop/loop_llm.c: pump hook; ctx is the canlin_llm. */
void canlin_loop_llm_pump(void *llm);

/* src/loop/loop_shell.c: the shell as the display. Each frame is canlin_render_begin,
 * canlin_shell_render, canlin_render_end, then present(ctx, r, rows) with end()'s dirty
 * row count (0 also when r does no dirty tracking) — the backend copies the rows out. */
typedef struct canlin_loop_shell_output {
    struct canlin_ui_output *o;
    struct canlin_render *r;
    void (*present)(void *ctx, struct canlin_render *r, int rows); /* may be NULL */
    void *ctx;
} canlin_loop_shell_output;

typedef struct canlin_loop_shell {
    struct canlin_shell *sh;
    canlin_loop_shell_output out[CANLIN_LOOP_OUTPUTS_MAX];
    size_t n;
} canlin_loop_shell;

/* s must outlive the loop; its outputs may change between iterations. */
canlin_loop_display canlin_loop_shell_display(canlin_loop_shell *s);

#ifdef __cplusplus
}
#endif

#endif
