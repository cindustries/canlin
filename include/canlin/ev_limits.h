/* CanLin event scheduler — numeric limits.
 *
 * v1 values from docs/superpowers/specs/2026-09-23-canlin-core-api-design.md §7, sized
 * for the 1 GB P2 floor and to be re-measured on real hardware (canlin#3). Nothing
 * outside this header may hard-code them; they are compile-time constants, not runtime
 * configuration. The spec's "max" column is the ceiling a future configurable value may
 * never exceed: each limit that has one names it as a *_CEILING constant below, and the
 * code that applies the limit is clamped by it.
 */
#ifndef CANLIN_EV_LIMITS_H
#define CANLIN_EV_LIMITS_H

/* Module instance slots (live plus retiring generations of the same module share one).
 * Ceiling: CANLIN_EV_MAX_MODULES_CEILING. */
#define CANLIN_EV_MAX_MODULES        16u

/* Per-module queue: queued items plus reserved terminal-outcome slots.
 * Ceiling: CANLIN_EV_MOD_MAX_ITEMS_CEILING. */
#define CANLIN_EV_MOD_MAX_ITEMS      64u
/* Per-module bytes: queued payload bytes plus reserved operation result bytes.
 * Ceiling: CANLIN_EV_MOD_MAX_BYTES_CEILING. */
#define CANLIN_EV_MOD_MAX_BYTES      (256u * 1024u)

/* Process-wide bounds across all modules. Spec §7 rows: CANLIN_EV_PROC_MAX_ITEMS (512,
 * max 2048) and CANLIN_EV_PROC_MAX_BYTES (4 MiB, max 16 MiB) — the table notes no
 * relation to the module bounds. The item bound is half the sum of the module item
 * bounds (512 of 1024), so it binds before any single module's; the byte bound is the
 * sum of the module byte bounds (16 slots at 256 KiB = 4 MiB) — what the slots may
 * hold together, not a cut below it.
 * Ceilings: CANLIN_EV_PROC_MAX_ITEMS_CEILING, CANLIN_EV_PROC_MAX_BYTES_CEILING. */
#define CANLIN_EV_PROC_MAX_ITEMS     512u
#define CANLIN_EV_PROC_MAX_BYTES     (4u * 1024u * 1024u)

/* Finite operations (timers included) — per module and process-wide (op table size).
 * Spec §7 rows: CANLIN_EV_MOD_MAX_OPS (32, max 128) and CANLIN_EV_PROC_MAX_OPS (256,
 * no max) — the process-wide row is noted "half the sum of module op limits", so the
 * 16 module slots at 32 each give 512, and half of that is 256.
 * Ceiling: CANLIN_EV_MOD_MAX_OPS_CEILING. */
#define CANLIN_EV_MOD_MAX_OPS        32u
#define CANLIN_EV_PROC_MAX_OPS       256u

/* One-shot timers, a subset of the operation bounds above.
 * Spec §7 rows: CANLIN_EV_MOD_MAX_TIMERS (16, max 64) and CANLIN_EV_PROC_MAX_TIMERS
 * (128, no max) — the process-wide value is half the sum of the module timer bounds
 * (16 slots at 16 each), the relation the op row above carries in the spec.
 * Ceiling: CANLIN_EV_MOD_MAX_TIMERS_CEILING. */
#define CANLIN_EV_MOD_MAX_TIMERS     16u
#define CANLIN_EV_PROC_MAX_TIMERS    128u

/* ---- the ceilings: the spec §7 "max" column ----
 *
 * "Each has a compile-time maximum; configuration can lower, never raise, a value
 * beyond its maximum" (spec §7). The limits above are the v1 values; these are the
 * maxima they may never exceed. A limit raised past its ceiling does not build: the
 * code that applies it is clamped by the ceiling (src/ev/ev.c). A configurable limit
 * will need the same clamp at runtime (as the sandbox does for its memory and budget,
 * src/sandbox/sandbox.c). */

#define CANLIN_EV_MAX_MODULES_CEILING     32u
#define CANLIN_EV_MOD_MAX_ITEMS_CEILING   256u
#define CANLIN_EV_MOD_MAX_BYTES_CEILING   (1024u * 1024u)
#define CANLIN_EV_PROC_MAX_ITEMS_CEILING  2048u
#define CANLIN_EV_PROC_MAX_BYTES_CEILING  (16u * 1024u * 1024u)
#define CANLIN_EV_MOD_MAX_OPS_CEILING      128u
#define CANLIN_EV_MOD_MAX_TIMERS_CEILING  64u

/* Callback dispatches per scheduler turn, and queued-item dispatches per module per
 * turn. A control delivery and one draw per module come on top of the per-module
 * quota but still consume the turn budget. */
#define CANLIN_EV_TURN_MAX_DISPATCH  16u
#define CANLIN_EV_TURN_MAX_PER_MODULE 4u

#endif
