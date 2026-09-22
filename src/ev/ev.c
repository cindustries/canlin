/* CanLin event scheduler core. See include/canlin/ev.h for the contract. */
#include "canlin/ev.h"

#include <stdlib.h>
#include <string.h>

typedef struct item {
    cl_ev_kind kind;           /* EVENT, OP or TIMER */
    int latest;                /* latest-state (coalescable) event */
    cl_ev_gen gen;
    uint32_t source, type;
    int32_t a, b;
    int op;                    /* op table index, -1 for plain events */
    void *payload;
    size_t bytes;
    cl_ev_release_fn release;
    uint64_t stamp;            /* turn sequence at enqueue; eligible when < current */
} item;

enum { OP_FREE = 0, OP_PENDING, OP_CANCELLED, OP_QUEUED, OP_DELIVERING };

typedef struct op {
    int state;
    int is_timer;
    unsigned slot;
    cl_ev_gen gen;
    cl_ev_handle h;
    size_t reserved;           /* reserved result bytes while pending */
    cl_ev_time deadline;       /* timers */
    cl_ev_producer prod;
} op;

typedef struct mod {
    cl_ev_gen gen;             /* active generation, 0 = inactive */
    uint32_t serial;           /* last handle serial issued in `gen` */
    unsigned ctl;
    int dirty;
    item ring[CL_EV_MOD_MAX_ITEMS];
    unsigned head, count;
    unsigned items;            /* queued + reserved */
    size_t bytes;
    unsigned ops, timers;
} mod;

struct cl_ev {
    cl_ev_clock clock;
    cl_ev_callbacks cb;
    void *ud;
    cl_ev_gen next_gen;        /* 0 once the space is exhausted */
    uint64_t turn_seq;
    cl_ev_time last_now;
    int in_cb;
    unsigned rr;
    mod mods[CL_EV_MAX_MODULES];
    op ops[CL_EV_PROC_MAX_OPS];
    unsigned items;
    size_t bytes;
    unsigned nops, ntimers;
};

/* ---- fake clock ---- */

static cl_ev_time fake_now(void *ctx) { return ((cl_ev_fake_clock *)ctx)->now; }

cl_ev_clock cl_ev_fake_clock_bind(cl_ev_fake_clock *fc)
{
    cl_ev_clock c;
    c.now = fake_now;
    c.ctx = fc;
    return c;
}

void cl_ev_fake_clock_advance(cl_ev_fake_clock *fc, cl_ev_time ms)
{
    fc->now = (fc->now > UINT64_MAX - ms) ? UINT64_MAX : fc->now + ms;
}

static cl_ev_time clock_now(cl_ev *ev)
{
    cl_ev_time t = ev->clock.now(ev->clock.ctx);
    if (t < ev->last_now)
        t = ev->last_now; /* monotonic clamp */
    ev->last_now = t;
    return t;
}

/* ---- accounting ---- */

static int has_room(const cl_ev *ev, const mod *m, size_t bytes)
{
    return m->items < CL_EV_MOD_MAX_ITEMS && ev->items < CL_EV_PROC_MAX_ITEMS &&
           bytes <= CL_EV_MOD_MAX_BYTES - m->bytes &&
           bytes <= CL_EV_PROC_MAX_BYTES - ev->bytes;
}

static void acct_add(cl_ev *ev, mod *m, size_t bytes)
{
    m->items++;
    ev->items++;
    m->bytes += bytes;
    ev->bytes += bytes;
}

static void acct_sub(cl_ev *ev, mod *m, size_t bytes)
{
    m->items--;
    ev->items--;
    m->bytes -= bytes;
    ev->bytes -= bytes;
}

/* ---- ring ---- */

static item *ring_at(mod *m, unsigned i)
{
    return &m->ring[(m->head + i) % CL_EV_MOD_MAX_ITEMS];
}

static void ring_push(mod *m, const item *it)
{
    /* Room is guaranteed: count <= items <= CL_EV_MOD_MAX_ITEMS by admission. */
    *ring_at(m, m->count) = *it;
    m->count++;
}

static void ring_remove(mod *m, unsigned i)
{
    for (; i + 1 < m->count; i++)
        *ring_at(m, i) = *ring_at(m, i + 1);
    m->count--;
}

static void ring_pop(mod *m, item *out)
{
    *out = m->ring[m->head];
    m->head = (m->head + 1) % CL_EV_MOD_MAX_ITEMS;
    m->count--;
}

/* ---- lookup ---- */

static mod *find_mod(cl_ev *ev, cl_ev_gen gen)
{
    unsigned s;
    if (gen == 0)
        return NULL;
    for (s = 0; s < CL_EV_MAX_MODULES; s++)
        if (ev->mods[s].gen == gen)
            return &ev->mods[s];
    return NULL;
}

static int find_op(const cl_ev *ev, cl_ev_handle h)
{
    unsigned i;
    if (h == 0)
        return -1;
    for (i = 0; i < CL_EV_PROC_MAX_OPS; i++)
        if (ev->ops[i].state != OP_FREE && ev->ops[i].h == h)
            return (int)i;
    return -1;
}

static void op_free(cl_ev *ev, int i)
{
    op *o = &ev->ops[i];
    mod *m = &ev->mods[o->slot];
    m->ops--;
    ev->nops--;
    if (o->is_timer) {
        m->timers--;
        ev->ntimers--;
    }
    memset(o, 0, sizeof *o);
}

static void release_payload(void *payload, size_t bytes, cl_ev_release_fn release)
{
    if (release)
        release(payload, bytes);
}

/* Retire a queued item without delivery: payload, accounting, op record. */
static void item_retire(cl_ev *ev, mod *m, item *it)
{
    release_payload(it->payload, it->bytes, it->release);
    acct_sub(ev, m, it->bytes);
    if (it->op >= 0)
        op_free(ev, it->op);
}

/* Convert an op's reservation into a queued terminal item. */
static void queue_terminal(cl_ev *ev, int i, int detail, void *payload, size_t bytes,
                           cl_ev_release_fn release, cl_ev_status st, uint64_t stamp)
{
    op *o = &ev->ops[i];
    mod *m = &ev->mods[o->slot];
    item it;

    memset(&it, 0, sizeof it);
    it.kind = o->is_timer ? CL_EV_D_TIMER : CL_EV_D_OP;
    it.gen = o->gen;
    it.op = i;
    it.a = (int32_t)st;
    it.b = detail;
    it.payload = payload;
    it.bytes = bytes;
    it.release = release;
    it.stamp = stamp;
    /* bytes <= reserved: shrink the byte reservation to the actual size. */
    m->bytes -= o->reserved - bytes;
    ev->bytes -= o->reserved - bytes;
    o->reserved = bytes;
    o->state = OP_QUEUED;
    ring_push(m, &it);
}

static int alloc_handle(mod *m, cl_ev_handle *out)
{
    if (m->serial == UINT32_MAX)
        return CL_EV_ELIMIT;
    m->serial++;
    *out = ((cl_ev_handle)m->gen << 32) | m->serial;
    return CL_EV_OK;
}

static int alloc_op(const cl_ev *ev)
{
    unsigned i;
    for (i = 0; i < CL_EV_PROC_MAX_OPS; i++)
        if (ev->ops[i].state == OP_FREE)
            return (int)i;
    return -1;
}

/* ---- public: construction ---- */

cl_ev *cl_ev_new(const cl_ev_clock *clock, const cl_ev_callbacks *cb, void *ud)
{
    cl_ev *ev;
    if (!clock || !clock->now || !cb || !cb->deliver)
        return NULL;
    ev = calloc(1, sizeof *ev);
    if (!ev)
        return NULL;
    ev->clock = *clock;
    ev->cb = *cb;
    ev->ud = ud;
    ev->next_gen = 1;
    ev->last_now = clock->now(clock->ctx);
    return ev;
}

int cl_ev_free(cl_ev *ev)
{
    unsigned s;
    if (!ev)
        return CL_EV_OK;
    if (ev->in_cb)
        return CL_EV_EREENTRY;
    for (s = 0; s < CL_EV_MAX_MODULES; s++) {
        mod *m = &ev->mods[s];
        while (m->count) {
            item it;
            ring_pop(m, &it);
            release_payload(it.payload, it.bytes, it.release);
        }
    }
    free(ev);
    return CL_EV_OK;
}

/* ---- public: lifecycle ---- */

static int start_in_slot(cl_ev *ev, mod *m, cl_ev_gen *gen_out)
{
    if (ev->next_gen == 0)
        return CL_EV_ELIMIT;
    m->gen = ev->next_gen;
    ev->next_gen = (ev->next_gen == UINT32_MAX) ? 0 : ev->next_gen + 1;
    m->serial = 0;
    m->ctl = 0;
    m->dirty = 0;
    *gen_out = m->gen;
    return CL_EV_OK;
}

int cl_ev_module_start(cl_ev *ev, cl_ev_gen *gen_out)
{
    unsigned s;
    for (s = 0; s < CL_EV_MAX_MODULES; s++) {
        mod *m = &ev->mods[s];
        if (m->gen == 0 && m->items == 0 && m->ops == 0 && m->count == 0)
            return start_in_slot(ev, m, gen_out);
    }
    return CL_EV_EFULL;
}

int cl_ev_module_stop(cl_ev *ev, cl_ev_gen gen)
{
    mod *m = find_mod(ev, gen);
    unsigned slot, i;

    if (!m)
        return CL_EV_ESTALE;
    slot = (unsigned)(m - ev->mods);

    /* 1. inactive: no admissions, no callbacks, latches dropped. */
    m->gen = 0;
    m->ctl = 0;
    m->dirty = 0;

    /* 2. cancel timers and pending operations. */
    for (i = 0; i < CL_EV_PROC_MAX_OPS; i++) {
        op *o = &ev->ops[i];
        if (o->state != OP_PENDING || o->slot != slot || o->gen != gen)
            continue;
        if (o->is_timer) {
            acct_sub(ev, m, 0);
            op_free(ev, (int)i);
        } else {
            o->state = OP_CANCELLED;
            if (o->prod.cancel)
                o->prod.cancel(o->prod.ctx, o->h); /* may complete synchronously */
        }
    }

    /* 3. retire queued old-generation work: accounting, no delivery. */
    for (i = 0; i < m->count;) {
        item *it = ring_at(m, i);
        if (it->gen == gen) {
            item copy = *it;
            ring_remove(m, i);
            item_retire(ev, m, &copy);
        } else {
            i++;
        }
    }
    return CL_EV_OK;
}

int cl_ev_module_reload(cl_ev *ev, cl_ev_gen old_gen, cl_ev_gen *new_gen)
{
    mod *m = find_mod(ev, old_gen);
    int rc;
    if (!m)
        return CL_EV_ESTALE;
    if (ev->next_gen == 0)
        return CL_EV_ELIMIT;
    rc = cl_ev_module_stop(ev, old_gen);
    if (rc < 0)
        return rc;
    return start_in_slot(ev, m, new_gen);
}

int cl_ev_module_slot(const cl_ev *ev, cl_ev_gen gen)
{
    unsigned s;
    if (gen == 0)
        return CL_EV_ESTALE;
    for (s = 0; s < CL_EV_MAX_MODULES; s++)
        if (ev->mods[s].gen == gen)
            return (int)s;
    return CL_EV_ESTALE;
}

/* ---- public: events and latches ---- */

int cl_ev_post(cl_ev *ev, cl_ev_gen gen, uint32_t source, uint32_t type,
               int32_t a, int32_t b, void *payload, size_t bytes, cl_ev_release_fn release)
{
    mod *m = find_mod(ev, gen);
    item it;

    if (!m)
        return CL_EV_ESTALE;
    if (!has_room(ev, m, bytes))
        return CL_EV_EFULL;
    memset(&it, 0, sizeof it);
    it.kind = CL_EV_D_EVENT;
    it.gen = gen;
    it.source = source;
    it.type = type;
    it.a = a;
    it.b = b;
    it.op = -1;
    it.payload = payload;
    it.bytes = bytes;
    it.release = release;
    it.stamp = ev->turn_seq;
    acct_add(ev, m, bytes);
    ring_push(m, &it);
    return CL_EV_OK;
}

int cl_ev_post_latest(cl_ev *ev, cl_ev_gen gen, uint32_t source, uint32_t type,
                      int32_t a, int32_t b)
{
    mod *m = find_mod(ev, gen);
    int rc;

    if (!m)
        return CL_EV_ESTALE;
    if (m->count) {
        item *tail = ring_at(m, m->count - 1);
        if (tail->kind == CL_EV_D_EVENT && tail->latest && tail->gen == gen &&
            tail->source == source && tail->type == type) {
            tail->a = a;
            tail->b = b;
            return CL_EV_OK;
        }
    }
    rc = cl_ev_post(ev, gen, source, type, a, b, NULL, 0, NULL);
    if (rc == CL_EV_OK)
        ring_at(m, m->count - 1)->latest = 1;
    return rc;
}

int cl_ev_signal(cl_ev *ev, cl_ev_gen gen, unsigned control_bits)
{
    mod *m = find_mod(ev, gen);
    if (!m)
        return CL_EV_ESTALE;
    if (control_bits == 0)
        return CL_EV_EINVAL;
    m->ctl |= control_bits;
    return CL_EV_OK;
}

int cl_ev_invalidate(cl_ev *ev, cl_ev_gen gen)
{
    mod *m = find_mod(ev, gen);
    if (!m)
        return CL_EV_ESTALE;
    m->dirty = 1;
    return CL_EV_OK;
}

/* ---- public: operations and timers ---- */

static int admit_op(cl_ev *ev, cl_ev_gen gen, size_t bytes, int is_timer, int *idx,
                    cl_ev_handle *h)
{
    mod *m = find_mod(ev, gen);
    int i, rc;

    if (!m)
        return CL_EV_ESTALE;
    if (m->ops >= CL_EV_MOD_MAX_OPS || ev->nops >= CL_EV_PROC_MAX_OPS)
        return CL_EV_EFULL;
    if (is_timer && (m->timers >= CL_EV_MOD_MAX_TIMERS ||
                     ev->ntimers >= CL_EV_PROC_MAX_TIMERS))
        return CL_EV_EFULL;
    if (!has_room(ev, m, bytes))
        return CL_EV_EFULL;
    i = alloc_op(ev);
    if (i < 0)
        return CL_EV_EFULL;
    rc = alloc_handle(m, h);
    if (rc < 0)
        return rc;

    ev->ops[i].state = OP_PENDING;
    ev->ops[i].is_timer = is_timer;
    ev->ops[i].slot = (unsigned)(m - ev->mods);
    ev->ops[i].gen = gen;
    ev->ops[i].h = *h;
    ev->ops[i].reserved = bytes;
    acct_add(ev, m, bytes);
    m->ops++;
    ev->nops++;
    if (is_timer) {
        m->timers++;
        ev->ntimers++;
    }
    *idx = i;
    return CL_EV_OK;
}

int cl_ev_op_begin(cl_ev *ev, cl_ev_gen gen, size_t max_result_bytes,
                   const cl_ev_producer *prod, cl_ev_handle *out)
{
    int i, rc;
    cl_ev_handle h;

    if (!out)
        return CL_EV_EINVAL;
    rc = admit_op(ev, gen, max_result_bytes, 0, &i, &h);
    if (rc < 0)
        return rc;
    if (prod)
        ev->ops[i].prod = *prod;
    *out = h;
    return CL_EV_OK;
}

int cl_ev_op_complete(cl_ev *ev, cl_ev_handle h, cl_ev_status status,
                      void *payload, size_t bytes, cl_ev_release_fn release)
{
    int i = find_op(ev, h);
    op *o;
    int rc = CL_EV_OK, detail = CL_EV_DETAIL_NONE;

    if (i < 0)
        return CL_EV_ESTALE;
    o = &ev->ops[i];
    if (o->is_timer || (status != CL_EV_SUCCESS && status != CL_EV_FAILURE))
        return CL_EV_EINVAL;

    if (o->state == OP_CANCELLED) {
        /* Terminal already CANCELLED: retire, no delivery. */
        release_payload(payload, bytes, release);
        acct_sub(ev, &ev->mods[o->slot], o->reserved);
        op_free(ev, i);
        return CL_EV_RETIRED;
    }
    if (o->state != OP_PENDING)
        return CL_EV_EINVAL; /* double completion */

    if (bytes > o->reserved) {
        release_payload(payload, bytes, release);
        payload = NULL;
        bytes = 0;
        release = NULL;
        status = CL_EV_FAILURE;
        detail = CL_EV_DETAIL_OVERSIZE;
        rc = CL_EV_OVERSIZE;
    }
    /* Stamped with the current turn: never eligible before the next turn. */
    queue_terminal(ev, i, detail, payload, bytes, release, status, ev->turn_seq);
    return rc;
}

int cl_ev_timer_start(cl_ev *ev, cl_ev_gen gen, cl_ev_time delay_ms, cl_ev_handle *out)
{
    int i, rc;
    cl_ev_handle h;
    cl_ev_time now;

    if (!out)
        return CL_EV_EINVAL;
    rc = admit_op(ev, gen, 0, 1, &i, &h);
    if (rc < 0)
        return rc;
    now = clock_now(ev);
    ev->ops[i].deadline = (now > UINT64_MAX - delay_ms) ? UINT64_MAX : now + delay_ms;
    *out = h;
    return CL_EV_OK;
}

int cl_ev_cancel(cl_ev *ev, cl_ev_gen gen, cl_ev_handle h)
{
    mod *m = find_mod(ev, gen);
    int i;
    op *o;

    if (!m || CL_EV_HANDLE_GEN(h) != gen)
        return CL_EV_ESTALE;
    i = find_op(ev, h);
    if (i < 0) {
        uint32_t serial = (uint32_t)(h & 0xffffffffu);
        /* Issued by this generation and already finished: repeat cancel is harmless. */
        return (serial != 0 && serial <= m->serial) ? CL_EV_ALREADY : CL_EV_ESTALE;
    }
    o = &ev->ops[i];
    switch (o->state) {
    case OP_PENDING:
        if (o->is_timer) {
            acct_sub(ev, m, 0);
            op_free(ev, i);
        } else {
            o->state = OP_CANCELLED;
            if (o->prod.cancel)
                o->prod.cancel(o->prod.ctx, h); /* may complete synchronously */
        }
        return CL_EV_OK;
    case OP_QUEUED: {
        unsigned k;
        for (k = 0; k < m->count; k++) {
            item *it = ring_at(m, k);
            if (it->op == i) {
                item copy = *it;
                ring_remove(m, k);
                item_retire(ev, m, &copy);
                return CL_EV_SUPPRESSED;
            }
        }
        return CL_EV_EINVAL; /* unreachable: queued op not in its ring */
    }
    default: /* CANCELLED or DELIVERING */
        return CL_EV_ALREADY;
    }
}

/* ---- public: turn ---- */

static void deliver(cl_ev *ev, const cl_ev_delivery *d)
{
    ev->in_cb = 1;
    ev->cb.deliver(ev->ud, ev, d);
    ev->in_cb = 0;
}

static void expire_timers(cl_ev *ev, cl_ev_time now)
{
    for (;;) {
        int best = -1;
        unsigned i;
        for (i = 0; i < CL_EV_PROC_MAX_OPS; i++) {
            const op *o = &ev->ops[i];
            if (o->state != OP_PENDING || !o->is_timer || o->deadline > now)
                continue;
            if (best < 0 || o->deadline < ev->ops[best].deadline ||
                (o->deadline == ev->ops[best].deadline && o->h < ev->ops[best].h))
                best = (int)i;
        }
        if (best < 0)
            return;
        /* Expired at this boundary: eligible in this turn. */
        queue_terminal(ev, best, CL_EV_DETAIL_NONE, NULL, 0, NULL, CL_EV_SUCCESS,
                       ev->turn_seq - 1);
    }
}

/* One dispatch for slot s, or 0 if it has nothing eligible. */
static int dispatch_one(cl_ev *ev, unsigned s, unsigned *quota, int *drawn, int *ctl_done)
{
    mod *m = &ev->mods[s];
    cl_ev_delivery d;

    if (m->gen == 0)
        return 0;
    memset(&d, 0, sizeof d);
    d.slot = s;
    d.gen = m->gen;

    if (m->ctl && !*ctl_done) {
        d.kind = CL_EV_D_CONTROL;
        d.control = m->ctl;
        m->ctl = 0;
        *ctl_done = 1;
        deliver(ev, &d);
        return 1;
    }
    if (*quota < CL_EV_TURN_MAX_PER_MODULE && m->count &&
        m->ring[m->head].stamp < ev->turn_seq) {
        item it;
        ring_pop(m, &it);
        if (it.op >= 0)
            ev->ops[it.op].state = OP_DELIVERING;
        d.kind = it.kind;
        d.source = it.source;
        d.type = it.type;
        if (it.kind == CL_EV_D_EVENT) {
            d.a = it.a;
            d.b = it.b;
        } else {
            d.handle = ev->ops[it.op].h;
            d.status = (cl_ev_status)it.a;
            d.detail = it.b;
        }
        d.payload = it.payload;
        d.bytes = it.bytes;
        (*quota)++;
        deliver(ev, &d);
        item_retire(ev, m, &it); /* after the boundary: payload, accounting, op */
        return 1;
    }
    if (m->dirty && !*drawn) {
        d.kind = CL_EV_D_DRAW;
        m->dirty = 0;
        *drawn = 1;
        deliver(ev, &d);
        return 1;
    }
    return 0;
}

int cl_ev_turn(cl_ev *ev, cl_ev_turn_result *out)
{
    unsigned quota[CL_EV_MAX_MODULES];
    int drawn[CL_EV_MAX_MODULES], ctl_done[CL_EV_MAX_MODULES];
    unsigned budget = CL_EV_TURN_MAX_DISPATCH, s, i, last = ev->rr;
    int progress = 1;
    cl_ev_turn_result r;

    if (ev->in_cb)
        return CL_EV_EREENTRY;
    memset(quota, 0, sizeof quota);
    memset(drawn, 0, sizeof drawn);
    memset(ctl_done, 0, sizeof ctl_done);
    memset(&r, 0, sizeof r);

    ev->turn_seq++;
    expire_timers(ev, clock_now(ev));

    while (budget && progress) {
        progress = 0;
        for (i = 0; i < CL_EV_MAX_MODULES && budget; i++) {
            s = (ev->rr + i) % CL_EV_MAX_MODULES;
            if (dispatch_one(ev, s, &quota[s], &drawn[s], &ctl_done[s])) {
                progress = 1;
                budget--;
                r.dispatched++;
                last = s;
            }
        }
    }
    ev->rr = (last + 1) % CL_EV_MAX_MODULES;

    for (s = 0; s < CL_EV_MAX_MODULES; s++) {
        const mod *m = &ev->mods[s];
        if (m->count || (m->gen && (m->ctl || m->dirty)))
            r.more = 1;
    }
    for (i = 0; i < CL_EV_PROC_MAX_OPS; i++) {
        const op *o = &ev->ops[i];
        if (o->state == OP_PENDING && o->is_timer &&
            (!r.has_deadline || o->deadline < r.deadline)) {
            r.has_deadline = 1;
            r.deadline = o->deadline;
        }
    }
    if (out)
        *out = r;
    return CL_EV_OK;
}

int cl_ev_get_stats(const cl_ev *ev, int slot, cl_ev_stats *out)
{
    memset(out, 0, sizeof *out);
    if (slot < 0) {
        out->items = ev->items;
        out->bytes = ev->bytes;
        out->ops = ev->nops;
        out->timers = ev->ntimers;
        for (unsigned s = 0; s < CL_EV_MAX_MODULES; s++)
            out->queued += ev->mods[s].count;
        return CL_EV_OK;
    }
    if ((unsigned)slot >= CL_EV_MAX_MODULES)
        return CL_EV_EINVAL;
    {
        const mod *m = &ev->mods[slot];
        out->items = m->items;
        out->bytes = m->bytes;
        out->queued = m->count;
        out->ops = m->ops;
        out->timers = m->timers;
        out->control = m->ctl;
        out->dirty = m->dirty;
        out->gen = m->gen;
    }
    return CL_EV_OK;
}
