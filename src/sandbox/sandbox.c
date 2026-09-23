/* CanLin module sandbox. See include/canlin/sandbox.h for the contract.
 *
 * Fault model: a budget or memory fault sets sb->fault. From then on every
 * count hook (stride 1), every sandbox pcall/xpcall continuation, every xpcall
 * message handler and every charged native call re-raises a light-userdata
 * sentinel, so no Lua frame can observe success after a fault. The host
 * pcall sees the flag, closes the state and marks the scope dead.
 *
 * Paths where Lua 5.4 runs code with hooks disabled (L->allowhook == 0) and
 * how they are closed:
 *   - __gc finalizers (lgc.c GCTM): setmetatable refuses metatables with __gc.
 *   - message handler invoked while the hook's error propagates: the xpcall
 *     handler is wrapped and does not call user code once faulted.
 *   - coroutine thread reset/close: coroutine library is not exposed.
 */
#include "canlin/sandbox.h"
#include "canlin/sandbox_limits.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

struct canlin_sandbox {
    lua_State *L;
    size_t mem_limit;
    size_t mem_used;
    uint32_t budget;
    uint32_t used;
    canlin_sandbox_status fault;   /* OK, FAULT_BUDGET or FAULT_MEMORY */
    int dead;
    canlin_sandbox_log_fn log;
    canlin_sandbox_require_fn require;
    void *ud;
    char errmsg[CANLIN_SB_ERRMSG_MAX];
};

static const char fault_sentinel = 0;   /* address is the uncatchable error value */
static const char loading_sentinel = 0; /* require: module in progress or failed */
static const char loaded_key[] = "canlin.sandbox.loaded";

static canlin_sandbox *sb_of(lua_State *L)
{
    void *ud;
    lua_getallocf(L, &ud);
    return (canlin_sandbox *)ud;
}

/* ---- allocator ------------------------------------------------------- */

static void *sb_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
    canlin_sandbox *sb = ud;
    size_t old = ptr ? osize : 0;
    void *p;

    if (nsize == 0) {
        free(ptr);
        sb->mem_used -= old;
        return NULL;
    }
    if (nsize > old && nsize - old > sb->mem_limit - sb->mem_used)
        return NULL; /* Lua runs an emergency GC and retries, then raises ERRMEM */
    p = realloc(ptr, nsize);
    if (p == NULL)
        return NULL;
    sb->mem_used = sb->mem_used - old + nsize;
    return p;
}

/* ---- fault and budget ------------------------------------------------ */

static int raise_fault(lua_State *L)
{
    lua_pushlightuserdata(L, (void *)&fault_sentinel); /* allocates nothing */
    return lua_error(L);
}

static void count_hook(lua_State *L, lua_Debug *ar);

static void arm_hook(lua_State *L, int stride)
{
    lua_sethook(L, count_hook, LUA_MASKCOUNT, stride);
}

/* Record the first fault; from now on the hook fires on every instruction. */
static void set_fault(lua_State *L, canlin_sandbox *sb, canlin_sandbox_status why)
{
    if (sb->fault == CANLIN_SANDBOX_OK)
        sb->fault = why;
    arm_hook(L, 1);
}

static void count_hook(lua_State *L, lua_Debug *ar)
{
    canlin_sandbox *sb = sb_of(L);
    (void)ar;
    if (sb->fault == CANLIN_SANDBOX_OK) {
        if ((uint32_t)CANLIN_SB_HOOK_STRIDE <= sb->budget - sb->used) {
            sb->used += CANLIN_SB_HOOK_STRIDE;
            return;
        }
        sb->used = sb->budget;
        set_fault(L, sb, CANLIN_SANDBOX_FAULT_BUDGET);
    }
    raise_fault(L);
}

/* Charge native work against the callback budget; faults if it does not fit. */
static void charge(lua_State *L, lua_Unsigned units)
{
    canlin_sandbox *sb = sb_of(L);
    if (sb->fault == CANLIN_SANDBOX_OK) {
        if (units <= (lua_Unsigned)(sb->budget - sb->used)) {
            sb->used += (uint32_t)units;
            return;
        }
        sb->used = sb->budget;
        set_fault(L, sb, CANLIN_SANDBOX_FAULT_BUDGET);
    }
    raise_fault(L);
}

/* Any LUA_ERRMEM that reaches sandbox code is a memory fault. */
static void check_status(lua_State *L, int status)
{
    canlin_sandbox *sb = sb_of(L);
    if (status == LUA_ERRMEM && sb->fault == CANLIN_SANDBOX_OK) {
        set_fault(L, sb, CANLIN_SANDBOX_FAULT_MEMORY);
    }
    if (sb->fault != CANLIN_SANDBOX_OK)
        raise_fault(L);
}

/* ---- base library replacements --------------------------------------- */

static int finish_pcall(lua_State *L, int status, lua_KContext extra)
{
    check_status(L, status);
    if (status != LUA_OK && status != LUA_YIELD) {
        lua_pushboolean(L, 0);
        lua_pushvalue(L, -2);
        return 2;
    }
    return lua_gettop(L) - (int)extra;
}

static int sb_pcall(lua_State *L)
{
    int status;
    luaL_checkany(L, 1);
    lua_pushboolean(L, 1);
    lua_insert(L, 1);
    status = lua_pcallk(L, lua_gettop(L) - 2, LUA_MULTRET, 0, 0, finish_pcall);
    return finish_pcall(L, status, 0);
}

/* Message handler guard: once faulted, user handlers never run (they would
 * run with hooks disabled while the hook's error propagates). */
static int msgh_guard(lua_State *L)
{
    canlin_sandbox *sb = sb_of(L);
    if (sb->fault != CANLIN_SANDBOX_OK || lua_touserdata(L, 1) == (void *)&fault_sentinel) {
        lua_settop(L, 1);
        return 1;
    }
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, lua_gettop(L) - 1, 1);
    return 1;
}

static int sb_xpcall(lua_State *L)
{
    int status;
    int n = lua_gettop(L);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushvalue(L, 2);
    lua_pushcclosure(L, msgh_guard, 1);
    lua_replace(L, 2);
    lua_pushboolean(L, 1);
    lua_pushvalue(L, 1);
    lua_rotate(L, 3, 2);
    status = lua_pcallk(L, n - 2, LUA_MULTRET, 2, 2, finish_pcall);
    return finish_pcall(L, status, 2);
}

static int sb_load(lua_State *L)
{
    size_t len;
    const char *s = luaL_checklstring(L, 1, &len); /* no reader functions */
    const char *name = luaL_optstring(L, 2, "=(load)");
    const char *mode = luaL_optstring(L, 3, "t");
    int status;
    if (strcmp(mode, "t") != 0)
        return luaL_error(L, "load: only text chunks are allowed");
    charge(L, (lua_Unsigned)len * CANLIN_SB_COST_LOAD_BYTE);
    status = luaL_loadbufferx(L, s, len, name, "t");
    check_status(L, status);
    if (status != LUA_OK) {
        lua_pushnil(L);
        lua_insert(L, -2);
        return 2;
    }
    if (!lua_isnone(L, 4)) {
        lua_pushvalue(L, 4);
        if (!lua_setupvalue(L, -2, 1))
            lua_pop(L, 1);
    }
    return 1;
}

static int sb_setmetatable(lua_State *L)
{
    int t = lua_type(L, 2);
    luaL_checktype(L, 1, LUA_TTABLE);
    luaL_argexpected(L, t == LUA_TNIL || t == LUA_TTABLE, 2, "nil or table");
    if (t == LUA_TTABLE) {
        lua_pushliteral(L, "__gc");
        if (lua_rawget(L, 2) != LUA_TNIL)
            return luaL_error(L, "__gc metamethods are not allowed in the sandbox");
        lua_pop(L, 1);
    }
    if (luaL_getmetafield(L, 1, "__metatable") != LUA_TNIL)
        return luaL_error(L, "cannot change a protected metatable");
    lua_settop(L, 2);
    lua_setmetatable(L, 1);
    return 1;
}

static int sb_collectgarbage(lua_State *L)
{
    const char *opt = luaL_optstring(L, 1, "count");
    if (strcmp(opt, "count") != 0)
        return luaL_error(L, "collectgarbage: only \"count\" is allowed");
    lua_pushnumber(L, (lua_Number)lua_gc(L, LUA_GCCOUNT, 0) +
                          (lua_Number)lua_gc(L, LUA_GCCOUNTB, 0) / 1024);
    return 1;
}

static int sb_print(lua_State *L)
{
    canlin_sandbox *sb = sb_of(L);
    char buf[CANLIN_SB_LOG_MAX];
    size_t used = 0;
    int i, n = lua_gettop(L);
    for (i = 1; i <= n; i++) {
        size_t l;
        const char *s = luaL_tolstring(L, i, &l);
        if (i > 1 && used < sizeof buf)
            buf[used++] = '\t';
        if (l > sizeof buf - used)
            l = sizeof buf - used;
        memcpy(buf + used, s, l);
        used += l;
        lua_pop(L, 1);
    }
    if (sb->log)
        sb->log(sb->ud, buf, used);
    return 0;
}

static int sb_require(lua_State *L)
{
    canlin_sandbox *sb = sb_of(L);
    size_t nlen, len;
    const char *name = luaL_checklstring(L, 1, &nlen);
    const char *src;
    int status;

    luaL_argcheck(L, nlen > 0 && nlen <= CANLIN_SB_REQUIRE_NAME_MAX &&
                         strlen(name) == nlen, 1, "invalid module name");
    lua_settop(L, 1);
    lua_getfield(L, LUA_REGISTRYINDEX, loaded_key); /* 2 */
    lua_pushvalue(L, 1);
    if (lua_rawget(L, 2) != LUA_TNIL) {
        if (lua_touserdata(L, -1) == (void *)&loading_sentinel)
            return luaL_error(L, "module '%s' is loading or failed to load", name);
        return 1;
    }
    lua_pop(L, 1);
    if (sb->require == NULL || !sb->require(sb->ud, name, nlen, &src, &len))
        return luaL_error(L, "module '%s' not found", name);
    lua_pushfstring(L, "=lua#%s", name);
    status = luaL_loadbufferx(L, src, len, lua_tostring(L, -1), "t");
    check_status(L, status);
    if (status != LUA_OK)
        return lua_error(L);
    lua_pushvalue(L, 1);
    lua_pushlightuserdata(L, (void *)&loading_sentinel);
    lua_rawset(L, 2);
    lua_pushvalue(L, 1);
    lua_call(L, 1, 1);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_pushboolean(L, 1);
    }
    lua_pushvalue(L, 1);
    lua_pushvalue(L, -2);
    lua_rawset(L, 2);
    return 1;
}

/* ---- string/table replacements --------------------------------------- */

/* Hook for the patched lstrlib (patches/lua/0001-lstrlib-match-step-budget.patch):
 * pattern-matching steps are charged to the callback budget. States not owned
 * by a sandbox (identified by their allocator) are not budgeted. */
void canlin_lstr_charge(lua_State *L, size_t steps);
void canlin_lstr_charge(lua_State *L, size_t steps)
{
    void *ud;
    if (lua_getallocf(L, &ud) != sb_alloc)
        return;
    charge(L, (lua_Unsigned)steps);
}

/* Call the wrapped original (upvalue 1) with all arguments; returns all results. */
static int call_orig(lua_State *L)
{
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, lua_gettop(L) - 1, LUA_MULTRET);
    return lua_gettop(L);
}

static lua_Unsigned bytes_cost(size_t n)
{
    return (lua_Unsigned)(n / CANLIN_SB_COST_BYTES_PER_UNIT) + 1u;
}

/* #t of a table argument (may run __len); 0 for anything else, which the
 * original rejects or handles without iterating. */
static lua_Unsigned tab_len(lua_State *L, int idx)
{
    lua_Integer n;
    if (lua_type(L, idx) != LUA_TTABLE)
        return 0;
    n = luaL_len(L, idx);
    return n > 0 ? (lua_Unsigned)n : 0;
}

/* table.concat(list [, sep [, i [, j]]]): j - i + 1 elements, charged up-front. */
static int sb_concat(lua_State *L)
{
    lua_Unsigned n = tab_len(L, 1);
    lua_Integer i = luaL_optinteger(L, 3, 1);
    lua_Integer j = lua_isnoneornil(L, 4) ? (lua_Integer)n : luaL_checkinteger(L, 4);
    if (j >= i)
        charge(L, ((lua_Unsigned)j - (lua_Unsigned)i + 1u) * CANLIN_SB_COST_ELEM);
    return call_orig(L);
}

/* table.insert/table.remove: with an explicit position up to #t elements move. */
static int sb_insert(lua_State *L)
{
    charge(L, lua_gettop(L) >= 3 ? tab_len(L, 1) * CANLIN_SB_COST_ELEM + 1u : 1u);
    return call_orig(L);
}

static int sb_remove(lua_State *L)
{
    charge(L, lua_gettop(L) >= 2 ? tab_len(L, 1) * CANLIN_SB_COST_ELEM + 1u : 1u);
    return call_orig(L);
}

/* table.sort: about n * log2(n) comparisons and swaps. */
static int sb_sort(lua_State *L)
{
    lua_Unsigned n = tab_len(L, 1), log2n = 1, m;
    for (m = n; m > 1; m >>= 1)
        log2n++;
    if (n < (lua_Unsigned)1 << 40) /* larger: the original rejects it */
        charge(L, n * log2n * CANLIN_SB_COST_ELEM);
    return call_orig(L);
}

/* string.upper/lower/reverse: output length equals input length. */
static int sb_strmap(lua_State *L)
{
    size_t l;
    luaL_checklstring(L, 1, &l);
    charge(L, bytes_cost(l));
    return call_orig(L);
}

/* string.char: one output byte per argument. */
static int sb_char(lua_State *L)
{
    charge(L, (lua_Unsigned)lua_gettop(L) * CANLIN_SB_COST_ELEM);
    return call_orig(L);
}

/* table.unpack/string.byte: charged per result after the call; the result
 * count is bounded by the Lua stack limit, which the original enforces. */
static int sb_per_result(lua_State *L)
{
    int n = call_orig(L);
    charge(L, (lua_Unsigned)n * CANLIN_SB_COST_ELEM);
    return n;
}

/* string.sub/string.format: charged by result length after the call; the
 * work is bounded by the result size, which the memory limit bounds. */
static int sb_per_output(lua_State *L)
{
    size_t l = 0;
    int n = call_orig(L);
    if (n > 0 && lua_type(L, -n) == LUA_TSTRING)
        lua_tolstring(L, -n, &l);
    charge(L, bytes_cost(l));
    return n;
}

/* string.rep: the stock loop runs n times even for empty pieces, without
 * hooks. Reject results above the memory limit and charge n units. */
static int sb_rep(lua_State *L)
{
    canlin_sandbox *sb = sb_of(L);
    size_t l, lsep;
    lua_Integer n;
    luaL_checklstring(L, 1, &l);
    n = luaL_checkinteger(L, 2);
    luaL_optlstring(L, 3, "", &lsep);
    if (n <= 0 || l + lsep == 0) {
        lua_pushliteral(L, "");
        return 1;
    }
    if ((lua_Unsigned)n > sb->mem_limit || l + lsep > sb->mem_limit / (size_t)n)
        return luaL_error(L, "resulting string too large");
    charge(L, (lua_Unsigned)n);
    return call_orig(L);
}

/* table.move iterates e-f+1 times regardless of content, without hooks. */
static int sb_move(lua_State *L)
{
    lua_Integer f = luaL_checkinteger(L, 2);
    lua_Integer e = luaL_checkinteger(L, 3);
    if (e >= f)
        charge(L, (lua_Unsigned)e - (lua_Unsigned)f + 1u);
    return call_orig(L);
}

static void wrap_field(lua_State *L, const char *field, lua_CFunction fn)
{
    lua_getfield(L, -1, field);
    lua_pushcclosure(L, fn, 1);
    lua_setfield(L, -2, field);
}

static void clear_field(lua_State *L, const char *field)
{
    lua_pushnil(L);
    lua_setfield(L, -2, field);
}

static int open_env(lua_State *L)
{
    static const char *const base_removed[] = {"dofile", "loadfile", "warn", NULL};
    const char *const *p;

    luaL_requiref(L, LUA_GNAME, luaopen_base, 1);
    for (p = base_removed; *p; p++)
        clear_field(L, *p);
    lua_register(L, "pcall", sb_pcall);
    lua_register(L, "xpcall", sb_xpcall);
    lua_register(L, "load", sb_load);
    lua_register(L, "setmetatable", sb_setmetatable);
    lua_register(L, "collectgarbage", sb_collectgarbage);
    lua_register(L, "print", sb_print);
    lua_register(L, "require", sb_require);
    lua_pop(L, 1);

    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
    clear_field(L, "dump");
    wrap_field(L, "rep", sb_rep);
    wrap_field(L, "upper", sb_strmap);
    wrap_field(L, "lower", sb_strmap);
    wrap_field(L, "reverse", sb_strmap);
    wrap_field(L, "char", sb_char);
    wrap_field(L, "byte", sb_per_result);
    wrap_field(L, "sub", sb_per_output);
    wrap_field(L, "format", sb_per_output);
    lua_pop(L, 1);

    luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
    wrap_field(L, "move", sb_move);
    wrap_field(L, "concat", sb_concat);
    wrap_field(L, "unpack", sb_per_result);
    wrap_field(L, "sort", sb_sort);
    wrap_field(L, "insert", sb_insert);
    wrap_field(L, "remove", sb_remove);
    lua_pop(L, 1);

    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 1);
    lua_pop(L, 1);

    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, loaded_key);
    return 0;
}

/* ---- host API -------------------------------------------------------- */

static void set_errmsg(canlin_sandbox *sb, const char *msg)
{
    snprintf(sb->errmsg, sizeof sb->errmsg, "%s", msg);
}

canlin_sandbox *canlin_sandbox_new(const canlin_sandbox_config *cfg)
{
    canlin_sandbox *sb = calloc(1, sizeof *sb);
    if (sb == NULL)
        return NULL;
    sb->mem_limit = cfg && cfg->mem_limit ? cfg->mem_limit : CANLIN_SB_MEM_DEFAULT;
    if (sb->mem_limit > CANLIN_SB_MEM_MAX)
        sb->mem_limit = CANLIN_SB_MEM_MAX;
    sb->budget = cfg && cfg->budget ? cfg->budget : CANLIN_SB_BUDGET_DEFAULT;
    if (sb->budget > CANLIN_SB_BUDGET_MAX)
        sb->budget = CANLIN_SB_BUDGET_MAX;
    if (cfg) {
        sb->log = cfg->log;
        sb->require = cfg->require;
        sb->ud = cfg->ud;
    }
    sb->L = lua_newstate(sb_alloc, sb);
    if (sb->L == NULL) {
        free(sb);
        return NULL;
    }
    /* Setup runs before the hook is armed and with the full limit; failure
     * (e.g. limit too small) means no usable sandbox. */
    lua_pushcfunction(sb->L, open_env);
    if (lua_pcall(sb->L, 0, 0, 0) != LUA_OK) {
        lua_close(sb->L);
        free(sb);
        return NULL;
    }
    lua_gc(sb->L, LUA_GCCOLLECT, 0);
    set_errmsg(sb, "");
    return sb;
}

void canlin_sandbox_free(canlin_sandbox *sb)
{
    if (sb == NULL)
        return;
    if (sb->L)
        lua_close(sb->L);
    free(sb);
}

static canlin_sandbox_status sb_kill(canlin_sandbox *sb, canlin_sandbox_status why)
{
    set_errmsg(sb, why == CANLIN_SANDBOX_FAULT_MEMORY ? "memory limit exceeded"
                                                      : "instruction budget exceeded");
    sb->fault = why;
    sb->dead = 1;
    lua_close(sb->L); /* no __gc finalizers can exist; any __close handler
                         re-raises at once under the still-armed stride-1 hook */
    sb->L = NULL;
    sb->mem_used = 0;
    return why;
}

canlin_sandbox_status canlin_sandbox_run(canlin_sandbox *sb, const char *src, size_t len,
                                         const char *chunkname)
{
    lua_State *L;
    int status;

    if (sb->dead) {
        set_errmsg(sb, "scope is dead");
        return CANLIN_SANDBOX_DEAD;
    }
    L = sb->L;
    lua_settop(L, 0);
    sb->used = 0;
    arm_hook(L, CANLIN_SB_HOOK_STRIDE); /* also resets the hook counter */

    status = luaL_loadbufferx(L, src, len, chunkname ? chunkname : "=chunk", "t");
    if (status == LUA_OK)
        status = lua_pcall(L, 0, 0, 0);
    if (status == LUA_ERRMEM && sb->fault == CANLIN_SANDBOX_OK)
        set_fault(L, sb, CANLIN_SANDBOX_FAULT_MEMORY);
    if (sb->fault != CANLIN_SANDBOX_OK)
        return sb_kill(sb, sb->fault); /* hook stays armed at stride 1 */
    lua_sethook(L, NULL, 0, 0);
    if (status != LUA_OK) {
        if (lua_type(L, -1) == LUA_TSTRING)
            set_errmsg(sb, lua_tostring(L, -1));
        else
            snprintf(sb->errmsg, sizeof sb->errmsg, "(error object is a %s value)",
                     luaL_typename(L, -1));
        lua_settop(L, 0);
        return CANLIN_SANDBOX_ERROR;
    }
    lua_settop(L, 0);
    return CANLIN_SANDBOX_OK;
}

int canlin_sandbox_is_dead(const canlin_sandbox *sb)
{
    return sb->dead;
}

const char *canlin_sandbox_errmsg(const canlin_sandbox *sb)
{
    return sb->errmsg;
}

size_t canlin_sandbox_mem_used(const canlin_sandbox *sb)
{
    return sb->mem_used;
}
