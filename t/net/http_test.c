/* Native HTTP/1.1 + TLS transport end to end (HTTP transport spec §3–§9): canlin_http
 * against the forked loopback server (t/net/tsrv.c), plain and TLS, with the fake
 * canlin_ev clock driving every transport timeout and a fake wall clock for certificate
 * dates. No external network. Exit 0 = pass.
 *
 * Environment: CANLIN_HTTP_BENCH=1 prints HTTPS round trips including the handshake (real
 * clock, client and forked server on the same machine; a number for the evidence record,
 * not a pass/fail criterion). TSRV_HOLD=<seconds> only starts the test server, prints
 * its ports and CA file and waits (debugging with curl/openssl). */
#define _POSIX_C_SOURCE 200809L
#include "canlin/http.h"
#include "tsrv.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "mbedtls/platform.h"

static int failures, checks;

#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; \
    fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #c); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

#define SECRET "sk-live-TOPSECRET-4242"

static tsrv *srv;
static canlin_ev_fake_clock clk;

/* ---- fake wall clock ---- */

static int64_t wall_now;

static int64_t days_from_civil(int y, int m, int d)
{
    int64_t era, yoe, doy, doe;
    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static int64_t at(int y, int m, int d)
{
    return days_from_civil(y, m, d) * 86400;
}

static int64_t fake_wall(void *ctx)
{
    (void)ctx;
    return wall_now;
}

/* ---- test resolver ---- */

static pthread_mutex_t rmu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t rcv = PTHREAD_COND_INITIALIZER;
static int block_release, block_entered;
static unsigned refused_port;

static void v4(canlin_net_addr *a, unsigned port)
{
    struct sockaddr_in *s = (struct sockaddr_in *)&a->sa;
    memset(a, 0, sizeof *a);
    s->sin_family = AF_INET;
    s->sin_port = htons((unsigned short)port);
    s->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a->len = sizeof *s;
}

static int test_resolve(void *ctx, const char *host, unsigned port, canlin_net_addr *out,
                        size_t max)
{
    (void)ctx;
    if (max < 2)
        return -1;
    if (strcmp(host, "block.test") == 0) {
        pthread_mutex_lock(&rmu);
        block_entered++;
        pthread_cond_broadcast(&rcv);
        while (!block_release)
            pthread_cond_wait(&rcv, &rmu);
        pthread_mutex_unlock(&rmu);
        v4(&out[0], port);
        return 1;
    }
    if (strcmp(host, "fallback.test") == 0) { /* first address refuses, second works */
        v4(&out[0], refused_port);
        v4(&out[1], port);
        return 2;
    }
    if (strcmp(host, "localhost") == 0 || strcmp(host, "127.0.0.1") == 0) {
        v4(&out[0], port);
        return 1;
    }
    return -1;
}

static void block(int on)
{
    pthread_mutex_lock(&rmu);
    block_release = !on;
    if (on)
        block_entered = 0;
    pthread_cond_broadcast(&rcv);
    pthread_mutex_unlock(&rmu);
}

/* ---- recording consumer ---- */

typedef struct rec {
    canlin_http *h;
    canlin_http_stream *s;
    int status, ended, errored, calls, after_done;
    char err[CANLIN_NET_MSG_MAX + 8];
    char hdrs[4096];
    size_t hl;
    char *body;
    size_t blen, bcap;
    unsigned bodies;
    size_t max_piece;
    int pause;        /* body returns nonzero every time */
    int close_at;     /* 1 status, 2 first header, 3 first body: close() inside callback */
    int closed;
} rec;

static void r_close(rec *r)
{
    if (!r->closed && r->s) {
        canlin_http_close(r->h, r->s);
        r->closed = 1;
    }
}

static void note(rec *r)
{
    r->calls++;
    if (r->ended || r->errored || r->closed)
        r->after_done++;
}

static void cb_status(void *ud, int st)
{
    rec *r = ud;
    note(r);
    r->status = st;
    if (r->close_at == 1)
        r_close(r);
}

static void cb_header(void *ud, const char *n, const char *v)
{
    rec *r = ud;
    int k;
    note(r);
    k = snprintf(r->hdrs + r->hl, sizeof r->hdrs - r->hl, "%s: %s\n", n, v);
    if (k > 0 && (size_t)k < sizeof r->hdrs - r->hl)
        r->hl += (size_t)k;
    if (r->close_at == 2)
        r_close(r);
}

static int cb_body(void *ud, const void *b, size_t n)
{
    rec *r = ud;
    note(r);
    if (r->blen + n + 1 > r->bcap) {
        r->bcap = (r->blen + n + 1) * 2;
        r->body = realloc(r->body, r->bcap);
    }
    memcpy(r->body + r->blen, b, n);
    r->blen += n;
    r->body[r->blen] = 0;
    r->bodies++;
    if (n > r->max_piece)
        r->max_piece = n;
    if (r->close_at == 3)
        r_close(r);
    return r->pause;
}

static void cb_end(void *ud)
{
    rec *r = ud;
    note(r);
    r->ended = 1;
}

static void cb_error(void *ud, const char *msg)
{
    rec *r = ud;
    note(r);
    r->errored = 1;
    snprintf(r->err, sizeof r->err, "%s", msg);
    CHECK(strstr(msg, SECRET) == NULL && strstr(msg, "TOPSECRET") == NULL,
          "secret in error message '%s'", msg);
}

static const canlin_http_cb rcb = {cb_status, cb_header, cb_body, cb_end, cb_error};

static void rec_free(rec *r)
{
    r_close(r);
    free(r->body);
    memset(r, 0, sizeof *r);
}

/* ---- context and driving ---- */

static canlin_http *mk(const char *ca, canlin_http_resolve_fn res)
{
    canlin_http_config c;
    memset(&c, 0, sizeof c);
    c.ca_file = ca;
    c.wall_clock = fake_wall;
    c.resolve = res;
    return canlin_http_new(&c);
}

static long real_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* One loop iteration of a native backend: poll the transport's descriptors until its
 * deadline (capped at 20 ms real time; the fake clock never moves by itself), step. */
static void iterate(canlin_http *h)
{
    struct pollfd p[CANLIN_HTTP_POLLFDS_MAX];
    canlin_ev_time dl;
    size_t n = canlin_http_pollfds(h, p, CANLIN_HTTP_POLLFDS_MAX, &dl);
    int to = 20;
    CHECK(n <= CANLIN_HTTP_POLLFDS_MAX, "pollfds wants %u", (unsigned)n);
    if (dl <= clk.now)
        to = 0;
    poll(p, (nfds_t)n, to);
    canlin_http_step(h, clk.now);
    tsrv_pump(srv);
}

static int done(const rec *r)
{
    return r->ended || r->errored || r->closed;
}

/* Drive until every rec is done or `ms` of real time passed. */
static void run(canlin_http *h, rec **rs, int n, int ms)
{
    long t0 = real_ms();
    for (;;) {
        int i, all = 1;
        for (i = 0; i < n; i++)
            all &= done(rs[i]);
        if (all || real_ms() - t0 > ms)
            break;
        iterate(h);
    }
}

/* Drive for a while (real time) without expecting completion. */
static void spin(canlin_http *h, int ms)
{
    long t0 = real_ms();
    while (real_ms() - t0 < ms)
        iterate(h);
}

static canlin_http_stream *go(canlin_http *h, rec *r, const char *method, const char *url,
                              const canlin_http_cred *cred, const char *body)
{
    canlin_http_request q;
    const char *msg = NULL;
    static const canlin_http_header hs[1] = {{"X-Test", "1"}};
    memset(&q, 0, sizeof q);
    q.method = method;
    q.url = url;
    q.headers = hs;
    q.nheaders = 1;
    q.body = body;
    q.body_len = body ? strlen(body) : 0;
    q.cred = cred;
    memset(r, 0, sizeof *r);
    r->h = h;
    r->s = canlin_http_open(h, &q, &rcb, r, &msg);
    CHECK(r->s != NULL, "open %s failed: %s", url, msg ? msg : "?");
    return r->s;
}

/* One request to completion; returns the rec (caller frees). */
static void one(canlin_http *h, rec *r, const char *method, const char *url,
                const canlin_http_cred *cred, const char *body)
{
    rec *rs[1];
    rs[0] = r;
    go(h, r, method, url, cred, body);
    if (r->s)
        run(h, rs, 1, 15000);
    CHECK(done(r), "%s did not finish", url);
    CHECK(r->after_done == 0, "%s: callback after end/error", url);
}

static char url_buf[8][256];
static const char *U(int slot, const char *scheme, const char *host, int listener, const char *path)
{
    snprintf(url_buf[slot], sizeof url_buf[slot], "%s://%s:%u%s", scheme, host,
             tsrv_port(srv, listener), path);
    return url_buf[slot];
}

static int err_is(const rec *r, const char *prefix)
{
    return r->errored && strncmp(r->err, prefix, strlen(prefix)) == 0;
}

/* ---- tests ---- */

/* HTTP transport spec §5: entropy is getrandom() or /dev/urandom. Without glibc (bionic,
 * musl) Mbed TLS reads mbedtls_platform_dev_random, whose default "/dev/random" blocks on
 * kernels before 5.6 while the entropy estimate is low: the owner thread's DRBG seed and
 * the test server's TLS stalled on the Android 4.4 tablet. The configuration pins it. */
static void test_entropy_source(void)
{
    CHECK(strcmp(mbedtls_platform_dev_random, "/dev/urandom") == 0,
          "platform entropy file is '%s'", mbedtls_platform_dev_random);
}

static void test_basic(void)
{
    canlin_http *h = mk(tsrv_ca_file(), test_resolve);
    rec r;
    const char *q;
    size_t ql;
    static const int kinds[3] = {TS_PLAIN, TS_EC, TS_RSA};
    int k;

    for (k = 0; k < 3; k++) {
        tsrv_clear(srv);
        one(h, &r, "GET", U(0, kinds[k] == TS_PLAIN ? "http" : "https", "localhost", kinds[k], "/ok"),
            NULL, NULL);
        CHECK(r.ended && r.status == 200 && r.blen == 5 && memcmp(r.body, "hello", 5) == 0,
              "kind %d: status %d err '%s'", k, r.status, r.err);
        CHECK(strstr(r.hdrs, "Content-Type: text/plain\n") != NULL, "headers '%s'", r.hdrs);
        CHECK(tsrv_wait(srv, 1, 2000) == 1, "server saw no request");
        q = tsrv_req(srv, 0, &ql);
        CHECK(q && strstr(q, "GET /ok HTTP/1.1\r\n") && strstr(q, "\r\nUser-Agent: CanLin/") &&
                  strstr(q, "\r\nAccept-Encoding: identity\r\n") &&
                  strstr(q, "\r\nConnection: close\r\n") && strstr(q, "\r\nX-Test: 1\r\n") &&
                  !strstr(q, "Content-Length"),
              "request on the wire:\n%s", q ? q : "(none)");
        rec_free(&r);
    }
    /* POST with a body, plain and TLS */
    for (k = 0; k < 2; k++) {
        tsrv_clear(srv);
        one(h, &r, "POST", U(0, k ? "https" : "http", "localhost", k ? TS_EC : TS_PLAIN, "/echo"),
            NULL, "{\"a\":1}");
        CHECK(r.ended && r.status == 200 && r.blen == 7 && memcmp(r.body, "{\"a\":1}", 7) == 0,
              "echo: %d '%s'", r.status, r.err);
        CHECK(strstr(r.hdrs, "X-Method: POST\n") != NULL, "echo headers");
        tsrv_wait(srv, 1, 2000);
        q = tsrv_req(srv, 0, NULL);
        CHECK(q && strstr(q, "\r\nContent-Length: 7\r\n"), "content-length on the wire");
        rec_free(&r);
    }
    /* chunked, close-delimited (with close_notify over TLS) */
    one(h, &r, "GET", U(0, "https", "localhost", TS_EC, "/chunked"), NULL, NULL);
    CHECK(r.ended && r.blen == 12 && memcmp(r.body, "hello, world", 12) == 0, "chunked '%s'", r.err);
    rec_free(&r);
    one(h, &r, "GET", U(0, "https", "localhost", TS_EC, "/close-delim"), NULL, NULL);
    CHECK(r.ended && r.blen == 6, "close-delimited TLS: %s", r.err);
    rec_free(&r);
    one(h, &r, "GET", U(0, "http", "localhost", TS_PLAIN, "/close-delim"), NULL, NULL);
    CHECK(r.ended && r.blen == 6, "close-delimited plain: %s", r.err);
    rec_free(&r);
    /* close-delimited TLS body ended by a bare TCP FIN: possible truncation */
    one(h, &r, "GET", U(0, "https", "localhost", TS_EC, "/close-noclose"), NULL, NULL);
    CHECK(err_is(&r, "eof:"), "no close_notify: '%s'", r.err);
    rec_free(&r);
    /* EOF inside a Content-Length body, protocol errors from the parser */
    one(h, &r, "GET", U(0, "https", "localhost", TS_EC, "/early-close"), NULL, NULL);
    CHECK(err_is(&r, "eof:") && r.status == 200, "early close: '%s'", r.err);
    rec_free(&r);
    one(h, &r, "GET", U(0, "http", "localhost", TS_PLAIN, "/gzip"), NULL, NULL);
    CHECK(err_is(&r, "protocol: unsupported content-encoding") && r.status == 0,
          "gzip: '%s'", r.err);
    rec_free(&r);
    one(h, &r, "GET", U(0, "http", "localhost", TS_PLAIN, "/nothing"), NULL, NULL);
    CHECK(r.ended && r.status == 404, "404: %d", r.status);
    rec_free(&r);
    /* addresses tried in order: the first refuses, the second answers */
    {
        char u[128];
        snprintf(u, sizeof u, "http://fallback.test:%u/ok", tsrv_port(srv, TS_PLAIN));
        one(h, &r, "GET", u, NULL, NULL);
        CHECK(r.ended && r.status == 200, "fallback: '%s'", r.err);
        rec_free(&r);
    }
    canlin_http_free(h);

    /* the platform resolver (getaddrinfo) for a numeric host */
    h = mk(tsrv_ca_file(), NULL);
    one(h, &r, "GET", U(0, "http", "127.0.0.1", TS_PLAIN, "/ok"), NULL, NULL);
    CHECK(r.ended && r.status == 200, "getaddrinfo path: '%s'", r.err);
    rec_free(&r);
    canlin_http_free(h);
}

static void test_sse_bytes(void)
{
    static const char want[] = TSRV_SSE_BYTES;
    canlin_http *h = mk(tsrv_ca_file(), test_resolve);
    rec r;
    int k;
    for (k = 0; k < 2; k++) {
        one(h, &r, "GET", U(0, k ? "https" : "http", "localhost", k ? TS_EC : TS_PLAIN, "/sse-bytes"),
            NULL, NULL);
        CHECK(r.ended && r.blen == sizeof want - 1 && memcmp(r.body, want, r.blen) == 0,
              "sse bytes (%s): %s", k ? "tls" : "plain", r.err);
        /* delivered incrementally, not buffered until the end */
        CHECK(r.bodies > 10, "sse delivered in only %u pieces", r.bodies);
        rec_free(&r);
    }
    canlin_http_free(h);
}

static void test_backpressure(void)
{
    canlin_http *h = mk(tsrv_ca_file(), test_resolve);
    rec r;
    size_t i;
    unsigned steps = 0;
    long t0;
    int k;
    for (k = 0; k < 2; k++) {
        go(h, &r, "GET", U(0, k ? "https" : "http", "localhost", k ? TS_EC : TS_PLAIN, "/big"),
           NULL, NULL);
        r.pause = 1;
        t0 = real_ms();
        while (!done(&r) && real_ms() - t0 < 20000) {
            unsigned before = r.bodies;
            iterate(h);
            steps++;
            /* a pausing consumer gets at most one body callback per step */
            CHECK(r.bodies - before <= 1, "%u body callbacks in one step", r.bodies - before);
        }
        CHECK(r.ended && r.blen == TSRV_BIG_LEN, "big (%d): %u bytes '%s'", k, (unsigned)r.blen, r.err);
        CHECK(r.max_piece <= CANLIN_NET_TURN_BYTES, "piece %u > TURN_BYTES", (unsigned)r.max_piece);
        for (i = 0; i < r.blen; i++)
            if ((unsigned char)r.body[i] != (unsigned char)(i * 7 % 251)) {
                CHECK(0, "big body byte %u", (unsigned)i);
                break;
            }
        rec_free(&r);
    }
    (void)steps;
    canlin_http_free(h);
}

static void test_redirects(void)
{
    canlin_http *h = mk(tsrv_ca_file(), test_resolve);
    canlin_http_cred cred;
    char origin[64], loc[256], path[400];
    rec r;
    const char *q0, *q1;

    snprintf(origin, sizeof origin, "https://localhost:%u", tsrv_port(srv, TS_EC));
    cred.origin = origin;
    cred.header = "Authorization";
    cred.prefix = "Bearer ";
    cred.secret = SECRET;
    cred.secret_len = strlen(SECRET);

    /* same origin keeps the credential */
    tsrv_clear(srv);
    one(h, &r, "GET", U(0, "https", "localhost", TS_EC, "/r?c=302&l=/ok"), &cred, NULL);
    CHECK(r.ended && r.status == 200 && r.blen == 5, "same-origin redirect: '%s'", r.err);
    CHECK(tsrv_wait(srv, 2, 2000) == 2, "two requests expected");
    q0 = tsrv_req(srv, 0, NULL);
    q1 = tsrv_req(srv, 1, NULL);
    CHECK(q0 && strstr(q0, "Authorization: Bearer " SECRET "\r\n"), "credential missing on hop 0");
    CHECK(q1 && strstr(q1, "GET /ok ") && strstr(q1, "Authorization: Bearer " SECRET "\r\n"),
          "credential missing on same-origin hop");
    rec_free(&r);

    /* cross origin (other port) drops it */
    tsrv_clear(srv);
    snprintf(loc, sizeof loc, "https://localhost:%u/ok", tsrv_port(srv, TS_RSA));
    snprintf(path, sizeof path, "/r?c=301&l=%s", loc);
    one(h, &r, "GET", U(0, "https", "localhost", TS_EC, path), &cred, NULL);
    CHECK(r.ended && r.status == 200, "cross-origin redirect: '%s'", r.err);
    CHECK(tsrv_wait(srv, 2, 2000) == 2, "two requests expected");
    q0 = tsrv_req(srv, 0, NULL);
    q1 = tsrv_req(srv, 1, NULL);
    CHECK(q0 && strstr(q0, "L=ec\n") && strstr(q0, SECRET), "hop 0 lacks the credential");
    CHECK(q1 && strstr(q1, "L=rsa\n") && !strstr(q1, SECRET) && !strstr(q1, "Authorization"),
          "credential leaked across origins:\n%s", q1 ? q1 : "(none)");
    rec_free(&r);

    /* the credential never goes to an unbound origin, even on the first request */
    tsrv_clear(srv);
    one(h, &r, "GET", U(0, "https", "localhost", TS_RSA, "/ok"), &cred, NULL);
    CHECK(r.ended, "unbound origin: '%s'", r.err);
    tsrv_wait(srv, 1, 2000);
    q0 = tsrv_req(srv, 0, NULL);
    CHECK(q0 && !strstr(q0, SECRET), "credential sent to an unbound origin");
    rec_free(&r);

    /* POST on 302 fails: no silent method change, nothing followed */
    tsrv_clear(srv);
    one(h, &r, "POST", U(0, "https", "localhost", TS_EC, "/r?c=302&l=/echo"), &cred, "{}");
    CHECK(err_is(&r, "redirect:") && r.status == 0, "POST 302: '%s'", r.err);
    CHECK(tsrv_wait(srv, 2, 300) == 1, "a refused redirect was followed");
    rec_free(&r);
    /* POST on 307 is followed with the same method and body */
    tsrv_clear(srv);
    one(h, &r, "POST", U(0, "https", "localhost", TS_EC, "/r?c=307&l=/echo"), &cred, "{\"b\":2}");
    CHECK(r.ended && r.blen == 7 && strstr(r.hdrs, "X-Method: POST"), "POST 307: '%s'", r.err);
    CHECK(tsrv_wait(srv, 2, 2000) == 2, "307 not followed");
    q1 = tsrv_req(srv, 1, NULL);
    CHECK(q1 && strstr(q1, "POST /echo ") && strstr(q1, "{\"b\":2}") && strstr(q1, SECRET),
          "307 hop");
    rec_free(&r);
    /* https -> http fails */
    snprintf(path, sizeof path, "/r?c=302&l=http://localhost:%u/ok", tsrv_port(srv, TS_PLAIN));
    one(h, &r, "GET", U(0, "https", "localhost", TS_EC, path), NULL, NULL);
    CHECK(err_is(&r, "redirect:"), "downgrade: '%s'", r.err);
    rec_free(&r);
    /* loopback http -> loopback http is allowed; the other host drops the credential */
    {
        canlin_http_cred c2 = cred;
        char o2[64];
        snprintf(o2, sizeof o2, "http://localhost:%u", tsrv_port(srv, TS_PLAIN));
        c2.origin = o2;
        tsrv_clear(srv);
        snprintf(path, sizeof path, "/r?c=302&l=http://127.0.0.1:%u/ok", tsrv_port(srv, TS_PLAIN));
        one(h, &r, "GET", U(0, "http", "localhost", TS_PLAIN, path), &c2, NULL);
        CHECK(r.ended && r.status == 200, "loopback http redirect: '%s'", r.err);
        tsrv_wait(srv, 2, 2000);
        q0 = tsrv_req(srv, 0, NULL);
        q1 = tsrv_req(srv, 1, NULL);
        CHECK(q0 && strstr(q0, SECRET) && q1 && !strstr(q1, SECRET), "loopback hop credential");
        rec_free(&r);
    }
    /* at most CANLIN_NET_REDIRECTS_MAX hops */
    one(h, &r, "GET", U(0, "https", "localhost", TS_EC, "/chain/3"), NULL, NULL);
    CHECK(r.ended && r.blen == 3, "3 redirects: '%s'", r.err);
    rec_free(&r);
    one(h, &r, "GET", U(0, "https", "localhost", TS_EC, "/chain/4"), NULL, NULL);
    CHECK(err_is(&r, "redirect: too many"), "4 redirects: '%s'", r.err);
    rec_free(&r);
    /* unsupported relative Location */
    one(h, &r, "GET", U(0, "http", "localhost", TS_PLAIN, "/r?c=302&l=relative/x"), NULL, NULL);
    CHECK(err_is(&r, "redirect: unsupported Location"), "relative: '%s'", r.err);
    rec_free(&r);
    canlin_http_free(h);
}

static void test_tls_failures(void)
{
    canlin_http *h = mk(tsrv_ca_file(), test_resolve);
    rec r;

    one(h, &r, "GET", U(0, "https", "localhost", TS_WRONG, "/ok"), NULL, NULL);
    CHECK(err_is(&r, "tls-verify:"), "wrong host: '%s'", r.err);
    rec_free(&r);
    one(h, &r, "GET", U(0, "https", "localhost", TS_SELF, "/ok"), NULL, NULL);
    CHECK(err_is(&r, "tls-verify:"), "self-signed: '%s'", r.err);
    rec_free(&r);
    one(h, &r, "GET", U(0, "https", "localhost", TS_OTHER, "/ok"), NULL, NULL);
    CHECK(err_is(&r, "tls-verify:"), "untrusted CA: '%s'", r.err);
    rec_free(&r);
    /* validity dates against the configured wall clock */
    one(h, &r, "GET", U(0, "https", "localhost", TS_EXPIRED, "/ok"), NULL, NULL);
    CHECK(err_is(&r, "tls-clock:"), "expired leaf at 2030: '%s'", r.err);
    rec_free(&r);
    wall_now = at(2050, 1, 2); /* also past the CA's own validity */
    one(h, &r, "GET", U(0, "https", "localhost", TS_EC, "/ok"), NULL, NULL);
    CHECK(err_is(&r, "tls-clock:"), "valid leaf at 2050: '%s'", r.err);
    rec_free(&r);
    wall_now = at(2020, 6, 1); /* within the expired leaf's window */
    one(h, &r, "GET", U(0, "https", "localhost", TS_EXPIRED, "/ok"), NULL, NULL);
    CHECK(r.ended && r.status == 200, "expired leaf at 2020: '%s'", r.err);
    rec_free(&r);
    wall_now = at(2024, 6, 1); /* before the leaf's notBefore: not yet valid */
    one(h, &r, "GET", U(0, "https", "localhost", TS_EC, "/ok"), NULL, NULL);
    CHECK(err_is(&r, "tls-clock:"), "leaf not yet valid: '%s'", r.err);
    rec_free(&r);
    wall_now = at(2030, 6, 1);
    /* a wrong clock and a wrong host: verification, not clock */
    wall_now = at(2050, 1, 2);
    one(h, &r, "GET", U(0, "https", "localhost", TS_WRONG, "/ok"), NULL, NULL);
    CHECK(err_is(&r, "tls-verify:"), "wrong host + clock: '%s'", r.err);
    rec_free(&r);
    wall_now = at(2030, 6, 1);
    canlin_http_free(h);

    /* trust store: missing, unparseable */
    h = mk("/nonexistent/ca-bundle.crt", test_resolve);
    one(h, &r, "GET", U(0, "https", "localhost", TS_EC, "/ok"), NULL, NULL);
    CHECK(err_is(&r, "tls-trust:"), "missing bundle: '%s'", r.err);
    rec_free(&r);
    /* plain http still works without a bundle */
    one(h, &r, "GET", U(0, "http", "localhost", TS_PLAIN, "/ok"), NULL, NULL);
    CHECK(r.ended, "plain without bundle: '%s'", r.err);
    rec_free(&r);
    canlin_http_free(h);
    {
        char p[256];
        const char *dir = getenv("TMPDIR");
        FILE *f;
        snprintf(p, sizeof p, "%s/canlin-garbage-%ld.crt", dir && *dir ? dir : "/tmp", (long)getpid());
        f = fopen(p, "w");
        if (f) {
            fputs("-----BEGIN CERTIFICATE-----\nnot base64 at all\n-----END CERTIFICATE-----\n", f);
            fclose(f);
        }
        h = mk(p, test_resolve);
        one(h, &r, "GET", U(0, "https", "localhost", TS_EC, "/ok"), NULL, NULL);
        CHECK(err_is(&r, "tls-trust:"), "garbage bundle: '%s'", r.err);
        rec_free(&r);
        canlin_http_free(h);
        unlink(p);
    }
    /* the other CA as the only anchor: the test CA's leaf is untrusted */
    h = mk(tsrv_other_ca_file(), test_resolve);
    one(h, &r, "GET", U(0, "https", "localhost", TS_EC, "/ok"), NULL, NULL);
    CHECK(err_is(&r, "tls-verify:"), "leaf from a CA not in the bundle: '%s'", r.err);
    rec_free(&r);
    canlin_http_free(h);
}

static unsigned closed_port(void)
{
    struct sockaddr_in a;
    socklen_t l = sizeof a;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    unsigned p;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(fd, (struct sockaddr *)&a, sizeof a);
    getsockname(fd, (struct sockaddr *)&a, &l);
    p = ntohs(a.sin_port);
    close(fd);
    return p;
}

static void test_timeouts(void)
{
    canlin_http *h = mk(tsrv_ca_file(), test_resolve);
    rec r, *rs[1];
    char u[128];
    rs[0] = &r;

    /* connect refused */
    snprintf(u, sizeof u, "http://localhost:%u/", refused_port);
    one(h, &r, "GET", u, NULL, NULL);
    CHECK(err_is(&r, "connect: connection refused"), "refused: '%s'", r.err);
    rec_free(&r);

    /* handshake stall: fires exactly at CANLIN_NET_HANDSHAKE_TIMEOUT_MS */
    go(h, &r, "GET", U(0, "https", "localhost", TS_STALL, "/"), NULL, NULL);
    spin(h, 200);
    CHECK(canlin_http_stream_phase(r.s) == CANLIN_HTTP_PH_TLS, "phase %d", canlin_http_stream_phase(r.s));
    canlin_ev_fake_clock_advance(&clk, CANLIN_NET_HANDSHAKE_TIMEOUT_MS - 1);
    spin(h, 50);
    CHECK(!done(&r), "handshake timed out early");
    canlin_ev_fake_clock_advance(&clk, 1);
    run(h, rs, 1, 2000);
    CHECK(err_is(&r, "timeout: TLS handshake"), "handshake stall: '%s'", r.err);
    rec_free(&r);

    /* idle stall inside the body */
    go(h, &r, "GET", U(0, "https", "localhost", TS_EC, "/stall-body"), NULL, NULL);
    {
        long t0 = real_ms();
        while (r.blen < 10 && real_ms() - t0 < 5000)
            iterate(h);
    }
    CHECK(r.status == 200 && r.blen == 10, "stall-body prefix %u", (unsigned)r.blen);
    canlin_ev_fake_clock_advance(&clk, CANLIN_NET_IDLE_TIMEOUT_MS - 1);
    spin(h, 50);
    CHECK(!done(&r), "idle timeout early");
    canlin_ev_fake_clock_advance(&clk, 1);
    run(h, rs, 1, 2000);
    CHECK(err_is(&r, "timeout: no data"), "idle stall: '%s'", r.err);
    rec_free(&r);

    /* connect stall (full accept queue): per-address connect timeout */
    go(h, &r, "GET", U(0, "http", "localhost", TS_BACKLOG, "/"), NULL, NULL);
    spin(h, 200);
    CHECK(canlin_http_stream_phase(r.s) == CANLIN_HTTP_PH_CONNECT, "connect phase %d",
          canlin_http_stream_phase(r.s));
    canlin_ev_fake_clock_advance(&clk, CANLIN_NET_CONNECT_TIMEOUT_MS);
    run(h, rs, 1, 2000);
    CHECK(err_is(&r, "timeout: connect"), "connect stall: '%s'", r.err);
    rec_free(&r);

    /* DNS: no answer within CANLIN_NET_DNS_TIMEOUT_MS; the late result is dropped */
    block(1);
    go(h, &r, "GET", "http://block.test/", NULL, NULL);
    spin(h, 100);
    CHECK(canlin_http_stream_phase(r.s) == CANLIN_HTTP_PH_DNS, "dns phase");
    canlin_ev_fake_clock_advance(&clk, CANLIN_NET_DNS_TIMEOUT_MS);
    run(h, rs, 1, 2000);
    CHECK(err_is(&r, "dns: lookup timed out"), "dns timeout: '%s'", r.err);
    rec_free(&r);
    {
        canlin_http_stats st;
        canlin_http_get_stats(h, &st);
        CHECK(st.dns_jobs == 1 && st.dns_orphans == 1, "orphan while blocked: %u/%u", st.dns_jobs,
              st.dns_orphans);
        block(0);
        spin(h, 100);
        canlin_http_get_stats(h, &st);
        CHECK(st.dns_jobs == 0 && st.streams == 0, "orphan not freed: %u", st.dns_jobs);
    }
    /* name that does not resolve */
    one(h, &r, "GET", "http://nx.test/", NULL, NULL);
    CHECK(err_is(&r, "dns: host not found"), "nx: '%s'", r.err);
    rec_free(&r);
    canlin_http_free(h);
}

/* Cancel in every phase: close() and nothing is called afterwards. */
static void test_cancel(void)
{
    canlin_http *h = mk(tsrv_ca_file(), test_resolve);
    rec r;
    canlin_http_stats st;

    /* before the first step (queued) */
    go(h, &r, "GET", U(0, "http", "localhost", TS_PLAIN, "/ok"), NULL, NULL);
    CHECK(canlin_http_stream_phase(r.s) == CANLIN_HTTP_PH_QUEUED, "queued");
    r_close(&r);
    spin(h, 100);
    CHECK(r.calls == 0, "callbacks after cancel while queued");

    /* during DNS: the worker's late answer is freed, never delivered */
    block(1);
    go(h, &r, "GET", "http://block.test/", NULL, NULL);
    spin(h, 100);
    CHECK(canlin_http_stream_phase(r.s) == CANLIN_HTTP_PH_DNS, "dns phase");
    r_close(&r);
    canlin_http_get_stats(h, &st);
    CHECK(st.streams == 0 && st.dns_orphans == 1, "orphan %u", st.dns_orphans);
    block(0);
    spin(h, 150);
    canlin_http_get_stats(h, &st);
    CHECK(r.calls == 0 && st.dns_jobs == 0, "dns cancel: calls %d jobs %u", r.calls, st.dns_jobs);

    /* during connect */
    go(h, &r, "GET", U(0, "http", "localhost", TS_BACKLOG, "/"), NULL, NULL);
    spin(h, 100);
    CHECK(canlin_http_stream_phase(r.s) == CANLIN_HTTP_PH_CONNECT, "connect phase");
    r_close(&r);
    canlin_ev_fake_clock_advance(&clk, CANLIN_NET_CONNECT_TIMEOUT_MS * 2);
    spin(h, 100);
    CHECK(r.calls == 0, "connect cancel");

    /* during the handshake */
    go(h, &r, "GET", U(0, "https", "localhost", TS_STALL, "/"), NULL, NULL);
    spin(h, 100);
    CHECK(canlin_http_stream_phase(r.s) == CANLIN_HTTP_PH_TLS, "tls phase");
    r_close(&r);
    canlin_ev_fake_clock_advance(&clk, CANLIN_NET_HANDSHAKE_TIMEOUT_MS * 2);
    spin(h, 100);
    CHECK(r.calls == 0, "handshake cancel");

    /* during the body, from outside a callback */
    go(h, &r, "GET", U(0, "https", "localhost", TS_EC, "/stall-body"), NULL, NULL);
    {
        long t0 = real_ms();
        while (r.blen < 10 && real_ms() - t0 < 5000)
            iterate(h);
    }
    CHECK(canlin_http_stream_phase(r.s) == CANLIN_HTTP_PH_RECV && r.blen == 10, "body phase");
    {
        int calls = r.calls;
        r_close(&r);
        canlin_ev_fake_clock_advance(&clk, CANLIN_NET_IDLE_TIMEOUT_MS * 2);
        spin(h, 100);
        CHECK(r.calls == calls, "body cancel: more callbacks");
    }
    free(r.body);

    /* from inside callbacks: status, header, body */
    {
        int at_;
        for (at_ = 1; at_ <= 3; at_++) {
            int calls;
            go(h, &r, "GET", U(0, "https", "localhost", TS_EC, "/big"), NULL, NULL);
            r.close_at = at_;
            spin(h, 300);
            calls = r.calls;
            CHECK(r.closed && r.after_done == 0, "close inside callback %d: after %d", at_, r.after_done);
            CHECK(calls == (at_ == 1 ? 1 : at_ == 2 ? 2 : 3), "close at %d: %d calls", at_, calls);
            free(r.body);
        }
    }
    canlin_http_get_stats(h, &st);
    CHECK(st.streams == 0, "streams left %u", st.streams);
    canlin_http_free(h);
}

static void test_admission(void)
{
    canlin_http *h = mk(tsrv_ca_file(), test_resolve);
    rec r[CANLIN_NET_STREAMS_MAX + 1];
    canlin_http_request q;
    canlin_http_header bad[1];
    const char *msg = NULL;
    unsigned i;
    struct pollfd p[CANLIN_HTTP_POLLFDS_MAX];
    canlin_ev_time dl;

    for (i = 0; i < CANLIN_NET_STREAMS_MAX; i++)
        go(h, &r[i], "GET", U(0, "https", "localhost", TS_STALL, "/"), NULL, NULL);
    /* open() makes no callback and no I/O; queued streams ask for an immediate step */
    CHECK(canlin_http_pollfds(h, p, CANLIN_HTTP_POLLFDS_MAX, &dl) == 0 && dl == 0, "queued pollfds");
    memset(&q, 0, sizeof q);
    q.method = "GET";
    q.url = U(1, "http", "localhost", TS_PLAIN, "/ok");
    memset(&r[i], 0, sizeof r[i]);
    CHECK(canlin_http_open(h, &q, &rcb, &r[i], &msg) == NULL && msg && strncmp(msg, "limit:", 6) == 0,
          "5th stream admitted (%s)", msg ? msg : "");
    spin(h, 200);
    /* all four are waiting on sockets now */
    CHECK(canlin_http_pollfds(h, p, CANLIN_HTTP_POLLFDS_MAX, &dl) == CANLIN_NET_STREAMS_MAX &&
              dl == clk.now + CANLIN_NET_HANDSHAKE_TIMEOUT_MS,
          "pollfds with 4 handshakes");
    CHECK(canlin_http_pollfds(h, p, 2, &dl) == CANLIN_NET_STREAMS_MAX, "truncated pollfds count");
    r_close(&r[0]);
    CHECK(canlin_http_open(h, &q, &rcb, &r[i], &msg) != NULL, "slot not released by close");
    r[i].h = h;
    r[i].s = NULL;
    for (i = 0; i <= CANLIN_NET_STREAMS_MAX; i++)
        if (r[i].s)
            r_close(&r[i]);
    canlin_http_free(h); /* also frees the admitted 5th stream */

    /* invalid requests fail in open(), with a static message and no callback */
    h = mk(tsrv_ca_file(), test_resolve);
    bad[0].name = "X-Evil";
    bad[0].value = "a\r\nInjected: yes";
    q.headers = bad;
    q.nheaders = 1;
    CHECK(canlin_http_open(h, &q, &rcb, &r[0], &msg) == NULL, "CRLF header accepted");
    q.nheaders = 0;
    q.url = "ftp://x/";
    CHECK(canlin_http_open(h, &q, &rcb, &r[0], &msg) == NULL, "ftp accepted");
    q.url = "https://user:pw@localhost/";
    CHECK(canlin_http_open(h, &q, &rcb, &r[0], &msg) == NULL, "userinfo accepted");
    canlin_http_free(h);
    CHECK(canlin_http_pollfds(NULL, p, 1, &dl) == 0 && dl == CANLIN_HTTP_NO_DEADLINE, "NULL pollfds");
}

/* Handshake time for the evidence record (real clock), plain vs TLS round trip. */
static void bench(void)
{
    canlin_http *h = mk(tsrv_ca_file(), test_resolve);
    rec r;
    int k;
    for (k = 0; k < 3; k++) {
        long t0 = real_ms();
        one(h, &r, "GET", U(0, "https", "localhost", k == 2 ? TS_RSA : TS_EC, "/ok"), NULL, NULL);
        printf("bench: https GET %s round trip %ld ms (%s)\n", k == 2 ? "RSA-2048" : "P-256",
               real_ms() - t0, r.ended ? "ok" : r.err);
        rec_free(&r);
    }
    {
        long t0 = real_ms();
        one(h, &r, "GET", U(0, "http", "localhost", TS_PLAIN, "/ok"), NULL, NULL);
        printf("bench: http GET round trip %ld ms\n", real_ms() - t0);
        rec_free(&r);
    }
    canlin_http_free(h);
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    srv = tsrv_start();
    if (srv == NULL || tsrv_ca_file() == NULL) {
        fprintf(stderr, "cannot start the loopback test server\n");
        return 1;
    }
    refused_port = closed_port();
    if (getenv("TSRV_HOLD")) { /* debug aid */
        int k;
        for (k = 0; k < TS_N; k++)
            printf("port %d %u\n", k, tsrv_port(srv, k));
        printf("ca %s\n", tsrv_ca_file());
        fflush(stdout);
        sleep(atoi(getenv("TSRV_HOLD")));
        tsrv_stop(srv);
        return 0;
    }
    wall_now = at(2030, 6, 1);
    clk.now = 1000;
    test_entropy_source();
    test_basic();
    test_sse_bytes();
    test_backpressure();
    test_redirects();
    test_tls_failures();
    test_timeouts();
    test_cancel();
    test_admission();
    if (getenv("CANLIN_HTTP_BENCH"))
        bench();
    tsrv_stop(srv);
    if (failures) {
        fprintf(stderr, "%d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("http_test: %d checks passed\n", checks);
    return 0;
}
