/* CanLin event scheduler core — owner-thread, no OS waiting, no threads.
 *
 * Contract source: docs/superpowers/specs/2026-09-22-canlin-event-runtime-design.md.
 * The C interface here is provisional until the core API spec defines it.
 *
 * Every function must be called on the owner thread. Backends/producers may call the
 * enqueueing functions from inside a delivery callback; they never deliver inline.
 * cl_ev_turn() is the only function that invokes the delivery callback, and it refuses
 * to run while a callback is active (CL_EV_EREENTRY).
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
typedef uint64_t cl_ev_time;

/* Monotonic clock interface. If a host clock steps backwards the scheduler clamps to
 * the last observed value. Suspension: a clock that keeps counting across suspend
 * makes every due one-shot timer fire once on resume (no catch-up backlog); a clock
 * that stops during suspend delays timers by the suspended time. The platform adapter
 * documents which one it provides. */
typedef struct cl_ev_clock {
    cl_ev_time (*now)(void *ctx);
    void *ctx;
} cl_ev_clock;

/* Fake clock for tests: time moves only when the test advances it. */
typedef struct cl_ev_fake_clock {
    cl_ev_time now;
} cl_ev_fake_clock;

cl_ev_clock cl_ev_fake_clock_bind(cl_ev_fake_clock *fc);
void cl_ev_fake_clock_advance(cl_ev_fake_clock *fc, cl_ev_time ms);

/* Module-instance generation. Globally unique for the process, never recycled; 0 is
 * never a valid generation. */
typedef uint32_t cl_ev_gen;

/* Generation-scoped handle: high 32 bits = generation, low 32 bits = serial within that
 * generation. Never reused; 0 is never valid. */
typedef uint64_t cl_ev_handle;

#define CL_EV_HANDLE_GEN(h) ((cl_ev_gen)((h) >> 32))

/* Return codes. */
enum {
    CL_EV_OK         = 0,
    CL_EV_RETIRED    = 1,  /* complete: accepted and accounted, no delivery (cancelled/dead) */
    CL_EV_SUPPRESSED = 2,  /* cancel: terminal outcome already existed; delivery suppressed */
    CL_EV_ALREADY    = 3,  /* cancel: nothing left to do (repeat cancel, or in delivery) */
    CL_EV_OVERSIZE   = 4,  /* complete: result exceeded reservation; terminal is FAILURE */
    CL_EV_ESTALE     = -1, /* unknown/dead generation or handle */
    CL_EV_EFULL      = -2, /* a count/byte bound refuses admission */
    CL_EV_EREENTRY   = -3, /* cl_ev_turn()/cl_ev_free() called from inside a callback */
    CL_EV_EINVAL     = -4, /* contract violation (bad argument, double completion) */
    CL_EV_ELIMIT     = -5  /* identifier space exhausted (generation or handle serial) */
};

/* Core terminal states of a finite operation. */
typedef enum cl_ev_status {
    CL_EV_SUCCESS   = 0,
    CL_EV_FAILURE   = 1,
    CL_EV_CANCELLED = 2  /* never delivered: canceller already knows, dead scopes get nothing */
} cl_ev_status;

/* Delivery detail codes. */
enum {
    CL_EV_DETAIL_NONE = 0,
    CL_EV_DETAIL_OVERSIZE = 1  /* producer result exceeded the admitted result budget */
};

/* Control latch bits — outside the data queue, coalesced, never blocked by it. */
enum {
    CL_EV_CTL_STOP           = 1u << 0,
    CL_EV_CTL_FAULT          = 1u << 1,
    CL_EV_CTL_INPUT_OVERFLOW = 1u << 2, /* ordered input dropped: reconcile input state */
    CL_EV_CTL_CONN_LOST      = 1u << 3
};

typedef enum cl_ev_kind {
    CL_EV_D_EVENT   = 1, /* posted event (input, stream data, ...) */
    CL_EV_D_OP      = 2, /* terminal outcome of a finite operation */
    CL_EV_D_TIMER   = 3, /* one-shot timer expiry */
    CL_EV_D_DRAW    = 4, /* module is dirty: draw now */
    CL_EV_D_CONTROL = 5  /* control latch bits for the embedder/lifecycle layer */
} cl_ev_kind;

typedef struct cl_ev_delivery {
    cl_ev_kind kind;
    unsigned slot;
    cl_ev_gen gen;
    uint32_t source, type;     /* EVENT */
    int32_t a, b;              /* EVENT inline values (e.g. pointer x/y) */
    cl_ev_handle handle;       /* OP, TIMER */
    cl_ev_status status;       /* OP */
    int detail;                /* OP */
    unsigned control;          /* CONTROL */
    const void *payload;       /* valid only during the callback */
    size_t bytes;
} cl_ev_delivery;

typedef struct cl_ev cl_ev;

typedef struct cl_ev_callbacks {
    void (*deliver)(void *ud, cl_ev *ev, const cl_ev_delivery *d);
} cl_ev_callbacks;

typedef void (*cl_ev_release_fn)(void *payload, size_t bytes);

/* Producer of a finite operation. `cancel` (optional) asks it to abort; the producer
 * must still call cl_ev_op_complete() exactly once — possibly from within the hook —
 * so its resources and the reservation are retired by their actual owner. */
typedef struct cl_ev_producer {
    void (*cancel)(void *ctx, cl_ev_handle h);
    void *ctx;
} cl_ev_producer;

typedef struct cl_ev_turn_result {
    int more;               /* runnable work remains (queued items, latches, dirty) */
    int has_deadline;       /* an armed timer exists */
    cl_ev_time deadline;    /* earliest armed timer deadline, monotonic ms */
    unsigned dispatched;    /* callbacks made in this turn */
} cl_ev_turn_result;

typedef struct cl_ev_stats {
    unsigned items;   /* queued items + reserved terminal slots */
    size_t bytes;     /* queued payload bytes + reserved result bytes */
    unsigned queued;  /* items in the queue only */
    unsigned ops;     /* live operation records, timers included */
    unsigned timers;
    unsigned control; /* pending control latch bits (module stats only) */
    int dirty;        /* (module stats only) */
    cl_ev_gen gen;    /* active generation, 0 if none (module stats only) */
} cl_ev_stats;

cl_ev *cl_ev_new(const cl_ev_clock *clock, const cl_ev_callbacks *cb, void *ud);
/* Releases every queued payload. Producers must have completed their operations. */
int cl_ev_free(cl_ev *ev);

/* Lifecycle. start picks a free slot; reload stops `old` and starts a new generation
 * in the same slot. Stop: mark inactive, cancel timers/ops (producer hooks), retire
 * queued work with accounting but without delivery. Allowed inside a callback. */
int cl_ev_module_start(cl_ev *ev, cl_ev_gen *gen_out);
int cl_ev_module_stop(cl_ev *ev, cl_ev_gen gen);
int cl_ev_module_reload(cl_ev *ev, cl_ev_gen old_gen, cl_ev_gen *new_gen);
int cl_ev_module_slot(const cl_ev *ev, cl_ev_gen gen); /* slot, or CL_EV_ESTALE */

/* Ordered event, FIFO per (module, generation, source). */
int cl_ev_post(cl_ev *ev, cl_ev_gen gen, uint32_t source, uint32_t type,
               int32_t a, int32_t b, void *payload, size_t bytes, cl_ev_release_fn release);
/* Latest-state event (pointer motion, resize): replaces a.b of the queue tail only if
 * the tail is a latest-state item of the same source/type, so it never crosses an
 * ordered item such as a button edge. */
int cl_ev_post_latest(cl_ev *ev, cl_ev_gen gen, uint32_t source, uint32_t type,
                      int32_t a, int32_t b);
/* Coalesced latches. */
int cl_ev_signal(cl_ev *ev, cl_ev_gen gen, unsigned control_bits);
int cl_ev_invalidate(cl_ev *ev, cl_ev_gen gen);

/* Finite operations: admission reserves one terminal slot plus max_result_bytes. */
int cl_ev_op_begin(cl_ev *ev, cl_ev_gen gen, size_t max_result_bytes,
                   const cl_ev_producer *prod, cl_ev_handle *out);
/* Producer side: status is CL_EV_SUCCESS or CL_EV_FAILURE. Never delivers inline. */
int cl_ev_op_complete(cl_ev *ev, cl_ev_handle h, cl_ev_status status,
                      void *payload, size_t bytes, cl_ev_release_fn release);
/* One-shot timer; reserves its expiry slot at admission. */
int cl_ev_timer_start(cl_ev *ev, cl_ev_gen gen, cl_ev_time delay_ms, cl_ev_handle *out);
/* Module side: cancel an op or timer owned by `gen`. Idempotent. */
int cl_ev_cancel(cl_ev *ev, cl_ev_gen gen, cl_ev_handle h);

/* One bounded scheduler turn. */
int cl_ev_turn(cl_ev *ev, cl_ev_turn_result *out);

/* slot < 0: process-wide totals. */
int cl_ev_get_stats(const cl_ev *ev, int slot, cl_ev_stats *out);

#ifdef __cplusplus
}
#endif

#endif
