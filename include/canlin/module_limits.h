/* CanLin module layer — numeric limits.
 *
 * v1 values from docs/superpowers/specs/2026-09-23-canlin-core-api-design.md §7, to be
 * re-measured on real hardware (canlin#3). Nothing outside this header may hard-code them.
 */
#ifndef CANLIN_MODULE_LIMITS_H
#define CANLIN_MODULE_LIMITS_H

/* Instruction budget of a draw callback; it runs every dirty frame. Belongs with the
 * sandbox limits; defined here until sandbox_limits.h adopts it (guarded so the two
 * cannot conflict). The effective draw budget is min(this, configured budget). */
#ifndef CANLIN_SB_BUDGET_DRAW
#define CANLIN_SB_BUDGET_DRAW        500000u
#endif

/* Consecutive erroring callbacks before the scope is faulted (spec §4). */
#define CANLIN_MODULE_MAX_ERRORS     8u

/* Longest module name / version string copied into ctx (not in the spec table). */
#define CANLIN_MODULE_NAME_MAX       64u

/* Longest single timer delay in ms (24 h); longer waits rearm. */
#define CANLIN_TIMER_MAX_MS          (24u * 60u * 60u * 1000u)

/* Payload marshalling into Lua, per payload (spec §5.2). */
#define CANLIN_MARSHAL_MAX_ARGS      4
#define CANLIN_MARSHAL_MAX_DEPTH     8u
#define CANLIN_MARSHAL_MAX_ENTRIES   1024u
#define CANLIN_MARSHAL_MAX_BYTES     (64u * 1024u)

#endif
