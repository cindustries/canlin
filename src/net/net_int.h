/* CanLin native transport — internal seams between http.c and the socket, resolver and
 * TLS layers (HTTP transport spec §2). Not a public API. */
#ifndef CANLIN_NET_INT_H
#define CANLIN_NET_INT_H

#include "canlin/http.h"

/* ---- sock.c: nonblocking TCP connect ---- */

/* Start a connect to a. Returns an fd with *pending set (in progress) or cleared
 * (connected), or -1 with *err = errno. */
int net_sock_connect(const canlin_net_addr *a, int *pending, int *err);
/* Poll an in-progress connect without waiting: 1 connected, 0 still pending, -1 failed
 * (*err = errno). */
int net_sock_check(int fd, int *err);
/* Nonblocking send/recv: > 0 bytes, 0 would block (send) / EOF (recv),
 * NET_AGAIN would block (recv), NET_ERR error (*err = errno). */
enum { NET_AGAIN = -2, NET_ERR = -1 };
long net_sock_send(int fd, const void *buf, size_t n, int *err);
long net_sock_recv(int fd, void *buf, size_t n, int *err);
void net_sock_close(int fd);
/* Static text for a connect/send errno (no addresses, no secrets). */
const char *net_sock_strerror(int err);

/* ---- dns.c: one resolver worker thread with a bounded job table (spec §4) ---- */

typedef struct net_dns net_dns;

net_dns *net_dns_new(canlin_http_resolve_fn fn, void *ctx);
/* Orphans every job and lets a busy worker finish on its own; never waits. */
void net_dns_free(net_dns *d);
/* Read end of the wakeup pipe (-1 before the first submit). */
int net_dns_fd(const net_dns *d);
/* Consume pending wakeup bytes. */
void net_dns_drain(net_dns *d);
/* Queue a lookup. Returns a job id > 0, 0 when CANLIN_NET_DNS_JOBS_MAX jobs exist (try
 * again later) or -1 when the worker cannot be started. */
long net_dns_submit(net_dns *d, const char *host, unsigned port);
/* Collect: 0 pending, n > 0 addresses copied (job released), -1 lookup failed (job
 * released). */
int net_dns_result(net_dns *d, long job, canlin_net_addr *out, size_t max);
/* Forget a job: a queued one is dropped, a running one is orphaned (freed on arrival). */
void net_dns_cancel(net_dns *d, long job);
void net_dns_stats(net_dns *d, unsigned *jobs, unsigned *orphans);

/* ---- tls.c: thin Mbed TLS wrapper (spec §5) ---- */

typedef struct net_tls net_tls;           /* shared config, trust store, RNG */
typedef struct net_tls_conn net_tls_conn; /* one connection */

/* Negative, so they never collide with a byte count returned by read/write. */
enum {
    NET_TLS_OK = 0,
    NET_TLS_WANT_READ = -100,
    NET_TLS_WANT_WRITE = -101,
    NET_TLS_E_TRUST = -1,  /* trust bundle missing/unparseable */
    NET_TLS_E_VERIFY = -2, /* chain, name or other verification failure */
    NET_TLS_E_CLOCK = -3,  /* verification failed only on validity dates */
    NET_TLS_E_TLS = -4,    /* any other handshake/record failure */
    NET_TLS_E_EOF = -5,    /* transport EOF without close_notify */
    NET_TLS_E_RESET = -6   /* transport error (reset) */
};

net_tls *net_tls_new(const char *ca_file, canlin_http_wall_fn wall, void *wall_ctx);
void net_tls_free(net_tls *t);
/* 1 when the trust bundle has been loaded. */
int net_tls_trust_loaded(const net_tls *t);
/* New connection over fd for SNI/verification name `host`. NULL with *err set
 * (NET_TLS_E_TRUST when the bundle cannot be loaded, else NET_TLS_E_TLS). The trust
 * bundle is loaded lazily here, once. */
net_tls_conn *net_tls_conn_new(net_tls *t, int fd, const char *host, int *err);
void net_tls_conn_free(net_tls_conn *c);
int net_tls_handshake(net_tls_conn *c);
/* > 0 bytes; 0 = clean close (close_notify); NET_TLS_WANT_*; NET_TLS_E_*. */
long net_tls_read(net_tls_conn *c, void *buf, size_t n);
/* > 0 bytes; NET_TLS_WANT_*; NET_TLS_E_*. */
long net_tls_write(net_tls_conn *c, const void *buf, size_t n);
/* 1 when decrypted or undecrypted input is buffered inside Mbed TLS (poll() cannot see
 * it: step again without waiting). */
int net_tls_pending(net_tls_conn *c);
/* Poll events the last WANT asked for. */
short net_tls_events(const net_tls_conn *c);

#endif
