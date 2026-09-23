/* CanLin module sandbox: one Lua 5.4 state per module scope.
 *
 * Owner-thread only: no function here may be called concurrently for the same
 * sandbox, and none may be called from a backend/audio callback.
 *
 * Environment exposed to Lua: filtered base library, string (without dump),
 * table, math, utf8 and a host-resolved require. No os, io, package, debug,
 * coroutine, dofile, loadfile or warn. Chunks load in text mode only.
 *
 * The per-callback budget counts VM instructions plus charged native work:
 * pattern matching and size-proportional string/table/load calls (factors in
 * sandbox_limits.h).
 *
 * A scope that exceeds its instruction budget or memory limit is FAULTED:
 * Lua code cannot catch the fault (pcall/xpcall/message handlers re-raise it),
 * the state is closed and every later call returns CANLIN_SANDBOX_DEAD
 * without entering Lua.
 */
#ifndef CANLIN_SANDBOX_H
#define CANLIN_SANDBOX_H

#include <stddef.h>
#include <stdint.h>

typedef struct canlin_sandbox canlin_sandbox;

typedef enum {
    CANLIN_SANDBOX_OK = 0,
    CANLIN_SANDBOX_ERROR,        /* ordinary Lua error; scope still alive */
    CANLIN_SANDBOX_FAULT_BUDGET, /* instruction/work budget exceeded; scope now dead */
    CANLIN_SANDBOX_FAULT_MEMORY, /* memory limit exceeded; scope now dead */
    CANLIN_SANDBOX_DEAD          /* scope faulted earlier; Lua was not entered */
} canlin_sandbox_status;

/* print() output. msg is not NUL-terminated and only valid during the call. */
typedef void (*canlin_sandbox_log_fn)(void *ud, const char *msg, size_t len);

/* require() resolver, restricted by the host to this module's executable keys
 * (lua#<name> of the verified bundle or an operator-enabled local override;
 * never state.bee). Return 1 and set *src and *len to Lua source text if found,
 * 0 if not. The buffer must stay valid until the resolver is called again or
 * the call returns to the host. */
typedef int (*canlin_sandbox_require_fn)(void *ud, const char *name, size_t name_len,
                                         const char **src, size_t *len);

typedef struct {
    size_t mem_limit;                 /* bytes; 0 = default; clamped to max */
    uint32_t budget;                  /* per callback; 0 = default; clamped to max */
    canlin_sandbox_log_fn log;        /* may be NULL: print output is discarded */
    canlin_sandbox_require_fn require;/* may be NULL: every require fails */
    void *ud;                         /* passed to log and require */
} canlin_sandbox_config;

/* Returns NULL if the state cannot be created within the memory limit. */
canlin_sandbox *canlin_sandbox_new(const canlin_sandbox_config *cfg);
void canlin_sandbox_free(canlin_sandbox *sb);

/* Load src as a text chunk and run it as one budgeted callback. */
canlin_sandbox_status canlin_sandbox_run(canlin_sandbox *sb, const char *src, size_t len,
                                         const char *chunkname);

int canlin_sandbox_is_dead(const canlin_sandbox *sb);
/* Message of the last non-OK status; never NULL. */
const char *canlin_sandbox_errmsg(const canlin_sandbox *sb);
/* Current Lua heap bytes (0 once dead). */
size_t canlin_sandbox_mem_used(const canlin_sandbox *sb);

/* ---- module callback API (canlin#9) ----
 * Contract: docs/superpowers/specs/2026-09-23-canlin-core-api-design.md §4, §6. */

struct lua_State;

/* Host code run inside the sandbox's protected call: installs globals or pushes
 * callback arguments. Runs under the scope's memory limit; an allocation failure is a
 * memory fault. It may raise a Lua error. */
typedef int (*canlin_sandbox_host_fn)(struct lua_State *L, void *ud);

/* Run fn(L, ud) once in protected mode (e.g. to install the canlin table). Same status
 * and fault rules as canlin_sandbox_run. */
canlin_sandbox_status canlin_sandbox_host(canlin_sandbox *sb, canlin_sandbox_host_fn fn,
                                          void *ud);

/* Load src (lua#main) as a text chunk, run it as one budgeted callback and store its
 * single result as the module table. A result that is not a table is an ERROR and no
 * module table is stored. */
canlin_sandbox_status canlin_sandbox_load_main(canlin_sandbox *sb, const char *src,
                                               size_t len, const char *chunkname);

/* Call field `field` of the stored module table as one budgeted callback. The budget is
 * reset for this call. push (may be NULL when nargs == 0) must push exactly `nargs`
 * values, 0 <= nargs <= CANLIN_MARSHAL_MAX_ARGS; it runs under the memory limit.
 * An absent (nil) field is skipped: Lua is not entered for it, the result is
 * CANLIN_SANDBOX_OK and *called (if not NULL) is set to 0. A present field that is not a
 * function is an ERROR. Return values of the callback are discarded.
 *
 * budget: 0 = the configured per-callback budget; otherwise the smaller of `budget` and
 * the configured one (a per-call budget can only lower it, e.g. CANLIN_SB_BUDGET_DRAW). */
canlin_sandbox_status canlin_sandbox_call_budget(canlin_sandbox *sb, const char *field,
                                                 uint32_t budget, int nargs,
                                                 canlin_sandbox_host_fn push, void *ud,
                                                 int *called);
/* canlin_sandbox_call_budget with the configured budget and no *called. */
canlin_sandbox_status canlin_sandbox_call(canlin_sandbox *sb, const char *field, int nargs,
                                          canlin_sandbox_host_fn push, void *ud);

/* Charge `units` of native host work (e.g. building payload values) against the budget of
 * the running callback. Only for host code inside canlin_sandbox_host/_call (a
 * canlin_sandbox_host_fn): if the units do not fit, the scope is faulted exactly like an
 * exhausted instruction budget and a Lua error is raised, so the host function does not
 * return. A lua_State not owned by a sandbox is not charged. */
void canlin_sandbox_charge(struct lua_State *L, size_t units);

#endif
