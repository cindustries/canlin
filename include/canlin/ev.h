/* CanLin event scheduler core — owner-thread, no OS waiting, no threads.
 *
 * Contract source: docs/superpowers/specs/2026-09-22-canlin-event-runtime-design.md.
 * The C interface here is provisional until the core API spec defines it.
 *
 * Every function must be called on the owner thread. Backends/producers may call the
 * enqueueing functions from inside a delivery callback; they never deliver inline.
 * canlin_ev_turn() is the only function that invokes the delivery callback, and it refuses
 * to run while a callback is active (CANLIN_EV_EREENTRY).
 *
 * Ownership rule for payloads: a return value >= 0 means the scheduler took ownership of
 * the payload and will call `release` exactly once (after delivery, or on retirement);
 * a negative return leaves the payload with the caller.
 */
#ifndef CANLIN_EV_H
#define CANLIN_EV_H

#include <stddef.h>
#include <stdint.h>

#include "canlin/ev_limits.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Monotonic milliseconds. Never wall time, never a server-supplied timestamp. */
typedef uint64_t canlin_ev_time;

/* Monotonic clock interface. If a host clock steps backwards the scheduler clamps to
 * the last observed value. Suspension: a clock that keeps counting across suspend
 * makes every due one-shot timer fire once on resume (no catch-up backlog); a clock
 * that stops during suspend delays timers by the suspended time. The platform adapter
 * documents which one it provides. */
typedef struct canlin_ev_clock {
    canlin_ev_time (*now)(void *ctx);
    void *ctx;
} canlin_ev_clock;

/* Fake clock for tests: time moves only when the test advances it. */
typedef struct canlin_ev_fake_clock {
    canlin_ev_time now;
} canlin_ev_fake_clock;

canlin_ev_clock canlin_ev_fake_clock_bind(canlin_ev_fake_clock *fc);
void canlin_ev_fake_clock_advance(canlin_ev_fake_clock *fc, canlin_ev_time ms);

/* Module-instance generation. Globally unique for the process, never recycled; 0 is
 * never a valid generation. */
typedef uint32_t canlin_ev_gen;

/* Generation-scoped handle: high 32 bits = generation, low 32 bits = serial within that
 * generation. Never reused; 0 is never valid. */
typedef uint64_t canlin_ev_handle;

#define CANLIN_EV_HANDLE_GEN(h) ((canlin_ev_gen)((h) >> 32))

/* Return codes. */
enum {
    CANLIN_EV_OK         = 0,
    CANLIN_EV_RETIRED    = 1,  /* complete: accepted and accounted, no delivery (cancelled/dead) */
    CANLIN_EV_SUPPRESSED = 2,  /* cancel: terminal outcome already existed; delivery suppressed */
    CANLIN_EV_ALREADY    = 3,  /* cancel: nothing left to do (repeat cancel, or in delivery) */
    CANLIN_EV_OVERSIZE   = 4,  /* complete: result exceeded reservation; terminal is FAILURE */
    CANLIN_EV_ESTALE     = -1, /* unknown/dead generation or handle */
    CANLIN_EV_EFULL      = -2, /* a count/byte bound refuses admission */
    CANLIN_EV_EREENTRY   = -3, /* canlin_ev_turn()/canlin_ev_free() called from inside a callback */
    CANLIN_EV_EINVAL     = -4, /* contract violation (bad argument, double completion) */
    CANLIN_EV_ELIMIT     = -5  /* identifier space exhausted (generation or handle serial) */
};

/* Core terminal states of a finite operation. */
typedef enum canlin_ev_status {
    CANLIN_EV_SUCCESS   = 0,
    CANLIN_EV_FAILURE   = 1,
    CANLIN_EV_CANCELLED = 2  /* never delivered: canceller already knows, dead scopes get nothing */
} canlin_ev_status;

/* Delivery detail codes. */
enum {
    CANLIN_EV_DETAIL_NONE = 0,
    CANLIN_EV_DETAIL_OVERSIZE = 1  /* producer result exceeded the admitted result budget */
};

/* Control latch bits — outside the data queue, coalesced, never blocked by it. */
enum {
    CANLIN_EV_CTL_STOP           = 1u << 0,
    CANLIN_EV_CTL_FAULT          = 1u << 1,
    CANLIN_EV_CTL_INPUT_OVERFLOW = 1u << 2, /* ordered input dropped: reconcile input state */
    CANLIN_EV_CTL_CONN_LOST      = 1u << 3
};

typedef enum canlin_ev_kind {
    CANLIN_EV_D_EVENT   = 1, /* posted event (input, stream data, ...) */
    CANLIN_EV_D_OP      = 2, /* terminal outcome of a finite operation */
    CANLIN_EV_D_TIMER   = 3, /* one-shot timer expiry */
    CANLIN_EV_D_DRAW    = 4, /* module is dirty: draw now */
    CANLIN_EV_D_CONTROL = 5  /* control latch bits for the embedder/lifecycle layer */
} canlin_ev_kind;

typedef struct canlin_ev_delivery {
    canlin_ev_kind kind;
    unsigned slot;
    canlin_ev_gen gen;
    uint32_t source, type;     /* EVENT */
    int32_t a, b;              /* EVENT inline values (e.g. pointer x/y) */
    canlin_ev_handle handle;   /* OP, TIMER */
    canlin_ev_status status;   /* OP */
    int detail;                /* OP */
    unsigned control;          /* CONTROL */
    const void *payload;       /* valid only during the callback */
    size_t bytes;
} canlin_ev_delivery;

typedef struct canlin_ev canlin_ev;

typedef struct canlin_ev_callbacks {
    void (*deliver)(void *ud, canlin_ev *ev, const canlin_ev_delivery *d);
} canlin_ev_callbacks;

typedef void (*canlin_ev_release_fn)(void *payload, size_t bytes);

/* Producer of a finite operation. `cancel` (optional) asks it to abort; the producer
 * must still call canlin_ev_op_complete() exactly once — possibly from within the hook —
 * so its resources and the reservation are retired by their actual owner. */
typedef struct canlin_ev_producer {
    void (*cancel)(void *ctx, canlin_ev_handle h);
    void *ctx;
} canlin_ev_producer;

typedef struct canlin_ev_turn_result {
    int more;                /* runnable work remains (queued items, latches, dirty) */
    int has_deadline;        /* an armed timer exists */
    canlin_ev_time deadline; /* earliest armed timer deadline, monotonic ms */
    unsigned dispatched;     /* callbacks made in this turn */
} canlin_ev_turn_result;

typedef struct canlin_ev_stats {
    unsigned items;   /* queued items + reserved terminal slots */
    size_t bytes;     /* queued payload bytes + reserved result bytes */
    unsigned queued;  /* items in the queue only */
    unsigned ops;     /* live operation records, timers included */
    unsigned timers;
    unsigned control;  /* pending control latch bits (module stats only) */
    int dirty;         /* (module stats only) */
    canlin_ev_gen gen; /* active generation, 0 if none (module stats only) */
} canlin_ev_stats;

canlin_ev *canlin_ev_new(const canlin_ev_clock *clock, const canlin_ev_callbacks *cb, void *ud);
/* Releases every queued payload. Producers must have completed their operations. */
int canlin_ev_free(canlin_ev *ev);

/* Lifecycle. start picks a free slot; reload stops `old` and starts a new generation
 * in the same slot. Stop: mark inactive, cancel timers/ops (producer hooks), retire
 * queued work with accounting but without delivery. Allowed inside a callback. */
int canlin_ev_module_start(canlin_ev *ev, canlin_ev_gen *gen_out);
int canlin_ev_module_stop(canlin_ev *ev, canlin_ev_gen gen);
int canlin_ev_module_reload(canlin_ev *ev, canlin_ev_gen old_gen, canlin_ev_gen *new_gen);
/* Returns the slot, or CANLIN_EV_ESTALE. */
int canlin_ev_module_slot(const canlin_ev *ev, canlin_ev_gen gen);

/* Ordered event, FIFO per (module, generation, source). */
int canlin_ev_post(canlin_ev *ev, canlin_ev_gen gen, uint32_t source, uint32_t type,
                   int32_t a, int32_t b, void *payload, size_t bytes,
                   canlin_ev_release_fn release);
/* Latest-state event (pointer motion, resize): replaces a.b of the queue tail only if
 * the tail is a latest-state item of the same source/type, so it never crosses an
 * ordered item such as a button edge. */
int canlin_ev_post_latest(canlin_ev *ev, canlin_ev_gen gen, uint32_t source, uint32_t type,
                          int32_t a, int32_t b);
/* Coalesced latches. */
int canlin_ev_signal(canlin_ev *ev, canlin_ev_gen gen, unsigned control_bits);
int canlin_ev_invalidate(canlin_ev *ev, canlin_ev_gen gen);

/* Finite operations: admission reserves one terminal slot plus max_result_bytes. */
int canlin_ev_op_begin(canlin_ev *ev, canlin_ev_gen gen, size_t max_result_bytes,
                       const canlin_ev_producer *prod, canlin_ev_handle *out);
/* Producer side: status is CANLIN_EV_SUCCESS or CANLIN_EV_FAILURE. Never delivers inline. */
int canlin_ev_op_complete(canlin_ev *ev, canlin_ev_handle h, canlin_ev_status status,
                          void *payload, size_t bytes, canlin_ev_release_fn release);
/* One-shot timer; reserves its expiry slot at admission. */
int canlin_ev_timer_start(canlin_ev *ev, canlin_ev_gen gen, canlin_ev_time delay_ms,
                          canlin_ev_handle *out);
/* Module side: cancel an op or timer owned by `gen`. Idempotent. */
int canlin_ev_cancel(canlin_ev *ev, canlin_ev_gen gen, canlin_ev_handle h);

/* One bounded scheduler turn. */
int canlin_ev_turn(canlin_ev *ev, canlin_ev_turn_result *out);

/* slot < 0: process-wide totals. */
int canlin_ev_get_stats(const canlin_ev *ev, int slot, canlin_ev_stats *out);

#ifdef __cplusplus
}
#endif

#endif
