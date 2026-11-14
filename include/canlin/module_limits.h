/* CanLin module layer — numeric limits.
 *
 * v1 values from docs/superpowers/specs/2026-09-23-canlin-core-api-design.md §7, to be
 * re-measured on real hardware (canlin#3). Nothing outside this header may hard-code them.
 */
#ifndef CANLIN_MODULE_LIMITS_H
#define CANLIN_MODULE_LIMITS_H

/* The draw budget CANLIN_SB_BUDGET_DRAW lives in sandbox_limits.h. */

/* Consecutive erroring callbacks before the scope is faulted (spec §4). */
#define CANLIN_MODULE_MAX_ERRORS     8u

/* Longest module name / version string copied into ctx (not in the spec table). */
#define CANLIN_MODULE_NAME_MAX       64u

/* Longest code string of the core API spec §3.1 set (the longest is "unavailable");
 * a code that is not NUL-terminated within this many bytes is refused, never read
 * past its end. */
#define CANLIN_MODULE_CODE_MAX       16u

/* Longest single timer delay in ms (24 h); longer waits rearm. */
#define CANLIN_TIMER_MAX_MS          (24u * 60u * 60u * 1000u)

/* Payload marshalling into Lua, per payload (spec §5.2). */
#define CANLIN_MARSHAL_MAX_ARGS      4
#define CANLIN_MARSHAL_MAX_DEPTH     8u
#define CANLIN_MARSHAL_MAX_ENTRIES   1024u
#define CANLIN_MARSHAL_MAX_BYTES     (64u * 1024u)

/* The ceiling of the byte budget: the spec §7 "max" column (1 MiB). The budget may be
 * lowered, never raised past this (the code that applies it is clamped by it,
 * src/module/module.c). */
#define CANLIN_MARSHAL_MAX_BYTES_CEILING (1024u * 1024u)

#endif
