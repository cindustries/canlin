/* CanLin native HTTP/1.1 client: admission, the per-stream state machine (lookup,
 * connect, TLS, request write, response), redirects, timeouts and the drive API
 * (HTTP transport spec §3–§8). See include/canlin/http.h for the contract.
 *
 * Streams live in one list. close() from outside step() frees at once; from inside
 * step() (any callback) it only marks the stream, and step() frees marked streams after
 * its loop, so no stream memory disappears under a running callback. */
#define _POSIX_C_SOURCE 200809L
#include "net_int.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct canlin_http {
    canlin_http_config cfg;
    char *ca_file, *ua;
    net_dns *dns;
    net_tls *tls;
    canlin_http_stream *streams;
    unsigned live;
    int in_step;
    canlin_ev_time now;
};

struct canlin_http_stream {
    canlin_http *h;
    canlin_http_stream *next;
    int closed;                 /* consumer closed (freed after step when in_step) */
    canlin_http_phase phase;
    canlin_http_cb cb;
    void *ud;
    /* request (borrowed until close) */
    const char *method;
    const canlin_http_header *hdrs;
    size_t nh;
    const char *body;
    size_t body_len;
    const canlin_http_cred *cred;
    int send_length;
    canlin_http_url url;        /* current hop */
    int orig_loop_http;         /* the first URL was loopback http */
    unsigned hops;
    /* lookup */
    long job;
    int started;                /* timers stamped by the first step */
    canlin_ev_time dns_deadline;
    canlin_net_addr addr[CANLIN_NET_ADDRS_MAX];
    size_t naddr, ai;
    int last_err, last_timeout;
    /* connection */
    int fd;
    net_tls_conn *tls;
    canlin_ev_time deadline;    /* connect / handshake / idle, per phase */
    /* request write */
    char *head;
    size_t head_len, head_off, body_off;
    /* response */
    char *pmem;
    canlin_http_parser parser;
    char *rx;
    size_t rx_off, rx_len;
    int paused;                 /* consumer refused more body this step */
    int send_blocked;           /* the last write would block (else: budget used up) */
    canlin_http_url redirect;
    int follow;                 /* head callback chose to follow `redirect` */
    const char *redirect_err;   /* head callback refused a redirect */
};

/* ---- context ---- */

static char *dupstr(const char *s)
{
    size_t n = strlen(s);
    char *d = malloc(n + 1);
    if (d)
        memcpy(d, s, n + 1);
    return d;
}

canlin_http *canlin_http_new(const canlin_http_config *cfg)
{
    canlin_http *h = calloc(1, sizeof *h);
    if (h == NULL)
        return NULL;
    if (cfg)
        h->cfg = *cfg;
    h->ca_file = dupstr(h->cfg.ca_file ? h->cfg.ca_file : CANLIN_HTTP_CA_FILE_DEFAULT);
    h->ua = dupstr(h->cfg.user_agent ? h->cfg.user_agent : CANLIN_HTTP_USER_AGENT);
    if (h->ca_file && h->ua) {
        /* the user agent goes on the wire verbatim: same rules as a header value */
        canlin_http_header ua;
        ua.name = "X-User-Agent";
        ua.value = h->ua;
        if (canlin_http_check_request("GET", &ua, 1, NULL) == 0) {
            h->cfg.ca_file = h->ca_file;
            h->cfg.user_agent = h->ua;
            return h;
        }
    }
    free(h->ca_file);
    free(h->ua);
    free(h);
    return NULL;
}

/* Release the connection and every buffer; the stream struct stays. */
static void conn_release(canlin_http_stream *s)
{
    if (s->job > 0) {
        net_dns_cancel(s->h->dns, s->job); /* orphaned: freed on arrival */
        s->job = 0;
    }
    if (s->tls) {
        net_tls_conn_free(s->tls);
        s->tls = NULL;
    }
    if (s->fd >= 0) {
        net_sock_close(s->fd);
        s->fd = -1;
    }
    if (s->head) {
        memset(s->head, 0, CANLIN_NET_REQUEST_HEAD_MAX); /* may hold the credential */
        free(s->head);
        s->head = NULL;
    }
    free(s->pmem);
    s->pmem = NULL;
    free(s->rx);
    s->rx = NULL;
    s->rx_off = s->rx_len = 0;
    s->paused = 0;
}

static void stream_free(canlin_http_stream *s)
{
    conn_release(s);
    memset(s, 0, sizeof *s);
    free(s);
}

void canlin_http_free(canlin_http *h)
{
    if (h == NULL)
        return;
    while (h->streams) {
        canlin_http_stream *n = h->streams->next;
        stream_free(h->streams);
        h->streams = n;
    }
    net_dns_free(h->dns);
    net_tls_free(h->tls);
    free(h->ca_file);
    free(h->ua);
    free(h);
}

/* ---- admission ---- */

static int is_get(const char *m)
{
    return strcmp(m, "GET") == 0 || strcmp(m, "HEAD") == 0;
}

canlin_http_stream *canlin_http_open(canlin_http *h, const canlin_http_request *req,
                                     const canlin_http_cb *cb, void *ud, const char **msg)
{
    canlin_http_stream *s;
    const char *m = NULL;
    char *probe;
    size_t plen;

    if (h == NULL || req == NULL || cb == NULL || req->method == NULL || req->url == NULL ||
        !cb->status || !cb->header || !cb->body || !cb->end || !cb->error)
        m = "invalid request";
    else if (h->live >= CANLIN_NET_STREAMS_MAX)
        m = "limit: too many open requests";
    else if (req->body_len && req->body == NULL)
        m = "invalid request body";
    else if (canlin_http_check_request(req->method, req->headers, req->nheaders, req->cred) != 0)
        m = "invalid method or header";
    if (m) {
        if (msg)
            *msg = m;
        return NULL;
    }
    s = calloc(1, sizeof *s);
    probe = malloc(CANLIN_NET_REQUEST_HEAD_MAX);
    if (s == NULL || probe == NULL) {
        free(s);
        free(probe);
        if (msg)
            *msg = "out of memory";
        return NULL;
    }
    s->fd = -1;
    s->h = h;
    s->cb = *cb;
    s->ud = ud;
    s->method = req->method;
    s->hdrs = req->headers;
    s->nh = req->nheaders;
    s->body = req->body;
    s->body_len = req->body_len;
    s->cred = req->cred;
    s->send_length = req->body_len > 0 || !is_get(req->method);
    if (canlin_http_url_parse(req->url, &s->url) != 0)
        m = "invalid URL";
    /* The head must fit (checked with the credential in place, then wiped). */
    else if (canlin_http_write_request(probe, CANLIN_NET_REQUEST_HEAD_MAX, &plen, s->method,
                                       &s->url, s->hdrs, s->nh, s->send_length, s->body_len,
                                       s->cred, h->ua) != 0)
        m = "limit: request head too large";
    memset(probe, 0, CANLIN_NET_REQUEST_HEAD_MAX);
    free(probe);
    if (m) {
        free(s);
        if (msg)
            *msg = m;
        return NULL;
    }
    s->orig_loop_http = !s->url.https && canlin_http_url_loopback(&s->url);
    s->phase = CANLIN_HTTP_PH_QUEUED;
    s->next = h->streams;
    h->streams = s;
    h->live++;
    return s;
}

void canlin_http_close(canlin_http *h, canlin_http_stream *s)
{
    canlin_http_stream **pp;
    if (h == NULL || s == NULL || s->closed)
        return;
    s->closed = 1;
    h->live--;
    if (h->in_step)
        return; /* released and freed by step after its loop: a callback may be running
                 * on this stream's buffers */
    for (pp = &h->streams; *pp; pp = &(*pp)->next)
        if (*pp == s) {
            *pp = s->next;
            break;
        }
    stream_free(s);
}

/* ---- terminal outcomes ---- */

static void fail(canlin_http_stream *s, const char *code, const char *detail)
{
    char m[CANLIN_NET_MSG_MAX];
    conn_release(s);
    s->phase = CANLIN_HTTP_PH_DONE;
    snprintf(m, sizeof m, "%s: %s", code, detail);
    s->cb.error(s->ud, m);
}

static void finish(canlin_http_stream *s)
{
    conn_release(s);
    s->phase = CANLIN_HTTP_PH_DONE;
    s->cb.end(s->ud);
}

/* ---- phases ---- */

static void start_lookup(canlin_http_stream *s)
{
    canlin_http *h = s->h;
    long id;
    if (!s->started) {
        s->started = 1;
        s->dns_deadline = h->now + CANLIN_NET_DNS_TIMEOUT_MS;
    }
    if (h->dns == NULL) {
        h->dns = net_dns_new(h->cfg.resolve, h->cfg.resolve_ctx);
        if (h->dns == NULL) {
            fail(s, "dns", "resolver unavailable");
            return;
        }
    }
    id = net_dns_submit(h->dns, s->url.host, s->url.port);
    if (id < 0) {
        fail(s, "dns", "resolver unavailable");
        return;
    }
    if (id == 0) { /* job table full (orphans still running): wait */
        if (h->now >= s->dns_deadline)
            fail(s, "dns", "lookup timed out");
        return;
    }
    s->job = id;
    s->phase = CANLIN_HTTP_PH_DNS;
}

static void start_connect(canlin_http_stream *s);

static void poll_lookup(canlin_http_stream *s)
{
    int n = net_dns_result(s->h->dns, s->job, s->addr, CANLIN_NET_ADDRS_MAX);
    if (n == 0) {
        if (s->h->now >= s->dns_deadline) {
            net_dns_cancel(s->h->dns, s->job);
            s->job = 0;
            fail(s, "dns", "lookup timed out");
        }
        return;
    }
    s->job = 0;
    if (n < 0) {
        fail(s, "dns", "host not found");
        return;
    }
    s->naddr = (size_t)n;
    s->ai = 0;
    s->last_err = 0;
    s->last_timeout = 0;
    start_connect(s);
}

static void connected(canlin_http_stream *s);

/* Try addresses from s->ai in order until one is in progress or connected. */
static void start_connect(canlin_http_stream *s)
{
    while (s->ai < s->naddr) {
        int pending = 0, err = 0;
        int fd = net_sock_connect(&s->addr[s->ai], &pending, &err);
        if (fd < 0) {
            s->last_err = err;
            s->last_timeout = 0;
            s->ai++;
            continue;
        }
        s->fd = fd;
        s->phase = CANLIN_HTTP_PH_CONNECT;
        s->deadline = s->h->now + CANLIN_NET_CONNECT_TIMEOUT_MS;
        if (!pending)
            connected(s);
        return;
    }
    if (s->last_timeout)
        fail(s, "timeout", "connect timed out");
    else
        fail(s, "connect", net_sock_strerror(s->last_err));
}

static void poll_connect(canlin_http_stream *s)
{
    int err = 0, rc = net_sock_check(s->fd, &err);
    if (rc > 0) {
        connected(s);
        return;
    }
    if (rc == 0 && s->h->now < s->deadline)
        return;
    net_sock_close(s->fd);
    s->fd = -1;
    s->last_err = rc < 0 ? err : 0;
    s->last_timeout = rc == 0;
    s->ai++;
    start_connect(s);
}

static void start_send(canlin_http_stream *s)
{
    size_t len;
    s->head = malloc(CANLIN_NET_REQUEST_HEAD_MAX);
    if (s->head == NULL) {
        fail(s, "limit", "out of memory");
        return;
    }
    if (canlin_http_write_request(s->head, CANLIN_NET_REQUEST_HEAD_MAX, &len, s->method,
                                  &s->url, s->hdrs, s->nh, s->send_length, s->body_len,
                                  s->cred, s->h->ua) != 0) {
        fail(s, "limit", "request head too large");
        return;
    }
    s->head_len = len;
    s->head_off = 0;
    s->body_off = 0;
    s->phase = CANLIN_HTTP_PH_SEND;
    s->deadline = s->h->now + CANLIN_NET_IDLE_TIMEOUT_MS;
}

static void connected(canlin_http_stream *s)
{
    canlin_http *h = s->h;
    int err = 0;
    if (!s->url.https) {
        start_send(s);
        return;
    }
    if (h->tls == NULL) {
        h->tls = net_tls_new(h->ca_file, h->cfg.wall_clock, h->cfg.wall_ctx);
        if (h->tls == NULL) {
            fail(s, "tls", "out of memory");
            return;
        }
    }
    s->tls = net_tls_conn_new(h->tls, s->fd, s->url.host, &err);
    if (s->tls == NULL) {
        if (err == NET_TLS_E_TRUST)
            fail(s, "tls-trust", "trust store missing or unreadable");
        else
            fail(s, "tls", "cannot set up TLS");
        return;
    }
    s->phase = CANLIN_HTTP_PH_TLS;
    s->deadline = h->now + CANLIN_NET_HANDSHAKE_TIMEOUT_MS;
}

static void tls_fail(canlin_http_stream *s, long rc, const char *what)
{
    switch (rc) {
    case NET_TLS_E_VERIFY:
        fail(s, "tls-verify", "certificate not trusted for this host");
        break;
    case NET_TLS_E_CLOCK:
        fail(s, "tls-clock", "certificate dates do not match the system time");
        break;
    case NET_TLS_E_EOF:
    case NET_TLS_E_RESET:
        fail(s, "eof", what);
        break;
    default:
        fail(s, "tls", what);
    }
}

static void poll_handshake(canlin_http_stream *s)
{
    int rc = net_tls_handshake(s->tls);
    if (rc == NET_TLS_OK) {
        start_send(s);
        return;
    }
    if (rc == NET_TLS_WANT_READ || rc == NET_TLS_WANT_WRITE) {
        if (s->h->now >= s->deadline)
            fail(s, "timeout", "TLS handshake timed out");
        return;
    }
    tls_fail(s, rc == NET_TLS_E_EOF || rc == NET_TLS_E_RESET ? NET_TLS_E_TLS : rc,
             "handshake failed");
}

static void start_recv(canlin_http_stream *s);

static long io_write(canlin_http_stream *s, const void *buf, size_t n, int *err)
{
    if (s->tls)
        return net_tls_write(s->tls, buf, n);
    return net_sock_send(s->fd, buf, n, err);
}

static void poll_send(canlin_http_stream *s)
{
    size_t budget = CANLIN_NET_TURN_BYTES;
    s->send_blocked = 0;
    while (budget) {
        const char *p;
        size_t n;
        long w;
        int err = 0;
        if (s->head_off < s->head_len) {
            p = s->head + s->head_off;
            n = s->head_len - s->head_off;
        } else if (s->body_off < s->body_len) {
            p = s->body + s->body_off;
            n = s->body_len - s->body_off;
        } else {
            memset(s->head, 0, CANLIN_NET_REQUEST_HEAD_MAX); /* the credential is sent */
            free(s->head);
            s->head = NULL;
            start_recv(s);
            return;
        }
        if (n > budget)
            n = budget;
        w = io_write(s, p, n, &err);
        if (w > 0) {
            if (s->head_off < s->head_len)
                s->head_off += (size_t)w;
            else
                s->body_off += (size_t)w;
            budget -= (size_t)w;
            s->deadline = s->h->now + CANLIN_NET_IDLE_TIMEOUT_MS;
            continue;
        }
        if (s->tls ? (w == NET_TLS_WANT_READ || w == NET_TLS_WANT_WRITE) : w == 0) {
            s->send_blocked = 1;
            break;
        }
        if (s->tls)
            tls_fail(s, w, "connection lost while sending");
        else
            fail(s, "eof", net_sock_strerror(err));
        return;
    }
    if (s->h->now >= s->deadline)
        fail(s, "timeout", "no progress sending the request");
}

/* ---- response ---- */

static int ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = *a >= 'A' && *a <= 'Z' ? (char)(*a - 'A' + 'a') : *a;
        if (x != *b)
            return 0;
    }
    return *a == *b;
}

static int is_redirect(int st)
{
    return st == 301 || st == 302 || st == 303 || st == 307 || st == 308;
}

static int on_head(void *ud, int status, const canlin_http_field *f, size_t n)
{
    canlin_http_stream *s = ud;
    size_t i;
    const char *loc = NULL;

    if (is_redirect(status))
        for (i = 0; i < n && loc == NULL; i++)
            if (ieq(f[i].name, "location"))
                loc = f[i].value;
    if (loc) { /* a 3xx without Location is an ordinary response */
        canlin_http_url t;
        s->follow = 1;
        if (s->hops >= CANLIN_NET_REDIRECTS_MAX)
            s->redirect_err = "too many redirects";
        else if (status <= 303 && !is_get(s->method))
            s->redirect_err = "method would change on a 301/302/303 redirect";
        else if (canlin_http_url_resolve(&s->url, loc, &t) != 0)
            s->redirect_err = "unsupported Location";
        else if (!t.https && !(s->orig_loop_http && canlin_http_url_loopback(&t)))
            s->redirect_err = "redirect to plain http refused";
        else
            s->redirect = t;
        return 1;
    }
    s->cb.status(s->ud, status);
    for (i = 0; i < n && !s->closed; i++)
        s->cb.header(s->ud, f[i].name, f[i].value);
    return s->closed;
}

static int on_body(void *ud, const char *buf, size_t n)
{
    canlin_http_stream *s = ud;
    if (s->closed)
        return 1;
    if (s->cb.body(s->ud, buf, n))
        s->paused = 1;
    return s->paused || s->closed;
}

static const canlin_http_parser_cb parser_cb = {on_head, on_body};

static void start_recv(canlin_http_stream *s)
{
    s->pmem = malloc(CANLIN_HTTP_PARSER_MEM);
    s->rx = malloc(CANLIN_NET_RX_MAX);
    if (s->pmem == NULL || s->rx == NULL) {
        fail(s, "limit", "out of memory");
        return;
    }
    canlin_http_parser_init(&s->parser, s->pmem, strcmp(s->method, "HEAD") == 0, &parser_cb, s);
    s->rx_off = s->rx_len = 0;
    s->phase = CANLIN_HTTP_PH_RECV;
    s->deadline = s->h->now + CANLIN_NET_IDLE_TIMEOUT_MS;
}

/* Follow s->redirect: new connection for the next hop, same method and body. */
static void follow(canlin_http_stream *s)
{
    canlin_http_url t = s->redirect;
    conn_release(s);
    s->follow = 0;
    s->hops++;
    s->url = t;
    s->phase = CANLIN_HTTP_PH_QUEUED;
    s->started = 0;
    start_lookup(s);
}

static void parser_error(canlin_http_stream *s, int rc)
{
    const char *why = canlin_http_parser_why(&s->parser);
    if (rc == CANLIN_HTTP_P_ELIMIT)
        fail(s, "limit", why);
    else if (rc == CANLIN_HTTP_P_EEOF)
        fail(s, "eof", why);
    else
        fail(s, "protocol", why);
}

/* Feed buffered bytes. Returns 1 when the stream left the RECV phase or was closed. */
static int feed(canlin_http_stream *s)
{
    while (s->rx_off < s->rx_len) {
        size_t used = 0;
        int rc = canlin_http_parser_feed(&s->parser, s->rx + s->rx_off, s->rx_len - s->rx_off,
                                         &used);
        s->rx_off += used;
        if (s->closed)
            return 1;
        if (rc < 0) {
            parser_error(s, rc);
            return 1;
        }
        if (rc == CANLIN_HTTP_P_STOPPED) {
            if (s->redirect_err)
                fail(s, "redirect", s->redirect_err);
            else if (s->follow)
                follow(s);
            return 1;
        }
        if (rc == CANLIN_HTTP_P_DONE) {
            finish(s);
            return 1;
        }
        if (rc == CANLIN_HTTP_P_PAUSED)
            return 0;
    }
    s->rx_off = s->rx_len = 0;
    return 0;
}

static void at_eof(canlin_http_stream *s, int clean)
{
    int rc;
    if (!clean && canlin_http_parser_needs_eof(&s->parser)) {
        /* A close-delimited body over TLS must end with close_notify: a bare TCP FIN
         * could be a truncation. */
        fail(s, "eof", "TLS connection closed without close_notify");
        return;
    }
    rc = canlin_http_parser_eof(&s->parser);
    if (rc == CANLIN_HTTP_P_DONE)
        finish(s);
    else
        parser_error(s, rc);
}

static void poll_recv(canlin_http_stream *s)
{
    size_t budget = CANLIN_NET_TURN_BYTES;

    s->paused = 0;
    if (feed(s) || s->paused)
        return;
    while (budget) {
        size_t n = budget < CANLIN_NET_RX_MAX ? budget : CANLIN_NET_RX_MAX;
        long r;
        int err = 0, blocked;
        if (s->tls) {
            r = net_tls_read(s->tls, s->rx, n);
            blocked = r == NET_TLS_WANT_READ || r == NET_TLS_WANT_WRITE;
        } else {
            r = net_sock_recv(s->fd, s->rx, n, &err);
            blocked = r == NET_AGAIN;
        }
        if (blocked)
            break;
        if (r > 0) {
            s->rx_off = 0;
            s->rx_len = (size_t)r;
            budget -= (size_t)r;
            s->deadline = s->h->now + CANLIN_NET_IDLE_TIMEOUT_MS;
            if (feed(s) || s->paused)
                return;
            continue;
        }
        if (r == 0) {
            at_eof(s, 1);
            return;
        }
        if (s->tls && r == NET_TLS_E_EOF) {
            at_eof(s, 0);
            return;
        }
        if (s->tls)
            tls_fail(s, r, "connection lost");
        else
            fail(s, "eof", net_sock_strerror(err));
        return;
    }
    if (budget && s->h->now >= s->deadline)
        fail(s, "timeout", "no data received");
}

static void phase_step(canlin_http_stream *s)
{
    switch (s->phase) {
    case CANLIN_HTTP_PH_QUEUED:
        start_lookup(s);
        if (s->phase == CANLIN_HTTP_PH_DNS)
            poll_lookup(s); /* a cached/instant answer can already be there */
        break;
    case CANLIN_HTTP_PH_DNS:
        poll_lookup(s);
        break;
    case CANLIN_HTTP_PH_CONNECT:
        poll_connect(s);
        break;
    case CANLIN_HTTP_PH_TLS:
        poll_handshake(s);
        break;
    case CANLIN_HTTP_PH_SEND:
        poll_send(s);
        break;
    case CANLIN_HTTP_PH_RECV:
        poll_recv(s);
        break;
    default:
        break;
    }
}

/* Run the current phase; continue into the next one within this step when a phase
 * finished without waiting (bounded: at most 8 phase changes per stream per step). */
static void stream_step(canlin_http_stream *s)
{
    unsigned k;
    for (k = 0; k < 8 && !s->closed; k++) {
        canlin_http_phase ph = s->phase;
        phase_step(s);
        if (s->phase == ph || s->phase == CANLIN_HTTP_PH_DONE || s->paused)
            break;
    }
}

void canlin_http_step(canlin_http *h, canlin_ev_time now)
{
    canlin_http_stream *s, **pp;
    if (h == NULL)
        return;
    h->now = now;
    h->in_step++;
    if (h->dns)
        net_dns_drain(h->dns);
    for (s = h->streams; s; s = s->next)
        if (!s->closed)
            stream_step(s);
    h->in_step--;
    if (h->in_step)
        return;
    for (pp = &h->streams; *pp;) {
        s = *pp;
        if (s->closed) {
            *pp = s->next;
            stream_free(s);
        } else
            pp = &s->next;
    }
}

/* ---- drive ---- */

size_t canlin_http_pollfds(canlin_http *h, struct pollfd *out, size_t max,
                           canlin_ev_time *deadline)
{
    canlin_http_stream *s;
    canlin_ev_time dl = CANLIN_HTTP_NO_DEADLINE;
    size_t n = 0;
    int dns_wait = 0;

    if (h == NULL) {
        if (deadline)
            *deadline = dl;
        return 0;
    }
    for (s = h->streams; s; s = s->next) {
        short ev = 0;
        canlin_ev_time d = CANLIN_HTTP_NO_DEADLINE;
        if (s->closed)
            continue;
        switch (s->phase) {
        case CANLIN_HTTP_PH_QUEUED:
            if (s->started) { /* waiting for a resolver slot: woken by the pipe */
                dns_wait = 1;
                d = s->dns_deadline;
            } else
                d = 0;
            break;
        case CANLIN_HTTP_PH_DNS:
            dns_wait = 1;
            d = s->dns_deadline;
            break;
        case CANLIN_HTTP_PH_CONNECT:
            ev = POLLOUT;
            d = s->deadline;
            break;
        case CANLIN_HTTP_PH_TLS:
            ev = net_tls_events(s->tls);
            d = s->deadline;
            break;
        case CANLIN_HTTP_PH_SEND:
            if (!s->send_blocked)
                d = 0; /* the turn budget ran out: continue next step */
            else {
                ev = s->tls ? net_tls_events(s->tls) : POLLOUT;
                d = s->deadline;
            }
            break;
        case CANLIN_HTTP_PH_RECV:
            if (s->rx_off < s->rx_len || (s->tls && net_tls_pending(s->tls)))
                d = 0; /* buffered input poll() cannot see */
            else {
                ev = s->tls ? net_tls_events(s->tls) : POLLIN;
                d = s->deadline;
            }
            break;
        default:
            break;
        }
        if (ev) {
            if (n < max) {
                out[n].fd = s->fd;
                out[n].events = ev;
                out[n].revents = 0;
            }
            n++;
        }
        if (d < dl)
            dl = d;
    }
    if (dns_wait && net_dns_fd(h->dns) >= 0) {
        if (n < max) {
            out[n].fd = net_dns_fd(h->dns);
            out[n].events = POLLIN;
            out[n].revents = 0;
        }
        n++;
    }
    if (deadline)
        *deadline = dl;
    return n;
}

canlin_http_phase canlin_http_stream_phase(const canlin_http_stream *s)
{
    return s->phase;
}

void canlin_http_get_stats(canlin_http *h, canlin_http_stats *out)
{
    memset(out, 0, sizeof *out);
    if (h == NULL)
        return;
    out->streams = h->live;
    net_dns_stats(h->dns, &out->dns_jobs, &out->dns_orphans);
    out->tls_trust = (unsigned)net_tls_trust_loaded(h->tls);
}
