/* CanLin module layer. See include/canlin/module.h for the contract.
 *
 * Isolation: every canlin.* closure carries this module as a light-userdata upvalue that
 * Lua cannot read (no debug library), and acts only on api_gen(m), the generation whose
 * Lua state is running. A Lua state belongs to exactly one generation: on reload/fault/
 * stop the state is closed before the module can be entered again, so a closure can
 * never act on a later generation.
 *
 * Staged reload (core API §6, canlin#16): the new generation's sandbox, lua#main and init
 * run while the old generation stays live in the scheduler (canlin_ev_module_stage) but
 * gets no deliveries. During that window `staged` is the generation the canlin.*
 * closures act on (only the new Lua state runs), while m->gen, m->sb and the input path
 * still belong to the old one. Commit retires the old generation; any failure aborts the
 * staged one and the old generation resumes exactly where it was.
 */
#include "canlin/module.h"
#include "canlin/sandbox_limits.h"
#include "canlin/ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

/* One ordered input edge in flight in the scheduler queue. */
typedef struct in_slot {
    canlin_ui_input in;
    unsigned char used;
} in_slot;

struct canlin_module {
    canlin_ev *ev;
    canlin_sandbox *sb;
    canlin_ev_gen gen;
    canlin_ev_gen staged;   /* generation being staged by a reload, else 0 */
    int reload_err;         /* code of the last failed staged reload, 0 if none */
    canlin_module_state state;
    unsigned errors;
    int in_call;
    int stop_pending;
    size_t mem_limit;
    uint32_t budget;
    canlin_sandbox_log_fn log;
    void *log_ud;
    canlin_sandbox_require_fn require;
    void *require_ud;
    char name[CANLIN_MODULE_NAME_MAX + 1];
    char version[CANLIN_MODULE_NAME_MAX + 1];
    char errmsg[CANLIN_MODULE_NAME_MAX + 320];
    canlin_ui_view *view; /* owned; outlives every Lua state of this module */

    /* Input routed through the scheduler (post_input -> EVENT -> view). An ordered edge
     * travels as a payload pointing into inpool; the scheduler releases it after delivery
     * or retirement, so a slot is never busy longer than its queue item. */
    in_slot inpool[CANLIN_EV_MOD_MAX_ITEMS];
    uint32_t in_epoch;      /* bumped when an edge is refused: older queued input is stale */
    uint32_t scroll_posted; /* cumulative scroll px posted (wraps), a latest-state value */
    uint32_t scroll_seen;   /* cumulative scroll px handed to the view */

    /* Core-owned one-shot timer for a time-driven redraw of the view (canlin#33); its
     * expiry requests a draw and is never delivered to Lua. 0 = none armed. */
    canlin_ev_handle ui_timer;
    uint64_t ui_timer_at;
};

/* ---- values ---- */

static int check_rec(const canlin_value *v, unsigned depth, size_t *entries, size_t *bytes)
{
    size_t k;
    int rc;

    switch (v->type) {
    case CANLIN_VALUE_NIL:
    case CANLIN_VALUE_BOOL:
    case CANLIN_VALUE_INT:
        return CANLIN_VALUE_OK;
    case CANLIN_VALUE_STR:
        if (v->s == NULL && v->len != 0)
            return CANLIN_VALUE_EINVAL;
        if (v->len > CANLIN_MARSHAL_MAX_BYTES - *bytes)
            return CANLIN_VALUE_ETOOBIG;
        *bytes += v->len;
        return CANLIN_VALUE_OK;
    case CANLIN_VALUE_TABLE:
        if (depth + 1 > CANLIN_MARSHAL_MAX_DEPTH)
            return CANLIN_VALUE_ETOOBIG;
        if (v->n > CANLIN_MARSHAL_MAX_ENTRIES - *entries)
            return CANLIN_VALUE_ETOOBIG;
        if (v->n != 0 && v->pairs == NULL)
            return CANLIN_VALUE_EINVAL;
        *entries += v->n;
        for (k = 0; k < v->n; k++) {
            const canlin_value_pair *p = &v->pairs[k];
            if (p->key.type != CANLIN_VALUE_INT && p->key.type != CANLIN_VALUE_STR)
                return CANLIN_VALUE_EINVAL;
            if ((rc = check_rec(&p->key, depth + 1, entries, bytes)) != CANLIN_VALUE_OK)
                return rc;
            if ((rc = check_rec(&p->val, depth + 1, entries, bytes)) != CANLIN_VALUE_OK)
                return rc;
        }
        return CANLIN_VALUE_OK;
    }
    return CANLIN_VALUE_EINVAL;
}

int canlin_value_check(const canlin_value *v)
{
    size_t entries = 0, bytes = 0;
    if (v == NULL)
        return CANLIN_VALUE_OK;
    return check_rec(v, 0, &entries, &bytes);
}

/* Budget units for building v in Lua (sandbox_limits.h). Only called on values that passed
 * canlin_value_check, so the recursion is depth-bounded and the sum cannot overflow. */
static size_t value_cost(const canlin_value *v)
{
    size_t k, cost = 0;
    if (v == NULL)
        return 0;
    if (v->type == CANLIN_VALUE_STR)
        return v->len / CANLIN_SB_COST_BYTES_PER_UNIT + 1u;
    if (v->type == CANLIN_VALUE_TABLE) {
        cost = v->n * CANLIN_SB_COST_ELEM;
        for (k = 0; k < v->n; k++)
            cost += value_cost(&v->pairs[k].key) + value_cost(&v->pairs[k].val);
    }
    return cost;
}

/* Only called on values that passed canlin_value_check (depth-bounded recursion). */
static void push_value(lua_State *L, const canlin_value *v)
{
    size_t k;
    if (v == NULL) {
        lua_pushnil(L);
        return;
    }
    switch (v->type) {
    case CANLIN_VALUE_BOOL:
        lua_pushboolean(L, v->i != 0);
        break;
    case CANLIN_VALUE_INT:
        lua_pushinteger(L, (lua_Integer)v->i);
        break;
    case CANLIN_VALUE_STR:
        lua_pushlstring(L, v->s ? v->s : "", v->len);
        break;
    case CANLIN_VALUE_TABLE:
        luaL_checkstack(L, 3, "payload");
        lua_createtable(L, 0, (int)v->n);
        for (k = 0; k < v->n; k++) {
            push_value(L, &v->pairs[k].key);
            push_value(L, &v->pairs[k].val);
            lua_rawset(L, -3);
        }
        break;
    default:
        lua_pushnil(L);
        break;
    }
}

/* ---- helpers ---- */

/* Length of a NUL-terminated string of at most max bytes, or -1. */
static long bounded_len(const char *s, size_t max)
{
    const char *z;
    if (s == NULL)
        return -1;
    z = memchr(s, 0, max + 1);
    return z ? (long)(z - s) : -1;
}

static void mlog(canlin_module *m, const char *what, const char *msg)
{
    char buf[sizeof m->errmsg];
    int n = snprintf(buf, sizeof buf, "module %s: %s: %s", m->name, what, msg);
    if (n < 0)
        return;
    if ((size_t)n >= sizeof buf)
        n = (int)sizeof buf - 1;
    if (m->log)
        m->log(m->log_ud, buf, (size_t)n);
}

static void set_err(canlin_module *m, const char *what, const char *msg)
{
    snprintf(m->errmsg, sizeof m->errmsg, "%s: %s", what, msg);
}

static void sb_log(void *ud, const char *msg, size_t len)
{
    canlin_module *m = ud;
    if (m->log)
        m->log(m->log_ud, msg, len);
}

static int sb_require(void *ud, const char *name, size_t name_len, const char **src,
                      size_t *len)
{
    canlin_module *m = ud;
    if (m->require == NULL)
        return 0;
    return m->require(m->require_ud, name, name_len, src, len);
}

/* Retire the generation and close its Lua state. Allowed inside an ev callback. */
static void end_gen(canlin_module *m, canlin_module_state state)
{
    if (m->gen)
        canlin_ev_module_stop(m->ev, m->gen);
    m->gen = 0;
    m->ui_timer = 0; /* retired with the generation */
    canlin_sandbox_free(m->sb);
    m->sb = NULL;
    m->state = state;
    m->errors = 0;
    m->stop_pending = 0;
}

static void fault(canlin_module *m, const char *what, const char *why)
{
    set_err(m, what, why);
    mlog(m, "faulted", m->errmsg);
    end_gen(m, CANLIN_MODULE_FAULTED);
}

/* ---- Lua surface: canlin.* ---- */

static canlin_module *mod_of(lua_State *L)
{
    return lua_touserdata(L, lua_upvalueindex(1));
}

/* The generation whose Lua is running: the staged one while a reload stages (the old
 * Lua state is never entered then), otherwise the live one. */
static canlin_ev_gen api_gen(const canlin_module *m)
{
    return m->staged ? m->staged : m->gen;
}

static int fail(lua_State *L, const char *code, const char *msg)
{
    lua_pushnil(L);
    lua_pushstring(L, code);
    lua_pushstring(L, msg);
    return 3;
}

static int l_timer_after(lua_State *L)
{
    canlin_module *m = mod_of(L);
    lua_Integer ms = luaL_checkinteger(L, 1);
    canlin_ev_handle h;
    int rc;

    if (ms < 0 || (lua_Unsigned)ms > (lua_Unsigned)CANLIN_TIMER_MAX_MS)
        return fail(L, "invalid", "timer delay out of range");
    rc = canlin_ev_timer_start(m->ev, api_gen(m), (canlin_ev_time)ms, &h);
    switch (rc) {
    case CANLIN_EV_OK:
        lua_pushinteger(L, (lua_Integer)h);
        return 1;
    case CANLIN_EV_EFULL:
        return fail(L, "full", "timer limit reached");
    case CANLIN_EV_ELIMIT:
        return fail(L, "unavailable", "handle space exhausted");
    default:
        return fail(L, "stale", "generation is not live");
    }
}

static int l_cancel(lua_State *L)
{
    canlin_module *m = mod_of(L);
    lua_Integer h = luaL_checkinteger(L, 1);
    if (m->ui_timer != 0 && (canlin_ev_handle)h == m->ui_timer)
        return fail(L, "stale", "handle unknown, foreign or dead"); /* core-owned */
    switch (canlin_ev_cancel(m->ev, api_gen(m), (canlin_ev_handle)h)) {
    case CANLIN_EV_OK:
        lua_pushliteral(L, "cancelled");
        return 1;
    case CANLIN_EV_SUPPRESSED:
        lua_pushliteral(L, "suppressed");
        return 1;
    case CANLIN_EV_ALREADY:
        lua_pushliteral(L, "already");
        return 1;
    default:
        return fail(L, "stale", "handle unknown, foreign or dead");
    }
}

static int l_invalidate(lua_State *L)
{
    canlin_module *m = mod_of(L);
    canlin_ev_invalidate(m->ev, api_gen(m));
    return 0;
}

static int l_now(lua_State *L)
{
    canlin_module *m = mod_of(L);
    lua_pushinteger(L, (lua_Integer)canlin_ev_now(m->ev));
    return 1;
}

static void set_fn(lua_State *L, canlin_module *m, const char *name, lua_CFunction fn)
{
    lua_pushlightuserdata(L, m);
    lua_pushcclosure(L, fn, 1);
    lua_setfield(L, -2, name);
}

static int install_api(lua_State *L, void *ud)
{
    canlin_module *m = ud;
    lua_createtable(L, 0, 4); /* canlin */
    lua_createtable(L, 0, 1);
    set_fn(L, m, "after", l_timer_after);
    lua_setfield(L, -2, "timer");
    set_fn(L, m, "cancel", l_cancel);
    set_fn(L, m, "invalidate", l_invalidate);
    lua_createtable(L, 0, 1);
    set_fn(L, m, "now", l_now);
    lua_setfield(L, -2, "sys");
    lua_setglobal(L, "canlin");
    return 0;
}

/* ---- callback arguments ---- */

static int ctx_newindex(lua_State *L)
{
    return luaL_error(L, "ctx is read-only");
}

static int push_ctx(lua_State *L, void *ud)
{
    canlin_module *m = ud;
    lua_createtable(L, 0, 0); /* proxy */
    lua_createtable(L, 0, 3); /* metatable */
    lua_createtable(L, 0, 3); /* data */
    lua_pushstring(L, m->name);
    lua_setfield(L, -2, "name");
    lua_pushstring(L, m->version);
    lua_setfield(L, -2, "version");
    lua_pushinteger(L, (lua_Integer)api_gen(m));
    lua_setfield(L, -2, "generation");
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, ctx_newindex);
    lua_setfield(L, -2, "__newindex");
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "__metatable"); /* hides the data table, blocks setmetatable */
    lua_setmetatable(L, -2);
    return 1;
}

enum { ARG_VALUE, ARG_TIMER, ARG_DONE_OK, ARG_DONE_FAIL, ARG_UI };

typedef struct {
    int kind;
    const char *name;
    canlin_ev_handle h;
    const canlin_value *v;
    const char *code;
    const char *msg;
    size_t msglen;
    canlin_ui_view *view; /* ARG_UI: the open frame's view, NULL = headless */
} call_args;

static int push_args(lua_State *L, void *ud)
{
    call_args *a = ud;
    /* Marshalling runs inside the budgeted callback (budget already reset): charge the
     * payload before building it; a payload that does not fit faults the scope. */
    if (a->kind == ARG_VALUE || a->kind == ARG_DONE_OK)
        canlin_sandbox_charge(L, value_cost(a->v));
    if (a->kind == ARG_UI)
        return canlin_ui_push(L, a->view); /* UI spec §3; charges its construction */
    lua_pushstring(L, a->name);
    if (a->kind == ARG_VALUE) {
        push_value(L, a->v);
        return 2;
    }
    lua_createtable(L, 0, 4);
    lua_pushinteger(L, (lua_Integer)a->h);
    lua_setfield(L, -2, "handle");
    if (a->kind == ARG_DONE_OK) {
        lua_pushboolean(L, 1);
        lua_setfield(L, -2, "ok");
        push_value(L, a->v);
        lua_setfield(L, -2, "result");
    } else if (a->kind == ARG_DONE_FAIL) {
        lua_pushboolean(L, 0);
        lua_setfield(L, -2, "ok");
        lua_pushstring(L, a->code);
        lua_setfield(L, -2, "code");
        lua_pushlstring(L, a->msg, a->msglen);
        lua_setfield(L, -2, "message");
    }
    return 2;
}

static const char *const result_codes[] = {"full", "invalid", "unsupported", "denied",
                                           "unavailable", "stale", NULL};

static int known_code(const char *code)
{
    const char *const *p;
    if (code == NULL || bounded_len(code, 16) < 0)
        return 0;
    for (p = result_codes; *p; p++)
        if (strcmp(*p, code) == 0)
            return 1;
    return 0;
}

static void done_fail(call_args *a, const char *code, const char *msg)
{
    long n;
    a->kind = ARG_DONE_FAIL;
    a->code = code;
    a->msg = msg ? msg : "";
    n = bounded_len(a->msg, CANLIN_SB_ERRMSG_MAX);
    /* Diagnostic text only: bounded, never parsed. */
    a->msglen = n < 0 ? CANLIN_SB_ERRMSG_MAX : (size_t)n;
}

static void map_op(const canlin_ev_delivery *d, call_args *a)
{
    const canlin_module_result *r = d->payload;
    a->name = "done";
    a->h = d->handle;
    if (d->status == CANLIN_EV_SUCCESS) {
        const canlin_value *v = r ? r->value : NULL;
        int rc = canlin_value_check(v);
        if (rc == CANLIN_VALUE_ETOOBIG)
            done_fail(a, "full", "result exceeds the marshalling limits");
        else if (rc != CANLIN_VALUE_OK)
            done_fail(a, "invalid", "result is not a marshallable value");
        else {
            a->kind = ARG_DONE_OK;
            a->v = v;
        }
    } else if (d->detail == CANLIN_EV_DETAIL_OVERSIZE) {
        done_fail(a, "full", "result exceeded its reserved size");
    } else if (r && known_code(r->code)) {
        done_fail(a, r->code, r->message);
    } else {
        done_fail(a, "unavailable", "operation failed");
    }
}

/* ---- lifecycle ---- */

static int copy_meta(canlin_module *m, const canlin_module_source *src)
{
    long nl = bounded_len(src->name, CANLIN_MODULE_NAME_MAX);
    long vl = bounded_len(src->version ? src->version : "", CANLIN_MODULE_NAME_MAX);
    if (nl <= 0 || vl < 0 || src->main == NULL)
        return CANLIN_MODULE_EINVAL;
    memcpy(m->name, src->name, (size_t)nl + 1);
    memcpy(m->version, src->version ? src->version : "", (size_t)vl + 1);
    m->require = src->require;
    m->require_ud = src->require_ud;
    return CANLIN_MODULE_OK;
}

static int src_valid(const canlin_module_source *src)
{
    return src != NULL && bounded_len(src->name, CANLIN_MODULE_NAME_MAX) > 0 &&
           (src->version == NULL || bounded_len(src->version, CANLIN_MODULE_NAME_MAX) >= 0) &&
           src->main != NULL;
}

static canlin_sandbox *new_sandbox(canlin_module *m)
{
    canlin_sandbox_config sc;
    memset(&sc, 0, sizeof sc);
    sc.mem_limit = m->mem_limit;
    sc.budget = m->budget;
    sc.log = sb_log;
    sc.require = sb_require;
    sc.ud = m;
    return canlin_sandbox_new(&sc);
}

/* Install canlin.*, run lua#main and M.init in sb, each under its budget. The generation
 * the closures act on (api_gen) must already exist in the scheduler. On failure *what
 * names the step; the sandbox's errmsg says why. */
static int boot(canlin_module *m, canlin_sandbox *sb, const canlin_module_source *src,
                const char **what)
{
    int called, rc = CANLIN_MODULE_OK;

    m->in_call = 1;
    if (canlin_sandbox_host(sb, install_api, m) != CANLIN_SANDBOX_OK) {
        *what = "install";
        rc = CANLIN_MODULE_ENOMEM;
    } else if (canlin_sandbox_load_main(sb, src->main, src->main_len, "=lua#main") !=
               CANLIN_SANDBOX_OK) {
        *what = "lua#main";
        rc = CANLIN_MODULE_ELOAD;
    } else if (canlin_sandbox_call_budget(sb, "init", 0, 1, push_ctx, m, &called) !=
               CANLIN_SANDBOX_OK) {
        *what = "init";
        rc = CANLIN_MODULE_EINIT;
    }
    m->in_call = 0;
    return rc;
}

/* Start a generation in a free scheduler slot: nothing is live (first load, or restart
 * of a stopped/faulted module). A failure leaves the module FAULTED. */
static int start_gen(canlin_module *m, const canlin_module_source *src)
{
    canlin_sandbox *sb;
    canlin_ev_gen g;
    const char *what = "";
    int rc;

    if (!src_valid(src))
        return CANLIN_MODULE_EINVAL;
    sb = new_sandbox(m);
    if (sb == NULL)
        return CANLIN_MODULE_ENOMEM;
    if (canlin_ev_module_start(m->ev, &g) < 0) {
        canlin_sandbox_free(sb);
        return CANLIN_MODULE_EEV;
    }
    canlin_ui_view_reset(m->view); /* widget state belongs to one generation */
    m->scroll_seen = m->scroll_posted;
    m->sb = sb;
    m->gen = g;
    m->ui_timer = 0;
    m->errors = 0;
    m->stop_pending = 0;
    m->reload_err = 0;
    copy_meta(m, src);
    m->state = CANLIN_MODULE_RUNNING;

    rc = boot(m, sb, src, &what);
    if (rc != CANLIN_MODULE_OK) {
        fault(m, what, canlin_sandbox_errmsg(sb));
        return rc;
    }
    canlin_ev_invalidate(m->ev, m->gen); /* first frame */
    return CANLIN_MODULE_OK;
}

/* Staged reload of a RUNNING module (core API §6). The old generation keeps its Lua
 * state, view, timers, operations and queued work, and gets no deliveries until the
 * outcome is known. Staging is one synchronous call on the owner thread, so no turn runs
 * in between; work that reaches the old generation meanwhile (a producer completing,
 * input posted by the backend) is queued for it as usual: delivered after an abort,
 * retired with it after a commit, where the view reset releases held input exactly like
 * the input-overflow reset. */
static int stage_reload(canlin_module *m, const canlin_module_source *src)
{
    char name[sizeof m->name], version[sizeof m->version];
    char why[sizeof m->errmsg - sizeof "reload: " + 1]; /* fits set_err(m, "reload", why) */
    canlin_sandbox_require_fn require = m->require;
    void *require_ud = m->require_ud;
    canlin_sandbox *sb;
    canlin_ev_gen g;
    const char *what = "";
    int rc;

    if (!src_valid(src))
        return CANLIN_MODULE_EINVAL;
    sb = new_sandbox(m);
    if (sb == NULL) {
        rc = CANLIN_MODULE_ENOMEM;
        set_err(m, "reload", "sandbox could not be created");
        goto refused;
    }
    rc = canlin_ev_module_stage(m->ev, m->gen, &g);
    if (rc < 0) {
        canlin_sandbox_free(sb);
        if (rc == CANLIN_EV_EFULL) {
            rc = CANLIN_MODULE_EFULL;
            set_err(m, "reload", "full: no scheduler room for a second generation");
        } else {
            rc = CANLIN_MODULE_EEV;
            set_err(m, "reload", "scheduler refused a new generation");
        }
        goto refused;
    }

    memcpy(name, m->name, sizeof name);
    memcpy(version, m->version, sizeof version);
    copy_meta(m, src); /* ctx, log lines and require of the new generation */
    m->staged = g;
    rc = boot(m, sb, src, &what);
    m->staged = 0;

    if (rc == CANLIN_MODULE_OK && !m->stop_pending) {
        /* Commit: event runtime §7 order for the old generation, then its Lua state. */
        canlin_ev_module_commit(m->ev, g);
        canlin_sandbox_free(m->sb);
        canlin_ui_view_reset(m->view); /* widget state belongs to one generation */
        m->scroll_seen = m->scroll_posted; /* old-generation scroll was retired with it */
        m->sb = sb;
        m->gen = g;
        m->ui_timer = 0; /* the old generation's view timer was retired with it */
        m->errors = 0;
        m->reload_err = 0;
        canlin_ev_invalidate(m->ev, m->gen); /* first frame */
        return CANLIN_MODULE_OK;
    }

    /* Rollback: retire everything the new generation admitted, close its Lua state and
     * give the old generation its metadata back. */
    if (rc == CANLIN_MODULE_OK)
        snprintf(why, sizeof why, "stopped while staging");
    else
        snprintf(why, sizeof why, "%s: %s", what, canlin_sandbox_errmsg(sb));
    canlin_ev_module_abort(m->ev, g);
    canlin_sandbox_free(sb);
    memcpy(m->name, name, sizeof name);
    memcpy(m->version, version, sizeof version);
    m->require = require;
    m->require_ud = require_ud;
    set_err(m, "reload", why);
    if (m->stop_pending) {
        /* canlin_module_stop was called while staging: it wins over both generations. */
        end_gen(m, CANLIN_MODULE_STOPPED);
        return CANLIN_MODULE_ESTALE;
    }
refused:
    m->reload_err = rc;
    mlog(m, "reload failed, old generation kept", m->errmsg);
    return rc;
}

canlin_module *canlin_module_load(canlin_ev *ev, const canlin_module_source *src,
                                  const canlin_sandbox_config *cfg, int *err)
{
    canlin_module *m;
    int rc;

    if (ev == NULL || src == NULL) {
        if (err)
            *err = CANLIN_MODULE_EINVAL;
        return NULL;
    }
    m = calloc(1, sizeof *m);
    if (m == NULL) {
        if (err)
            *err = CANLIN_MODULE_ENOMEM;
        return NULL;
    }
    m->ev = ev;
    m->state = CANLIN_MODULE_STOPPED;
    if (cfg) {
        m->mem_limit = cfg->mem_limit;
        m->budget = cfg->budget;
        m->log = cfg->log;
        m->log_ud = cfg->ud;
    }
    rc = copy_meta(m, src); /* name for log lines, even when start fails */
    if (rc == CANLIN_MODULE_OK)
        rc = start_gen(m, src);
    if (rc != CANLIN_MODULE_OK) {
        end_gen(m, CANLIN_MODULE_FAULTED);
        free(m);
        if (err)
            *err = rc;
        return NULL;
    }
    if (err)
        *err = CANLIN_MODULE_OK;
    return m;
}

int canlin_module_reload(canlin_module *m, const canlin_module_source *src)
{
    if (m == NULL)
        return CANLIN_MODULE_EINVAL;
    if (m->in_call)
        return CANLIN_MODULE_EBUSY;
    if (m->state == CANLIN_MODULE_RUNNING)
        return stage_reload(m, src);
    end_gen(m, m->state); /* restart: nothing live to fall back to, plain start */
    return start_gen(m, src);
}

void canlin_module_stop(canlin_module *m)
{
    if (m == NULL)
        return;
    if (m->in_call) {
        m->stop_pending = 1; /* never close a Lua state from inside its own callback */
        return;
    }
    if (m->state == CANLIN_MODULE_RUNNING)
        end_gen(m, CANLIN_MODULE_STOPPED);
}

void canlin_module_free(canlin_module *m)
{
    if (m == NULL)
        return;
    m->in_call = 0;
    canlin_module_stop(m);
    canlin_sandbox_free(m->sb);
    canlin_ui_view_free(m->view); /* after the Lua state: ui closures point at it */
    free(m);
}

canlin_module_state canlin_module_get_state(const canlin_module *m)
{
    return m->state;
}

canlin_ev_gen canlin_module_gen(const canlin_module *m)
{
    return m->state == CANLIN_MODULE_RUNNING ? m->gen : 0;
}

int canlin_module_reload_error(const canlin_module *m)
{
    return m->reload_err;
}

unsigned canlin_module_errors(const canlin_module *m)
{
    return m->errors;
}

const char *canlin_module_errmsg(const canlin_module *m)
{
    return m->errmsg;
}

/* ---- delivery ---- */

static void consume_control(canlin_module *m, unsigned bits)
{
    if (bits & CANLIN_EV_CTL_FAULT)
        fault(m, "control", "fault signalled");
    else if (bits & CANLIN_EV_CTL_STOP)
        end_gen(m, CANLIN_MODULE_STOPPED);
    else if ((bits & CANLIN_EV_CTL_INPUT_OVERFLOW) && m->view) {
        /* An input edge was refused (event runtime §5): release what is held and cancel
         * gestures; the queued input from before the gap is stale (in_epoch) and dropped
         * on delivery. The release needs a frame. */
        canlin_ui_view_input_reset(m->view);
        canlin_ev_invalidate(m->ev, m->gen);
    }
    /* CONN_LOST: consumed; transports send their own stream events (spec §5.2). */
}

/* Scheduler type of an input item: input type in the low byte, epoch above it. */
#define IN_TYPE(epoch, type) ((((uint32_t)(epoch) & 0xffffffu) << 8) | (uint32_t)(type))

/* An input delivery: feed the view, request a draw. Never enters Lua (spec §5.2). */
static void deliver_input(canlin_module *m, const canlin_ev_delivery *d)
{
    canlin_ui_input e;
    uint32_t type = d->type & 0xffu;

    if (m->view == NULL || (d->type >> 8) != (m->in_epoch & 0xffffffu)) {
        /* Before an overflow gap: the reset already covers it. Stale scroll is dropped
         * too, not carried into the next delta. */
        if (type == CANLIN_UI_IN_SCROLL)
            m->scroll_seen = (uint32_t)d->a;
        return;
    }
    memset(&e, 0, sizeof e);
    if (type == CANLIN_UI_IN_MOTION) {
        e.type = CANLIN_UI_IN_MOTION;
        e.x = d->a;
        e.y = d->b;
        e.t = canlin_ev_now(m->ev); /* coalesced: no edge time; nothing reads it */
    } else if (type == CANLIN_UI_IN_SCROLL) {
        e.type = CANLIN_UI_IN_SCROLL;
        e.dy = (int)(int32_t)((uint32_t)d->a - m->scroll_seen);
        m->scroll_seen = (uint32_t)d->a;
        e.t = canlin_ev_now(m->ev);
        if (e.dy == 0)
            return;
    } else if (d->payload != NULL && d->bytes == sizeof(in_slot)) {
        e = ((const in_slot *)d->payload)->in; /* stamped at post time */
    } else {
        return;
    }
    canlin_ui_view_input(m->view, &e); /* EFULL resets the view itself */
    canlin_ev_invalidate(m->ev, m->gen);
}

/* After a frame: keep exactly one core timer armed for the view's wake time, or none.
 * If the scheduler refuses it (timer quota), the feedback waits for the next input. */
static void ui_timer_sync(canlin_module *m)
{
    uint64_t at, now;
    int want = canlin_ui_view_wake(m->view, &at);
    if (m->ui_timer != 0 && (!want || at != m->ui_timer_at)) {
        canlin_ev_cancel(m->ev, m->gen, m->ui_timer);
        m->ui_timer = 0;
    }
    if (!want || m->ui_timer != 0)
        return;
    now = canlin_ev_now(m->ev);
    if (canlin_ev_timer_start(m->ev, m->gen, at > now ? at - now : 0, &m->ui_timer) !=
        CANLIN_EV_OK)
        m->ui_timer = 0;
    m->ui_timer_at = at;
}

canlin_sandbox_status canlin_module_deliver(canlin_module *m, const canlin_ev_delivery *d)
{
    call_args a;
    const char *cb = "event";
    uint32_t budget = 0;
    canlin_sandbox_status st;
    int called = 0;

    if (m == NULL || d == NULL || m->state != CANLIN_MODULE_RUNNING || m->sb == NULL ||
        d->gen != m->gen || m->in_call)
        return CANLIN_SANDBOX_DEAD;

    memset(&a, 0, sizeof a);
    switch (d->kind) {
    case CANLIN_EV_D_CONTROL:
        consume_control(m, d->control);
        return CANLIN_SANDBOX_OK;
    case CANLIN_EV_D_TIMER:
        if (m->ui_timer != 0 && d->handle == m->ui_timer) {
            m->ui_timer = 0; /* the view's redraw time: a draw, never M.event */
            canlin_ev_invalidate(m->ev, m->gen);
            return CANLIN_SANDBOX_OK;
        }
        a.kind = ARG_TIMER;
        a.name = "timer";
        a.h = d->handle;
        break;
    case CANLIN_EV_D_OP:
        map_op(d, &a);
        break;
    case CANLIN_EV_D_EVENT: {
        const canlin_module_event *e = d->payload;
        long nl;
        if (d->source == CANLIN_MODULE_SOURCE_INPUT) {
            deliver_input(m, d);
            return CANLIN_SANDBOX_OK;
        }
        if (d->source != CANLIN_MODULE_SOURCE_EVENT)
            return CANLIN_SANDBOX_OK; /* other sources are consumed by the core */
        nl = e ? bounded_len(e->name, CANLIN_MODULE_NAME_MAX) : -1;
        if (nl <= 0 || strcmp(e->name, "timer") == 0 || strcmp(e->name, "done") == 0) {
            mlog(m, "event", "dropped: invalid or reserved event name");
            return CANLIN_SANDBOX_OK;
        }
        if (canlin_value_check(e->value) != CANLIN_VALUE_OK) {
            mlog(m, "event", "dropped: payload exceeds the marshalling limits");
            return CANLIN_SANDBOX_OK;
        }
        a.kind = ARG_VALUE;
        a.name = e->name;
        a.v = e->value;
        break;
    }
    case CANLIN_EV_D_DRAW:
        a.kind = ARG_UI;
        cb = "draw";
        budget = CANLIN_SB_BUDGET_DRAW;
        if (m->view) {
            int rc;
            canlin_ui_view_set_now(m->view, canlin_ev_now(m->ev));
            rc = canlin_ui_view_frame_begin(m->view);
            if (rc != CANLIN_UI_OK) {
                /* Not the module's fault (font cache pinned or out of memory): skip this
                 * draw without calling Lua or counting an error; the previous frame
                 * stays on screen and the next invalidation retries. */
                const char *why = rc == CANLIN_UI_EFONT
                                      ? "skipped: every font size slot is pinned"
                                      : "skipped: view frame could not start";
                mlog(m, "draw", why);
                set_err(m, "draw", why);
                return CANLIN_SANDBOX_OK;
            }
            a.view = m->view;
        }
        break;
    default:
        return CANLIN_SANDBOX_OK;
    }

    m->in_call = 1;
    st = canlin_sandbox_call_budget(m->sb, cb, budget, a.kind == ARG_UI ? 1 : 2, push_args,
                                    &a, &called);
    m->in_call = 0;
    if (a.view)
        canlin_ui_view_frame_end(a.view); /* touches no Lua state; ui is dead from here */

    switch (st) {
    case CANLIN_SANDBOX_OK:
        if (called)
            m->errors = 0;
        break;
    case CANLIN_SANDBOX_ERROR:
        mlog(m, cb, canlin_sandbox_errmsg(m->sb));
        set_err(m, cb, canlin_sandbox_errmsg(m->sb));
        if (++m->errors >= CANLIN_MODULE_MAX_ERRORS)
            fault(m, cb, "too many consecutive callback errors");
        break;
    default: /* FAULT_BUDGET, FAULT_MEMORY (DEAD cannot happen: sb is live) */
        fault(m, cb, canlin_sandbox_errmsg(m->sb));
        break;
    }
    if (m->stop_pending && m->state == CANLIN_MODULE_RUNNING)
        end_gen(m, CANLIN_MODULE_STOPPED);
    if (a.view && m->state == CANLIN_MODULE_RUNNING && canlin_ui_view_pending(a.view))
        canlin_ev_invalidate(m->ev, m->gen); /* input left for the next frame */
    if (a.view && m->state == CANLIN_MODULE_RUNNING)
        ui_timer_sync(m);
    return st;
}

/* ---- UI view (UI spec §2–§3) ---- */

static void view_dirty(void *ud)
{
    canlin_module *m = ud;
    if (m->state == CANLIN_MODULE_RUNNING)
        canlin_ev_invalidate(m->ev, m->gen);
}

int canlin_module_set_ui(canlin_module *m, canlin_ui *ui, canlin_ui_output *out, int x, int y,
                         int w, int h)
{
    int rc;
    if (m == NULL || ui == NULL || out == NULL || w <= 0 || h <= 0)
        return CANLIN_MODULE_EINVAL;
    if (m->in_call)
        return CANLIN_MODULE_EBUSY;
    if (m->view == NULL) {
        m->view = canlin_ui_view_new(ui, out, &rc);
        if (m->view == NULL)
            return rc == CANLIN_UI_EINVAL ? CANLIN_MODULE_EINVAL : CANLIN_MODULE_ENOMEM;
        canlin_ui_view_set_dirty_fn(m->view, view_dirty, m);
    } else {
        rc = canlin_ui_view_bind(m->view, ui, out);
        if (rc != CANLIN_UI_OK)
            return rc == CANLIN_UI_EINVAL  ? CANLIN_MODULE_EINVAL
                   : rc == CANLIN_UI_EBUSY ? CANLIN_MODULE_EBUSY
                                           : CANLIN_MODULE_ENOMEM;
    }
    canlin_ui_view_set_rect(m->view, x, y, w, h);
    view_dirty(m);
    return CANLIN_MODULE_OK;
}

canlin_ui_view *canlin_module_view(canlin_module *m)
{
    return m ? m->view : NULL;
}

static void in_release(void *payload, size_t bytes)
{
    (void)bytes;
    ((in_slot *)payload)->used = 0;
}

/* Refused ordered edge: everything queued before it is stale, the view is reset when the
 * latch is consumed (event runtime §5, core API §5.2). */
static int input_gap(canlin_module *m)
{
    m->in_epoch++;
    canlin_ev_signal(m->ev, m->gen, CANLIN_EV_CTL_INPUT_OVERFLOW);
    return CANLIN_MODULE_EFULL;
}

int canlin_module_post_input(canlin_module *m, const canlin_ui_input *in)
{
    uint32_t type;
    unsigned k;
    in_slot *slot = NULL;
    int rc;

    if (m == NULL || m->view == NULL || !canlin_ui_input_valid(in))
        return CANLIN_MODULE_EINVAL;
    if (m->state != CANLIN_MODULE_RUNNING)
        return CANLIN_MODULE_ESTALE;
    type = IN_TYPE(m->in_epoch, in->type);

    if (in->type == CANLIN_UI_IN_MOTION) {
        rc = canlin_ev_post_latest(m->ev, m->gen, CANLIN_MODULE_SOURCE_INPUT, type,
                                   (int32_t)in->x, (int32_t)in->y);
        return rc == CANLIN_EV_OK ? CANLIN_MODULE_OK : CANLIN_MODULE_EFULL;
    }
    if (in->type == CANLIN_UI_IN_SCROLL) {
        /* Scroll deltas become a cumulative position so coalescing loses nothing; the
         * pending (undelivered) amount stays within +-2^30 px. */
        int64_t pend = (int64_t)(int32_t)(m->scroll_posted - m->scroll_seen) + in->dy;
        uint32_t next;
        if (pend > (1 << 30))
            pend = 1 << 30;
        if (pend < -(1 << 30))
            pend = -(1 << 30);
        next = m->scroll_seen + (uint32_t)(int32_t)pend;
        rc = canlin_ev_post_latest(m->ev, m->gen, CANLIN_MODULE_SOURCE_INPUT, type,
                                   (int32_t)next, 0);
        if (rc != CANLIN_EV_OK)
            return CANLIN_MODULE_EFULL; /* not an edge: nothing held depends on it */
        m->scroll_posted = next;
        return CANLIN_MODULE_OK;
    }

    for (k = 0; k < CANLIN_EV_MOD_MAX_ITEMS; k++)
        if (!m->inpool[k].used) {
            slot = &m->inpool[k];
            break;
        }
    if (slot == NULL)
        return input_gap(m);
    slot->in = *in;
    slot->in.t = canlin_ev_now(m->ev); /* post time: the user's timing, not the queue's */
    slot->used = 1;
    rc = canlin_ev_post(m->ev, m->gen, CANLIN_MODULE_SOURCE_INPUT, type, 0, 0, slot,
                        sizeof *slot, in_release);
    if (rc != CANLIN_EV_OK) {
        slot->used = 0;
        return input_gap(m);
    }
    return CANLIN_MODULE_OK;
}
