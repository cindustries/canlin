/* CanLin native main loop core (canlin#61). See include/canlin/loop.h and
 * docs/superpowers/specs/2026-09-24-canlin-native-loop-design.md. */
#define _POSIX_C_SOURCE 200809L
#include "canlin/loop.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct canlin_loop {
    canlin_loop_config cfg;
    canlin_loop_source src[CANLIN_LOOP_SOURCES_MAX];
    size_t nsrc;
    int wake_rd, wake_wr;
    volatile sig_atomic_t quit;
    int started, busy;
    canlin_loop_stats st;
    struct pollfd fds[CANLIN_LOOP_POLLFDS_MAX];
};

static int set_flags(int fd)
{
    int fl = fcntl(fd, F_GETFL);
    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0)
        return -1;
    fl = fcntl(fd, F_GETFD);
    if (fl < 0 || fcntl(fd, F_SETFD, fl | FD_CLOEXEC) < 0)
        return -1;
    return 0;
}

canlin_loop *canlin_loop_new(const canlin_loop_config *cfg, int *err)
{
    canlin_loop *l;
    int p[2];
    if (err)
        *err = CANLIN_LOOP_OK;
    if (cfg == NULL || cfg->ev == NULL) {
        if (err)
            *err = CANLIN_LOOP_EINVAL;
        return NULL;
    }
    l = calloc(1, sizeof *l);
    if (l == NULL) {
        if (err)
            *err = CANLIN_LOOP_ENOMEM;
        return NULL;
    }
    if (pipe(p) != 0) {
        free(l);
        if (err)
            *err = CANLIN_LOOP_ESYS;
        return NULL;
    }
    if (set_flags(p[0]) != 0 || set_flags(p[1]) != 0) {
        close(p[0]);
        close(p[1]);
        free(l);
        if (err)
            *err = CANLIN_LOOP_ESYS;
        return NULL;
    }
    l->cfg = *cfg;
    if (l->cfg.wait == NULL)
        l->cfg.wait = canlin_loop_poll_wait;
    l->wake_rd = p[0];
    l->wake_wr = p[1];
    l->st.last_timeout = -1;
    return l;
}

void canlin_loop_free(canlin_loop *l)
{
    if (l == NULL)
        return;
    close(l->wake_rd);
    close(l->wake_wr);
    free(l);
}

int canlin_loop_add_source(canlin_loop *l, const canlin_loop_source *src)
{
    if (l == NULL || src == NULL)
        return CANLIN_LOOP_EINVAL;
    if (l->started)
        return CANLIN_LOOP_ESTATE;
    if (l->nsrc >= CANLIN_LOOP_SOURCES_MAX)
        return CANLIN_LOOP_EFULL;
    l->src[l->nsrc++] = *src;
    return CANLIN_LOOP_OK;
}

void canlin_loop_quit(canlin_loop *l)
{
    if (l == NULL)
        return;
    l->quit = 1;
    canlin_loop_wake(l);
}

int canlin_loop_quitting(const canlin_loop *l)
{
    return l != NULL && l->quit != 0;
}

void canlin_loop_wake(canlin_loop *l)
{
    static const char b = 1;
    ssize_t rc;
    int saved = errno;
    if (l == NULL)
        return;
    do
        rc = write(l->wake_wr, &b, 1);
    while (rc < 0 && errno == EINTR);
    /* EAGAIN: the pipe is full, a wakeup is already pending. */
    errno = saved;
}

static void wake_drain(canlin_loop *l)
{
    char buf[64];
    ssize_t rc;
    do
        rc = read(l->wake_rd, buf, sizeof buf);
    while (rc > 0 || (rc < 0 && errno == EINTR));
}

/* Step 1: bounded scheduler turns. */
static int run_turns(canlin_loop *l)
{
    unsigned k;
    canlin_ev_turn_result tr;
    l->st.last_turns = 0;
    for (k = 0; k < CANLIN_LOOP_TURNS_MAX; k++) {
        int rc = canlin_ev_turn(l->cfg.ev, &tr);
        if (rc == CANLIN_EV_EREENTRY)
            return CANLIN_LOOP_EREENTRY;
        if (rc != CANLIN_EV_OK)
            return CANLIN_LOOP_EINVAL;
        l->st.last_turns++;
        if (!tr.more)
            break;
    }
    return CANLIN_LOOP_OK;
}

/* Step 4: one frame per dirty output. */
static void run_display(canlin_loop *l)
{
    const canlin_loop_display *d = &l->cfg.display;
    size_t i, n;
    if (d->update)
        d->update(d->ctx);
    if (d->outputs == NULL || d->dirty == NULL || d->frame == NULL)
        return;
    n = d->outputs(d->ctx);
    for (i = 0; i < n; i++)
        if (d->dirty(d->ctx, i)) {
            d->frame(d->ctx, i);
            l->st.frames++;
        }
}

static void take_min(canlin_ev_time *dl, canlin_ev_time d)
{
    if (d < *dl)
        *dl = d;
}

/* Step 5: descriptors and timeout, then the wait. */
static int run_wait(canlin_loop *l)
{
    canlin_ev_turn_result pk;
    canlin_ev_time now, dl = CANLIN_LOOP_NO_DEADLINE;
    size_t n = 1, i;
    int work = 0, timeout, rc;

    l->fds[0].fd = l->wake_rd;
    l->fds[0].events = POLLIN;
    l->fds[0].revents = 0;
    for (i = 0; i < l->nsrc; i++) {
        const canlin_loop_source *s = &l->src[i];
        canlin_ev_time sd = CANLIN_LOOP_NO_DEADLINE;
        size_t room = n < CANLIN_LOOP_POLLFDS_MAX ? CANLIN_LOOP_POLLFDS_MAX - n : 0, want;
        if (s->pollfds == NULL)
            continue;
        want = s->pollfds(s->ctx, l->fds + n, room, &sd);
        if (want > room) {
            work = 1; /* cannot wait on all of them: do not wait at all */
            l->st.fd_overflow++;
            want = room;
        }
        n += want;
        take_min(&dl, sd);
    }
    if (canlin_ev_peek(l->cfg.ev, &pk) != CANLIN_EV_OK)
        return CANLIN_LOOP_EINVAL;
    if (pk.more)
        work = 1;
    if (pk.has_deadline)
        take_min(&dl, pk.deadline);
    now = canlin_ev_now(l->cfg.ev);
    if (l->cfg.display.next_tick_ms) {
        int64_t t = l->cfg.display.next_tick_ms(l->cfg.display.ctx);
        if (t >= 0)
            take_min(&dl, now + (canlin_ev_time)t);
    }

    if (work || dl <= now)
        timeout = 0;
    else if (dl == CANLIN_LOOP_NO_DEADLINE)
        timeout = -1;
    else if (dl - now > (canlin_ev_time)INT_MAX)
        timeout = INT_MAX;
    else
        timeout = (int)(dl - now);

    l->st.last_timeout = timeout;
    l->st.waits++;
    rc = l->cfg.wait(l->cfg.wait_ctx, l->fds, n, timeout);
    if (rc < 0)
        return CANLIN_LOOP_ESYS;
    if (l->fds[0].revents) {
        l->st.wakeups++;
        wake_drain(l);
    }
    return CANLIN_LOOP_OK;
}

int canlin_loop_iterate(canlin_loop *l)
{
    canlin_ev_time now;
    size_t i;
    int rc;

    if (l == NULL)
        return CANLIN_LOOP_EINVAL;
    if (l->busy)
        return CANLIN_LOOP_EREENTRY;
    l->busy = 1;
    l->started = 1;
    l->st.iterations++;

    rc = run_turns(l);
    if (rc != CANLIN_LOOP_OK)
        goto out;
    if (l->cfg.pump)
        l->cfg.pump(l->cfg.pump_ctx);
    now = canlin_ev_now(l->cfg.ev);
    for (i = 0; i < l->nsrc; i++)
        if (l->src[i].step)
            l->src[i].step(l->src[i].ctx, now);
    run_display(l);
    if (!l->quit)
        rc = run_wait(l);
out:
    l->busy = 0;
    return rc;
}

int canlin_loop_run(canlin_loop *l)
{
    int rc = CANLIN_LOOP_OK;
    if (l == NULL)
        return CANLIN_LOOP_EINVAL;
    if (l->busy)
        return CANLIN_LOOP_EREENTRY;
    while (!l->quit) {
        rc = canlin_loop_iterate(l);
        if (rc != CANLIN_LOOP_OK)
            return rc;
    }
    l->quit = 0;
    wake_drain(l);
    return rc;
}

void canlin_loop_get_stats(const canlin_loop *l, canlin_loop_stats *out)
{
    if (l && out)
        *out = l->st;
}

int canlin_loop_poll_wait(void *ctx, struct pollfd *fds, size_t n, int timeout_ms)
{
    int rc;
    (void)ctx;
    rc = poll(fds, (nfds_t)n, timeout_ms < 0 ? -1 : timeout_ms);
    if (rc < 0 && errno == EINTR) {
        size_t i;
        for (i = 0; i < n; i++)
            fds[i].revents = 0;
        return 0;
    }
    return rc;
}

static canlin_ev_time mono_now(void *ctx)
{
    struct timespec ts;
    (void)ctx;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (canlin_ev_time)ts.tv_sec * 1000u + (canlin_ev_time)(ts.tv_nsec / 1000000);
}

canlin_ev_clock canlin_loop_clock_monotonic(void)
{
    canlin_ev_clock c;
    c.now = mono_now;
    c.ctx = NULL;
    return c;
}

/* ---- test wait ---- */

void canlin_loop_fake_wait_init(canlin_loop_fake_wait *fw, canlin_ev_fake_clock *fc)
{
    memset(fw, 0, sizeof *fw);
    fw->clock = fc;
    fw->ready_fd = -1;
    fw->last_timeout = -1;
}

int canlin_loop_fake_wait_poll(void *ctx, struct pollfd *fds, size_t n, int timeout_ms)
{
    canlin_loop_fake_wait *fw = ctx;
    size_t i;
    int hit = 0;
    fw->waits++;
    fw->last_timeout = timeout_ms;
    fw->last_nfds = n;
    for (i = 0; i < n; i++) {
        fds[i].revents = 0;
        if (fw->ready_fd != -1 && fds[i].fd == fw->ready_fd) {
            fds[i].revents = (short)(fw->ready_events & (fds[i].events | POLLERR | POLLHUP));
            if (fds[i].revents)
                hit++;
        }
    }
    if (hit) {
        fw->ready_fd = -1;
        return hit;
    }
    if (timeout_ms > 0 && fw->clock)
        canlin_ev_fake_clock_advance(fw->clock, (canlin_ev_time)timeout_ms);
    return 0;
}
