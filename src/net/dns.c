/* Resolver worker (HTTP transport spec §4): getaddrinfo() in one detached worker thread
 * over a bounded job table, waking the owner through a pipe.
 *
 * Ownership: the shared state is reference-counted by the owner and the worker. The
 * owner never waits for the worker: net_dns_free() orphans every job, closes the pipe's
 * read end and drops its reference; a worker still inside getaddrinfo() discards its
 * result on return, closes the write end and frees the state as the last holder. The
 * worker only touches the job table under the mutex and never calls back. */
#define _POSIX_C_SOURCE 200809L
#include "net_int.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { J_FREE = 0, J_QUEUED, J_RUNNING, J_DONE };

typedef struct dns_job {
    int state;
    int orphan;         /* running, its stream is gone: free on arrival */
    unsigned gen;       /* bumps on every reuse: stale ids never match */
    unsigned long seq;  /* FIFO order */
    char host[CANLIN_NET_HOST_MAX + 1];
    unsigned port;
    int n;              /* result: > 0 addresses, -1 failure */
    canlin_net_addr addr[CANLIN_NET_ADDRS_MAX];
} dns_job;

struct net_dns {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int refs;           /* owner + worker */
    int started, quit;
    int rfd, wfd;       /* wakeup pipe */
    unsigned long seq;
    canlin_http_resolve_fn fn;
    void *ctx;
    dns_job job[CANLIN_NET_DNS_JOBS_MAX];
};

/* Default resolver: the platform's getaddrinfo (resolv.conf, hosts, ...). */
static int resolve_gai(void *ctx, const char *host, unsigned port, canlin_net_addr *out,
                       size_t max)
{
    struct addrinfo hints, *res = NULL, *ai;
    char serv[8];
    size_t n = 0;
    (void)ctx;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_NUMERICSERV;
    snprintf(serv, sizeof serv, "%u", port);
    if (getaddrinfo(host, serv, &hints, &res) != 0 || res == NULL)
        return -1;
    for (ai = res; ai && n < max; ai = ai->ai_next) {
        if (ai->ai_addrlen > sizeof out[n].sa ||
            (ai->ai_family != AF_INET && ai->ai_family != AF_INET6))
            continue;
        memset(&out[n], 0, sizeof out[n]);
        memcpy(&out[n].sa, ai->ai_addr, ai->ai_addrlen);
        out[n].len = (socklen_t)ai->ai_addrlen;
        n++;
    }
    freeaddrinfo(res);
    return n ? (int)n : -1;
}

static void unref_locked(net_dns *d)
{
    /* called with d->mu held; frees d (and unlocks) when this was the last reference */
    if (--d->refs == 0) {
        pthread_mutex_unlock(&d->mu);
        if (d->rfd >= 0)
            close(d->rfd);
        if (d->wfd >= 0)
            close(d->wfd);
        pthread_cond_destroy(&d->cv);
        pthread_mutex_destroy(&d->mu);
        free(d);
        return;
    }
    pthread_mutex_unlock(&d->mu);
}

static void *worker(void *arg)
{
    net_dns *d = arg;
    canlin_net_addr tmp[CANLIN_NET_ADDRS_MAX];
    char host[CANLIN_NET_HOST_MAX + 1];

    pthread_mutex_lock(&d->mu);
    for (;;) {
        dns_job *j = NULL;
        unsigned i, port;
        int n;
        while (!d->quit) {
            for (i = 0; i < CANLIN_NET_DNS_JOBS_MAX; i++)
                if (d->job[i].state == J_QUEUED && (j == NULL || d->job[i].seq < j->seq))
                    j = &d->job[i];
            if (j)
                break;
            pthread_cond_wait(&d->cv, &d->mu);
        }
        if (d->quit)
            break;
        j->state = J_RUNNING;
        memcpy(host, j->host, sizeof host);
        port = j->port;
        pthread_mutex_unlock(&d->mu);
        n = d->fn(d->ctx, host, port, tmp, CANLIN_NET_ADDRS_MAX);
        if (n > (int)CANLIN_NET_ADDRS_MAX)
            n = (int)CANLIN_NET_ADDRS_MAX;
        pthread_mutex_lock(&d->mu);
        memset(host, 0, sizeof host);
        if (d->quit)
            break;
        if (j->orphan) {
            j->state = J_FREE; /* result freed on arrival, never delivered */
            j->orphan = 0;
            j->gen++;
        } else {
            j->n = n > 0 ? n : -1;
            if (n > 0)
                memcpy(j->addr, tmp, (size_t)n * sizeof tmp[0]);
            j->state = J_DONE;
        }
        memset(tmp, 0, sizeof tmp);
        {
            /* Wake the owner: a result, or a freed slot a queued stream waits for.
             * Nonblocking; a full pipe already wakes. Never after quit (read end gone). */
            char b = 1;
            ssize_t w = write(d->wfd, &b, 1);
            (void)w;
        }
    }
    unref_locked(d);
    return NULL;
}

net_dns *net_dns_new(canlin_http_resolve_fn fn, void *ctx)
{
    net_dns *d = calloc(1, sizeof *d);
    if (d == NULL)
        return NULL;
    if (pthread_mutex_init(&d->mu, NULL) != 0) {
        free(d);
        return NULL;
    }
    if (pthread_cond_init(&d->cv, NULL) != 0) {
        pthread_mutex_destroy(&d->mu);
        free(d);
        return NULL;
    }
    d->refs = 1;
    d->rfd = d->wfd = -1;
    d->fn = fn ? fn : resolve_gai;
    d->ctx = ctx;
    return d;
}

static int start(net_dns *d)
{
    int p[2], i;
    pthread_t t;
    pthread_attr_t at;

    if (pipe(p) != 0)
        return -1;
    for (i = 0; i < 2; i++) {
        int fl = fcntl(p[i], F_GETFL, 0);
        if (fl < 0 || fcntl(p[i], F_SETFL, fl | O_NONBLOCK) < 0 ||
            fcntl(p[i], F_SETFD, FD_CLOEXEC) < 0) {
            close(p[0]);
            close(p[1]);
            return -1;
        }
    }
    d->rfd = p[0];
    d->wfd = p[1];
    if (pthread_attr_init(&at) != 0)
        goto fail;
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    d->refs++; /* the worker's reference */
    if (pthread_create(&t, &at, worker, d) != 0) {
        d->refs--;
        pthread_attr_destroy(&at);
        goto fail;
    }
    pthread_attr_destroy(&at);
    d->started = 1;
    return 0;
fail:
    close(d->rfd);
    close(d->wfd);
    d->rfd = d->wfd = -1;
    return -1;
}

void net_dns_free(net_dns *d)
{
    unsigned i;
    if (d == NULL)
        return;
    pthread_mutex_lock(&d->mu);
    d->quit = 1;
    for (i = 0; i < CANLIN_NET_DNS_JOBS_MAX; i++)
        if (d->job[i].state == J_RUNNING)
            d->job[i].orphan = 1;
    if (d->rfd >= 0) {
        close(d->rfd); /* the worker checks quit before it writes */
        d->rfd = -1;
    }
    pthread_cond_broadcast(&d->cv);
    unref_locked(d);
}

int net_dns_fd(const net_dns *d)
{
    return d ? d->rfd : -1;
}

void net_dns_drain(net_dns *d)
{
    char b[64];
    if (d == NULL || d->rfd < 0)
        return;
    while (read(d->rfd, b, sizeof b) > 0)
        ;
}

static long job_id(net_dns *d, dns_job *j)
{
    return (long)((j->gen & 0xffffu) << 8 | (unsigned)(j - d->job)) + 1;
}

static dns_job *job_of(net_dns *d, long id)
{
    unsigned slot, gen;
    if (id <= 0)
        return NULL;
    id--;
    slot = (unsigned)id & 0xffu;
    gen = ((unsigned)id >> 8) & 0xffffu;
    if (slot >= CANLIN_NET_DNS_JOBS_MAX || (d->job[slot].gen & 0xffffu) != gen ||
        d->job[slot].state == J_FREE)
        return NULL;
    return &d->job[slot];
}

long net_dns_submit(net_dns *d, const char *host, unsigned port)
{
    unsigned i;
    long id = 0;
    size_t hl = strlen(host);
    if (hl > CANLIN_NET_HOST_MAX)
        return -1;
    pthread_mutex_lock(&d->mu);
    if (!d->started && start(d) != 0) {
        pthread_mutex_unlock(&d->mu);
        return -1;
    }
    for (i = 0; i < CANLIN_NET_DNS_JOBS_MAX; i++) {
        dns_job *j = &d->job[i];
        if (j->state != J_FREE)
            continue;
        j->gen++;
        j->state = J_QUEUED;
        j->orphan = 0;
        j->seq = ++d->seq;
        memcpy(j->host, host, hl + 1);
        j->port = port;
        j->n = 0;
        id = job_id(d, j);
        pthread_cond_signal(&d->cv);
        break;
    }
    pthread_mutex_unlock(&d->mu);
    return id;
}

int net_dns_result(net_dns *d, long id, canlin_net_addr *out, size_t max)
{
    dns_job *j;
    int n = 0;
    pthread_mutex_lock(&d->mu);
    j = job_of(d, id);
    if (j == NULL)
        n = -1;
    else if (j->state == J_DONE) {
        n = j->n;
        if (n > (int)max)
            n = (int)max;
        if (n > 0)
            memcpy(out, j->addr, (size_t)n * sizeof out[0]);
        memset(j->addr, 0, sizeof j->addr);
        j->state = J_FREE;
        j->gen++;
    }
    pthread_mutex_unlock(&d->mu);
    return n;
}

void net_dns_cancel(net_dns *d, long id)
{
    dns_job *j;
    if (d == NULL)
        return;
    pthread_mutex_lock(&d->mu);
    j = job_of(d, id);
    if (j) {
        if (j->state == J_RUNNING)
            j->orphan = 1;
        else {
            j->state = J_FREE;
            j->gen++;
        }
    }
    pthread_mutex_unlock(&d->mu);
}

void net_dns_stats(net_dns *d, unsigned *jobs, unsigned *orphans)
{
    unsigned i;
    *jobs = *orphans = 0;
    if (d == NULL)
        return;
    pthread_mutex_lock(&d->mu);
    for (i = 0; i < CANLIN_NET_DNS_JOBS_MAX; i++)
        if (d->job[i].state != J_FREE) {
            (*jobs)++;
            if (d->job[i].orphan)
                (*orphans)++;
        }
    pthread_mutex_unlock(&d->mu);
}
