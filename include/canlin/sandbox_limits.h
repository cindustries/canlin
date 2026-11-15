/* CanLin sandbox numeric limits.
 *
 * Memory and budget values are v1 values from
 * docs/superpowers/specs/2026-09-23-canlin-core-api-design.md §7, to be calibrated on
 * P2 (canlin#13). Values the spec table does not list stay provisional.
 * Configured values are clamped to the *_MAX ceilings; 0 selects the default.
 *
 * Measured and set, kept apart (canlin#13): the three cost factors below are
 * marked "measured" - they were measured against the emulated P2 (tools/sb_costs
 * under qemu-i386 -cpu pentium2) as a ratio, the VM instructions one unit of
 * native work is worth, because the emulation slows every instruction alike, so
 * the ratio survives it while an absolute time does not. Everything else is set:
 * it was not measured, because the P2's time per VM instruction is still unknown
 * (canlin#3) - the budgets, which are absolute instruction counts, keep the
 * spec's estimate.
 */
#ifndef CANLIN_SANDBOX_LIMITS_H
#define CANLIN_SANDBOX_LIMITS_H

/* Lua heap per module state, in bytes (includes the state's own bookkeeping).
 * v1 value (core API spec §7). SET, not measured: a size, not a time - the
 * measurement of canlin#13 was of the time factors. */
#define CANLIN_SB_MEM_DEFAULT      (16u * 1024u * 1024u)
#define CANLIN_SB_MEM_MAX          (64u * 1024u * 1024u)

/* Lua VM instructions (plus charged native work units) per callback.
 * v1 value (core API spec §7). SET, not measured: the budget is an absolute
 * instruction count, and the P2's time per instruction is still unknown - the
 * spec's "100-200 ms on a P2" stays its estimate until canlin#3 measures it. */
#define CANLIN_SB_BUDGET_DEFAULT   2000000u
#define CANLIN_SB_BUDGET_MAX       50000000u

/* Budget of a draw callback; it runs every dirty frame. The effective draw budget is
 * min(this, configured budget). v1 value (core API spec §7). SET, not measured, like
 * the budget above. */
#define CANLIN_SB_BUDGET_DRAW      500000u

/* Count-hook stride: the budget is checked every this many instructions,
 * so a callback may overrun its budget by less than one stride.
 * SET, not measured: a checkpoint interval, not a cost factor. The measured cost of
 * one VM instruction (tools/sb_costs) includes the hook's own overhead at this
 * stride. */
#define CANLIN_SB_HOOK_STRIDE      1000

/* Native work charged to the same budget (one unit ~ one VM instruction).
 * MEASURED (canlin#13): the three factors were measured with tools/sb_costs on the
 * host and under qemu-i386 -cpu pentium2, as the VM instructions one unit of native
 * work is worth:
 *   - an element touched: 1 .. 7 instructions per element (a concat of 100 elements
 *     measured 4 .. 7, an unpack of 100 results 1 .. 6) -> 8 units per element;
 *   - a byte copied: 0.15 .. 0.6 instructions per byte -> 2 bytes per unit;
 *   - a source byte parsed by load(): 7 .. 24 instructions per byte -> 16 units.
 * Each value is the round-up that keeps the charge at or above the measured cost (the
 * load factor is the round-up of the emulated target's median, 14; one sample
 * reached 24), so a callback cannot outrun its budget. The spread is the accuracy of
 * the calibration: the host and the emulated target disagree by up to a factor of two
 * (the host's memcpy uses SSE, the P2 has none), and the emulated target is the
 * closer model of a P2. What was measured is the ratio; the absolute time of a VM
 * instruction on the P2 is still unknown (canlin#3).
 *   - table elements touched by concat/unpack/insert/remove/move, string.char
 *     arguments and string.byte results: CANLIN_SB_COST_ELEM units each;
 *     table.sort: n * (floor(log2 n) + 1) elements; string.rep: 1 per repetition.
 *   - bytes copied by upper/lower/reverse (input) and sub/format (result):
 *     1 unit per CANLIN_SB_COST_BYTES_PER_UNIT bytes, plus 1.
 *   - load(): CANLIN_SB_COST_LOAD_BYTE units per source byte, before parsing.
 *   - pattern matching (find/match/gmatch/gsub, patched lstrlib): 1 unit per
 *     match step/compared byte, reported in chunks of 256 steps. SET, not measured:
 *     the pattern step was not part of the measurement.
 *   - payload marshalling (module delivery, canlin#15): CANLIN_SB_COST_ELEM units per
 *     table entry plus 1 unit per CANLIN_SB_COST_BYTES_PER_UNIT string bytes, plus 1
 *     per string, charged up-front to the callback that receives the payload.
 * Charged up-front where the size is known in advance; unpack, byte, sub and
 * format are charged after the call from their (stack/memory-bounded) result. */
#define CANLIN_SB_COST_ELEM           8u                  /* measured */
#define CANLIN_SB_COST_BYTES_PER_UNIT 2u                   /* measured */
#define CANLIN_SB_COST_LOAD_BYTE      16u                 /* measured */

/* Longest message handed to the host log callback by print(); longer is truncated. */
#define CANLIN_SB_LOG_MAX          1024u                  /* provisional */

/* Longest name accepted by require(). */
#define CANLIN_SB_REQUIRE_NAME_MAX 64u                    /* provisional */

/* Longest Lua error message copied out to the host. */
#define CANLIN_SB_ERRMSG_MAX       256u                   /* provisional */

#endif
