/* The openai-chat dialect through the real native transport (HTTP transport spec §9,
 * LLM providers spec §8, canlin#41): canlin_llm_http_transport over canlin_http against
 * the forked loopback server (t/net/tsrv.c), replaying the fixtures of t/llm/test_llm.c
 * (openai_fixtures.h) with Content-Length, chunked and close-delimited framing, split
 * into many writes, plain (local profile) and TLS (keyed profile). Fake canlin_ev clock,
 * no external network, no real provider key. Exit 0 on success. */
#define _POSIX_C_SOURCE 200809L
#include "canlin/llm.h"
#include "canlin/llm_http.h"
#include "canlin/module.h"
#include "../net/tsrv.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "openai_fixtures.h"

static int failures, checks;

#define CHECK(c)                                                                   \
    do {                                                                           \
        checks++;                                                                  \
        if (!(c)) {                                                                \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c);  \
            failures++;                                                            \
        }                                                                          \
    } while (0)

#define KEY "sk-test-7a6b5c4d3e2f10987654"
#define LOGMAX (1u << 20)

static tsrv *srv;

/* ---- harness (as in test_llm.c, with the native transport) ---- */

typedef struct logbuf {
    char *buf;
    size_t len;
} logbuf;

typedef struct harness {
    canlin_ev_fake_clock clock;
    canlin_ev *ev;
    canlin_http *http;
    canlin_llm *llm;
    canlin_module *m;
    logbuf log, llmlog;
} harness;

static void log_fn(void *ud, const char *msg, size_t len)
{
    logbuf *l = ud;
    if (len + 2 > LOGMAX - l->len)
        return;
    memcpy(l->buf + l->len, msg, len);
    l->len += len;
    l->buf[l->len++] = '\n';
    l->buf[l->len] = 0;
}

static void on_deliver(void *ud, canlin_ev *ev, const canlin_ev_delivery *d)
{
    harness *h = ud;
    (void)ev;
    if (h->m && canlin_module_gen(h->m) == d->gen)
        canlin_module_deliver(h->m, d);
}

static int has(const logbuf *l, const char *needle)
{
    return strstr(l->buf, needle) != NULL;
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

/* Every name maps to 127.0.0.1; the port in the URL selects the listener. */
static int resolve(void *ctx, const char *host, unsigned port, canlin_net_addr *out, size_t max)
{
    struct sockaddr_in *s = (struct sockaddr_in *)&out[0].sa;
    (void)ctx;
    (void)host;
    if (max < 1)
        return -1;
    memset(&out[0], 0, sizeof out[0]);
    s->sin_family = AF_INET;
    s->sin_port = htons((unsigned short)port);
    s->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    out[0].len = sizeof *s;
    return 1;
}

static void h_init(harness *h)
{
    canlin_ev_clock c;
    canlin_ev_callbacks cb;
    canlin_llm_config lc;
    canlin_http_config hc;
    memset(h, 0, sizeof *h);
    h->log.buf = calloc(1, LOGMAX);
    h->llmlog.buf = calloc(1, LOGMAX);
    h->clock.now = 1000;
    c = canlin_ev_fake_clock_bind(&h->clock);
    cb.deliver = on_deliver;
    h->ev = canlin_ev_new(&c, &cb, h);
    memset(&hc, 0, sizeof hc);
    hc.ca_file = tsrv_ca_file();
    hc.resolve = resolve;
    h->http = canlin_http_new(&hc);
    memset(&lc, 0, sizeof lc);
    lc.transport = canlin_llm_http_transport(h->http);
    lc.log = log_fn;
    lc.log_ud = &h->llmlog;
    h->llm = canlin_llm_new(h->ev, &lc);
    if (!h->ev || !h->http || !h->llm || !h->log.buf || !h->llmlog.buf) {
        fprintf(stderr, "harness setup failed\n");
        exit(1);
    }
    tsrv_clear(srv);
}

static void check_no_key(harness *h)
{
    CHECK(!has(&h->log, KEY));
    CHECK(!has(&h->log, "sk-test-"));
    CHECK(!has(&h->llmlog, KEY));
    CHECK(!has(&h->llmlog, "sk-test-"));
}

static void h_done(harness *h)
{
    canlin_ev_stats st;
    canlin_http_stats hs;
    check_no_key(h);
    canlin_module_free(h->m);
    canlin_llm_free(h->llm);
    canlin_http_get_stats(h->http, &hs);
    CHECK(hs.streams == 0);
    canlin_http_free(h->http);
    canlin_ev_get_stats(h->ev, -1, &st);
    CHECK(st.items == 0 && st.bytes == 0 && st.ops == 0);
    CHECK(canlin_ev_free(h->ev) == CANLIN_EV_OK);
    free(h->log.buf);
    free(h->llmlog.buf);
}

/* Profile "p": TLS on `listener` with KEY, or the plain listener as a local endpoint. */
static void h_profile(harness *h, int tls, int listener)
{
    canlin_llm_profile_config pc;
    char base[128];
    memset(&pc, 0, sizeof pc);
    pc.id = "p";
    pc.model = "m-1";
    if (tls) {
        snprintf(base, sizeof base, "https://localhost:%u/v1", tsrv_port(srv, listener));
        pc.preset = "openai";
    } else {
        snprintf(base, sizeof base, "http://localhost:%u/v1", tsrv_port(srv, TS_PLAIN));
        pc.preset = "custom";
        pc.dialect = CANLIN_LLM_DIALECT_OPENAI_CHAT;
        pc.auth = CANLIN_LLM_AUTH_NONE;
        pc.local = 1;
        pc.tools = 1;
    }
    pc.base_url = base;
    CHECK(canlin_llm_profile_create(h->llm, &pc) == CANLIN_LLM_OK);
    if (tls)
        CHECK(canlin_llm_profile_set_key(h->llm, "p", KEY, strlen(KEY)) == CANLIN_LLM_OK);
}

static void h_load(harness *h, const char *setup)
{
    canlin_module_source s;
    canlin_sandbox_config cfg;
    static char src[8192];
    int err = 1;
    snprintf(src, sizeof src, mod_tpl, setup);
    memset(&s, 0, sizeof s);
    memset(&cfg, 0, sizeof cfg);
    s.name = "assistant";
    s.version = "1";
    s.main = src;
    s.main_len = strlen(src);
    s.assistant = canlin_llm_install;
    s.assistant_ud = h->llm;
    cfg.log = log_fn;
    cfg.ud = &h->log;
    h->m = canlin_module_load(h->ev, &s, &cfg, &err);
    CHECK(err == CANLIN_MODULE_OK);
}

static long real_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* The native loop: poll the transport until its deadline (capped, real time), step it
 * with the scheduler's clock, pump the LLM core, run a scheduler turn. */
static void h_run(harness *h)
{
    long t0 = real_ms();
    int idle = 0;
    while (idle < 3 && real_ms() - t0 < 20000) {
        struct pollfd p[CANLIN_HTTP_POLLFDS_MAX];
        canlin_ev_time dl, now = canlin_ev_now(h->ev);
        canlin_ev_turn_result tr;
        size_t n = canlin_http_pollfds(h->http, p, CANLIN_HTTP_POLLFDS_MAX, &dl);
        poll(p, (nfds_t)n, dl <= now ? 0 : 10);
        canlin_http_step(h->http, now);
        canlin_llm_pump(h->llm);
        CHECK(canlin_ev_turn(h->ev, &tr) == CANLIN_EV_OK);
        tsrv_pump(srv);
        idle = (!tr.more && canlin_llm_inflight(h->llm) == 0) ? idle + 1 : 0;
    }
    CHECK(idle >= 3);
}

enum { F_LENGTH = 0, F_CHUNKED, F_CLOSE };

/* Script the response for `path`: status, content type, body in `framing`, written in
 * pieces of `piece` bytes; chunked framing uses HTTP chunks of `chunk` bytes. */
static void serve(const char *path, int status, const char *ctype, const char *body, size_t n,
                  int framing, size_t piece, size_t chunk)
{
    size_t cap = n * 2 + 4096, len, off;
    char *r = malloc(cap);
    len = (size_t)snprintf(r, cap, "HTTP/1.1 %d X\r\nContent-Type: %s\r\n", status,
                           ctype ? ctype : "text/event-stream");
    if (framing == F_LENGTH)
        len += (size_t)snprintf(r + len, cap - len, "Content-Length: %lu\r\n\r\n", (unsigned long)n);
    else if (framing == F_CHUNKED)
        len += (size_t)snprintf(r + len, cap - len, "Transfer-Encoding: chunked\r\n\r\n");
    else
        len += (size_t)snprintf(r + len, cap - len, "\r\n");
    if (framing == F_CHUNKED) {
        for (off = 0; off < n;) {
            size_t k = n - off < chunk ? n - off : chunk;
            len += (size_t)snprintf(r + len, cap - len, "%lx\r\n", (unsigned long)k);
            memcpy(r + len, body + off, k);
            len += k;
            memcpy(r + len, "\r\n", 2);
            len += 2;
            off += k;
        }
        memcpy(r + len, "0\r\n\r\n", 5);
        len += 5;
    } else {
        memcpy(r + len, body, n);
        len += n;
    }
    CHECK(tsrv_script(srv, path, r, len, piece, 0, 0) == 0);
    free(r);
}

static const char *const listener_name[2] = {"L=plain\n", "L=ec\n"};

/* ---- tests ---- */

static void check_text_ok(harness *h)
{
    CHECK(has(&h->log, "req ok"));
    CHECK(has(&h->log, text_expect));
    CHECK(has(&h->log, "usage\t7\t3"));
    CHECK(has(&h->log, "done ok\tend"));
    CHECK(count(&h->log, "done ") == 1);
    CHECK(!has(&h->log, "BAD"));
}

static void test_text(void)
{
    static const struct {
        int framing;
        size_t piece, chunk;
    } v[] = {{F_LENGTH, 0, 0}, {F_LENGTH, 1, 0}, {F_CHUNKED, 5, 7}, {F_CHUNKED, 0, 1},
             {F_CLOSE, 13, 0}, {F_CLOSE, 1, 0}};
    size_t i;
    int tls;
    for (tls = 0; tls <= 1; tls++)
        for (i = 0; i < sizeof v / sizeof v[0]; i++) {
            harness h;
            const char *q;
            h_init(&h);
            h_profile(&h, tls, TS_EC);
            serve("/v1/chat/completions", 200, NULL, text_stream, strlen(text_stream), v[i].framing,
                  v[i].piece, v[i].chunk);
            h_load(&h, req_simple);
            h_run(&h);
            check_text_ok(&h);
            if (!has(&h.log, "done ok\tend"))
                fprintf(stderr, "text tls=%d case %u:\n%s\n", tls, (unsigned)i, h.log.buf);
            /* what went over the wire */
            CHECK(tsrv_wait(srv, 1, 2000) == 1);
            q = tsrv_req(srv, 0, NULL);
            CHECK(q && strncmp(q, listener_name[tls], strlen(listener_name[tls])) == 0);
            CHECK(q && strstr(q, "POST /v1/chat/completions HTTP/1.1\r\n"));
            CHECK(q && strstr(q, "\r\nContent-Type: application/json\r\n"));
            CHECK(q && strstr(q, "\r\nAccept: text/event-stream\r\n"));
            CHECK(q && strstr(q, "\"model\":\"m-1\""));
            if (tls)
                CHECK(q && strstr(q, "\r\nAuthorization: Bearer " KEY "\r\n"));
            else
                CHECK(q && !strstr(q, "Authorization"));
            h_done(&h);
        }
}

static void test_large_and_tools(void)
{
    char *body = malloc(400000), *p, esc[8], line[64];
    size_t i, want = 0;
    harness h;
    static const char args[] = "{\"title\":\"Einkaufsliste\",\"widgets\":[{\"type\":\"heading\","
                               "\"text\":\"Was fehlt? \\u00e4\"}]}";

    /* 2000 text deltas, 97-byte writes, chunked in 1 KiB HTTP chunks, over TLS */
    p = body;
    for (i = 0; i < 2000; i++) {
        p += sprintf(p, "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"%04d-\xc3\xa4\\u20ac|\"}}]}\n\n", (int)i);
        want += 11;
    }
    p += sprintf(p, "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"length\"}]}\n\ndata: [DONE]\n\n");
    h_init(&h);
    h_profile(&h, 1, TS_RSA);
    serve("/v1/chat/completions", 200, NULL, body, (size_t)(p - body), F_CHUNKED, 97, 1024);
    h_load(&h, req_simple);
    h_run(&h);
    snprintf(line, sizeof line, "textlen\t%u", (unsigned)want);
    CHECK(has(&h.log, line));
    CHECK(has(&h.log, "done ok\tlength"));
    CHECK(!has(&h.log, "BAD"));
    h_done(&h);

    /* tool call arguments one byte per delta */
    p = body + sprintf(body, "%s", tool_stream_head);
    for (i = 0; args[i]; i++) {
        if (args[i] == '"' || args[i] == '\\')
            snprintf(esc, sizeof esc, "\\%c", args[i]);
        else
            snprintf(esc, sizeof esc, "%c", args[i]);
        p += sprintf(p, "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,"
                        "\"function\":{\"arguments\":\"%s\"}}]}}]}\n\n", esc);
    }
    p += sprintf(p, "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n\n"
                    "data: [DONE]\n\n");
    h_init(&h);
    h_profile(&h, 1, TS_EC);
    serve("/v1/chat/completions", 200, NULL, body, (size_t)(p - body), F_LENGTH, 13, 0);
    h_load(&h, req_tools);
    h_run(&h);
    CHECK(has(&h.log, "tool\tcall_A\tset_view\t{\"title\":\"Einkaufsliste\",\"widgets\":[{\"type\":\"heading\",\"text\":\"Was fehlt? \\u00e4\"}]}"));
    CHECK(has(&h.log, "done ok\ttool"));
    CHECK(count(&h.log, "tool\t") == 1);
    h_done(&h);

    /* two parallel tool calls, plain local endpoint, close-delimited */
    h_init(&h);
    h_profile(&h, 0, 0);
    serve("/v1/chat/completions", 200, NULL, two_tools_stream, strlen(two_tools_stream), F_CLOSE, 7, 0);
    h_load(&h, req_tools);
    h_run(&h);
    {
        const char *a = strstr(h.log.buf, "tool\ta\tset_view\t{\"widgets\":[]}");
        const char *b = strstr(h.log.buf, "tool\tb\tset_view\t{\"x\":1}");
        const char *t = strstr(h.log.buf, "frag\t7");
        CHECK(a && b && t && t < a && a < b);
    }
    CHECK(has(&h.log, "done ok\ttool"));
    h_done(&h);
    free(body);
}

static void expect(int tls, int status, const char *ctype, const char *body, int framing,
                   size_t cut, const char *line)
{
    harness h;
    size_t n = strlen(body);
    h_init(&h);
    h_profile(&h, tls, TS_EC);
    if (cut) {
        /* Content-Length announces the whole body, the server sends `cut` bytes and closes */
        char *r = malloc(n + 256);
        size_t len = (size_t)sprintf(r, "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                                        "Content-Length: %lu\r\n\r\n", (unsigned long)n);
        memcpy(r + len, body, cut);
        CHECK(tsrv_script(srv, "/v1/chat/completions", r, len + cut, 10, 0, 0) == 0);
        free(r);
    } else
        serve("/v1/chat/completions", status, ctype, body, n, framing, 5, 11);
    h_load(&h, req_simple);
    h_run(&h);
    if (!has(&h.log, line))
        fprintf(stderr, "expected '%s' in log:\n%s\n", line, h.log.buf);
    CHECK(has(&h.log, line));
    CHECK(count(&h.log, "done ") == 1);
    h_done(&h);
}

static void test_failures(void)
{
    int tls;
    for (tls = 0; tls <= 1; tls++) {
        /* dialect: EOF after finish_reason but before [DONE] (framing complete) */
        expect(tls, 200, NULL,
               "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"x\"},\"finish_reason\":\"stop\"}]}\n\n",
               F_CLOSE, 0, "done fail\tunavailable\topenai-chat protocol error: stream ended before [DONE]");
        expect(tls, 200, NULL,
               "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"part\"}}]}\n\n"
               "data: {\"error\":{\"message\":\"overloaded\",\"type\":\"server_error\"}}\n\n",
               F_CHUNKED, 0, "done fail\tunavailable\tprovider error: overloaded");
        /* transport: the connection ends before Content-Length is reached */
        expect(tls, 200, NULL, text_stream, F_LENGTH, 150,
               "done fail\tunavailable\ttransport: eof: eof before content-length bytes");
        /* HTTP status with a body echoing the key: withheld */
        expect(tls, 401, "application/json",
               "{\"error\":{\"message\":\"Incorrect API key provided: " KEY "\"}}", F_LENGTH, 0,
               "done fail\tdenied\tprovider answered HTTP 401");
        expect(tls, 429, "application/json", "{}", F_CHUNKED, 0,
               "done fail\tunavailable\tprovider answered HTTP 429");
        expect(tls, 200, "application/json", "{}", F_LENGTH, 0,
               "done fail\tunavailable\tprovider did not answer with text/event-stream");
    }
    /* TLS verification failure surfaces as a transport error without detail leaks */
    {
        harness h;
        h_init(&h);
        h_profile(&h, 1, TS_WRONG);
        h_load(&h, req_simple);
        h_run(&h);
        CHECK(has(&h.log, "done fail\tunavailable\ttransport: tls-verify: "));
        h_done(&h);
    }
}

static void test_redirect_credentials(void)
{
    harness h;
    char resp[512];
    const char *a, *b;
    int n;

    /* cross origin (other port): the second hop has no credential */
    h_init(&h);
    h_profile(&h, 1, TS_EC);
    n = snprintf(resp, sizeof resp,
                 "HTTP/1.1 307 Temporary Redirect\r\nLocation: https://localhost:%u/v2/chat/completions\r\n"
                 "Content-Length: 0\r\n\r\n", tsrv_port(srv, TS_RSA));
    CHECK(tsrv_script(srv, "/v1/chat/completions", resp, (size_t)n, 0, 0, 0) == 0);
    serve("/v2/chat/completions", 200, NULL, text_stream, strlen(text_stream), F_LENGTH, 9, 0);
    h_load(&h, req_simple);
    h_run(&h);
    check_text_ok(&h);
    CHECK(tsrv_wait(srv, 2, 2000) == 2);
    a = tsrv_req(srv, 0, NULL);
    b = tsrv_req(srv, 1, NULL);
    CHECK(a && strstr(a, "L=ec\n") && strstr(a, "Authorization: Bearer " KEY "\r\n"));
    CHECK(b && strstr(b, "L=rsa\n") && strstr(b, "POST /v2/chat/completions ") &&
          strstr(b, "\"model\":\"m-1\""));
    CHECK(b && !strstr(b, "Authorization") && !strstr(b, KEY));
    h_done(&h);

    /* same origin (308): the credential stays */
    h_init(&h);
    h_profile(&h, 1, TS_EC);
    n = snprintf(resp, sizeof resp, "HTTP/1.1 308 Permanent Redirect\r\nLocation: /v3/chat/completions\r\n"
                                    "Content-Length: 0\r\n\r\n");
    CHECK(tsrv_script(srv, "/v1/chat/completions", resp, (size_t)n, 0, 0, 0) == 0);
    serve("/v3/chat/completions", 200, NULL, text_stream, strlen(text_stream), F_CHUNKED, 0, 64);
    h_load(&h, req_simple);
    h_run(&h);
    check_text_ok(&h);
    CHECK(tsrv_wait(srv, 2, 2000) == 2);
    b = tsrv_req(srv, 1, NULL);
    CHECK(b && strstr(b, "POST /v3/chat/completions ") && strstr(b, "Authorization: Bearer " KEY "\r\n"));
    h_done(&h);

    /* a 302 on the POST is refused (no silent method change) */
    h_init(&h);
    h_profile(&h, 1, TS_EC);
    n = snprintf(resp, sizeof resp, "HTTP/1.1 302 Found\r\nLocation: /v3/chat/completions\r\n"
                                    "Content-Length: 0\r\n\r\n");
    CHECK(tsrv_script(srv, "/v1/chat/completions", resp, (size_t)n, 0, 0, 0) == 0);
    h_load(&h, req_simple);
    h_run(&h);
    CHECK(has(&h.log, "done fail\tunavailable\ttransport: redirect: "));
    h_done(&h);
}

/* canlin_llm_auth origins and canlin_http origins must be the same strings, or a key
 * would be sent nowhere (or, worse, somewhere else). */
static void test_origin_equivalence(void)
{
    static const char *const urls[] = {
        "https://api.openai.com/v1", "https://API.OpenAI.com:443/v1/x", "http://localhost:11434/v1",
        "http://127.0.0.1/", "https://[::1]:8443/v1", "https://[2001:DB8::1]/a", "HTTPS://h:1/",
        "https://llm.example/v1?x=1#f", NULL};
    size_t i;
    for (i = 0; urls[i]; i++) {
        char o[CANLIN_LLM_URL_MAX + 16];
        canlin_http_url u;
        CHECK(canlin_llm_url_origin(urls[i], o, sizeof o) > 0);
        CHECK(canlin_http_url_parse(urls[i], &u) == 0);
        if (strcmp(o, u.origin) != 0)
            fprintf(stderr, "origin mismatch for %s: llm '%s' http '%s'\n", urls[i], o, u.origin);
        CHECK(strcmp(o, u.origin) == 0);
    }
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    srv = tsrv_start();
    if (srv == NULL || tsrv_ca_file() == NULL) {
        fprintf(stderr, "cannot start the loopback test server\n");
        return 1;
    }
    test_origin_equivalence();
    test_text();
    test_large_and_tools();
    test_failures();
    test_redirect_credentials();
    tsrv_stop(srv);
    printf("llm over http: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
