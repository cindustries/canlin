/* Native loop core (native loop spec §3–§5, canlin#61): timeout computation, idle,
 * bounded turns, render only when dirty, quit — fake clock and fake wait; the wake pipe
 * from another thread with the real poll() wait and the monotonic clock. Exits 0 on
 * success. */
#define _POSIX_C_SOURCE 200809L
#include "canlin/loop.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

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

typedef struct harness harness;

typedef struct fsrc {
    harness *h;
    int fd;                  /* -1: no descriptor */
    size_t nfds;             /* descriptors wanted (all `fd`) */
    canlin_ev_time deadline; /* CANLIN_LOOP_NO_DEADLINE: none */
    unsigned steps;
    canlin_ev_time last_now;
    void (*on_step)(struct fsrc *s);
} fsrc;

struct harness {
    canlin_ev_fake_clock clock;
    canlin_ev *ev;
    canlin_loop *loop;
    canlin_loop_fake_wait fw;
    canlin_ev_gen g;
    unsigned events, timers, draws, pumps;
    unsigned order[64], norder;          /* 1 turn-delivery, 2 pump, 3 step, 4 frame */
    void (*on_event)(harness *h, const canlin_ev_delivery *d);
    void (*on_pump)(harness *h);
    /* fake display */
    size_t nout;
    int dirty[3];
    int sticky;                          /* dirty stays set after a frame */
    unsigned frames[3], updates;
    int64_t tick;
    void (*on_update)(harness *h);
};

static void rec(harness *h, unsigned what)
{
    if (h->norder < sizeof h->order / sizeof h->order[0])
        h->order[h->norder++] = what;
}

static void on_deliver(void *ud, canlin_ev *ev, const canlin_ev_delivery *d)
{
    harness *h = ud;
    (void)ev;
    rec(h, 1);
    if (d->kind == CANLIN_EV_D_EVENT)
        h->events++;
    if (d->kind == CANLIN_EV_D_TIMER)
        h->timers++;
    if (d->kind == CANLIN_EV_D_DRAW)
        h->draws++;
    if (h->on_event)
        h->on_event(h, d);
}

static void pump(void *ctx)
{
    harness *h = ctx;
    h->pumps++;
    rec(h, 2);
    if (h->on_pump)
        h->on_pump(h);
}

static void d_update(void *ctx)
{
    harness *h = ctx;
    h->updates++;
    if (h->on_update)
        h->on_update(h);
}

static size_t d_outputs(void *ctx)
{
    return ((harness *)ctx)->nout;
}

static int d_dirty(void *ctx, size_t i)
{
    return ((harness *)ctx)->dirty[i];
}

static void d_frame(void *ctx, size_t i)
{
    harness *h = ctx;
    rec(h, 4);
    h->frames[i]++;
    if (!h->sticky)
        h->dirty[i] = 0;
}

static int64_t d_tick(void *ctx)
{
    return ((harness *)ctx)->tick;
}

static size_t s_pollfds(void *ctx, struct pollfd *out, size_t max, canlin_ev_time *deadline)
{
    fsrc *s = ctx;
    size_t i;
    *deadline = s->deadline;
    if (s->fd < 0)
        return 0;
    for (i = 0; i < s->nfds && i < max; i++) {
        out[i].fd = s->fd;
        out[i].events = POLLIN;
        out[i].revents = 0;
    }
    return s->nfds;
}

static void s_step(void *ctx, canlin_ev_time now)
{
    fsrc *s = ctx;
    s->steps++;
    s->last_now = now;
    rec(s->h, 3);
    if (s->on_step)
        s->on_step(s);
}

static void src_init(fsrc *s, harness *h)
{
    memset(s, 0, sizeof *s);
    s->h = h;
    s->fd = -1;
    s->deadline = CANLIN_LOOP_NO_DEADLINE;
}

static void add_src(harness *h, fsrc *s)
{
    canlin_loop_source ls;
    ls.pollfds = s_pollfds;
    ls.step = s_step;
    ls.ctx = s;
    CHECK(canlin_loop_add_source(h->loop, &ls) == CANLIN_LOOP_OK);
}

static void h_init(harness *h, int display)
{
    canlin_ev_clock c;
    canlin_ev_callbacks cb;
    canlin_loop_config lc;
    int err = 1;
    memset(h, 0, sizeof *h);
    h->clock.now = 1000;
    h->tick = -1;
    c = canlin_ev_fake_clock_bind(&h->clock);
    cb.deliver = on_deliver;
    h->ev = canlin_ev_new(&c, &cb, h);
    canlin_loop_fake_wait_init(&h->fw, &h->clock);
    memset(&lc, 0, sizeof lc);
    lc.ev = h->ev;
    lc.pump = pump;
    lc.pump_ctx = h;
    if (display) {
        lc.display.update = d_update;
        lc.display.outputs = d_outputs;
        lc.display.dirty = d_dirty;
        lc.display.frame = d_frame;
        lc.display.next_tick_ms = d_tick;
        lc.display.ctx = h;
    }
    lc.wait = canlin_loop_fake_wait_poll;
    lc.wait_ctx = &h->fw;
    h->loop = canlin_loop_new(&lc, &err);
    if (!h->ev || !h->loop || err != CANLIN_LOOP_OK) {
        fprintf(stderr, "harness init failed\n");
        exit(1);
    }
    CHECK(canlin_ev_module_start(h->ev, &h->g) == CANLIN_EV_OK);
}

static void h_done(harness *h)
{
    canlin_loop_free(h->loop);
    CHECK(canlin_ev_module_stop(h->ev, h->g) == CANLIN_EV_OK);
    CHECK(canlin_ev_free(h->ev) == CANLIN_EV_OK);
}

static void post(harness *h, unsigned n)
{
    unsigned i;
    for (i = 0; i < n; i++)
        CHECK(canlin_ev_post(h->ev, h->g, 1, 1, 0, 0, NULL, 0, NULL) == CANLIN_EV_OK);
}

static canlin_loop_stats stats(harness *h)
{
    canlin_loop_stats st;
    canlin_loop_get_stats(h->loop, &st);
    return st;
}

/* ---- config and registration ---- */

static void test_config(void)
{
    canlin_loop_config lc;
    harness h;
    fsrc s[CANLIN_LOOP_SOURCES_MAX + 1];
    canlin_loop_source ls;
    unsigned i;
    int err = 0;

    memset(&lc, 0, sizeof lc);
    CHECK(canlin_loop_new(&lc, &err) == NULL && err == CANLIN_LOOP_EINVAL);
    CHECK(canlin_loop_new(NULL, &err) == NULL && err == CANLIN_LOOP_EINVAL);

    h_init(&h, 0);
    CHECK(canlin_loop_add_source(h.loop, NULL) == CANLIN_LOOP_EINVAL);
    for (i = 0; i < CANLIN_LOOP_SOURCES_MAX; i++) {
        src_init(&s[i], &h);
        add_src(&h, &s[i]);
    }
    src_init(&s[i], &h);
    ls.pollfds = s_pollfds;
    ls.step = s_step;
    ls.ctx = &s[i];
    CHECK(canlin_loop_add_source(h.loop, &ls) == CANLIN_LOOP_EFULL);
    h_done(&h);

    /* registration is fixed once the loop runs */
    h_init(&h, 0);
    CHECK(canlin_loop_iterate(h.loop) == CANLIN_LOOP_OK);
    src_init(&s[0], &h);
    ls.ctx = &s[0];
    CHECK(canlin_loop_add_source(h.loop, &ls) == CANLIN_LOOP_ESTATE);
    CHECK(canlin_loop_iterate(NULL) == CANLIN_LOOP_EINVAL);
    CHECK(canlin_loop_run(NULL) == CANLIN_LOOP_EINVAL);
    h_done(&h);
}

/* ---- timeout computation ---- */

static void test_idle_infinite(void)
{
    harness h;
    fsrc s;
    unsigned k;
    h_init(&h, 1);
    src_init(&s, &h);
    add_src(&h, &s);
    for (k = 0; k < 3; k++) {
        CHECK(canlin_loop_iterate(h.loop) == CANLIN_LOOP_OK);
        /* nothing runnable, no deadline, nothing dirty: block without a timeout */
        CHECK(h.fw.last_timeout == -1);
        CHECK(stats(&h).last_timeout == -1);
        CHECK(stats(&h).last_turns == 1);
    }
    CHECK(h.fw.waits == 3);
    CHECK(h.clock.now == 1000); /* an infinite fake wait does not move time */
    CHECK(h.events == 0 && h.draws == 0 && h.frames[0] == 0);
    CHECK(s.steps == 3 && h.pumps == 3 && h.updates == 3);
    CHECK(h.fw.last_nfds == 1); /* the wake pipe only */
    h_done(&h);
}

static void test_deadlines(void)
{
    harness h;
    fsrc a, b;
    canlin_ev_handle t;

    /* scheduler timer */
    h_init(&h, 1);
    CHECK(canlin_ev_timer_start(h.ev, h.g, 250, &t) == CANLIN_EV_OK);
    CHECK(canlin_loop_iterate(h.loop) == CANLIN_LOOP_OK);
    CHECK(h.fw.last_timeout == 250);
    CHECK(h.clock.now == 1250);
    CHECK(h.timers == 0);
    CHECK(canlin_loop_iterate(h.loop) == CANLIN_LOOP_OK);
    CHECK(h.timers == 1);                  /* expired and delivered in step 1 */
    CHECK(h.fw.last_timeout == -1);        /* and idle again */
    h_done(&h);

    /* the nearest of scheduler, sources and display tick */
    h_init(&h, 1);
    src_init(&a, &h);
    src_init(&b, &h);
    add_src(&h, &a);
    add_src(&h, &b);
    CHECK(canlin_ev_timer_start(h.ev, h.g, 500, &t) == CANLIN_EV_OK);
    a.deadline = 1000 + 300;
    b.deadline = 1000 + 120;
    h.tick = 700;
    CHECK(canlin_loop_iterate(h.loop) == CANLIN_LOOP_OK);
    CHECK(h.fw.last_timeout == 120);
    b.deadline = CANLIN_LOOP_NO_DEADLINE;
    h.tick = 50;
    CHECK(canlin_loop_iterate(h.loop) == CANLIN_LOOP_OK);
    CHECK(h.fw.last_timeout == 50);        /* 1120 -> tick at 1170 */
    h.tick = -1;
    CHECK(canlin_loop_iterate(h.loop) == CANLIN_LOOP_OK);
    CHECK(h.fw.last_timeout == 1300 - 1170);
    /* a source deadline already passed (buffered work): do not wait */
    a.deadline = 0;
    CHECK(canlin_loop_iterate(h.loop) == CANLIN_LOOP_OK);
    CHECK(h.fw.last_timeout == 0);
    CHECK(a.last_now == 1300);             /* sources step with the scheduler's clock */
    a.deadline = CANLIN_LOOP_NO_DEADLINE;
    CHECK(canlin_loop_iterate(h.loop) == CANLIN_LOOP_OK);
    CHECK(h.fw.last_timeout == 1500 - 1300);
    CHECK(canlin_loop_iterate(h.loop) == CANLIN_LOOP_OK);
    CHECK(h.timers == 1);
    CHECK(h.fw.last_timeout == -1);
    h_done(&h);
}

/* Work enqueued after step 1 — by the pump, a source callback or the display update —
 * must make the wait not block (lost wakeup, spec §3). */
static void pump_posts(harness *h)
{
    if (h->pumps == 1)
        post(h, 1);
}

static void step_posts(fsrc *s)
{
    if (s->steps == 1)
        post(s->h, 1);
}

static void update_invalidates(harness *h)
{
    if (h->updates == 1)
        CHECK(canlin_ev_invalidate(h->ev, h->g) == CANLIN_EV_OK);
}

static void test_zero_with_work(void)
{
    harness h;
    fsrc s;
    int k;

    for (k = 0; k < 3; k++) {
        h_init(&h, 1);
        src_init(&s, &h);
        add_src(&h, &s);
        if (k == 0)
            h.on_pump = pump_posts;
        else if (k == 1)
            s.on_step = step_posts;
        else
            h.on_update = update_invalidates;
        CHECK(canlin_loop_iterate(h.loop) == CANLIN_LOOP_OK);
        CHECK(h.fw.last_timeout == 0);
        CHECK(h.events == 0 && h.draws == 0);
        CHECK(canlin_loop_iterate(h.loop) == CANLIN_LOOP_OK);
        CHECK(h.events + h.draws == 1);
        CHECK(h.fw.last_timeout == -1);
        h_done(&h);
    }

    /* a scripted ready descriptor ends the wait without moving the clock */
    {
        int p[2];
        CHECK(pipe(p) == 0);
        h_init(&h, 0);
        src_init(&s, &h);
        s.fd = p[0];
        s.nfds = 1;
        s.deadline = 1000 + 5000;
        add_src(&h, &s);
        h.fw.ready_fd = p[0];
        h.fw.ready_events = POLLIN;
        CHECK(canlin_loop_iterate(h.loop) == CANLIN_LOOP_OK);
        CHECK(h.fw.last_timeout == 5000);
        CHECK(h.fw.last_nfds == 2);
        CHECK(h.clock.now == 1000);
        CHECK(canlin_loop_iterate(h.loop) == CANLIN_LOOP_OK);
        CHECK(h.clock.now == 6000);        /* script consumed: now it times out */
        CHECK(s.steps == 2);
        h_done(&h);
        close(p[0]);
        close(p[1]);
    }

    /* more descriptors than fit: never wait on a subset */
    {
        int p[2];
        CHECK(pipe(p) == 0);
        h_init(&h, 0);
        src_init(&s, &h);
        s.fd = p[0];
        s.nfds = CANLIN_LOOP_POLLFDS_MAX;  /* + the wake pipe = one too many */
        add_src(&h, &s);
        CHECK(canlin_loop_iterate(h.loop) == CANLIN_LOOP_OK);
        CHECK(h.fw.last_timeout == 0);
        CHECK(h.fw.last_nfds == CANLIN_LOOP_POLLFDS_MAX);
        CHECK(stats(&h).fd_overflow == 1);
        s.nfds = CANLIN_LOOP_POLLFDS_MAX - 1;
        CHECK(canlin_loop_iterate(h.loop) == CANLIN_LOOP_OK);
        CHECK(h.fw.last_timeout == -1);
        CHECK(stats(&h).fd_overflow == 1);
        h_done(&h);
        close(p[0]);
        close(p[1]);
    }
}

/* ---- bounded turns and fairness ---- */

static void refeed(harness *h, const canlin_ev_delivery *d)
{
    if (d->kind == CANLIN_EV_D_EVENT)
        post(h, 1); /* a self-feeding module: never runs dry */
}

static void test_bounded_turns(void)
{
    harness h;
    fsrc s;
    unsigned k, e0;

    h_init(&h, 1);
    src_init(&s, &h);
    add_src(&h, &s);
    h.nout = 1;
    h.on_event = refeed;
    post(&h, 8);
    for (k = 1; k <= 5; k++) {
        e0 = h.events;
        h.dirty[0] = 1;
        CHECK(canlin_loop_iterate(h.loop) == CANLIN_LOOP_OK);
        CHECK(stats(&h).last_turns == CANLIN_LOOP_TURNS_MAX);
        CHECK(h.events - e0 >= CANLIN_LOOP_TURNS_MAX);
        CHECK(h.events - e0 <= CANLIN_LOOP_TURNS_MAX * CANLIN_EV_TURN_MAX_DISPATCH);
        /* the busy module did not starve the pump, the sources or rendering */
        CHECK(h.pumps == k && s.steps == k && h.frames[0] == k);
        CHECK(h.fw.last_timeout == 0);
    }
    h_done(&h);

    /* iteration order: turns, pump, sources, frame */
    h_init(&h, 1);
    src_init(&s, &h);
    add_src(&h, &s);
    h.nout = 1;
    h.dirty[0] = 1;
    post(&h, 1);
    CHECK(canlin_loop_iterate(h.loop) == CANLIN_LOOP_OK);
    CHECK(h.norder == 4);
    CHECK(h.order[0] == 1 && h.order[1] == 2 && h.order[2] == 3 && h.order[3] == 4);
    h_done(&h);
}

/* ---- render only when dirty ---- */

static void test_render_dirty(void)
{
    harness h;
    h_init(&h, 1);
    h.nout = 3;
    CHECK(canlin_loop_iterate(h.loop) == CANLIN_LOOP_OK);
    CHECK(h.frames[0] + h.frames[1] + h.frames[2] == 0);
    CHECK(h.fw.last_timeout == -1);
    h.dirty[1] = 1;
    CHECK(canlin_loop_iterate(h.loop) == CANLIN_LOOP_OK);
    CHECK(h.frames[0] == 0 && h.frames[1] == 1 && h.frames[2] == 0);
    CHECK(h.fw.last_timeout == -1);         /* no frame loop */
    CHECK(canlin_loop_iterate(h.loop) == CANLIN_LOOP_OK);
    CHECK(h.frames[1] == 1);
    /* still dirty after its frame: at most one frame per output per iteration */
    h.sticky = 1;
    h.dirty[0] = h.dirty[2] = 1;
    CHECK(canlin_loop_iterate(h.loop) == CANLIN_LOOP_OK);
    CHECK(h.frames[0] == 1 && h.frames[1] == 1 && h.frames[2] == 1);
    CHECK(stats(&h).frames == 3);
    h_done(&h);
}

/* ---- quit ---- */

static canlin_loop *quit_loop;

static void quit_in_delivery(harness *h, const canlin_ev_delivery *d)
{
    (void)d;
    (void)h;
    canlin_loop_quit(quit_loop);
    CHECK(canlin_loop_iterate(quit_loop) == CANLIN_LOOP_EREENTRY);
    CHECK(canlin_loop_run(quit_loop) == CANLIN_LOOP_EREENTRY);
}

static void quit_on_third(fsrc *s)
{
    if (s->steps == 3)
        canlin_loop_quit(quit_loop);
}

static void test_quit(void)
{
    harness h;
    fsrc s;

    /* from a delivery: the iteration finishes its steps and does not wait */
    h_init(&h, 1);
    src_init(&s, &h);
    add_src(&h, &s);
    quit_loop = h.loop;
    h.on_event = quit_in_delivery;
    post(&h, 1);
    CHECK(canlin_loop_run(h.loop) == CANLIN_LOOP_OK);
    CHECK(h.events == 1 && s.steps == 1 && h.pumps == 1 && h.updates == 1);
    CHECK(h.fw.waits == 0);
    CHECK(!canlin_loop_quitting(h.loop)); /* cleared by run */
    h_done(&h);

    /* from a source step, after waits */
    h_init(&h, 0);
    src_init(&s, &h);
    s.deadline = 2000000;
    s.on_step = quit_on_third;
    add_src(&h, &s);
    quit_loop = h.loop;
    CHECK(canlin_loop_run(h.loop) == CANLIN_LOOP_OK);
    CHECK(s.steps == 3);
    CHECK(h.fw.waits == 2);
    CHECK(stats(&h).waits == 2 && stats(&h).iterations == 3);
    /* a later run starts afresh */
    s.on_step = NULL;
    canlin_loop_quit(h.loop);
    CHECK(canlin_loop_quitting(h.loop));
    CHECK(canlin_loop_run(h.loop) == CANLIN_LOOP_OK);
    CHECK(s.steps == 3); /* quit already pending: run returns at once */
    h_done(&h);
}

/* ---- wake pipe from another thread (real poll, monotonic clock) ---- */

typedef struct waker {
    canlin_loop *loop;
    unsigned delay_ms;
    pthread_mutex_t mu;
    int produced;        /* the "work" a producer thread hands over */
} waker;

static void sleep_ms(unsigned ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static void *waker_main(void *arg)
{
    waker *w = arg;
    sleep_ms(w->delay_ms);
    pthread_mutex_lock(&w->mu);
    w->produced = 1;
    pthread_mutex_unlock(&w->mu);
    canlin_loop_wake(w->loop);
    return NULL;
}

static void consume(void *ctx, canlin_ev_time now)
{
    waker *w = ctx;
    int got;
    (void)now;
    pthread_mutex_lock(&w->mu);
    got = w->produced;
    pthread_mutex_unlock(&w->mu);
    if (got)
        canlin_loop_quit(w->loop);
}

static uint64_t real_ms(void)
{
    canlin_ev_clock c = canlin_loop_clock_monotonic();
    return c.now(c.ctx);
}

static void noop_deliver(void *ud, canlin_ev *ev, const canlin_ev_delivery *d)
{
    (void)ud;
    (void)ev;
    (void)d;
}

static void test_wake_thread(void)
{
    canlin_ev_clock c = canlin_loop_clock_monotonic();
    canlin_ev_callbacks cb;
    canlin_ev *ev;
    canlin_loop_config lc;
    canlin_loop *l;
    canlin_loop_source src;
    canlin_loop_stats st;
    pthread_t th;
    waker w;
    uint64_t t0, dt;
    int err;

    cb.deliver = noop_deliver;
    ev = canlin_ev_new(&c, &cb, NULL);
    memset(&lc, 0, sizeof lc);
    lc.ev = ev;
    l = canlin_loop_new(&lc, &err); /* default wait: poll() */
    CHECK(ev && l);
    if (!ev || !l)
        return;
    memset(&w, 0, sizeof w);
    pthread_mutex_init(&w.mu, NULL);
    w.loop = l;
    w.delay_ms = 150;
    src.pollfds = NULL;
    src.step = consume;
    src.ctx = &w;
    CHECK(canlin_loop_add_source(l, &src) == CANLIN_LOOP_OK);

    /* idle: the first wait blocks without a timeout until the other thread wakes it */
    t0 = real_ms();
    CHECK(pthread_create(&th, NULL, waker_main, &w) == 0);
    CHECK(canlin_loop_run(l) == CANLIN_LOOP_OK);
    dt = real_ms() - t0;
    pthread_join(th, NULL);
    canlin_loop_get_stats(l, &st);
    CHECK(dt >= 100 && dt < 5000);
    CHECK(st.last_timeout == -1);
    CHECK(st.waits == 1 && st.wakeups == 1 && st.iterations == 2);
    w.produced = 0; /* handed over; the quit request was cleared by run */

    /* a wake that happens before the wait is not lost */
    canlin_loop_wake(l);
    t0 = real_ms();
    CHECK(canlin_loop_iterate(l) == CANLIN_LOOP_OK);
    canlin_loop_get_stats(l, &st);
    CHECK(real_ms() - t0 < 1000);
    CHECK(st.last_timeout == -1 && st.wakeups == 2);
    /* many wakes coalesce and are drained: the next wait blocks again (bounded here by
     * another producer thread) */
    {
        int k;
        for (k = 0; k < 100000; k++)
            canlin_loop_wake(l); /* fills the pipe; must not block */
        CHECK(canlin_loop_iterate(l) == CANLIN_LOOP_OK);
        canlin_loop_get_stats(l, &st);
        CHECK(st.wakeups == 3);
        w.produced = 0;
        w.delay_ms = 100;
        t0 = real_ms();
        CHECK(pthread_create(&th, NULL, waker_main, &w) == 0);
        CHECK(canlin_loop_iterate(l) == CANLIN_LOOP_OK);
        dt = real_ms() - t0;
        pthread_join(th, NULL);
        CHECK(dt >= 50);
        canlin_loop_get_stats(l, &st);
        CHECK(st.wakeups == 4);
    }
    canlin_loop_free(l);
    pthread_mutex_destroy(&w.mu);
    CHECK(canlin_ev_free(ev) == CANLIN_EV_OK);
}

int main(void)
{
    test_config();
    test_idle_infinite();
    test_deadlines();
    test_zero_with_work();
    test_bounded_turns();
    test_render_dirty();
    test_quit();
    test_wake_thread();
    printf("test_loop: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
