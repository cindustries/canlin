/* CanLin native HTTP/1.1 + TLS client (HTTP transport spec, canlin#41).
 *
 * Contract: docs/superpowers/specs/2026-09-23-canlin-http-transport-design.md.
 * One outgoing request per connection (Connection: close), optionally over TLS
 * (Mbed TLS 3.6, verification always on). Owner thread only; the only other thread is
 * the resolver worker (spec §4), which never calls back.
 *
 * Drive contract (spec §3): the transport never waits. The native backend polls the
 * descriptors from canlin_http_pollfds() until the returned deadline and then calls
 * canlin_http_step() once per loop iteration. Every callback is made from inside
 * canlin_http_step(), never from open(), close() or pollfds().
 *
 * Per stream: status, header*, body*, then exactly one of end/error. After close()
 * returns no callback is made for that stream; close() is allowed from inside that
 * stream's (or another stream's) callbacks, and after end/error it releases the slot.
 */
#ifndef CANLIN_HTTP_H
#define CANLIN_HTTP_H

#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

#include "canlin/ev.h"
#include "canlin/http_wire.h"
#include "canlin/net_limits.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Not in the spec: no CanLin version string exists yet; the build may define one. */
#ifndef CANLIN_VERSION
#define CANLIN_VERSION "0.0-dev"
#endif
#define CANLIN_HTTP_USER_AGENT "CanLin/" CANLIN_VERSION
#define CANLIN_HTTP_CA_FILE_DEFAULT "/etc/ssl/certs/ca-certificates.crt"
/* pollfds(): no transport deadline. */
#define CANLIN_HTTP_NO_DEADLINE UINT64_MAX
/* pollfds(): an array of this many entries always suffices. */
#define CANLIN_HTTP_POLLFDS_MAX (CANLIN_NET_STREAMS_MAX + 1u)

typedef struct canlin_http canlin_http;
typedef struct canlin_http_stream canlin_http_stream;

/* One resolved address. */
typedef struct canlin_net_addr {
    struct sockaddr_storage sa;
    socklen_t len;
} canlin_net_addr;

/* Resolver (runs on the resolver worker thread, may block). Writes at most `max`
 * addresses in preference order; returns their count (> 0), or -1 when the name does
 * not resolve. The default is getaddrinfo(AF_UNSPEC, SOCK_STREAM). Tests inject a
 * controllable one. */
typedef int (*canlin_http_resolve_fn)(void *ctx, const char *host, unsigned port,
                                      canlin_net_addr *out, size_t max);

/* Wall clock in seconds since the epoch, used only for certificate validity dates
 * (spec §5 "tls-clock"). Default: time(NULL). */
typedef int64_t (*canlin_http_wall_fn)(void *ctx);

typedef struct canlin_http_config {
    const char *ca_file;          /* PEM trust bundle; NULL = CANLIN_HTTP_CA_FILE_DEFAULT */
    const char *user_agent;       /* NULL = CANLIN_HTTP_USER_AGENT */
    canlin_http_wall_fn wall_clock;
    void *wall_ctx;
    canlin_http_resolve_fn resolve;
    void *resolve_ctx;
} canlin_http_config;

typedef struct canlin_http_request {
    const char *method;                /* token, e.g. "GET", "POST" */
    const char *url;                   /* absolute http(s) URL */
    const canlin_http_header *headers; /* non-secret caller headers (validated) */
    size_t nheaders;
    const void *body;                  /* not copied: valid until close() */
    size_t body_len;
    const canlin_http_cred *cred;      /* NULL = none; valid until close() */
} canlin_http_request;

typedef struct canlin_http_cb {
    void (*status)(void *ud, int http_status);
    void (*header)(void *ud, const char *name, const char *value);
    /* Body bytes (valid during the call). Return nonzero to stop reading this stream
     * for the rest of this step; undelivered bytes stay buffered (at most
     * CANLIN_NET_RX_MAX) and are offered again by the next step. */
    int (*body)(void *ud, const void *buf, size_t len);
    void (*end)(void *ud);
    /* "<code>: <detail>", code one of dns, connect, timeout, tls-trust, tls-verify,
     * tls-clock, tls, protocol, eof, redirect, limit (spec §7). Never holds a secret,
     * a URL or response bytes. */
    void (*error)(void *ud, const char *msg);
} canlin_http_cb;

canlin_http *canlin_http_new(const canlin_http_config *cfg);
/* Closes every stream without callbacks. An in-progress lookup finishes on the worker
 * and is discarded there; free never waits for the resolver. Not from a callback. */
void canlin_http_free(canlin_http *h);

/* Admit and queue a request; no I/O and no callback happen here. Everything *req
 * points to (headers, body, cred) must stay valid until close(). Returns NULL with a
 * static *msg when the request is invalid or CANLIN_NET_STREAMS_MAX streams are live. */
canlin_http_stream *canlin_http_open(canlin_http *h, const canlin_http_request *req,
                                     const canlin_http_cb *cb, void *ud, const char **msg);
/* Cancel/release. Idempotent per stream until it returns; the stream pointer is invalid
 * afterwards. A pending lookup is orphaned (its result is freed on arrival). */
void canlin_http_close(canlin_http *h, canlin_http_stream *s);

/* Descriptors to wait on (at most max written; returns the number wanted) and the
 * nearest transport deadline in canlin_ev monotonic ms (CANLIN_HTTP_NO_DEADLINE if
 * none; a deadline <= now means: do not wait, step now). */
size_t canlin_http_pollfds(canlin_http *h, struct pollfd *out, size_t max,
                           canlin_ev_time *deadline);
/* Bounded work for every live stream: at most CANLIN_NET_TURN_BYTES read per stream,
 * timeouts against `now`, callbacks on the calling (owner) thread. */
void canlin_http_step(canlin_http *h, canlin_ev_time now);

/* ---- diagnostics (tests, status UI) ---- */

typedef enum canlin_http_phase {
    CANLIN_HTTP_PH_QUEUED = 0, /* admitted, lookup not submitted yet */
    CANLIN_HTTP_PH_DNS,
    CANLIN_HTTP_PH_CONNECT,
    CANLIN_HTTP_PH_TLS,
    CANLIN_HTTP_PH_SEND,
    CANLIN_HTTP_PH_RECV,
    CANLIN_HTTP_PH_DONE        /* end/error delivered, waiting for close() */
} canlin_http_phase;

canlin_http_phase canlin_http_stream_phase(const canlin_http_stream *s);

typedef struct canlin_http_stats {
    unsigned streams;     /* live (not closed) streams */
    unsigned dns_jobs;    /* resolver jobs not yet collected, orphans included */
    unsigned dns_orphans; /* jobs whose stream is gone, still running on the worker */
    unsigned tls_trust;   /* 1 once the trust bundle is loaded */
} canlin_http_stats;

void canlin_http_get_stats(canlin_http *h, canlin_http_stats *out);

#ifdef __cplusplus
}
#endif

#endif
