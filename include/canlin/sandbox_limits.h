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

/* Longest message handed to the host log callback by print(); longer is truncated. */
#define CANLIN_SB_LOG_MAX          1024u                  /* provisional */

/* Longest name accepted by require(). */
#define CANLIN_SB_REQUIRE_NAME_MAX 64u                    /* provisional */

/* Longest Lua error message copied out to the host. */
#define CANLIN_SB_ERRMSG_MAX       256u                   /* provisional */

#endif
