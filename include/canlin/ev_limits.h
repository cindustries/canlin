/* CanLin event scheduler — numeric limits.
 *
 * PROVISIONAL. The event-runtime spec (section 5) requires declared finite limits for
 * item count and bytes, per module and process-wide; the core API spec is to fix the
 * real numbers. These are deliberately small so tests hit every bound quickly. Nothing
 * outside this header may hard-code them; they are compile-time constants, not runtime
 * configuration, so every limit has a bounded maximum.
 */
#ifndef CANLIN_EV_LIMITS_H
#define CANLIN_EV_LIMITS_H

/* Module instance slots (live plus retiring generations of the same module share one). */
#define CL_EV_MAX_MODULES        8u

/* Per-module queue: queued items plus reserved terminal-outcome slots. */
#define CL_EV_MOD_MAX_ITEMS      32u
/* Per-module bytes: queued payload bytes plus reserved operation result bytes. */
#define CL_EV_MOD_MAX_BYTES      (64u * 1024u)

/* Process-wide bounds across all modules (smaller than the sum of module bounds). */
#define CL_EV_PROC_MAX_ITEMS     128u
#define CL_EV_PROC_MAX_BYTES     (256u * 1024u)

/* Finite operations (timers included) — per module and process-wide (table size). */
#define CL_EV_MOD_MAX_OPS        16u
#define CL_EV_PROC_MAX_OPS       64u

/* One-shot timers, a subset of the operation bounds above. */
#define CL_EV_MOD_MAX_TIMERS     8u
#define CL_EV_PROC_MAX_TIMERS    32u

/* Callback dispatches per scheduler turn, and queued-item dispatches per module per
 * turn. A control delivery and one draw per module come on top of the per-module
 * quota but still consume the turn budget. */
#define CL_EV_TURN_MAX_DISPATCH  16u
#define CL_EV_TURN_MAX_PER_MODULE 4u

#endif
