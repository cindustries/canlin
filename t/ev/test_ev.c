/* Event scheduler contract tests (event-runtime spec section 9, rows that apply
 * without Lua/network). Fake clock, no sleeps; exits 0 on success. */
#include "canlin/ev.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(c)                                                                   \
    do {                                                                           \
        if (!(c)) {                                                                \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c);  \
            failures++;                                                            \
        }                                                                          \
    } while (0)

/* ---- recording harness ---- */

#define MAXREC 1024

typedef struct rec {
    cl_ev_delivery d;
    unsigned turn;
} rec;

typedef struct harness {
    cl_ev_fake_clock clock;
    cl_ev *ev;
    rec log[MAXREC];
    unsigned n;
    unsigned turn;
    int depth, max_depth;
    void (*hook)(struct harness *h, const cl_ev_delivery *d);
    void *hook_ctx;
} harness;

static void on_deliver(void *ud, cl_ev *ev, const cl_ev_delivery *d)
{
    harness *h = ud;
    (void)ev;
    h->depth++;
    if (h->depth > h->max_depth)
        h->max_depth = h->depth;
    if (h->n < MAXREC) {
        h->log[h->n].d = *d;
        h->log[h->n].d.payload = NULL; /* not valid after the callback */
        h->log[h->n].turn = h->turn;
        h->n++;
    }
    if (h->hook)
        h->hook(h, d);
    h->depth--;
}

static void h_init(harness *h)
{
    cl_ev_clock c;
    cl_ev_callbacks cb;
    memset(h, 0, sizeof *h);
    c = cl_ev_fake_clock_bind(&h->clock);
    cb.deliver = on_deliver;
    h->ev = cl_ev_new(&c, &cb, h);
    if (!h->ev) {
        fprintf(stderr, "cl_ev_new failed\n");
        exit(1);
    }
}

static cl_ev_turn_result h_turn(harness *h)
{
    cl_ev_turn_result r;
    h->turn++;
    CHECK(cl_ev_turn(h->ev, &r) == CL_EV_OK);
    CHECK(r.dispatched <= CL_EV_TURN_MAX_DISPATCH);
    return r;
}

static void h_done(harness *h)
{
    CHECK(h->max_depth <= 1); /* no nested callbacks, ever */
    CHECK(cl_ev_free(h->ev) == CL_EV_OK);
}

static unsigned count_kind(const harness *h, cl_ev_kind k, cl_ev_gen gen)
{
    unsigned i, c = 0;
    for (i = 0; i < h->n; i++)
        if (h->log[i].d.kind == k && (gen == 0 || h->log[i].d.gen == gen))
            c++;
    return c;
}

static int released;
static void rel(void *p, size_t n)
{
    (void)p;
    (void)n;
    released++;
}

static void check_proc_empty(const cl_ev *ev)
{
    cl_ev_stats st;
    cl_ev_get_stats(ev, -1, &st);
    CHECK(st.items == 0);
    CHECK(st.bytes == 0);
    CHECK(st.queued == 0);
    CHECK(st.ops == 0);
    CHECK(st.timers == 0);
}

/* ---- inline completion during a callback ---- */

static cl_ev_handle inline_h;
static unsigned inline_turn;

static void hook_inline(harness *h, const cl_ev_delivery *d)
{
    static char payload[4] = "ok!";
    cl_ev_turn_result r;
    if (d->kind != CL_EV_D_DRAW)
        return;
    /* A cache hit: begin and complete synchronously inside draw. */
    CHECK(cl_ev_op_begin(h->ev, d->gen, sizeof payload, NULL, &inline_h) == CL_EV_OK);
    CHECK(cl_ev_op_complete(h->ev, inline_h, CL_EV_SUCCESS, payload, sizeof payload,
                            rel) == CL_EV_OK);
    inline_turn = h->turn;
    /* Re-entering the scheduler from a callback is refused. */
    CHECK(cl_ev_turn(h->ev, &r) == CL_EV_EREENTRY);
    CHECK(cl_ev_free(h->ev) == CL_EV_EREENTRY);
    CHECK(count_kind(h, CL_EV_D_OP, 0) == 0);
}

static void test_inline_completion_after_boundary(void)
{
    harness h;
    cl_ev_gen g;
    cl_ev_turn_result r;
    unsigned i;

    h_init(&h);
    released = 0;
    h.hook = hook_inline;
    CHECK(cl_ev_module_start(h.ev, &g) == CL_EV_OK);
    CHECK(cl_ev_invalidate(h.ev, g) == CL_EV_OK);
    r = h_turn(&h);
    CHECK(count_kind(&h, CL_EV_D_DRAW, g) == 1);
    CHECK(count_kind(&h, CL_EV_D_OP, g) == 0); /* not in the same turn either */
    CHECK(r.more == 1);
    h.hook = NULL;
    h_turn(&h);
    CHECK(count_kind(&h, CL_EV_D_OP, g) == 1);
    for (i = 0; i < h.n; i++)
        if (h.log[i].d.kind == CL_EV_D_OP) {
            CHECK(h.log[i].turn > inline_turn);
            CHECK(h.log[i].d.handle == inline_h);
            CHECK(h.log[i].d.status == CL_EV_SUCCESS);
            CHECK(h.log[i].d.bytes == 4);
        }
    CHECK(released == 1);
    /* Handle is finished: cancel is harmless, completion is stale. */
    CHECK(cl_ev_cancel(h.ev, g, inline_h) == CL_EV_ALREADY);
    CHECK(cl_ev_op_complete(h.ev, inline_h, CL_EV_SUCCESS, NULL, 0, NULL) == CL_EV_ESTALE);
    check_proc_empty(h.ev);
    h_done(&h);
}

/* ---- busy module beside interactive module ---- */

static cl_ev_gen busy_g, ui_g;

static void hook_busy(harness *h, const cl_ev_delivery *d)
{
    /* The busy module feeds itself and self-invalidates on every callback. */
    if (d->gen == busy_g && d->kind == CL_EV_D_EVENT) {
        cl_ev_post(h->ev, busy_g, 1, 1, 0, 0, NULL, 0, NULL);
        cl_ev_post(h->ev, busy_g, 1, 1, 0, 0, NULL, 0, NULL);
        cl_ev_invalidate(h->ev, busy_g);
    }
    if (d->gen == ui_g && d->kind == CL_EV_D_EVENT)
        cl_ev_invalidate(h->ev, ui_g);
}

static void test_busy_beside_interactive(void)
{
    harness h;
    unsigned t, i;

    h_init(&h);
    h.hook = hook_busy;
    CHECK(cl_ev_module_start(h.ev, &busy_g) == CL_EV_OK);
    CHECK(cl_ev_module_start(h.ev, &ui_g) == CL_EV_OK);
    for (i = 0; i < 20; i++)
        CHECK(cl_ev_post(h.ev, busy_g, 1, 1, 0, 0, NULL, 0, NULL) == CL_EV_OK);

    for (t = 0; t < 10; t++) {
        unsigned n0 = h.n, ui_ev = 0, ui_draw = 0, busy_n = 0, busy_draw = 0;
        cl_ev_turn_result r;
        /* User input for the interactive module arrives between turns. */
        CHECK(cl_ev_post(h.ev, ui_g, 7, 2, (int32_t)t, 0, NULL, 0, NULL) == CL_EV_OK);
        r = h_turn(&h);
        CHECK(r.more == 1);
        for (i = n0; i < h.n; i++) {
            const cl_ev_delivery *d = &h.log[i].d;
            if (d->gen == ui_g && d->kind == CL_EV_D_EVENT) ui_ev++;
            if (d->gen == ui_g && d->kind == CL_EV_D_DRAW) ui_draw++;
            if (d->gen == busy_g && d->kind == CL_EV_D_EVENT) busy_n++;
            if (d->gen == busy_g && d->kind == CL_EV_D_DRAW) busy_draw++;
        }
        /* Input posted before the turn is delivered and redrawn in that turn, while
         * the self-feeding module progresses within its per-turn quota. */
        CHECK(ui_ev == 1);
        CHECK(ui_draw == 1);
        CHECK(busy_n >= 1 && busy_n <= CL_EV_TURN_MAX_PER_MODULE);
        CHECK(busy_draw <= 1);
    }
    h_done(&h);
}

/* ---- saturated queue ---- */

static void test_saturated_queue(void)
{
    harness h;
    cl_ev_gen g, g2, gs[CL_EV_MAX_MODULES];
    cl_ev_handle op, op2, extra;
    cl_ev_stats st;
    unsigned i, posted = 0;
    int slot;
    static char buf[8];

    h_init(&h);
    released = 0;
    CHECK(cl_ev_module_start(h.ev, &g) == CL_EV_OK);
    CHECK(cl_ev_module_start(h.ev, &g2) == CL_EV_OK);
    /* Two operations admitted before saturation: their terminals are reserved. */
    CHECK(cl_ev_op_begin(h.ev, g, sizeof buf, NULL, &op) == CL_EV_OK);
    CHECK(cl_ev_op_begin(h.ev, g, 2, NULL, &op2) == CL_EV_OK);
    while (cl_ev_post(h.ev, g, 1, 1, 0, 0, buf, 1, rel) == CL_EV_OK)
        posted++;
    CHECK(posted == CL_EV_MOD_MAX_ITEMS - 2);
    CHECK(cl_ev_post(h.ev, g, 1, 1, 0, 0, NULL, 0, NULL) == CL_EV_EFULL);
    CHECK(cl_ev_op_begin(h.ev, g, 0, NULL, &extra) == CL_EV_EFULL);
    CHECK(cl_ev_timer_start(h.ev, g, 5, &extra) == CL_EV_EFULL);
    /* Coalescable input can also not allocate a new item. */
    CHECK(cl_ev_post_latest(h.ev, g, 9, 9, 1, 1) == CL_EV_EFULL);
    /* Control state is outside the data queue and still accepted. */
    CHECK(cl_ev_signal(h.ev, g, CL_EV_CTL_INPUT_OVERFLOW) == CL_EV_OK);
    CHECK(cl_ev_signal(h.ev, g, CL_EV_CTL_INPUT_OVERFLOW) == CL_EV_OK); /* coalesces */
    /* Reserved terminals retire even though the queue is full. */
    CHECK(cl_ev_op_complete(h.ev, op, CL_EV_SUCCESS, buf, sizeof buf, rel) == CL_EV_OK);
    /* Result larger than admitted: consumed, turned into a failure. */
    CHECK(cl_ev_op_complete(h.ev, op2, CL_EV_SUCCESS, buf, sizeof buf, rel) ==
          CL_EV_OVERSIZE);
    CHECK(released == 1);
    /* Other modules are unaffected by one module's saturation. */
    CHECK(cl_ev_post(h.ev, g2, 1, 1, 0, 0, NULL, 0, NULL) == CL_EV_OK);

    /* Per-module byte bound, and process byte bound for results. */
    CHECK(cl_ev_op_begin(h.ev, g2, CL_EV_MOD_MAX_BYTES + 1, NULL, &extra) == CL_EV_EFULL);

    /* Drain: control first, then everything including both terminal outcomes. */
    {
        cl_ev_turn_result r = h_turn(&h);
        CHECK(h.log[0].d.kind == CL_EV_D_CONTROL);
        CHECK(h.log[0].d.control == CL_EV_CTL_INPUT_OVERFLOW);
        for (i = 0; i < 100 && r.more; i++)
            r = h_turn(&h);
        CHECK(!r.more);
    }
    CHECK(count_kind(&h, CL_EV_D_CONTROL, g) == 1);
    CHECK(count_kind(&h, CL_EV_D_OP, g) == 2);
    for (i = 0; i < h.n; i++)
        if (h.log[i].d.kind == CL_EV_D_OP && h.log[i].d.handle == op2) {
            CHECK(h.log[i].d.status == CL_EV_FAILURE);
            CHECK(h.log[i].d.detail == CL_EV_DETAIL_OVERSIZE);
            CHECK(h.log[i].d.bytes == 0);
        }
    CHECK(released == 1 + (int)posted + 1);
    slot = cl_ev_module_slot(h.ev, g);
    CHECK(slot >= 0);
    cl_ev_get_stats(h.ev, slot, &st);
    CHECK(st.items == 0 && st.bytes == 0 && st.ops == 0);
    check_proc_empty(h.ev);

    /* Process-wide item bound hits before any single module bound. */
    CHECK(cl_ev_module_stop(h.ev, g) == CL_EV_OK);
    CHECK(cl_ev_module_stop(h.ev, g2) == CL_EV_OK);
    for (i = 0; i < CL_EV_MAX_MODULES; i++)
        CHECK(cl_ev_module_start(h.ev, &gs[i]) == CL_EV_OK);
    CHECK(cl_ev_module_start(h.ev, &g) == CL_EV_EFULL);
    posted = 0;
    for (i = 0; i < CL_EV_MAX_MODULES; i++) {
        unsigned k;
        for (k = 0; k < CL_EV_MOD_MAX_ITEMS; k++)
            if (cl_ev_post(h.ev, gs[i], 1, 1, 0, 0, NULL, 0, NULL) == CL_EV_OK)
                posted++;
    }
    CHECK(posted == CL_EV_PROC_MAX_ITEMS);
    cl_ev_get_stats(h.ev, cl_ev_module_slot(h.ev, gs[CL_EV_MAX_MODULES - 1]), &st);
    CHECK(st.items < CL_EV_MOD_MAX_ITEMS); /* refused by the process bound */
    h_done(&h);
}

/* ---- timer expire then cancel before dispatch ---- */

static cl_ev_handle timer_h;
static int cancel_rc1, cancel_rc2;

static void hook_cancel_timer(harness *h, const cl_ev_delivery *d)
{
    if (d->kind == CL_EV_D_EVENT && d->type == 42) {
        cancel_rc1 = cl_ev_cancel(h->ev, d->gen, timer_h);
        cancel_rc2 = cl_ev_cancel(h->ev, d->gen, timer_h);
    }
}

static void test_timer_expire_then_cancel(void)
{
    harness h;
    cl_ev_gen g;
    cl_ev_turn_result r;
    cl_ev_stats st;

    h_init(&h);
    h.hook = hook_cancel_timer;
    CHECK(cl_ev_module_start(h.ev, &g) == CL_EV_OK);
    CHECK(cl_ev_timer_start(h.ev, g, 10, &timer_h) == CL_EV_OK);
    r = h_turn(&h);
    CHECK(!r.more && r.has_deadline && r.deadline == 10);
    /* Event queued before the timer is due; expiry is queued behind it at the turn
     * boundary and cancelled by the event callback before it is dispatched. */
    CHECK(cl_ev_post(h.ev, g, 3, 42, 0, 0, NULL, 0, NULL) == CL_EV_OK);
    cl_ev_fake_clock_advance(&h.clock, 10);
    r = h_turn(&h);
    CHECK(cancel_rc1 == CL_EV_SUPPRESSED);
    CHECK(cancel_rc2 == CL_EV_ALREADY);
    CHECK(cl_ev_cancel(h.ev, g, timer_h) == CL_EV_ALREADY);
    CHECK(!r.more && !r.has_deadline);
    cl_ev_fake_clock_advance(&h.clock, 1000);
    h_turn(&h);
    CHECK(count_kind(&h, CL_EV_D_TIMER, 0) == 0);
    cl_ev_get_stats(h.ev, cl_ev_module_slot(h.ev, g), &st);
    CHECK(st.timers == 0 && st.items == 0);

    /* Cancel of a still-armed timer, twice. */
    CHECK(cl_ev_timer_start(h.ev, g, 5, &timer_h) == CL_EV_OK);
    CHECK(cl_ev_cancel(h.ev, g, timer_h) == CL_EV_OK);
    CHECK(cl_ev_cancel(h.ev, g, timer_h) == CL_EV_ALREADY);
    cl_ev_fake_clock_advance(&h.clock, 100);
    r = h_turn(&h);
    CHECK(!r.more && !r.has_deadline);
    CHECK(count_kind(&h, CL_EV_D_TIMER, 0) == 0);
    /* A foreign module cannot cancel it, and a forged serial is stale. */
    {
        cl_ev_gen g2;
        cl_ev_handle t2;
        CHECK(cl_ev_module_start(h.ev, &g2) == CL_EV_OK);
        CHECK(cl_ev_timer_start(h.ev, g, 5, &t2) == CL_EV_OK);
        CHECK(cl_ev_cancel(h.ev, g2, t2) == CL_EV_ESTALE);
        CHECK(cl_ev_cancel(h.ev, g, t2 + 1000) == CL_EV_ESTALE);
        cl_ev_fake_clock_advance(&h.clock, 5);
        h_turn(&h);
        CHECK(count_kind(&h, CL_EV_D_TIMER, g) == 1);
    }
    check_proc_empty(h.ev);
    h_done(&h);
}

/* ---- resume after long suspension ---- */

static void test_timer_resume_fires_once(void)
{
    harness h;
    cl_ev_gen g;
    cl_ev_handle t[3];
    cl_ev_turn_result r;
    unsigned i, k = 0;

    h_init(&h);
    CHECK(cl_ev_module_start(h.ev, &g) == CL_EV_OK);
    CHECK(cl_ev_timer_start(h.ev, g, 30, &t[2]) == CL_EV_OK);
    CHECK(cl_ev_timer_start(h.ev, g, 10, &t[0]) == CL_EV_OK);
    CHECK(cl_ev_timer_start(h.ev, g, 20, &t[1]) == CL_EV_OK);
    r = h_turn(&h);
    CHECK(r.has_deadline && r.deadline == 10);
    cl_ev_fake_clock_advance(&h.clock, 1000000000u); /* suspended for ~11 days */
    for (i = 0; i < 5; i++)
        r = h_turn(&h);
    CHECK(count_kind(&h, CL_EV_D_TIMER, g) == 3);
    for (i = 0; i < h.n; i++)
        if (h.log[i].d.kind == CL_EV_D_TIMER)
            CHECK(h.log[i].d.handle == t[k++]); /* deadline order */
    CHECK(!r.more && !r.has_deadline);
    /* Timer bound. */
    for (i = 0; i < CL_EV_MOD_MAX_TIMERS; i++)
        CHECK(cl_ev_timer_start(h.ev, g, 1, &t[0]) == CL_EV_OK);
    CHECK(cl_ev_timer_start(h.ev, g, 1, &t[0]) == CL_EV_EFULL);
    cl_ev_fake_clock_advance(&h.clock, 1);
    r = h_turn(&h);
    CHECK(r.dispatched == CL_EV_TURN_MAX_PER_MODULE); /* budgeted, rest next turn */
    CHECK(r.more);
    h_done(&h);
}

/* ---- reload with pending timer / operation ---- */

typedef struct producer {
    int cancelled;
    cl_ev_handle h;
} producer;

static void prod_cancel(void *ctx, cl_ev_handle h)
{
    producer *p = ctx;
    p->cancelled++;
    p->h = h;
}

static void sync_cancel(void *ctx, cl_ev_handle h)
{
    cl_ev *ev = ctx;
    /* Producer that finishes its cleanup synchronously inside the cancel hook. */
    CHECK(cl_ev_op_complete(ev, h, CL_EV_FAILURE, NULL, 0, NULL) == CL_EV_RETIRED);
}

static void test_reload_retires_old_generation(void)
{
    harness h;
    cl_ev_gen g_old, g_new;
    cl_ev_handle tmr, op, op_sync, op_done, op_new;
    producer p = {0, 0};
    cl_ev_producer pr, pr_sync;
    cl_ev_stats st;
    cl_ev_turn_result r;
    int slot;
    static char buf[16];

    h_init(&h);
    released = 0;
    pr.cancel = prod_cancel;
    pr.ctx = &p;
    pr_sync.cancel = sync_cancel;
    pr_sync.ctx = h.ev;

    CHECK(cl_ev_module_start(h.ev, &g_old) == CL_EV_OK);
    slot = cl_ev_module_slot(h.ev, g_old);
    CHECK(cl_ev_timer_start(h.ev, g_old, 50, &tmr) == CL_EV_OK);
    CHECK(cl_ev_op_begin(h.ev, g_old, 16, &pr, &op) == CL_EV_OK);
    CHECK(cl_ev_op_begin(h.ev, g_old, 4, &pr_sync, &op_sync) == CL_EV_OK);
    CHECK(cl_ev_op_begin(h.ev, g_old, 16, NULL, &op_done) == CL_EV_OK);
    CHECK(cl_ev_op_complete(h.ev, op_done, CL_EV_SUCCESS, buf, 8, rel) == CL_EV_OK);
    CHECK(cl_ev_post(h.ev, g_old, 1, 1, 0, 0, buf, 3, rel) == CL_EV_OK);
    CHECK(cl_ev_invalidate(h.ev, g_old) == CL_EV_OK);
    CHECK(cl_ev_signal(h.ev, g_old, CL_EV_CTL_CONN_LOST) == CL_EV_OK);

    CHECK(cl_ev_module_reload(h.ev, g_old, &g_new) == CL_EV_OK);
    CHECK(g_new != g_old);
    CHECK(cl_ev_module_slot(h.ev, g_new) == slot);
    CHECK(cl_ev_module_slot(h.ev, g_old) == CL_EV_ESTALE);
    CHECK(p.cancelled == 1 && p.h == op);
    CHECK(released == 2); /* queued completion payload + queued event payload */

    /* Old generation is dead for every admission path. */
    CHECK(cl_ev_post(h.ev, g_old, 1, 1, 0, 0, NULL, 0, NULL) == CL_EV_ESTALE);
    CHECK(cl_ev_timer_start(h.ev, g_old, 1, &tmr) == CL_EV_ESTALE);
    CHECK(cl_ev_cancel(h.ev, g_old, op) == CL_EV_ESTALE);
    CHECK(cl_ev_cancel(h.ev, g_new, op) == CL_EV_ESTALE); /* handle is not the new gen's */
    CHECK(cl_ev_invalidate(h.ev, g_old) == CL_EV_ESTALE);

    /* The retiring operation still holds its reservation until its producer finishes. */
    cl_ev_get_stats(h.ev, slot, &st);
    CHECK(st.ops == 1 && st.items == 1 && st.bytes == 16 && st.queued == 0);

    /* New generation: fresh handles, never equal to old ones. */
    CHECK(cl_ev_op_begin(h.ev, g_new, 0, NULL, &op_new) == CL_EV_OK);
    CHECK(CL_EV_HANDLE_GEN(op_new) == g_new);
    CHECK(op_new != op && op_new != tmr && op_new != op_sync && op_new != op_done);
    /* Producers cannot confuse an old handle with a new one. */
    CHECK(cl_ev_op_complete(h.ev, op_new, CL_EV_SUCCESS, NULL, 0, NULL) == CL_EV_OK);

    /* Late completion of the old operation: accounting done, no delivery. */
    CHECK(cl_ev_op_complete(h.ev, op, CL_EV_SUCCESS, buf, 16, rel) == CL_EV_RETIRED);
    CHECK(released == 3);
    CHECK(cl_ev_op_complete(h.ev, op, CL_EV_SUCCESS, NULL, 0, NULL) == CL_EV_ESTALE);

    cl_ev_fake_clock_advance(&h.clock, 1000);
    r = h_turn(&h);
    r = h_turn(&h);
    CHECK(!r.more && !r.has_deadline);
    {
        unsigned i;
        for (i = 0; i < h.n; i++)
            CHECK(h.log[i].d.gen == g_new);
    }
    CHECK(count_kind(&h, CL_EV_D_OP, g_new) == 1);
    CHECK(count_kind(&h, CL_EV_D_TIMER, 0) == 0);
    check_proc_empty(h.ev);

    /* A plain stop frees the slot for another module only once retired. */
    CHECK(cl_ev_op_begin(h.ev, g_new, 0, &pr, &op) == CL_EV_OK);
    CHECK(cl_ev_module_stop(h.ev, g_new) == CL_EV_OK);
    CHECK(cl_ev_module_stop(h.ev, g_new) == CL_EV_ESTALE);
    cl_ev_get_stats(h.ev, slot, &st);
    CHECK(st.gen == 0 && st.ops == 1);
    CHECK(cl_ev_op_complete(h.ev, op, CL_EV_FAILURE, NULL, 0, NULL) == CL_EV_RETIRED);
    check_proc_empty(h.ev);
    h_done(&h);
}

/* ---- exactly one terminal state ---- */

static void test_exactly_one_terminal(void)
{
    harness h;
    cl_ev_gen g;
    cl_ev_handle a, b;
    producer p = {0, 0};
    cl_ev_producer pr;

    h_init(&h);
    pr.cancel = prod_cancel;
    pr.ctx = &p;
    CHECK(cl_ev_module_start(h.ev, &g) == CL_EV_OK);
    /* cancel, then producer completes -> cancelled, no delivery */
    CHECK(cl_ev_op_begin(h.ev, g, 0, &pr, &a) == CL_EV_OK);
    CHECK(cl_ev_cancel(h.ev, g, a) == CL_EV_OK);
    CHECK(cl_ev_cancel(h.ev, g, a) == CL_EV_ALREADY);
    CHECK(p.cancelled == 1);
    CHECK(cl_ev_op_complete(h.ev, a, CL_EV_SUCCESS, NULL, 0, NULL) == CL_EV_RETIRED);
    /* complete, then cancel before dispatch -> success stays, delivery suppressed */
    CHECK(cl_ev_op_begin(h.ev, g, 0, &pr, &b) == CL_EV_OK);
    CHECK(cl_ev_op_complete(h.ev, b, CL_EV_SUCCESS, NULL, 0, NULL) == CL_EV_OK);
    CHECK(cl_ev_op_complete(h.ev, b, CL_EV_FAILURE, NULL, 0, NULL) == CL_EV_EINVAL);
    CHECK(cl_ev_cancel(h.ev, g, b) == CL_EV_SUPPRESSED);
    CHECK(p.cancelled == 1); /* producer already finished: not asked again */
    CHECK(cl_ev_op_complete(h.ev, b, CL_EV_CANCELLED, NULL, 0, NULL) == CL_EV_ESTALE);
    h_turn(&h);
    CHECK(h.n == 0);
    check_proc_empty(h.ev);
    h_done(&h);
}

/* ---- idle ---- */

static void test_idle_reports_nothing(void)
{
    harness h;
    cl_ev_gen g;
    cl_ev_handle t;
    cl_ev_turn_result r;

    h_init(&h);
    r = h_turn(&h);
    CHECK(!r.more && !r.has_deadline && r.dispatched == 0);
    CHECK(cl_ev_module_start(h.ev, &g) == CL_EV_OK);
    r = h_turn(&h);
    CHECK(!r.more && !r.has_deadline && r.dispatched == 0);
    cl_ev_fake_clock_advance(&h.clock, 7);
    CHECK(cl_ev_timer_start(h.ev, g, 100, &t) == CL_EV_OK);
    r = h_turn(&h);
    CHECK(!r.more && r.has_deadline && r.deadline == 107 && r.dispatched == 0);
    CHECK(cl_ev_invalidate(h.ev, g) == CL_EV_OK);
    CHECK(cl_ev_invalidate(h.ev, g) == CL_EV_OK); /* coalesced latch */
    r = h_turn(&h);
    CHECK(r.dispatched == 1 && !r.more);
    CHECK(count_kind(&h, CL_EV_D_DRAW, g) == 1);
    r = h_turn(&h);
    CHECK(r.dispatched == 0 && !r.more);
    CHECK(h.n == 1);
    h_done(&h);
}

/* ---- pointer-motion coalescing never crosses a button edge ---- */

enum { SRC_PTR = 5, T_MOTION = 1, T_BUTTON = 2 };

static void test_motion_coalescing_preserves_button_edges(void)
{
    harness h;
    cl_ev_gen g;
    cl_ev_stats st;

    h_init(&h);
    CHECK(cl_ev_module_start(h.ev, &g) == CL_EV_OK);
    CHECK(cl_ev_post_latest(h.ev, g, SRC_PTR, T_MOTION, 1, 1) == CL_EV_OK);
    CHECK(cl_ev_post_latest(h.ev, g, SRC_PTR, T_MOTION, 2, 2) == CL_EV_OK);
    CHECK(cl_ev_post(h.ev, g, SRC_PTR, T_BUTTON, 1, 0, NULL, 0, NULL) == CL_EV_OK);
    CHECK(cl_ev_post_latest(h.ev, g, SRC_PTR, T_MOTION, 3, 3) == CL_EV_OK);
    CHECK(cl_ev_post_latest(h.ev, g, SRC_PTR, T_MOTION, 4, 4) == CL_EV_OK);
    cl_ev_get_stats(h.ev, cl_ev_module_slot(h.ev, g), &st);
    CHECK(st.queued == 3);
    h_turn(&h);
    CHECK(h.n == 3);
    CHECK(h.log[0].d.type == T_MOTION && h.log[0].d.a == 2);
    CHECK(h.log[1].d.type == T_BUTTON);
    CHECK(h.log[2].d.type == T_MOTION && h.log[2].d.a == 4);
    /* An ordered item of the same source/type is never overwritten. */
    CHECK(cl_ev_post(h.ev, g, SRC_PTR, T_MOTION, 5, 5, NULL, 0, NULL) == CL_EV_OK);
    CHECK(cl_ev_post_latest(h.ev, g, SRC_PTR, T_MOTION, 6, 6) == CL_EV_OK);
    h_turn(&h);
    CHECK(h.n == 5);
    CHECK(h.log[3].d.a == 5 && h.log[4].d.a == 6);
    /* Different latest-state kinds of one source do not merge. */
    CHECK(cl_ev_post_latest(h.ev, g, SRC_PTR, T_MOTION + 10, 7, 7) == CL_EV_OK);
    CHECK(cl_ev_post_latest(h.ev, g, SRC_PTR, T_MOTION, 8, 8) == CL_EV_OK);
    h_turn(&h);
    CHECK(h.n == 7);
    CHECK(h.log[5].d.a == 7 && h.log[6].d.a == 8);
    h_done(&h);
}

/* ---- FIFO per stream, fair rotation ---- */

static void test_fifo_and_rotation(void)
{
    harness h;
    cl_ev_gen a, b;
    unsigned i, last_a = 0, last_b = 0;
    int ok = 1;

    h_init(&h);
    CHECK(cl_ev_module_start(h.ev, &a) == CL_EV_OK);
    CHECK(cl_ev_module_start(h.ev, &b) == CL_EV_OK);
    for (i = 1; i <= 10; i++) {
        CHECK(cl_ev_post(h.ev, a, 1, 1, (int32_t)i, 0, NULL, 0, NULL) == CL_EV_OK);
        CHECK(cl_ev_post(h.ev, b, 1, 1, (int32_t)i, 0, NULL, 0, NULL) == CL_EV_OK);
    }
    h_turn(&h);
    /* Both modules served in the same turn, alternating. */
    CHECK(count_kind(&h, CL_EV_D_EVENT, a) == CL_EV_TURN_MAX_PER_MODULE);
    CHECK(count_kind(&h, CL_EV_D_EVENT, b) == CL_EV_TURN_MAX_PER_MODULE);
    CHECK(h.log[0].d.gen != h.log[1].d.gen);
    for (i = 0; i < 10; i++)
        h_turn(&h);
    for (i = 0; i < h.n; i++) {
        unsigned *last = h.log[i].d.gen == a ? &last_a : &last_b;
        if ((unsigned)h.log[i].d.a != *last + 1)
            ok = 0;
        *last = (unsigned)h.log[i].d.a;
    }
    CHECK(ok && last_a == 10 && last_b == 10);
    h_done(&h);
}

int main(void)
{
    test_inline_completion_after_boundary();
    test_busy_beside_interactive();
    test_saturated_queue();
    test_timer_expire_then_cancel();
    test_timer_resume_fires_once();
    test_reload_retires_old_generation();
    test_exactly_one_terminal();
    test_idle_reports_nothing();
    test_motion_coalescing_preserves_button_edges();
    test_fifo_and_rotation();
    if (failures) {
        fprintf(stderr, "test_ev: %d check(s) failed\n", failures);
        return 1;
    }
    printf("test_ev: all checks passed\n");
    return 0;
}
