/* CanLin module: one sandbox plus one scheduler generation, kept in step.
 *
 * Contract: docs/superpowers/specs/2026-09-23-canlin-core-api-design.md §4–§6.
 * Owner-thread only. canlin_module_deliver() is the only path into the module's Lua and
 * must only be called from the scheduler's deliver callback (inside canlin_ev_turn), so
 * the no-nesting rule of the event runtime holds by construction.
 *
 * Lua surface installed as the global `canlin` (spec §5.1):
 *   canlin.timer.after(ms)  -> handle | nil, code, message
 *   canlin.cancel(h)        -> "cancelled" | "suppressed" | "already" | nil, "stale", message
 *   canlin.invalidate()
 *   canlin.sys.now()        -> monotonic ms (integer)
 * Every function acts only on the calling module's current generation.
 */
#ifndef CANLIN_MODULE_H
#define CANLIN_MODULE_H

#include <stddef.h>
#include <stdint.h>

#include "canlin/ev.h"
#include "canlin/module_limits.h"
#include "canlin/sandbox.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- core-owned values marshalled into Lua (spec §5.2, §7 CANLIN_MARSHAL_*) ---- */

typedef enum canlin_value_type {
    CANLIN_VALUE_NIL = 0,
    CANLIN_VALUE_BOOL,
    CANLIN_VALUE_INT,
    CANLIN_VALUE_STR,   /* bytes, not NUL-terminated; strlen is never used */
    CANLIN_VALUE_TABLE
} canlin_value_type;

typedef struct canlin_value_pair canlin_value_pair;

typedef struct canlin_value {
    canlin_value_type type;
    int64_t i;                      /* BOOL (0/1) and INT */
    const char *s;                  /* STR */
    size_t len;
    const canlin_value_pair *pairs; /* TABLE */
    size_t n;
} canlin_value;

/* Keys are INT or STR; a later duplicate key overwrites an earlier one. */
struct canlin_value_pair {
    canlin_value key;
    canlin_value val;
};

enum {
    CANLIN_VALUE_OK = 0,
    CANLIN_VALUE_ETOOBIG = -1, /* exceeds CANLIN_MARSHAL_MAX_DEPTH/_ENTRIES/_BYTES */
    CANLIN_VALUE_EINVAL = -2   /* unknown type or key type */
};

/* Check v against the marshalling limits. A producer calls this before completing an
 * operation and completes with code "full" instead of an oversized result; delivery
 * checks again and converts any violation into ok=false. */
int canlin_value_check(const canlin_value *v);

/* Payload of an OP completion for a module (canlin_ev_op_complete payload). */
typedef struct canlin_module_result {
    const canlin_value *value; /* SUCCESS: `result` (NULL = nil) */
    const char *code;          /* FAILURE: a spec §3.1 code string */
    const char *message;       /* FAILURE: diagnostic text, may be NULL */
} canlin_module_result;

/* canlin_ev_post source whose payload is a canlin_module_event, delivered to Lua as
 * M.event(name, payload). Events of every other source are consumed by the core (input is
 * routed to UI state, never to M.event; spec §5.2). */
#define CANLIN_MODULE_SOURCE_EVENT 0x4d4f4445u

typedef struct canlin_module_event {
    const char *name;           /* NUL-terminated, at most CANLIN_MODULE_NAME_MAX bytes */
    const canlin_value *value;  /* payload; NULL = nil */
} canlin_module_event;

/* ---- module lifecycle (spec §6) ---- */

typedef struct canlin_module_source {
    const char *name;                  /* NUL-terminated, <= CANLIN_MODULE_NAME_MAX */
    const char *version;               /* NUL-terminated, <= CANLIN_MODULE_NAME_MAX */
    const char *main;                  /* lua#main source text */
    size_t main_len;
    canlin_sandbox_require_fn require; /* may be NULL; must outlive the generation */
    void *require_ud;
} canlin_module_source;

typedef struct canlin_module canlin_module;

typedef enum canlin_module_state {
    CANLIN_MODULE_RUNNING = 0,
    CANLIN_MODULE_STOPPED,
    CANLIN_MODULE_FAULTED
} canlin_module_state;

enum {
    CANLIN_MODULE_OK = 0,
    CANLIN_MODULE_EINVAL = -1, /* bad argument or contract violation */
    CANLIN_MODULE_ENOMEM = -2, /* host allocation or sandbox creation failed */
    CANLIN_MODULE_EEV = -3,    /* scheduler refused a generation (slots full, ids exhausted) */
    CANLIN_MODULE_ELOAD = -4,  /* lua#main failed, faulted or did not return a table */
    CANLIN_MODULE_EINIT = -5,  /* M.init raised or faulted: fatal for the generation */
    CANLIN_MODULE_EBUSY = -6   /* called while this module is inside a callback */
};

/* Start a generation, create its sandbox (cfg: limits and log sink; cfg->require is
 * ignored in favour of src->require), install `canlin`, run lua#main and M.init(ctx).
 * On success the module is RUNNING and marked dirty for its first draw. On failure
 * returns NULL, sets *err and leaves nothing behind in the scheduler. */
canlin_module *canlin_module_load(canlin_ev *ev, const canlin_module_source *src,
                                  const canlin_sandbox_config *cfg, int *err);

/* Map one delivery for this module into Lua (TIMER/OP/EVENT -> M.event, DRAW -> M.draw,
 * CONTROL consumed). A delivery for any other generation is refused without entering Lua
 * (CANLIN_SANDBOX_DEAD). A fault stops the generation (spec §5.3). */
canlin_sandbox_status canlin_module_deliver(canlin_module *m, const canlin_ev_delivery *d);

/* Retire the current generation (if any) and load `src` as a new one. On failure the
 * module is FAULTED with no generation and the error is returned. */
int canlin_module_reload(canlin_module *m, const canlin_module_source *src);

/* Stop the generation and close its Lua state; the module is STOPPED. Idempotent. */
void canlin_module_stop(canlin_module *m);
/* Stop and free. */
void canlin_module_free(canlin_module *m);

canlin_module_state canlin_module_get_state(const canlin_module *m);
/* Current generation, 0 if not RUNNING. */
canlin_ev_gen canlin_module_gen(const canlin_module *m);
/* Consecutive erroring callbacks so far. */
unsigned canlin_module_errors(const canlin_module *m);
/* Last error/fault message; never NULL. */
const char *canlin_module_errmsg(const canlin_module *m);

/* ---- UI view (UI spec §2–§3) ----
 * Without a view the module is headless: M.draw still runs and receives a `ui` object
 * whose every method raises. The view is owned by the module, created on the first
 * canlin_module_set_ui and freed with the module, after its Lua state; its widget state
 * is reset for each new generation. */
struct canlin_ui;
struct canlin_ui_view;
struct canlin_ui_input;

/* Attach (or switch, e.g. on theme reload) the shared ui and set the view's screen rect
 * in px. Requests a draw. EBUSY from inside the module's own callback. */
int canlin_module_set_ui(canlin_module *m, struct canlin_ui *ui, int x, int y, int w, int h);
/* The module's view (NULL if headless): render it after a draw, inspect its record. */
struct canlin_ui_view *canlin_module_view(canlin_module *m);
/* Route one normalised input event to the module's view (never to M.event, core API
 * §5.2) and request a draw. EINVAL for a malformed event or no view; an overflowing queue
 * is reset (ui.h) and still requests the draw that releases held buttons/keys. */
int canlin_module_input(canlin_module *m, const struct canlin_ui_input *in);

#ifdef __cplusplus
}
#endif

#endif
