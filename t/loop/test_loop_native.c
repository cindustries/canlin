/* Native loop end to end (native loop spec §5 item 3, canlin#61): canlin_http as a loop
 * source against the forked loopback server (t/net/tsrv.c), a streamed openai-chat
 * request through canlin_llm and the loop until `done`, and a cancel mid-stream with no
 * later callback — all with the real poll() wait and the monotonic clock. Plus the shell
 * display adapter (fake clock). No external network. Exits 0 on success. */
#define _POSIX_C_SOURCE 200809L
#include "canlin/http.h"
#include "canlin/llm.h"
#include "canlin/llm_http.h"
#include "canlin/loop.h"
#include "canlin/module.h"
#include "canlin/shell.h"
#include "../net/tsrv.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../llm/openai_fixtures.h"

static int failures, checks;

#define CHECK(c)                                                                   \
    do {                                                                           \
        checks++;                                                                  \
        if (!(c)) {                                                                \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c);  \
            failures++;                                                            \
        }                                                                          \
    } while (0)

#define LOGMAX (1u << 20)
#define GUARD_MS 20000u

static tsrv *srv;

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

/* A source with only a deadline: quits the loop when it passes (the test's guard, and
 * the "stay idle for a while" window after a cancel). */
typedef struct guard {
    canlin_loop *loop;
    canlin_ev_time deadline;
    int fired;
} guard;

static size_t guard_pollfds(void *ctx, struct pollfd *out, size_t max, canlin_ev_time *deadline)
{
    guard *g = ctx;
    (void)out;
    (void)max;
    *deadline = g->deadline;
    return 0;
}

static void guard_step(void *ctx, canlin_ev_time now)
{
    guard *g = ctx;
    if (now >= g->deadline && !g->fired) {
        g->fired = 1;
        canlin_loop_quit(g->loop);
    }
}

static canlin_loop_source guard_source(guard *g)
{
    canlin_loop_source s;
    s.pollfds = guard_pollfds;
    s.step = guard_step;
    s.ctx = g;
    return s;
}

/* ---- canlin_http as a source ---- */

typedef struct hres {
    canlin_loop *loop;
    int status, ended;
    char err[160];
    size_t body_len;
    char body[512];
    unsigned body_calls;
} hres;

static void h_status(void *ud, int st)
{
    ((hres *)ud)->status = st;
}

static void h_header(void *ud, const char *n, const char *v)
{
    (void)ud;
    (void)n;
    (void)v;
}

static int h_body(void *ud, const void *buf, size_t len)
{
    hres *r = ud;
    if (r->body_len + len < sizeof r->body)
        memcpy(r->body + r->body_len, buf, len);
    r->body_len += len;
    r->body_calls++;
    return 0;
}

static void h_end(void *ud)
{
    hres *r = ud;
    r->ended = 1;
    canlin_loop_quit(r->loop);
}

static void h_error(void *ud, const char *msg)
{
    hres *r = ud;
    snprintf(r->err, sizeof r->err, "%s", msg);
    canlin_loop_quit(r->loop);
}

static void noop_deliver(void *ud, canlin_ev *ev, const canlin_ev_delivery *d)
{
    (void)ud;
    (void)ev;
    (void)d;
}

static void fetch(int listener, const char *path, hres *r, canlin_loop_stats *st_out)
{
    canlin_ev_clock c = canlin_loop_clock_monotonic();
    canlin_ev_callbacks cb;
    canlin_http_config hc;
    canlin_http_request rq;
    canlin_http_cb hcb;
    canlin_loop_config lc;
    canlin_loop_source hs, gs;
    canlin_http_stats hst;
    canlin_ev *ev;
    canlin_http *h;
    canlin_loop *l;
    canlin_http_stream *s;
    guard g;
    char url[128];
    const char *msg = NULL;
    int err;

    cb.deliver = noop_deliver;
    ev = canlin_ev_new(&c, &cb, NULL);
    memset(&hc, 0, sizeof hc);
    hc.ca_file = tsrv_ca_file();
    hc.resolve = resolve;
    h = canlin_http_new(&hc);
    memset(&lc, 0, sizeof lc);
    lc.ev = ev;
    l = canlin_loop_new(&lc, &err);
    if (!ev || !h || !l) {
        fprintf(stderr, "fetch setup failed\n");
        exit(1);
    }
    hs = canlin_loop_http_source(h);
    CHECK(canlin_loop_add_source(l, &hs) == CANLIN_LOOP_OK);
    g.loop = l;
    g.deadline = canlin_ev_now(ev) + GUARD_MS;
    g.fired = 0;
    gs = guard_source(&g);
    CHECK(canlin_loop_add_source(l, &gs) == CANLIN_LOOP_OK);

    memset(r, 0, sizeof *r);
    r->loop = l;
    snprintf(url, sizeof url, "%s://localhost:%u%s", listener == TS_PLAIN ? "http" : "https",
             tsrv_port(srv, listener), path);
    memset(&rq, 0, sizeof rq);
    rq.method = "GET";
    rq.url = url;
    hcb.status = h_status;
    hcb.header = h_header;
    hcb.body = h_body;
    hcb.end = h_end;
    hcb.error = h_error;
    s = canlin_http_open(h, &rq, &hcb, r, &msg);
    CHECK(s != NULL);
    CHECK(canlin_loop_run(l) == CANLIN_LOOP_OK);
    CHECK(!g.fired);
    if (r->err[0])
        fprintf(stderr, "fetch %s: %s\n", path, r->err);
    canlin_loop_get_stats(l, st_out);
    canlin_http_close(h, s);
    canlin_http_get_stats(h, &hst);
    CHECK(hst.streams == 0);
    canlin_loop_free(l);
    canlin_http_free(h);
    CHECK(canlin_ev_free(ev) == CANLIN_EV_OK);
}

static void test_http_source(void)
{
    hres r;
    canlin_loop_stats st;

    fetch(TS_PLAIN, "/ok", &r, &st);
    CHECK(r.status == 200 && r.ended && !r.err[0]);
    CHECK(r.body_len == 5 && memcmp(r.body, "hello", 5) == 0);
    CHECK(st.waits >= 1);

    /* one byte per server write, over TLS: many waits, the loop blocks between them */
    fetch(TS_EC, "/sse-bytes", &r, &st);
    CHECK(r.status == 200 && r.ended && !r.err[0]);
    CHECK(r.body_len == strlen(TSRV_SSE_BYTES));
    CHECK(r.body_len < sizeof r.body && memcmp(r.body, TSRV_SSE_BYTES, r.body_len) == 0);
    CHECK(r.body_calls > 1);
    CHECK(st.waits >= 2);

    /* large body: bounded per step, the loop does not wait while TLS holds input */
    fetch(TS_RSA, "/big", &r, &st);
    CHECK(r.status == 200 && r.ended && !r.err[0]);
    CHECK(r.body_len == TSRV_BIG_LEN);
    CHECK(r.body_calls >= TSRV_BIG_LEN / CANLIN_NET_TURN_BYTES);
}

/* ---- LLM through the loop ---- */

typedef struct lharness {
    canlin_ev *ev;
    canlin_http *http;
    canlin_llm *llm;
    canlin_module *m;
    canlin_loop *loop;
    guard g;
    char *log;
    size_t loglen;
    int cancel_mode;       /* post "cancel" at the first text fragment */
    int cancel_posted;
    size_t cancel_at;      /* log offset of the "cancel\t" line */
    canlin_loop_stats at_cancel;
} lharness;

static lharness *LH;

static const canlin_module_event ev_cancel = {"cancel", NULL};

static void llog_fn(void *ud, const char *msg, size_t len)
{
    lharness *h = ud;
    if (len + 2 > LOGMAX - h->loglen)
        return;
    memcpy(h->log + h->loglen, msg, len);
    h->loglen += len;
    h->log[h->loglen++] = '\n';
    h->log[h->loglen] = 0;
    if (strncmp(msg, "done ", 5) == 0)
        canlin_loop_quit(h->loop);
    if (h->cancel_mode && !h->cancel_posted && strncmp(msg, "frag", 4) == 0) {
        h->cancel_posted = 1;
        CHECK(canlin_ev_post(h->ev, canlin_module_gen(h->m), CANLIN_MODULE_SOURCE_EVENT, 0, 0,
                             0, (void *)&ev_cancel, 0, NULL) == CANLIN_EV_OK);
    }
    if (h->cancel_mode && strncmp(msg, "cancel\t", 7) == 0) {
        /* stay in the loop a while longer: nothing may arrive after the cancel */
        h->cancel_at = h->loglen - len - 1;
        h->g.deadline = canlin_ev_now(h->ev) + 400;
        canlin_loop_get_stats(h->loop, &h->at_cancel);
    }
}

static void l_deliver(void *ud, canlin_ev *ev, const canlin_ev_delivery *d)
{
    lharness *h = ud;
    (void)ev;
    if (h->m && canlin_module_gen(h->m) == d->gen)
        canlin_module_deliver(h->m, d);
}

static void lh_init(lharness *h, int cancel_mode)
{
    canlin_ev_clock c = canlin_loop_clock_monotonic();
    canlin_ev_callbacks cb;
    canlin_http_config hc;
    canlin_llm_config lc;
    canlin_loop_config pc;
    canlin_loop_source hs, gs;
    int err;

    memset(h, 0, sizeof *h);
    LH = h;
    h->cancel_mode = cancel_mode;
    h->log = calloc(1, LOGMAX);
    cb.deliver = l_deliver;
    h->ev = canlin_ev_new(&c, &cb, h);
    memset(&hc, 0, sizeof hc);
    hc.ca_file = tsrv_ca_file();
    hc.resolve = resolve;
    h->http = canlin_http_new(&hc);
    memset(&lc, 0, sizeof lc);
    lc.transport = canlin_llm_http_transport(h->http);
    lc.log = llog_fn;
    lc.log_ud = h;
    h->llm = canlin_llm_new(h->ev, &lc);
    memset(&pc, 0, sizeof pc);
    pc.ev = h->ev;
    pc.pump = canlin_loop_llm_pump;
    pc.pump_ctx = h->llm;
    h->loop = canlin_loop_new(&pc, &err);
    if (!h->log || !h->ev || !h->http || !h->llm || !h->loop) {
        fprintf(stderr, "llm harness setup failed\n");
        exit(1);
    }
    hs = canlin_loop_http_source(h->http);
    CHECK(canlin_loop_add_source(h->loop, &hs) == CANLIN_LOOP_OK);
    h->g.loop = h->loop;
    h->g.deadline = canlin_ev_now(h->ev) + GUARD_MS;
    gs = guard_source(&h->g);
    CHECK(canlin_loop_add_source(h->loop, &gs) == CANLIN_LOOP_OK);
    tsrv_clear(srv);
}

static void lh_profile_and_load(lharness *h)
{
    canlin_llm_profile_config pc;
    canlin_module_source s;
    canlin_sandbox_config cfg;
    static char src[8192];
    char base[128];
    int err = 1;

    memset(&pc, 0, sizeof pc);
    snprintf(base, sizeof base, "http://localhost:%u/v1", tsrv_port(srv, TS_PLAIN));
    pc.id = "p";
    pc.model = "m-1";
    pc.preset = "custom";
    pc.dialect = CANLIN_LLM_DIALECT_OPENAI_CHAT;
    pc.auth = CANLIN_LLM_AUTH_NONE;
    pc.local = 1;
    pc.base_url = base;
    CHECK(canlin_llm_profile_create(h->llm, &pc) == CANLIN_LLM_OK);

    snprintf(src, sizeof src, mod_tpl, req_simple);
    memset(&s, 0, sizeof s);
    memset(&cfg, 0, sizeof cfg);
    s.name = "assistant";
    s.version = "1";
    s.main = src;
    s.main_len = strlen(src);
    s.assistant = canlin_llm_install;
    s.assistant_ud = h->llm;
    cfg.log = llog_fn;
    cfg.ud = h;
    h->m = canlin_module_load(h->ev, &s, &cfg, &err);
    CHECK(err == CANLIN_MODULE_OK && h->m != NULL);
}

static void lh_done(lharness *h)
{
    canlin_ev_stats st;
    canlin_http_stats hs;
    canlin_module_free(h->m);
    canlin_llm_free(h->llm);
    canlin_http_get_stats(h->http, &hs);
    CHECK(hs.streams == 0);
    canlin_loop_free(h->loop);
    canlin_http_free(h->http);
    canlin_ev_get_stats(h->ev, -1, &st);
    CHECK(st.items == 0 && st.bytes == 0 && st.ops == 0);
    CHECK(canlin_ev_free(h->ev) == CANLIN_EV_OK);
    free(h->log);
}

static unsigned count(const char *hay, const char *needle)
{
    unsigned n = 0;
    while ((hay = strstr(hay, needle)) != NULL) {
        n++;
        hay += strlen(needle);
    }
    return n;
}

static void test_llm_stream(void)
{
    lharness h;
    canlin_loop_stats st;
    char resp[2048];
    int n;

    lh_init(&h, 0);
    n = snprintf(resp, sizeof resp,
                 "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                 "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(text_stream), text_stream);
    /* 7-byte writes, 1 ms apart: the stream arrives over many waits */
    CHECK(tsrv_script(srv, "/v1/chat/completions", resp, (size_t)n, 7, 1000, 0) == 0);
    lh_profile_and_load(&h);
    CHECK(canlin_loop_run(h.loop) == CANLIN_LOOP_OK);
    CHECK(!h.g.fired);
    CHECK(strstr(h.log, "req ok") != NULL);
    CHECK(strstr(h.log, text_expect) != NULL);
    CHECK(strstr(h.log, "usage\t7\t3") != NULL);
    CHECK(strstr(h.log, "done ok\tend") != NULL);
    CHECK(count(h.log, "done ") == 1);
    CHECK(strstr(h.log, "BAD") == NULL);
    if (strstr(h.log, "done ok\tend") == NULL)
        fprintf(stderr, "llm log:\n%s\n", h.log);
    CHECK(canlin_llm_inflight(h.llm) == 0);
    canlin_loop_get_stats(h.loop, &st);
    CHECK(st.waits >= 2);
    CHECK(tsrv_wait(srv, 1, 2000) == 1);
    lh_done(&h);
}

static void test_llm_cancel(void)
{
    lharness h;
    canlin_loop_stats st;
    canlin_http_stats hs;
    char *body = malloc(64000), *resp = malloc(66000), *p;
    const char *after;
    unsigned i;
    int n;

    p = body;
    for (i = 0; i < 400; i++)
        p += sprintf(p, "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"%04u \"}}]}\n\n", i);
    p += sprintf(p, "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
                    "data: [DONE]\n\n");
    n = sprintf(resp, "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                      "Content-Length: %u\r\n\r\n%s", (unsigned)(p - body), body);

    lh_init(&h, 1);
    /* ~60-byte writes 2 ms apart: well over a second of stream, cancelled early on */
    CHECK(tsrv_script(srv, "/v1/chat/completions", resp, (size_t)n, 60, 2000, 0) == 0);
    lh_profile_and_load(&h);
    CHECK(canlin_loop_run(h.loop) == CANLIN_LOOP_OK);
    CHECK(h.g.fired); /* ended by the idle window after the cancel, not by a callback */
    CHECK(h.cancel_posted);
    CHECK(strstr(h.log, "cancel\tcancelled") != NULL);
    if (strstr(h.log, "cancel\tcancelled") == NULL)
        fprintf(stderr, "cancel log:\n%.2000s\n", h.log);
    after = h.log + h.cancel_at;
    CHECK(strncmp(after, "cancel\t", 7) == 0);
    CHECK(strstr(after, "frag") == NULL);   /* no text after the cancel */
    CHECK(strstr(h.log, "done ") == NULL);  /* cancelled is never delivered */
    CHECK(count(h.log, "frag") < 400);      /* it really was mid-stream */
    CHECK(canlin_llm_inflight(h.llm) == 0);
    canlin_http_get_stats(h.http, &hs);
    CHECK(hs.streams == 0);                 /* the connection is gone */
    /* the 400 ms after the cancel were idle: a few waits, not a spin */
    canlin_loop_get_stats(h.loop, &st);
    CHECK(st.waits - h.at_cancel.waits <= 10);
    lh_done(&h);
    free(body);
    free(resp);
}

/* ---- the shell as the display (fake clock and wait) ---- */

static const char *SHELL_SRC =
    "local M = {}\n"
    "function M.init() end\n"
    "function M.draw(ui) ui:text('hello') end\n"
    "return M\n";

typedef struct shh {
    canlin_ev_fake_clock clock;
    canlin_ev *ev;
    canlin_module *m;
    unsigned presents;
    int last_rows;
} shh;

static void sh_deliver(void *ud, canlin_ev *ev, const canlin_ev_delivery *d)
{
    shh *s = ud;
    (void)ev;
    if (s->m && canlin_module_gen(s->m) == d->gen)
        canlin_module_deliver(s->m, d);
}

static void present(void *ctx, canlin_render *r, int rows)
{
    shh *s = ctx;
    (void)r;
    s->presents++;
    s->last_rows = rows;
}

#define SW 800
#define SH 480

static void test_shell_display(void)
{
    static unsigned char px[SW * SH * 3], dirty[SH];
    shh s;
    canlin_ev_clock c;
    canlin_ev_callbacks cb;
    canlin_theme t;
    canlin_ui *ui;
    canlin_ui_output_desc od;
    canlin_ui_output *out;
    canlin_shell_config sc;
    canlin_shell *sh;
    canlin_render r;
    canlin_module_source src;
    canlin_sandbox_config cfg;
    canlin_loop_shell ls;
    canlin_loop_config lc;
    canlin_loop_fake_wait fw;
    canlin_loop *l;
    canlin_loop_stats st;
    unsigned k, p0;
    int err;

    memset(&s, 0, sizeof s);
    s.clock.now = 1000;
    c = canlin_ev_fake_clock_bind(&s.clock);
    cb.deliver = sh_deliver;
    s.ev = canlin_ev_new(&c, &cb, &s);
    canlin_theme_builtin(&t);
    ui = canlin_ui_new(&t);
    memset(&od, 0, sizeof od);
    od.id = "loop-output";
    od.w = SW;
    od.h = SH;
    od.dpi = 1.0f;
    od.scale = 100;
    out = canlin_ui_output_new(&od, NULL);
    memset(&sc, 0, sizeof sc);
    sc.ui = ui;
    sh = canlin_shell_new(&sc, &err);
    if (!s.ev || !ui || !out || !sh) {
        fprintf(stderr, "shell setup failed\n");
        exit(1);
    }
    CHECK(canlin_shell_add_output(sh, out, CANLIN_SHELL_IN_POINTER) == CANLIN_SHELL_OK);
    memset(&src, 0, sizeof src);
    memset(&cfg, 0, sizeof cfg);
    src.name = "hello";
    src.version = "1";
    src.main = SHELL_SRC;
    src.main_len = strlen(SHELL_SRC);
    s.m = canlin_module_load(s.ev, &src, &cfg, &err);
    CHECK(s.m != NULL);
    CHECK(canlin_shell_add_module(sh, s.m, &src) == CANLIN_SHELL_OK);
    CHECK(canlin_render_init(&r, px, SW, SH, (size_t)SW * 3, dirty, NULL) == 0);

    memset(&ls, 0, sizeof ls);
    ls.sh = sh;
    ls.out[0].o = out;
    ls.out[0].r = &r;
    ls.out[0].present = present;
    ls.out[0].ctx = &s;
    ls.n = 1;
    canlin_loop_fake_wait_init(&fw, &s.clock);
    memset(&lc, 0, sizeof lc);
    lc.ev = s.ev;
    lc.display = canlin_loop_shell_display(&ls);
    lc.wait = canlin_loop_fake_wait_poll;
    lc.wait_ctx = &fw;
    l = canlin_loop_new(&lc, &err);
    CHECK(l != NULL);

    /* settles: first frame drawn whole, then an idle wait without a timeout (no clock) */
    k = 0;
    do
        CHECK(canlin_loop_iterate(l) == CANLIN_LOOP_OK);
    while (++k < 8 && fw.last_timeout != -1);
    CHECK(fw.last_timeout == -1);
    CHECK(s.presents >= 1);
    CHECK(!canlin_shell_dirty(sh, out));
    p0 = s.presents;
    CHECK(canlin_loop_iterate(l) == CANLIN_LOOP_OK);
    CHECK(canlin_loop_iterate(l) == CANLIN_LOOP_OK);
    CHECK(s.presents == p0);               /* nothing dirty: no frame */
    canlin_loop_get_stats(l, &st);
    CHECK(st.frames == s.presents);

    /* the module is invalidated: it draws, the shell is dirty, one more frame */
    CHECK(canlin_ev_invalidate(s.ev, canlin_module_gen(s.m)) == CANLIN_EV_OK);
    for (k = 0; k < 4; k++)
        CHECK(canlin_loop_iterate(l) == CANLIN_LOOP_OK);
    CHECK(s.presents > p0);
    CHECK(fw.last_timeout == -1);

    canlin_loop_free(l);
    canlin_shell_free(sh);
    canlin_module_free(s.m);
    CHECK(canlin_ev_free(s.ev) == CANLIN_EV_OK);
    CHECK(canlin_ui_output_free(out) == CANLIN_UI_OK);
    canlin_ui_free(ui);
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    srv = tsrv_start();
    if (srv == NULL || tsrv_ca_file() == NULL) {
        fprintf(stderr, "cannot start the loopback test server\n");
        return 1;
    }
    test_http_source();
    test_llm_stream();
    test_llm_cancel();
    tsrv_stop(srv);
    test_shell_display();
    printf("test_loop_native: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
