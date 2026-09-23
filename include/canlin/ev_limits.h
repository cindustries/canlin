/* CanLin event scheduler — numeric limits.
 *
 * v1 values from docs/superpowers/specs/2026-09-23-canlin-core-api-design.md §7, sized
 * for the 1 GB P2 floor and to be re-measured on real hardware (canlin#3). Nothing
 * outside this header may hard-code them; they are compile-time constants, not runtime
 * configuration. The spec's "max" column is the ceiling a future configurable value may
 * never exceed; it is recorded in the comment until configuration exists.
 */
#ifndef CANLIN_EV_LIMITS_H
#define CANLIN_EV_LIMITS_H

/* Module instance slots (live plus retiring generations of the same module share one).
 * Spec max 32. */
#define CANLIN_EV_MAX_MODULES        16u

/* Per-module queue: queued items plus reserved terminal-outcome slots. Spec max 256. */
#define CANLIN_EV_MOD_MAX_ITEMS      64u
/* Per-module bytes: queued payload bytes plus reserved operation result bytes.
 * Spec max 1 MiB. */
#define CANLIN_EV_MOD_MAX_BYTES      (256u * 1024u)

/* Process-wide bounds across all modules (smaller than the sum of module bounds).
 * Spec max 2048 items / 16 MiB. */
#define CANLIN_EV_PROC_MAX_ITEMS     512u
#define CANLIN_EV_PROC_MAX_BYTES     (4u * 1024u * 1024u)

/* Finite operations (timers included) — per module (spec max 128) and process-wide
 * (op table size). The spec table has no process-wide op/timer row; these are chosen
 * as half the sum of the module bounds, like PROC_MAX_ITEMS. */
#define CANLIN_EV_MOD_MAX_OPS        32u
#define CANLIN_EV_PROC_MAX_OPS       256u

/* One-shot timers, a subset of the operation bounds above. Module spec max 64. */
#define CANLIN_EV_MOD_MAX_TIMERS     16u
#define CANLIN_EV_PROC_MAX_TIMERS    128u

/* Callback dispatches per scheduler turn, and queued-item dispatches per module per
 * turn. A control delivery and one draw per module come on top of the per-module
 * quota but still consume the turn budget. */
#define CANLIN_EV_TURN_MAX_DISPATCH  16u
#define CANLIN_EV_TURN_MAX_PER_MODULE 4u

#endif
