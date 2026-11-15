/* Sandbox cost calibration (core API spec §7, canlin#13).
 *
 * The budget counts one unit per VM instruction (the count hook charges
 * CANLIN_SB_HOOK_STRIDE units per stride instructions), so a charge factor is
 * calibrated when the measured time of one unit of native work, divided by the
 * measured time of one VM instruction, is known: that quotient says how many VM
 * instructions the native work is worth, and the factor is that number.
 *
 * Only ratios are usable. The emulated P2 runs every instruction slower than the
 * real one, and by how much depends on the instruction mix, so a time measured
 * here is an upper bound for the P2 - never the P2's own time. The factors are
 * therefore measured as "VM instructions per unit of work", and the budgets
 * (absolute instruction counts) stay set: the P2's time per instruction is still
 * unknown until the real hardware (canlin#3) or the canlin-os harness measures it.
 *
 * Timing: the two sizes of a workload are measured alternately and the minimum of
 * each is kept - the minimum is the least noisy estimate of the work, and the
 * alternating order cancels a drift of the host. The sizes are large enough that
 * one measurement is far longer than the host's scheduling noise, even under the
 * emulation.
 *
 * Build and run:
 *   make sandbox-costs BUILD=build/host
 *   make TARGET=i686 BUILD=build/i686-sb sandbox-costs
 *   qemu-i386 -cpu pentium2 ./build/i686-sb/tools/sb_costs
 *
 * The host and the emulated target are both worth measuring: if the two ratios
 * agree, the ratio is a property of the work, not of the emulation.
 */
#define _POSIX_C_SOURCE 200809L /* clock_gettime() */

#include "canlin/sandbox.h"
#include "canlin/sandbox_limits.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The budgets below are the calibration's own; the factors under test are not
 * needed to run a workload (the charge must simply fit). */
#define CAL_BUDGET 200000000u
#define CAL_MEM    (32u * 1024u * 1024u)
#define REPS       5

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static canlin_sandbox *mk(uint32_t budget)
{
    canlin_sandbox_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.mem_limit = CAL_MEM;
    cfg.budget = budget;
    return canlin_sandbox_new(&cfg);
}

static void die(const char *what, const canlin_sandbox *sb, canlin_sandbox_status st)
{
    fprintf(stderr, "sandbox_costs: %s failed (status %d): %s\n", what, st,
            sb ? canlin_sandbox_errmsg(sb) : "no sandbox");
    exit(2);
}

/* ---- the VM instruction: the unit the budget counts ----
 *
 * "while true do end" runs until the budget is exhausted; the budget counts the
 * instructions, so the elapsed time per budget unit is the cost of one VM
 * instruction (the count hook's own overhead included - it is part of the
 * production cost at this stride). The marginal slope over two budgets cancels
 * the start-up cost of the sandbox and the chunk. */

static double ms_per_instruction(void)
{
    const uint32_t budgets[2] = {1000000u, 4000000u};
    double best[2] = {0, 0};
    int i, r;
    for (r = 0; r < REPS; r++) {
        for (i = 0; i < 2; i++) {
            canlin_sandbox *sb = mk(budgets[i]);
            canlin_sandbox_status st;
            double t0;
            if (!sb) {
                fprintf(stderr, "sandbox_costs: cannot create a sandbox\n");
                exit(2);
            }
            t0 = now_ms();
            st = canlin_sandbox_run(sb, "while true do end", 17, "=(cal)");
            t0 = now_ms() - t0;
            if (st != CANLIN_SANDBOX_FAULT_BUDGET)
                die("the vm baseline did not fault", sb, st);
            if (r == 0 || t0 < best[i])
                best[i] = t0;
            canlin_sandbox_free(sb);
        }
    }
    printf("  vm baseline: %.1f ms for %u units, %.1f ms for %u units\n", best[0], budgets[0],
           best[1], budgets[1]);
    return (best[1] - best[0]) / (double)(budgets[1] - budgets[0]);
}

/* ---- the native work: marginal cost per unit of work ----
 *
 * Each measurement runs the same operation on two sizes and takes the difference,
 * so the Lua overhead of the call and the fixed costs cancel. */

/* One workload: a setup chunk (untimed) that builds the data, and a measured chunk
 * that does the work once. */
typedef struct {
    const char *what;
    char setup_small[128];
    char setup_large[128];
    const char *src;
    double small_units; /* elements or bytes */
    double large_units;
} workload;

static double measure_workload(const workload *w)
{
    canlin_sandbox *sb[2];
    double best[2] = {0, 0};
    int k, r;

    for (k = 0; k < 2; k++) {
        sb[k] = mk(CAL_BUDGET);
        if (!sb[k]) {
            fprintf(stderr, "sandbox_costs: cannot create a sandbox\n");
            exit(2);
        }
    }
    for (k = 0; k < 2; k++) {
        const char *setup = k == 0 ? w->setup_small : w->setup_large;
        canlin_sandbox_status st = canlin_sandbox_run(sb[k], setup, strlen(setup), "=(setup)");
        if (st != CANLIN_SANDBOX_OK)
            die("setup", sb[k], st);
    }
    /* Alternate the two sizes, so a drift of the host hits both equally. */
    for (r = 0; r < REPS; r++) {
        for (k = 0; k < 2; k++) {
            canlin_sandbox_status st;
            double t0 = now_ms();
            st = canlin_sandbox_run(sb[k], w->src, strlen(w->src), "=(measure)");
            t0 = now_ms() - t0;
            if (st != CANLIN_SANDBOX_OK)
                die("measurement", sb[k], st);
            if (r == 0 || t0 < best[k])
                best[k] = t0;
        }
    }
    printf("  %-14s %10.2f ms for %.0f units, %10.2f ms for %.0f units\n", w->what, best[0],
           w->small_units, best[1], w->large_units);
    for (k = 0; k < 2; k++)
        canlin_sandbox_free(sb[k]);
    return (best[1] - best[0]) / (w->large_units - w->small_units);
}

int main(void)
{
    workload w;
    double vm, concat, unpack, sbyte, lbyte;

    printf("sandbox cost calibration (canlin#13)\n");
    vm = ms_per_instruction();

    memset(&w, 0, sizeof w);
    w.what = "concat element";
    snprintf(w.setup_small, sizeof w.setup_small, "t = {} for i = 1, 20000 do t[i] = 'x' end");
    snprintf(w.setup_large, sizeof w.setup_large, "t = {} for i = 1, 80000 do t[i] = 'x' end");
    w.src = "local s = table.concat(t)";
    w.small_units = 20000;
    w.large_units = 80000;
    concat = measure_workload(&w);

    memset(&w, 0, sizeof w);
    w.what = "unpack element";
    snprintf(w.setup_small, sizeof w.setup_small, "t = {} for i = 1, 20000 do t[i] = i end");
    snprintf(w.setup_large, sizeof w.setup_large, "t = {} for i = 1, 80000 do t[i] = i end");
    w.src = "local r = table.unpack(t)";
    w.small_units = 20000;
    w.large_units = 80000;
    unpack = measure_workload(&w);

    memset(&w, 0, sizeof w);
    w.what = "upper byte";
    snprintf(w.setup_small, sizeof w.setup_small, "s = ('x'):rep(262144)");
    snprintf(w.setup_large, sizeof w.setup_large, "s = ('x'):rep(1048576)");
    w.src = "local u = s:upper()";
    w.small_units = 262144;
    w.large_units = 1048576;
    sbyte = measure_workload(&w);

    memset(&w, 0, sizeof w);
    w.what = "load() byte";
    snprintf(w.setup_small, sizeof w.setup_small, "src = ('a=1;'):rep(16384)");
    snprintf(w.setup_large, sizeof w.setup_large, "src = ('a=1;'):rep(65536)");
    w.src = "local f = load(src)";
    w.small_units = 65536;
    w.large_units = 262144;
    lbyte = measure_workload(&w);

    printf("\n");
    printf("  one VM instruction: %.1f ns  (the budget's unit)\n", vm * 1e6);
    printf("  table.concat  element: %8.1f ns -> %6.2f VM instructions per element\n",
           concat * 1e6, concat / vm);
    printf("  table.unpack  element: %8.1f ns -> %6.2f VM instructions per element\n",
           unpack * 1e6, unpack / vm);
    printf("  string.upper   byte:   %8.2f ns -> %6.3f VM instructions per byte\n",
           sbyte * 1e6, sbyte / vm);
    printf("  load()         byte:   %8.2f ns -> %6.2f VM instructions per byte\n", lbyte * 1e6,
           lbyte / vm);
    printf("\n");
    printf("  factor now            measured (instructions)   factor now (units)\n");
    printf("  COST_ELEM             %6.2f .. %6.2f            %u\n", concat / vm, unpack / vm,
           CANLIN_SB_COST_ELEM);
    printf("  COST_BYTES_PER_UNIT   %6.2f                     %u\n", 1.0 / (sbyte / vm),
           CANLIN_SB_COST_BYTES_PER_UNIT);
    printf("  COST_LOAD_BYTE        %6.2f                     %u\n", lbyte / vm,
           CANLIN_SB_COST_LOAD_BYTE);
    printf("\n");
    printf("  an absolute time here is an upper bound for the P2, never its own time:\n");
    printf("  the emulated instruction costs %.1f ns, the P2's is unknown.\n", vm * 1e6);
    return 0;
}
