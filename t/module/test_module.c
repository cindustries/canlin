/* Module layer contract tests (core API spec §4–§7): real sandbox, real scheduler, fake
 * clock, no sleeps. Exits 0 on success. */
#include "canlin/module.h"
#include "canlin/sandbox_limits.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;

#define CHECK(c)                                                                   \
    do {                                                                           \
        checks++;                                                                  \
        if (!(c)) {                                                                \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c);  \
            failures++;                                                            \
        }                                                                          \
    } while (0)

/* ---- harness ---- */

#define NMOD 4
#define LOGMAX 16384

typedef struct logbuf {
    char buf[LOGMAX];
    size_t len;
} logbuf;

typedef struct harness harness;
struct harness {
    canlin_ev_fake_clock clock;
    canlin_ev *ev;
    canlin_module *m[NMOD];
    logbuf log[NMOD];
    unsigned orphans; /* deliveries for a generation no module owns */
    unsigned turn;
    void (*hook)(harness *h, int i, const canlin_ev_delivery *d);
    canlin_ev_handle op; /* op test */
    int op_completed_turn;
};

static void log_fn(void *ud, const char *msg, size_t len)
{
    logbuf *l = ud;
    if (len + 1 > sizeof l->buf - l->len - 1)
        return;
    memcpy(l->buf + l->len, msg, len);
    l->len += len;
    l->buf[l->len++] = '\n';
    l->buf[l->len] = 0;
}

static void on_deliver(void *ud, canlin_ev *ev, const canlin_ev_delivery *d)
{
    harness *h = ud;
    int i;
    (void)ev;
    for (i = 0; i < NMOD; i++) {
        if (h->m[i] && canlin_module_gen(h->m[i]) == d->gen) {
            if (h->hook)
                h->hook(h, i, d);
            canlin_module_deliver(h->m[i], d);
            return;
        }
    }
    h->orphans++;
}

static void h_init(harness *h)
{
    canlin_ev_clock c;
    canlin_ev_callbacks cb;
    memset(h, 0, sizeof *h);
    c = canlin_ev_fake_clock_bind(&h->clock);
    cb.deliver = on_deliver;
    h->ev = canlin_ev_new(&c, &cb, h);
    if (!h->ev) {
        fprintf(stderr, "canlin_ev_new failed\n");
        exit(1);
    }
}

static void h_done(harness *h)
{
    int i;
    canlin_ev_stats st;
    CHECK(h->orphans == 0);
    for (i = 0; i < NMOD; i++)
        canlin_module_free(h->m[i]);
    canlin_ev_get_stats(h->ev, -1, &st);
    CHECK(st.items == 0 && st.bytes == 0 && st.ops == 0 && st.timers == 0);
    CHECK(canlin_ev_free(h->ev) == CANLIN_EV_OK);
}

static int h_load(harness *h, int i, const char *name, const char *src, uint32_t budget)
{
    canlin_module_source s;
    canlin_sandbox_config cfg;
    int err = 12345;
    memset(&s, 0, sizeof s);
    memset(&cfg, 0, sizeof cfg);
    s.name = name;
    s.version = "1.0";
    s.main = src;
    s.main_len = strlen(src);
    cfg.budget = budget;
    cfg.log = log_fn;
    cfg.ud = &h->log[i];
    h->m[i] = canlin_module_load(h->ev, &s, &cfg, &err);
    CHECK((h->m[i] != NULL) == (err == CANLIN_MODULE_OK));
    return err;
}

static void h_turn(harness *h)
{
    canlin_ev_turn_result r;
    h->turn++;
    CHECK(canlin_ev_turn(h->ev, &r) == CANLIN_EV_OK);
}

static void h_drain(harness *h)
{
    int k;
    for (k = 0; k < 64; k++)
        h_turn(h);
}

static unsigned count(const logbuf *l, const char *needle)
{
    unsigned n = 0;
    const char *p = l->buf;
    while ((p = strstr(p, needle)) != NULL) {
        n++;
        p += strlen(needle);
    }
    return n;
}

static int has(const logbuf *l, const char *needle)
{
    return strstr(l->buf, needle) != NULL;
}

static void post_event(harness *h, int i, const canlin_module_event *e)
{
    CHECK(canlin_ev_post(h->ev, canlin_module_gen(h->m[i]), CANLIN_MODULE_SOURCE_EVENT, 0, 0,
                         0, (void *)e, 0, NULL) == CANLIN_EV_OK);
}

/* ---- tests ---- */

static const char timer_src[] =
    "local M = {}\n"
    "local n = 0\n"
    "function M.init(ctx)\n"
    "  print('init', ctx.name, ctx.version, math.type(ctx.generation))\n"
    "  print('ro', pcall(function() ctx.name = 'x' end), getmetatable(ctx))\n"
    "  print('bad', canlin.timer.after(-1))\n"
    "  print('big', canlin.timer.after(" "%u" " + 1))\n"
    "  print('type', pcall(canlin.timer.after, {}))\n"
    "  assert(math.type(canlin.timer.after(100)) == 'integer')\n"
    "end\n"
    "function M.event(name, p)\n"
    "  if name == 'timer' then\n"
    "    n = n + 1\n"
    "    print('tick', n, canlin.sys.now(), math.type(p.handle))\n"
    "    canlin.timer.after(100)\n"
    "  end\n"
    "end\n"
    "return M\n";

static void test_timer_rearm(void)
{
    harness h;
    char src[2048];
    h_init(&h);
    snprintf(src, sizeof src, timer_src, (unsigned)CANLIN_TIMER_MAX_MS);
    CHECK(h_load(&h, 0, "ticker", src, 0) == CANLIN_MODULE_OK);
    CHECK(has(&h.log[0], "init\tticker\t1.0\tinteger"));
    CHECK(has(&h.log[0], "ro\tfalse\tfalse"));
    CHECK(has(&h.log[0], "bad\tnil\tinvalid"));
    CHECK(has(&h.log[0], "big\tnil\tinvalid"));
    CHECK(has(&h.log[0], "type\tfalse"));
    h_turn(&h);
    CHECK(count(&h.log[0], "tick\t") == 0);
    canlin_ev_fake_clock_advance(&h.clock, 99);
    h_turn(&h);
    CHECK(count(&h.log[0], "tick\t") == 0);
    canlin_ev_fake_clock_advance(&h.clock, 1);
    h_turn(&h);
    CHECK(has(&h.log[0], "tick\t1\t100\tinteger"));
    canlin_ev_fake_clock_advance(&h.clock, 100);
    h_turn(&h);
    CHECK(has(&h.log[0], "tick\t2\t200\tinteger"));
    CHECK(count(&h.log[0], "tick\t") == 2);
    h_done(&h);
}

static const char cancel_src[] =
    "local M = {}\n"
    "local b\n"
    "function M.init(ctx)\n"
    "  local h = canlin.timer.after(1000)\n"
    "  print('c1', canlin.cancel(h))\n"
    "  print('c2', canlin.cancel(h))\n"
    "  print('c3', canlin.cancel(0))\n"
    "  print('c4', canlin.cancel(h + 1000))\n"
    "  print('c5', canlin.cancel(((ctx.generation + 100) << 32) | 1))\n"
    "  print('c6', canlin.cancel(-1))\n"
    "  print('c7', pcall(canlin.cancel, 'x'))\n"
    "  print('c8', pcall(canlin.cancel))\n"
    "  canlin.timer.after(0)\n"
    "  b = canlin.timer.after(0)\n"
    "end\n"
    "function M.event(name, p)\n"
    "  if p.handle ~= b then print('s1', canlin.cancel(b)) print('s2', canlin.cancel(b))\n"
    "  else print('b fired') end\n"
    "  print('s3', canlin.cancel(p.handle))\n"
    "end\n"
    "return M\n";

static void test_cancel(void)
{
    harness h;
    h_init(&h);
    CHECK(h_load(&h, 0, "cancel", cancel_src, 0) == CANLIN_MODULE_OK);
    CHECK(has(&h.log[0], "c1\tcancelled"));
    CHECK(has(&h.log[0], "c2\talready"));
    CHECK(has(&h.log[0], "c3\tnil\tstale"));
    CHECK(has(&h.log[0], "c4\tnil\tstale"));
    CHECK(has(&h.log[0], "c5\tnil\tstale"));
    CHECK(has(&h.log[0], "c6\tnil\tstale"));
    CHECK(has(&h.log[0], "c7\tfalse"));
    CHECK(has(&h.log[0], "c8\tfalse"));
    h_drain(&h);
    CHECK(has(&h.log[0], "s1\tsuppressed"));
    CHECK(has(&h.log[0], "s2\talready"));
    CHECK(has(&h.log[0], "s3\talready")); /* the delivery in progress */
    CHECK(!has(&h.log[0], "b fired"));
    h_done(&h);
}

static const char spin_src[] =
    "local M = {}\n"
    "function M.event(name) print('spin') while true do end end\n"
    "return M\n";

static const char steady_src[] =
    "local M = {}\n"
    "local n = 0\n"
    "function M.init() canlin.timer.after(10) end\n"
    "function M.event(name) n = n + 1 print('tick', n) canlin.invalidate() canlin.timer.after(10) end\n"
    "function M.draw(ui) print('draw', type(ui)) end\n"
    "return M\n";

static const canlin_module_event ev_go = {"go", NULL};

static void test_fault_isolated(void)
{
    harness h;
    canlin_ev_stats st;
    int k, slot;
    h_init(&h);
    CHECK(h_load(&h, 0, "spinner", spin_src, 100000) == CANLIN_MODULE_OK);
    CHECK(h_load(&h, 1, "steady", steady_src, 100000) == CANLIN_MODULE_OK);
    slot = canlin_ev_module_slot(h.ev, canlin_module_gen(h.m[0]));
    CHECK(slot >= 0);
    post_event(&h, 0, &ev_go);
    h_turn(&h);
    CHECK(has(&h.log[0], "spin"));
    CHECK(canlin_module_get_state(h.m[0]) == CANLIN_MODULE_FAULTED);
    CHECK(canlin_module_gen(h.m[0]) == 0);
    CHECK(strstr(canlin_module_errmsg(h.m[0]), "budget") != NULL);
    canlin_ev_get_stats(h.ev, slot, &st);
    CHECK(st.gen == 0 && st.items == 0 && st.ops == 0);
    CHECK(has(&h.log[1], "draw\ttable")); /* first frame after load */
    for (k = 0; k < 5; k++) {
        canlin_ev_fake_clock_advance(&h.clock, 10);
        h_turn(&h);
    }
    CHECK(count(&h.log[1], "tick") == 5);
    CHECK(count(&h.log[1], "draw") == 6);
    CHECK(canlin_module_get_state(h.m[1]) == CANLIN_MODULE_RUNNING);
    CHECK(count(&h.log[0], "spin\n") == 1); /* never called again */
    /* A faulted module refuses delivery without entering Lua. */
    {
        canlin_ev_delivery d;
        memset(&d, 0, sizeof d);
        d.kind = CANLIN_EV_D_DRAW;
        CHECK(canlin_module_deliver(h.m[0], &d) == CANLIN_SANDBOX_DEAD);
    }
    h_done(&h);
}

static const char draw_budget_src[] =
    "local M = {}\n"
    "local big = false\n"
    "local function spin(n) local x = 0 for i = 1, n do x = x + i end return x end\n"
    "function M.event(name) spin(%u) print('event ok') big = true canlin.invalidate() end\n"
    "function M.draw(ui) spin(big and %u or %u) print('draw ok') end\n"
    "return M\n";

static void test_draw_budget(void)
{
    harness h;
    char src[1024];
    unsigned n = CANLIN_SB_BUDGET_DRAW;
    h_init(&h);
    /* ~2 VM instructions per iteration: n iterations exceed the draw budget and fit
     * the (4x larger) event budget; n/8 fits both. */
    snprintf(src, sizeof src, draw_budget_src, n, n, n / 8);
    CHECK(h_load(&h, 0, "drawer", src, 4u * CANLIN_SB_BUDGET_DRAW) == CANLIN_MODULE_OK);
    h_turn(&h);
    CHECK(count(&h.log[0], "draw ok") == 1);
    post_event(&h, 0, &ev_go);
    h_turn(&h);
    CHECK(has(&h.log[0], "event ok"));
    CHECK(count(&h.log[0], "draw ok") == 1);
    CHECK(canlin_module_get_state(h.m[0]) == CANLIN_MODULE_FAULTED);
    CHECK(strstr(canlin_module_errmsg(h.m[0]), "draw") != NULL);
    h_done(&h);
}

static const char err_src[] =
    "local M = {}\n"
    "function M.event(name, p) if p.fail then error('bad ' .. name) end print('good') end\n"
    "return M\n";

static const canlin_value v_true = {CANLIN_VALUE_BOOL, 1, NULL, 0, NULL, 0};
static const canlin_value k_fail = {CANLIN_VALUE_STR, 0, "fail", 4, NULL, 0};
static const canlin_value_pair fail_pairs[] = {{{CANLIN_VALUE_STR, 0, "fail", 4, NULL, 0},
                                                {CANLIN_VALUE_BOOL, 1, NULL, 0, NULL, 0}}};
static const canlin_value v_fail = {CANLIN_VALUE_TABLE, 0, NULL, 0, fail_pairs, 1};
static const canlin_value v_empty = {CANLIN_VALUE_TABLE, 0, NULL, 0, NULL, 0};
static const canlin_module_event ev_bad = {"e", &v_fail};
static const canlin_module_event ev_ok = {"e", &v_empty};

static void test_error_count(void)
{
    harness h;
    unsigned k;
    h_init(&h);
    (void)v_true;
    (void)k_fail;
    CHECK(h_load(&h, 0, "err", err_src, 0) == CANLIN_MODULE_OK);
    for (k = 0; k + 1 < CANLIN_MODULE_MAX_ERRORS; k++)
        post_event(&h, 0, &ev_bad);
    h_drain(&h);
    CHECK(canlin_module_errors(h.m[0]) == CANLIN_MODULE_MAX_ERRORS - 1);
    CHECK(canlin_module_get_state(h.m[0]) == CANLIN_MODULE_RUNNING);
    CHECK(has(&h.log[0], "module err: event:"));
    CHECK(has(&h.log[0], "bad e"));
    post_event(&h, 0, &ev_ok);
    h_drain(&h);
    CHECK(canlin_module_errors(h.m[0]) == 0); /* success resets the count */
    CHECK(count(&h.log[0], "good") == 1);
    for (k = 0; k + 1 < CANLIN_MODULE_MAX_ERRORS; k++)
        post_event(&h, 0, &ev_bad);
    h_drain(&h);
    CHECK(canlin_module_get_state(h.m[0]) == CANLIN_MODULE_RUNNING);
    post_event(&h, 0, &ev_bad);
    post_event(&h, 0, &ev_ok); /* retired with the generation, never delivered */
    h_drain(&h);
    CHECK(canlin_module_get_state(h.m[0]) == CANLIN_MODULE_FAULTED);
    CHECK(count(&h.log[0], "good") == 1);
    h_done(&h);
}

static const char v1_src[] =
    "local M = {}\n"
    "function M.init(ctx) print('v1 init') canlin.timer.after(100) end\n"
    "function M.event(name) print('v1 ' .. name) end\n"
    "function M.draw() print('v1 draw') end\n"
    "return M\n";

static const char v2_src[] =
    "local M = {}\n"
    "function M.init(ctx) print('v2 init', ctx.generation) canlin.timer.after(100) end\n"
    "function M.event(name) print('v2 ' .. name) end\n"
    "return M\n";

static void test_reload(void)
{
    harness h;
    canlin_module_source s;
    canlin_ev_gen old;
    canlin_ev_stats st;
    canlin_ev_delivery d;
    char line[64];
    h_init(&h);
    CHECK(h_load(&h, 0, "rel", v1_src, 0) == CANLIN_MODULE_OK);
    old = canlin_module_gen(h.m[0]);
    post_event(&h, 0, &ev_go); /* queued for v1, must be retired */
    memset(&s, 0, sizeof s);
    s.name = "rel";
    s.version = "2.0";
    s.main = v2_src;
    s.main_len = strlen(v2_src);
    CHECK(canlin_module_reload(h.m[0], &s) == CANLIN_MODULE_OK);
    CHECK(canlin_module_gen(h.m[0]) != old && canlin_module_gen(h.m[0]) != 0);
    snprintf(line, sizeof line, "v2 init\t%u", (unsigned)canlin_module_gen(h.m[0]));
    CHECK(has(&h.log[0], line));
    canlin_ev_get_stats(h.ev, -1, &st);
    CHECK(st.timers == 1 && st.queued == 0);
    canlin_ev_fake_clock_advance(&h.clock, 100);
    h_drain(&h);
    CHECK(has(&h.log[0], "v2 timer"));
    CHECK(!has(&h.log[0], "v1 timer") && !has(&h.log[0], "v1 go") &&
          !has(&h.log[0], "v1 draw"));
    /* A forged delivery for the old generation never enters Lua. */
    memset(&d, 0, sizeof d);
    d.kind = CANLIN_EV_D_TIMER;
    d.gen = old;
    CHECK(canlin_module_deliver(h.m[0], &d) == CANLIN_SANDBOX_DEAD);
    CHECK(!has(&h.log[0], "v1 timer"));
    h_done(&h);
}

/* ---- staged reload with rollback (core API §6, canlin#16) ---- */

/* Old generation: state (n) that must survive a failed reload, a rearming timer, a draw. */
static const char old_src[] =
    "local M = {}\n"
    "local n = 0\n"
    "function M.init(ctx) print('old init', ctx.generation) print('oldh', canlin.timer.after(10)) end\n"
    "function M.event(name, p)\n"
    "  n = n + 1\n"
    "  print('old ' .. name, n)\n"
    "  if name == 'timer' then canlin.timer.after(10) end\n"
    "end\n"
    "function M.draw() print('old draw') end\n"
    "return M\n";

static const char new_src[] =
    "local probe = require('probe')\n"
    "local M = {}\n"
    "canlin.timer.after(10)\n"
    "function M.init(ctx) print('new init', ctx.generation) end\n"
    "function M.event(name, p) print('new ' .. name, p and p.handle) end\n"
    "function M.draw() print('new draw') end\n"
    "return M\n";

typedef struct stage_probe {
    harness *h;
    int i;
    int calls;
    int stop;             /* call canlin_module_stop from inside the staging */
    int turn;             /* run a scheduler turn from inside the staging */
    canlin_ev_gen live;   /* canlin_module_gen seen during staging */
    int reload_rc;        /* nested reload from inside the staging */
} stage_probe;

static stage_probe probe;

/* require('probe') runs on the owner thread in the middle of the staging (inside
 * lua#main of the new generation): the old generation is still live but must get
 * nothing delivered. */
static int probe_require(void *ud, const char *name, size_t len, const char **src,
                         size_t *slen)
{
    stage_probe *p = ud;
    harness *h = p->h;
    static const char body[] = "return {}";
    if (len != 5 || memcmp(name, "probe", 5) != 0)
        return 0;
    p->calls++;
    p->live = canlin_module_gen(h->m[p->i]);
    /* Work for the old generation arriving during the staging: an event and a due timer. */
    post_event(h, p->i, &ev_go);
    canlin_ev_fake_clock_advance(&h->clock, 10);
    if (p->turn)
        h_turn(h); /* the slot is staged: nothing is dispatched to either generation */
    p->reload_rc = canlin_module_reload(h->m[p->i], NULL);
    if (p->stop)
        canlin_module_stop(h->m[p->i]);
    *src = body;
    *slen = sizeof body - 1;
    return 1;
}

static void mk_src(canlin_module_source *s, const char *main)
{
    memset(s, 0, sizeof *s);
    s->name = "stage";
    s->version = "2.0";
    s->main = main;
    s->main_len = strlen(main);
    s->require = probe_require;
    s->require_ud = &probe;
}

static int h_load_mem(harness *h, int i, const char *src, size_t mem, uint32_t budget)
{
    canlin_module_source s;
    canlin_sandbox_config cfg;
    int err = 12345;
    memset(&s, 0, sizeof s);
    memset(&cfg, 0, sizeof cfg);
    s.name = "stage";
    s.version = "1.0";
    s.main = src;
    s.main_len = strlen(src);
    cfg.mem_limit = mem;
    cfg.budget = budget;
    cfg.log = log_fn;
    cfg.ud = &h->log[i];
    h->m[i] = canlin_module_load(h->ev, &s, &cfg, &err);
    return err;
}

static int same_stats(const canlin_ev_stats *a, const canlin_ev_stats *b)
{
    return a->items == b->items && a->bytes == b->bytes && a->queued == b->queued &&
           a->ops == b->ops && a->timers == b->timers && a->gen == b->gen &&
           a->staged == b->staged;
}

/* Success: the new generation sees its own id in init, the old one is never called after
 * the commit, its handles are stale and its queued work (including what arrived during
 * the staging) is retired. */
static void test_stage_commit(void)
{
    harness h;
    canlin_module_source s;
    canlin_ev_gen old, g;
    canlin_ev_stats st;
    long long oldh = 0;
    const char *p;
    char line[64];
    int slot;

    h_init(&h);
    memset(&probe, 0, sizeof probe);
    probe.h = &h;
    probe.turn = 1;
    CHECK(h_load(&h, 0, "stage", old_src, 0) == CANLIN_MODULE_OK);
    old = canlin_module_gen(h.m[0]);
    slot = canlin_ev_module_slot(h.ev, old);
    p = strstr(h.log[0].buf, "oldh\t");
    CHECK(p && sscanf(p + 5, "%lld", &oldh) == 1 && oldh != 0);
    h_turn(&h);
    CHECK(count(&h.log[0], "old draw") == 1);

    mk_src(&s, new_src);
    CHECK(canlin_module_reload(h.m[0], &s) == CANLIN_MODULE_OK);
    CHECK(probe.calls == 1);
    CHECK(probe.live == old); /* the old generation stayed live during the staging */
    CHECK(probe.reload_rc == CANLIN_MODULE_EBUSY);
    g = canlin_module_gen(h.m[0]);
    CHECK(g != 0 && g != old && canlin_ev_module_slot(h.ev, g) == slot);
    CHECK(canlin_ev_module_slot(h.ev, old) == CANLIN_EV_ESTALE);
    CHECK(canlin_module_reload_error(h.m[0]) == 0);
    snprintf(line, sizeof line, "new init\t%u", (unsigned)g);
    CHECK(has(&h.log[0], line));
    /* The turn during staging delivered nothing: the old generation saw only init/draw. */
    CHECK(!has(&h.log[0], "old go") && !has(&h.log[0], "old timer"));
    CHECK(h.orphans == 0);
    /* Old timer and queued event retired; the new generation's timer remains. */
    canlin_ev_get_stats(h.ev, slot, &st);
    CHECK(st.gen == g && st.staged == 0 && st.timers == 1 && st.queued == 0 && st.items == 1);
    /* Old handles are stale for every path. */
    CHECK(canlin_ev_cancel(h.ev, g, (canlin_ev_handle)oldh) == CANLIN_EV_ESTALE);
    CHECK(canlin_ev_cancel(h.ev, old, (canlin_ev_handle)oldh) == CANLIN_EV_ESTALE);
    {
        canlin_ev_handle t;
        CHECK(canlin_ev_timer_start(h.ev, old, 1, &t) == CANLIN_EV_ESTALE);
    }
    CHECK(canlin_ev_post(h.ev, old, CANLIN_MODULE_SOURCE_EVENT, 0, 0, 0, NULL, 0, NULL) ==
          CANLIN_EV_ESTALE);
    h_drain(&h);
    canlin_ev_fake_clock_advance(&h.clock, 10);
    h_drain(&h);
    CHECK(has(&h.log[0], "new draw"));
    CHECK(count(&h.log[0], "new timer") == 1);
    CHECK(count(&h.log[0], "old draw") == 1);
    CHECK(!has(&h.log[0], "old go") && !has(&h.log[0], "old timer"));
    h_done(&h);
}

/* One failing reload: everything the new generation admitted is retired, the old one
 * resumes with its Lua state, timers and the work that arrived during the staging. */
static void expect_rollback(const char *bad, int want, const char *step, const char *why,
                            size_t mem, uint32_t budget)
{
    harness h;
    canlin_module_source s;
    canlin_ev_gen old, g;
    canlin_ev_stats before, proc_before, st;
    int slot;

    h_init(&h);
    memset(&probe, 0, sizeof probe);
    probe.h = &h;
    probe.turn = 1;
    CHECK(h_load_mem(&h, 0, old_src, mem, budget) == CANLIN_MODULE_OK);
    old = canlin_module_gen(h.m[0]);
    slot = canlin_ev_module_slot(h.ev, old);
    h_turn(&h);
    canlin_ev_fake_clock_advance(&h.clock, 10);
    h_turn(&h);
    CHECK(has(&h.log[0], "old timer\t1"));
    canlin_ev_get_stats(h.ev, slot, &before);
    canlin_ev_get_stats(h.ev, -1, &proc_before);

    mk_src(&s, bad);
    CHECK(canlin_module_reload(h.m[0], &s) == want);
    CHECK(canlin_module_get_state(h.m[0]) == CANLIN_MODULE_RUNNING);
    CHECK(canlin_module_gen(h.m[0]) == old);
    CHECK(canlin_module_reload_error(h.m[0]) == want);
    CHECK(strncmp(canlin_module_errmsg(h.m[0]), "reload: ", 8) == 0);
    CHECK(strstr(canlin_module_errmsg(h.m[0]), step) != NULL);
    if (why)
        CHECK(strstr(canlin_module_errmsg(h.m[0]), why) != NULL);
    CHECK(has(&h.log[0], "reload failed, old generation kept"));
    CHECK(h.orphans == 0);

    /* The new generation's resources are fully retired: only the event and the timer
     * expiry that arrived for the old generation during the staging are left. */
    canlin_ev_get_stats(h.ev, slot, &st);
    CHECK(st.gen == old && st.staged == 0);
    if (probe.calls) {
        CHECK(st.queued == before.queued + 1 + (probe.turn ? 1u : 0u));
        CHECK(st.items == before.items + 1 && st.timers == before.timers);
    } else {
        CHECK(same_stats(&st, &before));
    }
    /* Old generation resumes: Lua state (n) intact, its timer still fires. */
    h_turn(&h);
    if (probe.calls) {
        CHECK(has(&h.log[0], "old go\t2"));
        CHECK(has(&h.log[0], "old timer\t3"));
    }
    h_drain(&h);
    canlin_ev_fake_clock_advance(&h.clock, 10);
    h_drain(&h);
    CHECK(has(&h.log[0], probe.calls ? "old timer\t4" : "old timer\t2"));
    CHECK(!has(&h.log[0], "new "));
    canlin_ev_get_stats(h.ev, slot, &st);
    CHECK(same_stats(&st, &before));
    canlin_ev_get_stats(h.ev, -1, &st);
    CHECK(st.items == proc_before.items && st.ops == proc_before.ops &&
          st.timers == proc_before.timers && st.bytes == proc_before.bytes);

    /* The aborted generation id is never reused. */
    mk_src(&s, new_src);
    CHECK(canlin_module_reload(h.m[0], &s) == CANLIN_MODULE_OK);
    g = canlin_module_gen(h.m[0]);
    CHECK(g > old + 1);
    CHECK(canlin_module_reload_error(h.m[0]) == 0);
    h_done(&h);
}

static void test_stage_rollback(void)
{
    expect_rollback("this is not lua", CANLIN_MODULE_ELOAD, "lua#main", NULL, 0, 0);
    expect_rollback("local p = require('probe') canlin.timer.after(1) canlin.invalidate() "
                    "error('top')",
                    CANLIN_MODULE_ELOAD, "lua#main", "top", 0, 0);
    expect_rollback("local p = require('probe') return 42", CANLIN_MODULE_ELOAD, "lua#main",
                    NULL, 0, 0);
    expect_rollback("local p = require('probe') local M = {}\n"
                    "function M.init(ctx) canlin.timer.after(5) canlin.invalidate() "
                    "error('boom') end\n"
                    "return M",
                    CANLIN_MODULE_EINIT, "init", "boom", 0, 0);
    expect_rollback("local p = require('probe') local M = {}\n"
                    "function M.init() canlin.timer.after(5) while true do end end\n"
                    "return M",
                    CANLIN_MODULE_EINIT, "init", "budget", 0, 100000);
    expect_rollback("local p = require('probe') local M = {}\n"
                    "function M.init() canlin.timer.after(5) local t = {} "
                    "for i = 1, 400000 do t[i] = i end end\n"
                    "return M",
                    CANLIN_MODULE_EINIT, "init", "memory", 1024 * 1024, 0);
}

/* Several aborts in a row never hand out a generation twice; a stop requested during the
 * staging wins over both generations. */
static void test_stage_abort_ids_and_stop(void)
{
    harness h;
    canlin_module_source s;
    canlin_ev_gen seen[4];
    canlin_ev_stats st;
    char line[64];
    int k, j;
    const char *bad = "local p = require('probe') local M = {}\n"
                      "function M.init(ctx) print('try', ctx.generation) error('x') end\n"
                      "return M";

    h_init(&h);
    memset(&probe, 0, sizeof probe);
    probe.h = &h;
    CHECK(h_load(&h, 0, "stage", old_src, 0) == CANLIN_MODULE_OK);
    mk_src(&s, bad);
    for (k = 0; k < 4; k++) {
        const char *p;
        long long v = 0;
        CHECK(canlin_module_reload(h.m[0], &s) == CANLIN_MODULE_EINIT);
        p = h.log[0].buf;
        for (j = 0; j <= k; j++) {
            p = strstr(p, "try\t");
            if (!p)
                break;
            p += 4;
        }
        CHECK(p && sscanf(p, "%lld", &v) == 1);
        seen[k] = (canlin_ev_gen)v;
        CHECK(seen[k] != canlin_module_gen(h.m[0]));
        for (j = 0; j < k; j++)
            CHECK(seen[j] != seen[k]);
    }
    mk_src(&s, new_src);
    CHECK(canlin_module_reload(h.m[0], &s) == CANLIN_MODULE_OK);
    for (j = 0; j < 4; j++)
        CHECK(seen[j] != canlin_module_gen(h.m[0]));
    snprintf(line, sizeof line, "new init\t%u", (unsigned)canlin_module_gen(h.m[0]));
    CHECK(has(&h.log[0], line));

    /* Stop from inside the staging (deferred like a stop from a callback). */
    probe.stop = 1;
    CHECK(canlin_module_reload(h.m[0], &s) == CANLIN_MODULE_ESTALE);
    CHECK(canlin_module_get_state(h.m[0]) == CANLIN_MODULE_STOPPED);
    CHECK(canlin_module_gen(h.m[0]) == 0);
    canlin_ev_get_stats(h.ev, -1, &st);
    CHECK(st.items == 0 && st.ops == 0 && st.timers == 0 && st.queued == 0);
    h_drain(&h);
    h_done(&h);
}

/* Both generations share the slot's limits: a slot with no room for one more item is
 * refused with full and the old generation is untouched. */
static void test_stage_full(void)
{
    harness h;
    canlin_module_source s;
    canlin_ev_gen old;
    canlin_ev_stats before, st;
    unsigned n = 0;
    int slot;

    h_init(&h);
    memset(&probe, 0, sizeof probe);
    probe.h = &h;
    CHECK(h_load(&h, 0, "stage", old_src, 0) == CANLIN_MODULE_OK);
    old = canlin_module_gen(h.m[0]);
    slot = canlin_ev_module_slot(h.ev, old);
    h_turn(&h);
    while (canlin_ev_post(h.ev, old, CANLIN_MODULE_SOURCE_EVENT, 0, 0, 0, (void *)&ev_go, 0,
                          NULL) == CANLIN_EV_OK)
        n++;
    CHECK(n == CANLIN_EV_MOD_MAX_ITEMS - 1); /* one item is the old timer's reservation */
    canlin_ev_get_stats(h.ev, slot, &before);
    mk_src(&s, new_src);
    CHECK(canlin_module_reload(h.m[0], &s) == CANLIN_MODULE_EFULL);
    CHECK(probe.calls == 0); /* nothing of the new generation ran */
    CHECK(canlin_module_reload_error(h.m[0]) == CANLIN_MODULE_EFULL);
    CHECK(strstr(canlin_module_errmsg(h.m[0]), "full") != NULL);
    CHECK(canlin_module_get_state(h.m[0]) == CANLIN_MODULE_RUNNING);
    CHECK(canlin_module_gen(h.m[0]) == old);
    canlin_ev_get_stats(h.ev, slot, &st);
    CHECK(same_stats(&st, &before));
    h_drain(&h);
    CHECK(count(&h.log[0], "old go") == n);
    /* With room again the reload is admitted. */
    CHECK(canlin_module_reload(h.m[0], &s) == CANLIN_MODULE_OK);
    CHECK(canlin_module_gen(h.m[0]) != old);
    h_done(&h);
}

/* A faulted module has no old generation to fall back to: reload is a plain start, and a
 * failing one leaves it FAULTED. */
static void test_reload_faulted(void)
{
    harness h;
    canlin_module_source s;
    h_init(&h);
    memset(&probe, 0, sizeof probe);
    probe.h = &h;
    CHECK(h_load(&h, 0, "stage", spin_src, 100000) == CANLIN_MODULE_OK);
    post_event(&h, 0, &ev_go);
    h_turn(&h);
    CHECK(canlin_module_get_state(h.m[0]) == CANLIN_MODULE_FAULTED);
    mk_src(&s, "local M = {} function M.init() error('again') end return M");
    CHECK(canlin_module_reload(h.m[0], &s) == CANLIN_MODULE_EINIT);
    CHECK(canlin_module_get_state(h.m[0]) == CANLIN_MODULE_FAULTED);
    CHECK(canlin_module_gen(h.m[0]) == 0);
    CHECK(canlin_module_reload_error(h.m[0]) == 0);
    mk_src(&s, new_src);
    CHECK(canlin_module_reload(h.m[0], &s) == CANLIN_MODULE_OK);
    CHECK(probe.calls == 1 && probe.live == canlin_module_gen(h.m[0])); /* no old one */
    CHECK(canlin_module_get_state(h.m[0]) == CANLIN_MODULE_RUNNING);
    h_drain(&h);
    CHECK(has(&h.log[0], "new draw"));
    h_done(&h);
}

/* Reload from inside the module's own callback is refused, as before: the module calls
 * canlin.cancel on an operation whose producer hook tries to reload it. */
typedef struct busy_prod {
    harness *h;
    int rc;
} busy_prod;

static void busy_cancel(void *ctx, canlin_ev_handle hd)
{
    busy_prod *b = ctx;
    canlin_module_source s;
    mk_src(&s, new_src);
    b->rc = canlin_module_reload(b->h->m[0], &s);
    CHECK(canlin_ev_op_complete(b->h->ev, hd, CANLIN_EV_FAILURE, NULL, 0, NULL) ==
          CANLIN_EV_RETIRED);
}

static const char self_reload_src[] =
    "local M = {}\n"
    "function M.event(name, p) print('cancel', canlin.cancel(p.h)) end\n"
    "return M\n";

static void test_reload_busy(void)
{
    harness h;
    busy_prod b;
    canlin_ev_producer pr;
    canlin_ev_handle op;
    canlin_ev_gen g;
    canlin_value_pair pair;
    canlin_value val;
    canlin_module_event e;

    h_init(&h);
    memset(&probe, 0, sizeof probe);
    probe.h = &h;
    CHECK(h_load(&h, 0, "stage", self_reload_src, 0) == CANLIN_MODULE_OK);
    g = canlin_module_gen(h.m[0]);
    b.h = &h;
    b.rc = 1;
    pr.cancel = busy_cancel;
    pr.ctx = &b;
    CHECK(canlin_ev_op_begin(h.ev, g, 0, &pr, &op) == CANLIN_EV_OK);
    memset(&pair, 0, sizeof pair);
    pair.key.type = CANLIN_VALUE_STR;
    pair.key.s = "h";
    pair.key.len = 1;
    pair.val.type = CANLIN_VALUE_INT;
    pair.val.i = (int64_t)op;
    memset(&val, 0, sizeof val);
    val.type = CANLIN_VALUE_TABLE;
    val.pairs = &pair;
    val.n = 1;
    e.name = "go";
    e.value = &val;
    post_event(&h, 0, &e);
    h_drain(&h);
    CHECK(has(&h.log[0], "cancel\tcancelled"));
    CHECK(b.rc == CANLIN_MODULE_EBUSY);
    CHECK(canlin_module_gen(h.m[0]) == g && probe.calls == 0);
    h_done(&h);
}

static void expect_load_fail(const char *src, int want)
{
    harness h;
    canlin_ev_stats st;
    unsigned s;
    h_init(&h);
    CHECK(h_load(&h, 0, "bad", src, 100000) == want);
    CHECK(h.m[0] == NULL);
    canlin_ev_get_stats(h.ev, -1, &st);
    CHECK(st.items == 0 && st.ops == 0 && st.timers == 0);
    for (s = 0; s < CANLIN_EV_MAX_MODULES; s++) {
        canlin_ev_get_stats(h.ev, (int)s, &st);
        CHECK(st.gen == 0);
    }
    h_done(&h);
}

static void test_load_errors(void)
{
    expect_load_fail("local M = {} function M.init() canlin.timer.after(5) error('boom') end "
                     "return M", CANLIN_MODULE_EINIT);
    expect_load_fail("local M = {} function M.init() while true do end end return M",
                     CANLIN_MODULE_EINIT);
    expect_load_fail("return 42", CANLIN_MODULE_ELOAD);
    expect_load_fail("return", CANLIN_MODULE_ELOAD);
    expect_load_fail("this is not lua", CANLIN_MODULE_ELOAD);
    expect_load_fail("canlin.timer.after(1) error('top')", CANLIN_MODULE_ELOAD);
    expect_load_fail("local M = {} M.init = 5 return M", CANLIN_MODULE_EINIT);
    {
        harness h;
        canlin_module_source s;
        int err = 0;
        h_init(&h);
        memset(&s, 0, sizeof s);
        s.name = "";
        s.main = "return {}";
        s.main_len = 9;
        CHECK(canlin_module_load(h.ev, &s, NULL, &err) == NULL && err == CANLIN_MODULE_EINVAL);
        h_done(&h);
    }
}

static const char done_src[] =
    "local M = {}\n"
    "function M.event(name, p)\n"
    "  if name == 'done' then\n"
    "    if p.ok then print('done ok', p.handle, p.result.n, p.result.s)\n"
    "    else print('done fail', p.handle, p.code, p.message) end\n"
    "  else print('ev', name) end\n"
    "end\n"
    "return M\n";

static const canlin_value_pair res_pairs[] = {
    {{CANLIN_VALUE_STR, 0, "n", 1, NULL, 0}, {CANLIN_VALUE_INT, 42, NULL, 0, NULL, 0}},
    {{CANLIN_VALUE_STR, 0, "s", 1, NULL, 0}, {CANLIN_VALUE_STR, 0, "hi", 2, NULL, 0}},
};
static const canlin_value res_val = {CANLIN_VALUE_TABLE, 0, NULL, 0, res_pairs, 2};
static canlin_module_result res_ok = {&res_val, NULL, NULL};

static void complete_hook(harness *h, int i, const canlin_ev_delivery *d)
{
    (void)i;
    if (d->kind == CANLIN_EV_D_EVENT && h->op_completed_turn == 0) {
        CHECK(canlin_ev_op_complete(h->ev, h->op, CANLIN_EV_SUCCESS, &res_ok, 64, NULL) ==
              CANLIN_EV_OK);
        h->op_completed_turn = (int)h->turn;
    }
}

static void test_done_next_turn(void)
{
    harness h;
    char line[128];
    h_init(&h);
    CHECK(h_load(&h, 0, "ops", done_src, 0) == CANLIN_MODULE_OK);
    CHECK(canlin_ev_op_begin(h.ev, canlin_module_gen(h.m[0]), 64, NULL, &h.op) ==
          CANLIN_EV_OK);
    h.hook = complete_hook;
    post_event(&h, 0, &ev_go);
    h_turn(&h);
    CHECK(h.op_completed_turn == 1);
    CHECK(has(&h.log[0], "ev\tgo"));
    CHECK(!has(&h.log[0], "done")); /* never inline, never the same turn */
    h_turn(&h);
    snprintf(line, sizeof line, "done ok\t%lld\t42\thi", (long long)h.op);
    CHECK(has(&h.log[0], line));
    h_done(&h);
}

/* Payloads exceeding the reservation or the marshalling limits become ok=false "full". */
static canlin_value_pair many_pairs[CANLIN_MARSHAL_MAX_ENTRIES + 1];
static canlin_value deep[CANLIN_MARSHAL_MAX_DEPTH + 1];
static canlin_value_pair deep_pairs[CANLIN_MARSHAL_MAX_DEPTH + 1];

static void test_oversize(void)
{
    harness h;
    canlin_ev_handle a, b, c, dd;
    canlin_ev_gen g;
    canlin_value many;
    canlin_module_result r_many, r_deep, r_fail;
    char line[160];
    unsigned k;

    for (k = 0; k <= CANLIN_MARSHAL_MAX_ENTRIES; k++) {
        many_pairs[k].key.type = CANLIN_VALUE_INT;
        many_pairs[k].key.i = (int64_t)k + 1;
        many_pairs[k].val.type = CANLIN_VALUE_BOOL;
        many_pairs[k].val.i = 1;
    }
    memset(&many, 0, sizeof many);
    many.type = CANLIN_VALUE_TABLE;
    many.pairs = many_pairs;
    many.n = CANLIN_MARSHAL_MAX_ENTRIES + 1;
    CHECK(canlin_value_check(&many) == CANLIN_VALUE_ETOOBIG);
    many.n = CANLIN_MARSHAL_MAX_ENTRIES;
    CHECK(canlin_value_check(&many) == CANLIN_VALUE_OK);
    many.n = CANLIN_MARSHAL_MAX_ENTRIES + 1;

    /* deep[0] -> deep[1] -> ... : MAX_DEPTH + 1 nested tables. */
    for (k = 0; k <= CANLIN_MARSHAL_MAX_DEPTH; k++) {
        deep[k].type = CANLIN_VALUE_TABLE;
        deep[k].n = k < CANLIN_MARSHAL_MAX_DEPTH ? 1 : 0;
        deep[k].pairs = &deep_pairs[k];
        deep_pairs[k].key.type = CANLIN_VALUE_INT;
        deep_pairs[k].key.i = 1;
        if (k < CANLIN_MARSHAL_MAX_DEPTH)
            deep_pairs[k].val = deep[k + 1];
    }
    /* Rebuild bottom-up so every copy holds its complete subtree. */
    for (k = CANLIN_MARSHAL_MAX_DEPTH; k-- > 0;)
        deep_pairs[k].val = deep[k + 1];
    CHECK(canlin_value_check(&deep[0]) == CANLIN_VALUE_ETOOBIG);
    CHECK(canlin_value_check(&deep[1]) == CANLIN_VALUE_OK);

    h_init(&h);
    CHECK(h_load(&h, 0, "big", done_src, 0) == CANLIN_MODULE_OK);
    g = canlin_module_gen(h.m[0]);
    CHECK(canlin_ev_op_begin(h.ev, g, 16, NULL, &a) == CANLIN_EV_OK);
    CHECK(canlin_ev_op_begin(h.ev, g, 64, NULL, &b) == CANLIN_EV_OK);
    CHECK(canlin_ev_op_begin(h.ev, g, 64, NULL, &c) == CANLIN_EV_OK);
    CHECK(canlin_ev_op_begin(h.ev, g, 64, NULL, &dd) == CANLIN_EV_OK);
    r_many.value = &many;
    r_deep.value = &deep[0];
    memset(&r_fail, 0, sizeof r_fail);
    r_fail.code = "denied";
    r_fail.message = "not allowed";
    CHECK(canlin_ev_op_complete(h.ev, a, CANLIN_EV_SUCCESS, &res_ok, 17, NULL) ==
          CANLIN_EV_OVERSIZE);
    CHECK(canlin_ev_op_complete(h.ev, b, CANLIN_EV_SUCCESS, &r_many, 64, NULL) == CANLIN_EV_OK);
    CHECK(canlin_ev_op_complete(h.ev, c, CANLIN_EV_SUCCESS, &r_deep, 64, NULL) == CANLIN_EV_OK);
    CHECK(canlin_ev_op_complete(h.ev, dd, CANLIN_EV_FAILURE, &r_fail, 8, NULL) == CANLIN_EV_OK);
    h_drain(&h);
    snprintf(line, sizeof line, "done fail\t%lld\tfull", (long long)a);
    CHECK(has(&h.log[0], line));
    snprintf(line, sizeof line, "done fail\t%lld\tfull", (long long)b);
    CHECK(has(&h.log[0], line));
    snprintf(line, sizeof line, "done fail\t%lld\tfull", (long long)c);
    CHECK(has(&h.log[0], line));
    snprintf(line, sizeof line, "done fail\t%lld\tdenied\tnot allowed", (long long)dd);
    CHECK(has(&h.log[0], line));
    CHECK(!has(&h.log[0], "done ok"));
    CHECK(canlin_module_get_state(h.m[0]) == CANLIN_MODULE_RUNNING);
    h_done(&h);
}

/* canlin#15: building a payload in Lua is charged to the receiving callback's budget
 * (CANLIN_SB_COST_* in sandbox_limits.h), before the callback runs. */
static const char marshal_src[] =
    "local M = {}\n"
    "function M.event(name, p)\n"
    "  if name == 'done' then p = p.result end\n"
    "  local n = 0 for _ in pairs(p) do n = n + 1 end\n"
    "  print('got', name, n)\n"
    "end\n"
    "return M\n";

#define MARSHAL_STR_LEN (CANLIN_MARSHAL_MAX_BYTES / CANLIN_MARSHAL_MAX_ENTRIES)
static char marshal_bytes[CANLIN_MARSHAL_MAX_BYTES];
static canlin_value_pair marshal_pairs[CANLIN_MARSHAL_MAX_ENTRIES];

static void test_marshal_budget(void)
{
    harness h;
    canlin_value big, small;
    canlin_module_event ev_big, ev_small;
    canlin_module_result r_big;
    canlin_ev_handle op;
    uint32_t big_cost, tiny;
    char line[64];
    unsigned k;

    /* A payload at the marshalling limits: MAX_ENTRIES entries whose string values use
     * exactly MAX_BYTES. It is legal, and building it costs big_cost units. */
    memset(marshal_bytes, 'x', sizeof marshal_bytes);
    for (k = 0; k < CANLIN_MARSHAL_MAX_ENTRIES; k++) {
        marshal_pairs[k].key.type = CANLIN_VALUE_INT;
        marshal_pairs[k].key.i = (int64_t)k + 1;
        marshal_pairs[k].val.type = CANLIN_VALUE_STR;
        marshal_pairs[k].val.s = marshal_bytes + (size_t)k * MARSHAL_STR_LEN;
        marshal_pairs[k].val.len = MARSHAL_STR_LEN;
    }
    memset(&big, 0, sizeof big);
    big.type = CANLIN_VALUE_TABLE;
    big.pairs = marshal_pairs;
    big.n = CANLIN_MARSHAL_MAX_ENTRIES;
    CHECK(canlin_value_check(&big) == CANLIN_VALUE_OK);
    big_cost = CANLIN_MARSHAL_MAX_ENTRIES *
               (CANLIN_SB_COST_ELEM + MARSHAL_STR_LEN / CANLIN_SB_COST_BYTES_PER_UNIT + 1u);
    small = big;
    small.n = 2;
    /* The tiny budget cannot build the big payload but leaves ample room for small ones. */
    tiny = big_cost - 1u;
    CHECK(tiny > 100u * (big_cost / CANLIN_MARSHAL_MAX_ENTRIES) * small.n);
    CHECK(tiny < CANLIN_SB_BUDGET_DRAW);

    ev_big.name = "big";
    ev_big.value = &big;
    ev_small.name = "small";
    ev_small.value = &small;
    memset(&r_big, 0, sizeof r_big);
    r_big.value = &big;

    h_init(&h);
    CHECK(h_load(&h, 0, "tiny", marshal_src, tiny) == CANLIN_MODULE_OK);
    CHECK(h_load(&h, 1, "normal", marshal_src, 0) == CANLIN_MODULE_OK);
    CHECK(h_load(&h, 2, "tinyop", marshal_src, tiny) == CANLIN_MODULE_OK);

    /* Small payloads fit the tiny budget, repeatedly: the budget is reset per callback. */
    for (k = 0; k < 3; k++)
        post_event(&h, 0, &ev_small);
    post_event(&h, 1, &ev_big); /* default budget: unaffected */
    h_drain(&h);
    CHECK(count(&h.log[0], "got\tsmall\t2") == 3);
    CHECK(canlin_module_get_state(h.m[0]) == CANLIN_MODULE_RUNNING);
    snprintf(line, sizeof line, "got\tbig\t%u", (unsigned)CANLIN_MARSHAL_MAX_ENTRIES);
    CHECK(has(&h.log[1], line));
    CHECK(canlin_module_get_state(h.m[1]) == CANLIN_MODULE_RUNNING);

    /* Big event payload with the tiny budget: the scope faults before Lua runs. */
    post_event(&h, 0, &ev_big);
    h_drain(&h);
    CHECK(!has(&h.log[0], "got\tbig"));
    CHECK(canlin_module_get_state(h.m[0]) == CANLIN_MODULE_FAULTED);
    CHECK(canlin_module_gen(h.m[0]) == 0);
    CHECK(strstr(canlin_module_errmsg(h.m[0]), "budget") != NULL);

    /* Same for a successful operation result. */
    CHECK(canlin_ev_op_begin(h.ev, canlin_module_gen(h.m[2]), 64, NULL, &op) == CANLIN_EV_OK);
    CHECK(canlin_ev_op_complete(h.ev, op, CANLIN_EV_SUCCESS, &r_big, 64, NULL) == CANLIN_EV_OK);
    h_drain(&h);
    CHECK(!has(&h.log[2], "got\tdone"));
    CHECK(canlin_module_get_state(h.m[2]) == CANLIN_MODULE_FAULTED);
    CHECK(strstr(canlin_module_errmsg(h.m[2]), "budget") != NULL);

    CHECK(canlin_module_get_state(h.m[1]) == CANLIN_MODULE_RUNNING); /* isolated */
    h_done(&h);
}

/* Module B knows module A's timer handle but cannot cancel it, cannot mark A dirty and
 * cannot see A's globals. */
static const char victim_src[] =
    "local M = {}\n"
    "secret = 'A'\n"
    "function M.init() handle = canlin.timer.after(50) print('h', handle) end\n"
    "function M.event(name) print('A ' .. name) end\n"
    "return M\n";

static const char attacker_src[] =
    "local M = {}\n"
    "function M.event(name, p)\n"
    "  print('x1', canlin.cancel(p.h))\n"
    "  print('x2', secret, handle, debug)\n"
    "  canlin.invalidate()\n"
    "end\n"
    "function M.draw() print('B draw') end\n"
    "return M\n";

static canlin_value_pair atk_pairs[1];
static canlin_value atk_val;
static canlin_module_event atk_ev;

static void test_isolation(void)
{
    harness h;
    canlin_ev_stats st;
    long long ha = 0;
    const char *p;
    int sa;
    h_init(&h);
    CHECK(h_load(&h, 0, "victim", victim_src, 0) == CANLIN_MODULE_OK);
    CHECK(h_load(&h, 1, "attacker", attacker_src, 0) == CANLIN_MODULE_OK);
    p = strstr(h.log[0].buf, "h\t");
    CHECK(p && sscanf(p + 2, "%lld", &ha) == 1 && ha != 0);
    h_turn(&h); /* first frames */
    atk_pairs[0].key.type = CANLIN_VALUE_STR;
    atk_pairs[0].key.s = "h";
    atk_pairs[0].key.len = 1;
    atk_pairs[0].val.type = CANLIN_VALUE_INT;
    atk_pairs[0].val.i = ha;
    atk_val.type = CANLIN_VALUE_TABLE;
    atk_val.pairs = atk_pairs;
    atk_val.n = 1;
    atk_ev.name = "attack";
    atk_ev.value = &atk_val;
    post_event(&h, 1, &atk_ev);
    h_turn(&h);
    CHECK(has(&h.log[1], "x1\tnil\tstale"));
    CHECK(has(&h.log[1], "x2\tnil\tnil\tnil"));
    sa = canlin_ev_module_slot(h.ev, canlin_module_gen(h.m[0]));
    canlin_ev_get_stats(h.ev, sa, &st);
    CHECK(st.dirty == 0 && st.timers == 1);
    canlin_ev_fake_clock_advance(&h.clock, 50);
    h_drain(&h);
    CHECK(has(&h.log[0], "A timer"));
    CHECK(count(&h.log[1], "B draw") == 2);
    h_done(&h);
}

static void test_control_consumed(void)
{
    harness h;
    h_init(&h);
    CHECK(h_load(&h, 0, "ctl", done_src, 0) == CANLIN_MODULE_OK);
    CHECK(canlin_ev_signal(h.ev, canlin_module_gen(h.m[0]), CANLIN_EV_CTL_INPUT_OVERFLOW) ==
          CANLIN_EV_OK);
    h_drain(&h);
    CHECK(h.log[0].len == 0);
    CHECK(canlin_module_get_state(h.m[0]) == CANLIN_MODULE_RUNNING);
    CHECK(canlin_ev_signal(h.ev, canlin_module_gen(h.m[0]), CANLIN_EV_CTL_STOP) ==
          CANLIN_EV_OK);
    h_drain(&h);
    CHECK(h.log[0].len == 0);
    CHECK(canlin_module_get_state(h.m[0]) == CANLIN_MODULE_STOPPED);
    h_done(&h);
}

int main(void)
{
    test_timer_rearm();
    test_cancel();
    test_fault_isolated();
    test_draw_budget();
    test_error_count();
    test_reload();
    test_stage_commit();
    test_stage_rollback();
    test_stage_abort_ids_and_stop();
    test_stage_full();
    test_reload_faulted();
    test_reload_busy();
    test_load_errors();
    test_done_next_turn();
    test_oversize();
    test_marshal_budget();
    test_isolation();
    test_control_consumed();
    if (failures) {
        fprintf(stderr, "test_module: %d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("test_module: %d checks passed\n", checks);
    return 0;
}
