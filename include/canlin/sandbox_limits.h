/* CanLin sandbox numeric limits.
 *
 * PROVISIONAL: every value here is a placeholder until the core API spec fixes
 * per-module and process-wide budgets (system design §4, event runtime §2/§5).
 * Configured values are clamped to the *_MAX ceilings; 0 selects the default.
 */
#ifndef CANLIN_SANDBOX_LIMITS_H
#define CANLIN_SANDBOX_LIMITS_H

/* Lua heap per module state, in bytes (includes the state's own bookkeeping). */
#define CANLIN_SB_MEM_DEFAULT      (8u * 1024u * 1024u)   /* provisional */
#define CANLIN_SB_MEM_MAX          (64u * 1024u * 1024u)  /* provisional */

/* Lua VM instructions (plus charged native work units) per callback. */
#define CANLIN_SB_BUDGET_DEFAULT   10000000u              /* provisional */
#define CANLIN_SB_BUDGET_MAX       200000000u             /* provisional */

/* Count-hook stride: the budget is checked every this many instructions,
 * so a callback may overrun its budget by less than one stride. */
#define CANLIN_SB_HOOK_STRIDE      1000                   /* provisional */

/* Native work charged to the same budget (one unit ~ one VM instruction).
 * PROVISIONAL factors, to be recalibrated against P2 measurements:
 *   - table elements touched by concat/unpack/insert/remove/move, string.char
 *     arguments and string.byte results: CANLIN_SB_COST_ELEM units each;
 *     table.sort: n * (floor(log2 n) + 1) elements; string.rep: 1 per repetition.
 *   - bytes copied by upper/lower/reverse (input) and sub/format (result):
 *     1 unit per CANLIN_SB_COST_BYTES_PER_UNIT bytes, plus 1.
 *   - load(): CANLIN_SB_COST_LOAD_BYTE units per source byte, before parsing.
 *   - pattern matching (find/match/gmatch/gsub, patched lstrlib): 1 unit per
 *     match step/compared byte, reported in chunks of 256 steps.
 * Charged up-front where the size is known in advance; unpack, byte, sub and
 * format are charged after the call from their (stack/memory-bounded) result. */
#define CANLIN_SB_COST_ELEM           1u                  /* provisional */
#define CANLIN_SB_COST_BYTES_PER_UNIT 16u                 /* provisional */
#define CANLIN_SB_COST_LOAD_BYTE      1u                  /* provisional */

/* Longest message handed to the host log callback by print(); longer is truncated. */
#define CANLIN_SB_LOG_MAX          1024u                  /* provisional */

/* Longest name accepted by require(). */
#define CANLIN_SB_REQUIRE_NAME_MAX 64u                    /* provisional */

/* Longest Lua error message copied out to the host. */
#define CANLIN_SB_ERRMSG_MAX       256u                   /* provisional */

#endif
