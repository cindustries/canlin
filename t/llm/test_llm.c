/* LLM providers contract tests (LLM providers spec §2–§9, core API §3, §5): the real
 * sandbox, scheduler, module layer, openai-chat and anthropic-messages dialects against
 * the fixture transport, fake clock, no network, no key of any real provider. Exits 0 on
 * success. */
#include "canlin/json.h"
#include "canlin/llm.h"
#include "canlin/module.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

#define KEY "sk-test-9f8e7d6c5b4a39281706"
#define LOGMAX (1u << 20)

/* ---- harness ---- */

typedef struct logbuf {
    char *buf;
    size_t len;
} logbuf;

typedef struct harness {
    canlin_ev_fake_clock clock;
    canlin_ev *ev;
    canlin_llm *llm;
    canlin_llm_fixture *fx;
    canlin_module *m[2];
    logbuf log;    /* module print() and module diagnostics */
    logbuf llmlog; /* llm diagnostics */
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
    int i;
    (void)ev;
    for (i = 0; i < 2; i++)
        if (h->m[i] && canlin_module_gen(h->m[i]) == d->gen) {
            canlin_module_deliver(h->m[i], d);
            return;
        }
}

static void h_init(harness *h, int dev)
{
    canlin_ev_clock c;
    canlin_ev_callbacks cb;
    canlin_llm_config lc;
    memset(h, 0, sizeof *h);
    h->log.buf = calloc(1, LOGMAX);
    h->llmlog.buf = calloc(1, LOGMAX);
    c = canlin_ev_fake_clock_bind(&h->clock);
    cb.deliver = on_deliver;
    h->ev = canlin_ev_new(&c, &cb, h);
    h->fx = canlin_llm_fixture_new();
    memset(&lc, 0, sizeof lc);
    lc.transport = canlin_llm_fixture_transport(h->fx);
    lc.allow_insecure_dev = dev;
    lc.log = log_fn;
    lc.log_ud = &h->llmlog;
    h->llm = canlin_llm_new(h->ev, &lc);
    if (!h->ev || !h->fx || !h->llm || !h->log.buf || !h->llmlog.buf) {
        fprintf(stderr, "harness setup failed\n");
        exit(1);
    }
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

/* The credential must never be visible to Lua or in a log line (spec §5). */
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
    int i;
    check_no_key(h);
    for (i = 0; i < 2; i++)
        canlin_module_free(h->m[i]);
    canlin_llm_free(h->llm);
    canlin_ev_get_stats(h->ev, -1, &st);
    CHECK(st.items == 0 && st.bytes == 0 && st.ops == 0);
    CHECK(canlin_ev_free(h->ev) == CANLIN_EV_OK);
    canlin_llm_fixture_free(h->fx);
    free(h->log.buf);
    free(h->llmlog.buf);
}

/* One profile "p" on a custom https endpoint with key KEY. */
static void h_profile(harness *h)
{
    canlin_llm_profile_config pc;
    memset(&pc, 0, sizeof pc);
    pc.id = "p";
    pc.preset = "openai";
    pc.base_url = "https://llm.example/v1";
    pc.model = "m-1";
    CHECK(canlin_llm_profile_create(h->llm, &pc) == CANLIN_LLM_OK);
    CHECK(canlin_llm_profile_set_key(h->llm, "p", KEY, strlen(KEY)) == CANLIN_LLM_OK);
}

/* Load the assistant module mod_tpl (openai_fixtures.h) with `setup`. */
static int h_load(harness *h, int i, const char *setup, int assistant)
{
    canlin_module_source s;
    canlin_sandbox_config cfg;
    static char src[8192];
    int err = 1;
    snprintf(src, sizeof src, mod_tpl, setup);
    memset(&s, 0, sizeof s);
    memset(&cfg, 0, sizeof cfg);
    s.name = assistant ? "assistant" : "other";
    s.version = "1";
    s.main = src;
    s.main_len = strlen(src);
    if (assistant) {
        s.assistant = canlin_llm_install;
        s.assistant_ud = h->llm;
    }
    cfg.log = log_fn;
    cfg.ud = &h->log;
    h->m[i] = canlin_module_load(h->ev, &s, &cfg, &err);
    CHECK(err == CANLIN_MODULE_OK);
    return err;
}

/* Drive fixture, pump and scheduler until everything is idle. */
static void h_run(harness *h)
{
    int idle = 0, k;
    for (k = 0; k < 200000 && idle < 3; k++) {
        canlin_ev_turn_result tr;
        unsigned made = canlin_llm_fixture_step(h->fx);
        canlin_llm_pump(h->llm);
        CHECK(canlin_ev_turn(h->ev, &tr) == CANLIN_EV_OK);
        idle = (made == 0 && !tr.more && canlin_llm_inflight(h->llm) == 0) ? idle + 1 : 0;
    }
    CHECK(idle >= 3);
}

static void push_sse(harness *h, int status, const char *body, size_t chunk, unsigned seed)
{
    canlin_llm_fixture_response r;
    memset(&r, 0, sizeof r);
    r.status = status;
    r.body = body;
    r.body_len = strlen(body);
    r.chunk = chunk;
    r.seed = seed;
    CHECK(canlin_llm_fixture_push(h->fx, &r) == CANLIN_LLM_OK);
}

/* One complete run: profile, response, assistant module with `setup`. */
static void run_one(harness *h, const char *body, size_t chunk, unsigned seed, const char *setup)
{
    h_init(h, 0);
    h_profile(h);
    push_sse(h, 200, body, chunk, seed);
    h_load(h, 0, setup, 1);
    h_run(h);
}

/* ---- fixtures ---- */

/* ---- tests ---- */

static void check_text_ok(harness *h)
{
    CHECK(has(&h->log, "req ok"));
    CHECK(has(&h->log, text_expect));
    CHECK(has(&h->log, "usage\t7\t3"));
    CHECK(has(&h->log, "done ok\tend\n")); /* no dropped_tools outside length (spec §4) */
    CHECK(count(&h->log, "done ") == 1);
    CHECK(!has(&h->log, "BAD"));
}

static void test_text_every_split(void)
{
    size_t n = strlen(text_stream), k;
    harness h;
    /* one byte at a time */
    run_one(&h, text_stream, 1, 0, req_simple);
    check_text_ok(&h);
    h_done(&h);
    /* two chunks, split at every byte boundary */
    for (k = 1; k < n; k++) {
        canlin_llm_fixture_response r;
        h_init(&h, 0);
        h_profile(&h);
        memset(&r, 0, sizeof r);
        r.status = 200;
        r.body = text_stream;
        r.body_len = n;
        r.chunk = k; /* first chunk k bytes, then k more ... */
        CHECK(canlin_llm_fixture_push(h.fx, &r) == CANLIN_LLM_OK);
        h_load(&h, 0, req_simple, 1);
        h_run(&h);
        check_text_ok(&h);
        h_done(&h);
        if (failures)
            break;
    }
}

static void test_text_random_large(void)
{
    char *body = malloc(400000), *p;
    unsigned seed;
    int i;
    size_t want = 0;
    char line[128];
    p = body;
    for (i = 0; i < 2000; i++) {
        p += sprintf(p, "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"%04d-\xc3\xa4\\u20ac|\"}}]}\n\n", i);
        want += 4 + 1 + 2 + 3 + 1;
    }
    p += sprintf(p, "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"length\"}]}\n\ndata: [DONE]\n\n");
    for (seed = 1; seed <= 8; seed++) {
        harness h;
        run_one(&h, body, 97, seed * 7919u, req_simple);
        snprintf(line, sizeof line, "textlen\t%u", (unsigned)want);
        CHECK(has(&h.log, line));
        CHECK(has(&h.log, "done ok\tlength\t0")); /* dropped_tools only with length */
        CHECK(!has(&h.log, "BAD"));
        h_done(&h);
    }
    free(body);
}

static void test_tool_many_deltas(void)
{
    static const char args[] = "{\"title\":\"Einkaufsliste\",\"widgets\":[{\"type\":\"heading\","
                               "\"text\":\"Was fehlt? \\u00e4\"}]}";
    char *body = malloc(65536), *p, esc[8];
    size_t i;
    harness h;
    p = body + sprintf(body, "%s", tool_stream_head);
    for (i = 0; args[i]; i++) { /* one argument byte per delta, JSON-escaped */
        if (args[i] == '"' || args[i] == '\\')
            snprintf(esc, sizeof esc, "\\%c", args[i]);
        else
            snprintf(esc, sizeof esc, "%c", args[i]);
        p += sprintf(p, "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,"
                        "\"function\":{\"arguments\":\"%s\"}}]}}]}\n\n", esc);
    }
    p += sprintf(p, "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n\n"
                    "data: [DONE]\n\n");
    run_one(&h, body, 13, 5, req_tools);
    CHECK(has(&h.log, "tool\tcall_A\tset_view\t{\"title\":\"Einkaufsliste\",\"widgets\":[{\"type\":\"heading\",\"text\":\"Was fehlt? \\u00e4\"}]}"));
    CHECK(has(&h.log, "done ok\ttool"));
    CHECK(count(&h.log, "tool\t") == 1);
    h_done(&h);
    free(body);
}

static void test_two_parallel_tools(void)
{
    harness h;
    const char *a, *b, *t;
    run_one(&h, two_tools_stream, 7, 3, req_tools);
    CHECK(has(&h.log, "T=Moment."));
    a = strstr(h.log.buf, "tool\ta\tset_view\t{\"widgets\":[]}");
    b = strstr(h.log.buf, "tool\tb\tset_view\t{\"x\":1}");
    t = strstr(h.log.buf, "frag\t7");
    CHECK(a && b && t && t < a && a < b); /* text first, then tools in index order */
    CHECK(has(&h.log, "done ok\ttool\n"));
    h_done(&h);
}

static void expect_fail(const char *body, const char *setup, const char *line)
{
    harness h;
    run_one(&h, body, 5, 11, setup);
    if (!has(&h.log, line))
        fprintf(stderr, "expected '%s' in log:\n%s\n", line, h.log.buf);
    CHECK(has(&h.log, line));
    CHECK(count(&h.log, "done ") == 1);
    h_done(&h);
}

static void test_failures(void)
{
    /* provider error JSON mid-stream: text before it stays delivered */
    expect_fail("data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"part\"}}]}\n\n"
                "data: {\"error\":{\"message\":\"overloaded\",\"type\":\"server_error\"}}\n\n",
                req_simple, "done fail\tunavailable\tprovider error: overloaded");
    expect_fail("data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"part\"}}]}\n\n"
                "data: {\"error\":{\"message\":\"overloaded\"}}\n\n",
                req_simple, "T=part");
    /* EOF after finish_reason but before [DONE] */
    expect_fail("data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"x\"},\"finish_reason\":\"stop\"}]}\n\n",
                req_simple, "done fail\tunavailable\topenai-chat protocol error: stream ended before [DONE]");
    /* EOF with nothing terminal */
    expect_fail("data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"x\"}}]}\n\n",
                req_simple, "done fail\tunavailable\topenai-chat protocol error: stream ended without finish_reason");
    /* incomplete final SSE record */
    expect_fail("data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"x\"},\"finish_reason\":\"stop\"}]}\n\ndata: [DO",
                req_simple, "done fail\tunavailable\topenai-chat protocol error: SSE framing");
    /* missing finish_reason */
    expect_fail("data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"x\"}}]}\n\ndata: [DONE]\n\n",
                req_simple, "done fail\tunavailable\topenai-chat protocol error: [DONE] without finish_reason");
    /* malformed JSON chunk */
    expect_fail("data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"x\"}]}\n\n",
                req_simple, "done fail\tunavailable\topenai-chat protocol error: malformed JSON chunk");
    /* invalid UTF-8 inside content */
    expect_fail("data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"ok \xc3\x28\"}}]}\n\n",
                req_simple, "done fail\tunavailable\topenai-chat protocol error: malformed JSON chunk");
    expect_fail("data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"\\ud800\"}}]}\n\n",
                req_simple, "done fail\tunavailable");
    /* tool arguments that are not one JSON object */
    expect_fail("data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"a\","
                "\"function\":{\"name\":\"set_view\",\"arguments\":\"{\\\"a\\\":\"}}]}}]}\n\n"
                "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n\n"
                "data: [DONE]\n\n",
                req_tools, "done fail\tunavailable\topenai-chat protocol error: tool call arguments");
    /* partial tool call never executes when the stream breaks */
    expect_fail("data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"a\","
                "\"function\":{\"name\":\"set_view\",\"arguments\":\"{}\"}}]}}]}\n\n",
                req_tools, "done fail\tunavailable");
}

static void test_no_partial_tool(void)
{
    harness h;
    run_one(&h, "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"a\","
                "\"function\":{\"name\":\"set_view\",\"arguments\":\"{}\"}}]}}]}\n\n",
            3, 0, req_tools);
    CHECK(!has(&h.log, "tool\t"));
    CHECK(has(&h.log, "done fail\tunavailable"));
    h_done(&h);
}

/* ---- length stop with an unfinished tool call (spec §2.3, decided 2026-09-23) ---- */

static void run_one_am(harness *h, const char *body, size_t chunk, unsigned seed,
                       const char *setup);

/* Run body in chunks of every size 1..n and of random sizes; check(h) each time. */
static void every_split(const char *body, int am, const char *setup, void (*check)(harness *))
{
    size_t n = strlen(body), k;
    harness h;
    for (k = 1; k <= n; k++) {
        if (am)
            run_one_am(&h, body, k, 0, setup);
        else
            run_one(&h, body, k, 0, setup);
        check(&h);
        h_done(&h);
        if (failures)
            return;
    }
    for (k = 1; k <= 16; k++) {
        if (am)
            run_one_am(&h, body, 29, (unsigned)k * 7919u, setup);
        else
            run_one(&h, body, 29, (unsigned)k * 7919u, setup);
        check(&h);
        h_done(&h);
    }
}

/* Text, call a complete (arguments split over deltas), call b cut off mid-arguments. */
static const char oa_len_mixed[] =
    "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"Moment.\"}}]}\n\n"
    "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":["
    "{\"index\":0,\"id\":\"a\",\"function\":{\"name\":\"set_view\",\"arguments\":\"{\\\"wid\"}}]}}]}\n\n"
    "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":["
    "{\"index\":0,\"function\":{\"arguments\":\"gets\\\":[]}\"}}]}}]}\n\n"
    "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":["
    "{\"index\":1,\"id\":\"b\",\"function\":{\"name\":\"set_view\",\"arguments\":\"{\\\"widgets\\\":[{\"}}]}}]}\n\n"
    "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"length\"}]}\n\n"
    "data: [DONE]\n\n";

static void check_len_mixed(harness *h)
{
    const char *t = strstr(h->log.buf, "frag\t7"), *a = strstr(h->log.buf, "tool\ta\tset_view\t{\"widgets\":[]}");
    if (!has(&h->log, "done ok\tlength\t1"))
        fprintf(stderr, "log:\n%s\n", h->log.buf);
    CHECK(has(&h->log, "T=Moment."));
    CHECK(t && a && t < a);                  /* the complete call is delivered as usual */
    CHECK(count(&h->log, "tool\t") == 1);    /* the unfinished one never is */
    CHECK(!has(&h->log, "tool\tb"));
    CHECK(has(&h->log, "done ok\tlength\t1"));
    CHECK(count(&h->log, "done ") == 1);
    CHECK(!has(&h->log, "BAD"));
}

static void check_len_only_unfinished(harness *h)
{
    CHECK(!has(&h->log, "tool\t"));
    CHECK(has(&h->log, "T=abc"));
    CHECK(has(&h->log, "done ok\tlength\t1"));
    CHECK(count(&h->log, "done ") == 1);
}

static void check_len_complete(harness *h)
{
    CHECK(has(&h->log, "tool\ta\tset_view\t{}"));
    CHECK(has(&h->log, "done ok\tlength\t0"));
    CHECK(count(&h->log, "done ") == 1);
}

static void test_length_tools(void)
{
    /* mixed: complete call delivered, unfinished last call dropped and counted */
    every_split(oa_len_mixed, 0, req_tools, check_len_mixed);
    /* only an unfinished call (its arguments never started): dropped, text stays */
    every_split("data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"abc\"}}]}\n\n"
                "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"a\","
                "\"function\":{\"name\":\"set_view\"}}]}}]}\n\n"
                "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"length\"}]}\n\n"
                "data: [DONE]\n\n",
                0, req_tools, check_len_only_unfinished);
    /* a call cut off before its name arrived is unfinished too */
    every_split("data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"abc\"}}]}\n\n"
                "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0}]}}]}\n\n"
                "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"length\"}]}\n\n"
                "data: [DONE]\n\n",
                0, req_tools, check_len_only_unfinished);
    /* all calls complete at the length stop: delivered, nothing dropped */
    every_split("data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"a\","
                "\"function\":{\"name\":\"set_view\",\"arguments\":\"{}\"}}]}}]}\n\n"
                "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"length\"}]}\n\n"
                "data: [DONE]\n\n",
                0, req_tools, check_len_complete);
    /* only the last call may be unfinished: an earlier incomplete one is a failure */
    expect_fail("data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":["
                "{\"index\":0,\"id\":\"a\",\"function\":{\"name\":\"set_view\",\"arguments\":\"{\\\"a\"}},"
                "{\"index\":1,\"id\":\"b\",\"function\":{\"name\":\"set_view\",\"arguments\":\"{}\"}}]}}]}\n\n"
                "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"length\"}]}\n\n"
                "data: [DONE]\n\n",
                req_tools, "done fail\tunavailable\topenai-chat protocol error: tool call arguments are not one complete JSON object");
    /* an unfinished call with any other stop, or at EOF, stays a failure */
    expect_fail("data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"a\","
                "\"function\":{\"name\":\"set_view\",\"arguments\":\"{\\\"a\"}}]}}]}\n\n"
                "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
                "data: [DONE]\n\n",
                req_tools, "done fail\tunavailable\topenai-chat protocol error: tool call deltas without finish_reason tool_calls");
    expect_fail("data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"a\","
                "\"function\":{\"name\":\"set_view\",\"arguments\":\"{\\\"a\"}}]}}]}\n\n"
                "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n\n"
                "data: [DONE]\n\n",
                req_tools, "done fail\tunavailable\topenai-chat protocol error: tool call arguments are not one complete JSON object");
    /* length stop but the stream breaks before [DONE]: a failure, delivered calls included */
    expect_fail("data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"a\","
                "\"function\":{\"name\":\"set_view\",\"arguments\":\"{\\\"a\"}}]}}]}\n\n"
                "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"length\"}]}\n\n",
                req_tools, "done fail\tunavailable\topenai-chat protocol error: stream ended before [DONE]");
}

static void test_http_status(void)
{
    static const struct {
        int status;
        const char *line;
    } cases[] = {{401, "done fail\tdenied\tprovider answered HTTP 401"},
                 {403, "done fail\tdenied"},
                 {404, "done fail\tunsupported\tprovider answered HTTP 404"},
                 {429, "done fail\tunavailable\tprovider answered HTTP 429"},
                 {500, "done fail\tunavailable\tprovider answered HTTP 500"}};
    size_t i;
    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        harness h;
        canlin_llm_fixture_response r;
        h_init(&h, 0);
        h_profile(&h);
        memset(&r, 0, sizeof r);
        r.status = cases[i].status;
        r.content_type = "application/json";
        /* a provider body echoing the key must not reach Lua or logs */
        r.body = "{\"error\":{\"message\":\"Incorrect API key provided: " KEY "\"}}";
        r.body_len = strlen(r.body);
        CHECK(canlin_llm_fixture_push(h.fx, &r) == CANLIN_LLM_OK);
        h_load(&h, 0, req_simple, 1);
        h_run(&h);
        CHECK(has(&h.log, cases[i].line));
        CHECK(!has(&h.log, "frag"));
        h_done(&h);
    }
    /* wrong content type on 200 */
    {
        harness h;
        canlin_llm_fixture_response r;
        h_init(&h, 0);
        h_profile(&h);
        memset(&r, 0, sizeof r);
        r.status = 200;
        r.content_type = "application/json";
        r.body = "{}";
        r.body_len = 2;
        CHECK(canlin_llm_fixture_push(h.fx, &r) == CANLIN_LLM_OK);
        h_load(&h, 0, req_simple, 1);
        h_run(&h);
        CHECK(has(&h.log, "done fail\tunavailable\tprovider did not answer with text/event-stream"));
        h_done(&h);
    }
    /* transport error mid-stream */
    {
        harness h;
        canlin_llm_fixture_response r;
        h_init(&h, 0);
        h_profile(&h);
        memset(&r, 0, sizeof r);
        r.status = 200;
        r.body = text_stream;
        r.body_len = strlen(text_stream);
        r.chunk = 10;
        r.fault = CANLIN_LLM_FX_ERROR_AT;
        r.fault_at = 150;
        CHECK(canlin_llm_fixture_push(h.fx, &r) == CANLIN_LLM_OK);
        h_load(&h, 0, req_simple, 1);
        h_run(&h);
        CHECK(has(&h.log, "done fail\tunavailable\ttransport: fixture: connection reset"));
        h_done(&h);
    }
}

static void test_oversize(void)
{
    char *body = malloc(CANLIN_LLM_TEXT_MAX + 200000), *p;
    size_t total = 0;
    int i;
    harness h;
    char frag[4097];
    /* text: 4 KiB fragments until beyond CANLIN_LLM_TEXT_MAX */
    memset(frag, 'a', 4096);
    frag[4096] = 0;
    p = body;
    while (total <= CANLIN_LLM_TEXT_MAX) {
        p += sprintf(p, "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"%s\"}}]}\n\n", frag);
        total += 4096;
    }
    p += sprintf(p, "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}\n\ndata: [DONE]\n\n");
    run_one(&h, body, 4000, 0, req_simple);
    CHECK(has(&h.log, "done fail\tfull\tresponse text exceeds CANLIN_LLM_TEXT_MAX"));
    CHECK(has(&h.log, "textlen\t262144")); /* everything admitted before the bound */
    h_done(&h);

    /* tool arguments beyond CANLIN_LLM_TOOL_ARGS_MAX */
    p = body + sprintf(body, "%s", tool_stream_head);
    total = 0;
    while (total <= CANLIN_LLM_TOOL_ARGS_MAX) {
        p += sprintf(p, "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,"
                        "\"function\":{\"arguments\":\"%s\"}}]}}]}\n\n", frag);
        total += 4096;
    }
    p += sprintf(p, "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n\ndata: [DONE]\n\n");
    run_one(&h, body, 5000, 0, req_tools);
    CHECK(has(&h.log, "done fail\tfull\ttool arguments exceed CANLIN_LLM_TOOL_ARGS_MAX"));
    CHECK(!has(&h.log, "tool\t"));
    h_done(&h);

    /* tool count beyond CANLIN_LLM_TOOL_CALLS_MAX */
    p = body;
    for (i = 0; i <= (int)CANLIN_LLM_TOOL_CALLS_MAX; i++)
        p += sprintf(p, "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":%d,"
                        "\"id\":\"c%d\",\"function\":{\"name\":\"set_view\",\"arguments\":\"{}\"}}]}}]}\n\n", i, i);
    p += sprintf(p, "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n\ndata: [DONE]\n\n");
    run_one(&h, body, 64, 0, req_tools);
    CHECK(has(&h.log, "done fail\tfull\tmore tool calls than CANLIN_LLM_TOOL_CALLS_MAX"));
    CHECK(!has(&h.log, "tool\t"));
    h_done(&h);

    /* one SSE line beyond CANLIN_LLM_SSE_LINE_MAX */
    p = body + sprintf(body, "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"");
    memset(p, 'b', CANLIN_LLM_SSE_LINE_MAX);
    p += CANLIN_LLM_SSE_LINE_MAX;
    sprintf(p, "\"}}]}\n\n");
    run_one(&h, body, 8192, 0, req_simple);
    CHECK(has(&h.log, "done fail\tfull\tSSE record exceeds a limit"));
    h_done(&h);
    free(body);
}

static int tok_nop(void *ud, const canlin_json_token *t)
{
    (void)ud;
    (void)t;
    return 0;
}

/* The whole body (tool schema included) is one valid JSON document. */
static int json_ok(const char *p, size_t n)
{
    static char buf[CANLIN_LLM_JSON_STRING_MAX];
    canlin_json js;
    canlin_json_limits lim = {CANLIN_LLM_JSON_DEPTH, sizeof buf, 64, CANLIN_LLM_REQUEST_MAX};
    return canlin_json_init(&js, &lim, buf, sizeof buf, tok_nop, NULL) == CANLIN_JSON_OK &&
           canlin_json_feed(&js, p, n) == CANLIN_JSON_OK && canlin_json_finish(&js) == CANLIN_JSON_OK;
}

static void test_golden_request(void)
{
    static const char setup[] =
        "go({messages = {\n"
        "  {role = 'system', text = 'Be brief.'},\n"
        "  {role = 'user', text = 'Hallo \"Welt\"\\n\\t\\1 \xc3\xa4'},\n"
        "  {role = 'assistant', text = 'Sure.', tool_calls = {{id = 'c1', name = 'set_view', args = '{\"widgets\":[]}'}}},\n"
        "  {role = 'tool', tool_call_id = 'c1', text = '{\"revision\":1}'},\n"
        "  {role = 'assistant', tool_calls = {{id = 'c2', name = 'set_view', args = '{}'}}},\n"
        "  {role = 'assistant', text = 'Done.'},\n"
        "}, tools = {'set_view'}, max_tokens = 100})";
    static const char golden_head[] =
        /* the openai preset sets stream_usage (canlin#51) */
        "{\"model\":\"m-1\",\"stream\":true,\"stream_options\":{\"include_usage\":true},"
        "\"max_tokens\":100,\"messages\":["
        "{\"role\":\"system\",\"content\":\"Be brief.\"},"
        "{\"role\":\"user\",\"content\":\"Hallo \\\"Welt\\\"\\n\\t\\u0001 \xc3\xa4\"},"
        "{\"role\":\"assistant\",\"content\":\"Sure.\",\"tool_calls\":[{\"id\":\"c1\",\"type\":\"function\","
        "\"function\":{\"name\":\"set_view\",\"arguments\":\"{\\\"widgets\\\":[]}\"}}]},"
        "{\"role\":\"tool\",\"tool_call_id\":\"c1\",\"content\":\"{\\\"revision\\\":1}\"},"
        "{\"role\":\"assistant\",\"content\":null,\"tool_calls\":[{\"id\":\"c2\",\"type\":\"function\","
        "\"function\":{\"name\":\"set_view\",\"arguments\":\"{}\"}}]},"
        "{\"role\":\"assistant\",\"content\":\"Done.\"}],"
        "\"tools\":[{\"type\":\"function\",\"function\":{\"name\":\"set_view\",\"description\":";
    static const char golden_tail[] = ",\"required\":[\"widgets\"]}}}]}";
    harness h;
    const canlin_llm_fixture_record *rec;
    run_one(&h, text_stream, 0, 0, setup);
    CHECK(has(&h.log, "done ok\tend"));
    CHECK(canlin_llm_fixture_records(h.fx) == 1);
    rec = canlin_llm_fixture_record_at(h.fx, 0);
    CHECK(rec && strcmp(rec->method, "POST") == 0);
    CHECK(rec && strcmp(rec->url, "https://llm.example/v1/chat/completions") == 0);
    CHECK(rec && strstr(rec->headers, "Content-Type: application/json\n"));
    CHECK(rec && strstr(rec->headers, "Accept: text/event-stream\n"));
    CHECK(rec && strstr(rec->headers, "Authorization: Bearer " KEY "\n"));
    if (rec && strncmp(rec->body, golden_head, strlen(golden_head)) != 0)
        fprintf(stderr, "body: %s\n", rec->body);
    CHECK(rec && strncmp(rec->body, golden_head, strlen(golden_head)) == 0);
    CHECK(rec && json_ok(rec->body, rec->body_len));
    CHECK(rec && rec->body_len > strlen(golden_tail) &&
          strcmp(rec->body + rec->body_len - strlen(golden_tail), golden_tail) == 0);
    h_done(&h);

    /* domain errors are nil, code, message; type errors raise */
    run_one(&h, text_stream, 0, 0,
            "go({messages = {{role = 'robot', text = 'x'}}})\n"
            "go({messages = {}})\n"
            "go({messages = {{role = 'user', text = 'x'}}, tools = {'shell'}})\n"
            "go({messages = {{role = 'user', text = 'x'}}, max_tokens = 0})\n"
            "go({messages = {{role = 'user', text = '\\255'}}})\n"
            "go({messages = {{role = 'user', text = 'x'}}}, 'nope')\n"
            "print('type', pcall(go, {messages = 'x'}))\n"
            "print('type2', pcall(go, {messages = {{role = 'user'}}}))\n");
    CHECK(has(&h.log, "req\tinvalid\tunknown message role"));
    CHECK(has(&h.log, "req\tinvalid\tmessage count out of range"));
    CHECK(has(&h.log, "req\tinvalid\tunknown tool"));
    CHECK(has(&h.log, "req\tinvalid\tmax_tokens out of range"));
    CHECK(has(&h.log, "req\tinvalid\tmessage text is not valid UTF-8"));
    CHECK(has(&h.log, "req\tinvalid\tunknown profile"));
    CHECK(has(&h.log, "type\tfalse"));
    CHECK(has(&h.log, "type2\tfalse"));
    CHECK(canlin_llm_fixture_records(h.fx) == 0); /* nothing was sent */
    h_done(&h);
}

static void test_cancel_late(void)
{
    static const canlin_module_event ev_cancel = {"cancel", NULL};
    harness h;
    int k;
    h_init(&h, 0);
    h_profile(&h);
    push_sse(&h, 200, text_stream, 20, 0);
    h_load(&h, 0, req_simple, 1);
    /* status + header + a few chunks: text events end up queued behind "cancel" */
    CHECK(canlin_llm_fixture_step(h.fx) == 1);
    CHECK(canlin_llm_fixture_step(h.fx) == 1);
    CHECK(canlin_ev_post(h.ev, canlin_module_gen(h.m[0]), CANLIN_MODULE_SOURCE_EVENT, 0, 0, 0,
                         (void *)&ev_cancel, 0, NULL) == CANLIN_EV_OK);
    for (k = 0; k < 8; k++) {
        canlin_llm_fixture_step(h.fx);
        canlin_llm_pump(h.llm);
    }
    h_run(&h);
    CHECK(has(&h.log, "cancel\tcancelled"));
    CHECK(!has(&h.log, "frag"));   /* queued text was retracted */
    CHECK(!has(&h.log, "done "));  /* D1: cancelled is never delivered */
    CHECK(canlin_llm_fixture_open_streams(h.fx) == 0);
    CHECK(canlin_llm_inflight(h.llm) == 0);
    h_done(&h);

    /* module stop cancels as well; nothing reaches a later generation */
    h_init(&h, 0);
    h_profile(&h);
    push_sse(&h, 200, text_stream, 20, 0);
    h_load(&h, 0, req_simple, 1);
    canlin_llm_fixture_step(h.fx);
    canlin_llm_fixture_step(h.fx);
    canlin_llm_fixture_step(h.fx);
    canlin_llm_pump(h.llm);
    canlin_module_stop(h.m[0]);
    CHECK(canlin_llm_inflight(h.llm) == 0);
    CHECK(canlin_llm_fixture_open_streams(h.fx) == 0);
    h_run(&h);
    CHECK(!has(&h.log, "frag"));
    h_done(&h);
}

static void test_credentials(void)
{
    harness h;
    canlin_llm_fixture_response r;
    const canlin_llm_fixture_record *a, *b;
    canlin_llm_profile_config pc;
    size_t k;

    /* cross-origin redirect: header dropped */
    h_init(&h, 0);
    h_profile(&h);
    memset(&r, 0, sizeof r);
    r.status = 200;
    r.body = text_stream;
    r.body_len = strlen(text_stream);
    r.redirect_to = "https://evil.example/v1/chat/completions";
    CHECK(canlin_llm_fixture_push(h.fx, &r) == CANLIN_LLM_OK);
    h_load(&h, 0, req_simple, 1);
    h_run(&h);
    CHECK(canlin_llm_fixture_records(h.fx) == 2);
    a = canlin_llm_fixture_record_at(h.fx, 0);
    b = canlin_llm_fixture_record_at(h.fx, 1);
    CHECK(a && strstr(a->headers, "Authorization: Bearer " KEY));
    CHECK(b && strcmp(b->url, "https://evil.example/v1/chat/completions") == 0);
    CHECK(b && !strstr(b->headers, "Authorization"));
    CHECK(b && !strstr(b->headers, KEY));
    h_done(&h);

    /* same host, other port or scheme: other origin; same origin keeps it */
    h_init(&h, 0);
    h_profile(&h);
    r.redirect_to = "https://llm.example:8443/v1/chat/completions";
    CHECK(canlin_llm_fixture_push(h.fx, &r) == CANLIN_LLM_OK);
    r.redirect_to = "https://LLM.example:443/v2/chat/completions";
    CHECK(canlin_llm_fixture_push(h.fx, &r) == CANLIN_LLM_OK);
    h_load(&h, 0, "go({messages = {{role = 'user', text = 'hi'}}}) MULTI = true\n"
                  "go({messages = {{role = 'user', text = 'hi'}}})", 1);
    h_run(&h);
    CHECK(canlin_llm_fixture_records(h.fx) == 4);
    a = b = NULL;
    for (k = 0; k < 4; k++) { /* redirects are recorded in stream order: find by URL */
        const canlin_llm_fixture_record *x = canlin_llm_fixture_record_at(h.fx, k);
        if (strstr(x->url, ":8443/"))
            a = x;
        if (strstr(x->url, "LLM.example:443"))
            b = x;
    }
    CHECK(a && !strstr(a->headers, "Authorization"));
    CHECK(b && strstr(b->headers, "Authorization: Bearer " KEY));
    CHECK(count(&h.log, "done ok\tend") == 2);
    h_done(&h);

    /* key never in Lua: profiles() carries no URL or key; changing the endpoint drops it */
    h_init(&h, 0);
    h_profile(&h);
    h_load(&h, 0, "", 1);
    CHECK(has(&h.log, "prof.1.id\tp"));
    CHECK(has(&h.log, "prof.1.model\tm-1"));
    CHECK(has(&h.log, "prof.1.tools\ttrue"));
    CHECK(!has(&h.log, "llm.example"));
    CHECK(canlin_llm_profile_has_key(h.llm, "p"));
    memset(&pc, 0, sizeof pc);
    pc.id = "p";
    pc.preset = "openai";
    pc.base_url = "https://llm.example/v1/"; /* same origin: key kept */
    pc.model = "m-2";
    CHECK(canlin_llm_profile_update(h.llm, &pc) == CANLIN_LLM_OK);
    CHECK(canlin_llm_profile_has_key(h.llm, "p"));
    pc.base_url = "https://other.example/v1";
    CHECK(canlin_llm_profile_update(h.llm, &pc) == CANLIN_LLM_OK);
    CHECK(!canlin_llm_profile_has_key(h.llm, "p"));
    CHECK(canlin_llm_profile_delete(h.llm, "p") == CANLIN_LLM_OK);
    CHECK(canlin_llm_profile_delete(h.llm, "p") == CANLIN_LLM_ENOENT);
    CHECK(canlin_llm_profile_set_key(h.llm, "p", KEY, strlen(KEY)) == CANLIN_LLM_ENOENT);
    h_done(&h);

    /* required key missing -> denied, nothing sent; header-unsafe keys refused */
    h_init(&h, 0);
    memset(&pc, 0, sizeof pc);
    pc.id = "nokey";
    pc.preset = "groq";
    pc.model = "llama";
    CHECK(canlin_llm_profile_create(h.llm, &pc) == CANLIN_LLM_OK);
    CHECK(canlin_llm_profile_set_key(h.llm, "nokey", "a\r\nX: y", 8) == CANLIN_LLM_EINVAL);
    h_load(&h, 0, "go({messages = {{role = 'user', text = 'hi'}}}, 'nokey')", 1);
    CHECK(has(&h.log, "req\tdenied\tthe profile has no credential"));
    CHECK(canlin_llm_fixture_records(h.fx) == 0);
    h_done(&h);

    /* a provider error message quoting key material is withheld */
    h_init(&h, 0);
    h_profile(&h);
    push_sse(&h, 200, "data: {\"error\":{\"message\":\"bad key " KEY "\"}}\n\n", 0, 0);
    h_load(&h, 0, req_simple, 1);
    h_run(&h);
    CHECK(has(&h.log, "done fail\tunavailable\t[message withheld"));
    h_done(&h);
}

static void test_https_rule(void)
{
    harness h;
    canlin_llm_profile_config pc;
    h_init(&h, 0);
    memset(&pc, 0, sizeof pc);
    pc.id = "a";
    pc.preset = "openai";
    pc.base_url = "http://llm.example/v1";
    pc.model = "m";
    CHECK(canlin_llm_profile_create(h.llm, &pc) == CANLIN_LLM_EINSECURE);
    pc.preset = "ollama"; /* local preset, but not loopback */
    pc.base_url = "http://192.168.1.5:11434/v1";
    CHECK(canlin_llm_profile_create(h.llm, &pc) == CANLIN_LLM_EINSECURE);
    pc.base_url = NULL; /* localhost default */
    pc.model = NULL;
    CHECK(canlin_llm_profile_create(h.llm, &pc) == CANLIN_LLM_OK);
    pc.id = "b";
    pc.preset = "custom";
    pc.dialect = CANLIN_LLM_DIALECT_OPENAI_CHAT;
    pc.auth = CANLIN_LLM_AUTH_NONE;
    pc.base_url = "http://127.0.0.1:9000/v1";
    pc.model = "x";
    CHECK(canlin_llm_profile_create(h.llm, &pc) == CANLIN_LLM_EINSECURE); /* not local */
    pc.local = 1;
    CHECK(canlin_llm_profile_create(h.llm, &pc) == CANLIN_LLM_OK);
    CHECK(canlin_llm_profile_set_key(h.llm, "b", "k", 1) == CANLIN_LLM_EUNSUPPORTED);
    pc.id = "c";
    pc.base_url = "http://user@127.0.0.1/v1";
    CHECK(canlin_llm_profile_create(h.llm, &pc) == CANLIN_LLM_EINVAL);
    pc.id = "d";
    pc.preset = "anthropic"; /* anthropic-messages is implemented (canlin#39) */
    pc.base_url = NULL;
    pc.model = NULL;
    CHECK(canlin_llm_profile_create(h.llm, &pc) == CANLIN_LLM_OK);
    CHECK(canlin_llm_preset_find("anthropic") != NULL);
    CHECK(canlin_llm_dialect_available(canlin_llm_preset_find("anthropic")->dialect));
    CHECK(canlin_llm_dialect_available(canlin_llm_preset_find("minimax")->dialect));
    CHECK(canlin_llm_dialect_available(canlin_llm_preset_find("moonshot")->dialect));
    CHECK(canlin_llm_dialect_available(canlin_llm_preset_find("groq")->dialect));
    pc.id = "e";
    pc.preset = "groq"; /* no default model: the user must choose */
    CHECK(canlin_llm_profile_create(h.llm, &pc) == CANLIN_LLM_EINVAL);
    pc.id = "Bad Id";
    pc.model = "m";
    CHECK(canlin_llm_profile_create(h.llm, &pc) == CANLIN_LLM_EINVAL);
    h_done(&h);

    /* explicit local-development setting allows plain http anywhere */
    h_init(&h, 1);
    memset(&pc, 0, sizeof pc);
    pc.id = "dev";
    pc.preset = "openai";
    pc.base_url = "http://llm.example/v1";
    pc.model = "m";
    CHECK(canlin_llm_profile_create(h.llm, &pc) == CANLIN_LLM_OK);
    h_done(&h);
}

static void test_not_assistant_and_inflight(void)
{
    harness h;
    h_init(&h, 0);
    h_profile(&h);
    h_load(&h, 1, "", 0);
    CHECK(has(&h.log, "llm\tnil"));
    canlin_module_free(h.m[1]);
    h.m[1] = NULL;
    push_sse(&h, 200, text_stream, 0, 0);
    push_sse(&h, 200, text_stream, 0, 0);
    push_sse(&h, 200, text_stream, 0, 0);
    h_load(&h, 0,
           "MULTI = true\n"
           "go({messages = {{role = 'user', text = 'a'}}})\n"
           "go({messages = {{role = 'user', text = 'b'}}})\n"
           "go({messages = {{role = 'user', text = 'c'}}})\n", 1);
    CHECK(has(&h.log, "llm\ttable"));
    CHECK(count(&h.log, "req ok") == 2);
    CHECK(has(&h.log, "req\tfull\tCANLIN_LLM_INFLIGHT_MAX requests in flight"));
    CHECK(canlin_llm_inflight(h.llm) == 2);
    h_run(&h);
    CHECK(count(&h.log, "done ok\tend") == 2);
    h_done(&h);
}

static void test_idle_timeout(void)
{
    harness h;
    canlin_llm_fixture_response r;
    int k;
    h_init(&h, 0);
    h_profile(&h);
    memset(&r, 0, sizeof r);
    r.status = 200;
    r.body = text_stream;
    r.body_len = strlen(text_stream);
    r.chunk = 30;
    CHECK(canlin_llm_fixture_push(h.fx, &r) == CANLIN_LLM_OK);
    h_load(&h, 0, req_simple, 1);
    for (k = 0; k < 3; k++)
        canlin_llm_fixture_step(h.fx); /* then the provider goes silent */
    canlin_llm_pump(h.llm);
    canlin_ev_fake_clock_advance(&h.clock, CANLIN_LLM_IDLE_TIMEOUT_MS - 1);
    canlin_llm_pump(h.llm);
    CHECK(canlin_llm_inflight(h.llm) == 1);
    canlin_ev_fake_clock_advance(&h.clock, 1);
    canlin_llm_pump(h.llm);
    CHECK(canlin_llm_inflight(h.llm) == 0);
    h_run(&h);
    CHECK(has(&h.log, "done fail\tunavailable\tidle timeout"));
    h_done(&h);
}

/* stream_options.include_usage is a preset flag (canlin#51): sent for a preset that sets
 * it (openai: golden in test_golden_request), absent for one that does not and for a
 * custom profile. */
static void test_stream_usage_flag(void)
{
    static const char plain_head[] =
        "{\"model\":\"llama\",\"stream\":true,\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}";
    static const char *const ids[] = {"g", "c"};
    harness h;
    canlin_llm_profile_config pc;
    size_t k;

    CHECK(canlin_llm_preset_find("openai")->stream_usage == 1);
    CHECK(canlin_llm_preset_find("groq")->stream_usage == 0);
    for (k = 0; k < 2; k++) {
        char setup[128];
        const canlin_llm_fixture_record *rec;
        h_init(&h, 0);
        memset(&pc, 0, sizeof pc);
        pc.id = ids[k];
        pc.model = "llama";
        if (k == 0) {
            pc.preset = "groq";
        } else {
            pc.preset = "custom";
            pc.dialect = CANLIN_LLM_DIALECT_OPENAI_CHAT;
            pc.auth = CANLIN_LLM_AUTH_BEARER;
            pc.base_url = "https://gw.example/v1";
        }
        CHECK(canlin_llm_profile_create(h.llm, &pc) == CANLIN_LLM_OK);
        CHECK(canlin_llm_profile_set_key(h.llm, ids[k], KEY, strlen(KEY)) == CANLIN_LLM_OK);
        push_sse(&h, 200, text_stream, 0, 0);
        snprintf(setup, sizeof setup, "go({messages = {{role = 'user', text = 'hi'}}}, '%s')",
                 ids[k]);
        h_load(&h, 0, setup, 1);
        h_run(&h);
        CHECK(has(&h.log, "done ok\tend"));
        rec = canlin_llm_fixture_record_at(h.fx, 0);
        if (rec && strcmp(rec->body, plain_head) != 0)
            fprintf(stderr, "body: %s\n", rec->body);
        CHECK(rec && strcmp(rec->body, plain_head) == 0);
        CHECK(rec && !strstr(rec->body, "stream_options"));
        h_done(&h);
    }
}

/* With include_usage the provider sends "usage": null in every chunk and one chunk with
 * "choices": [] and the usage object after finish_reason, before [DONE]. */
static const char usage_stream[] =
    "data: {\"id\":\"c1\",\"object\":\"chat.completion.chunk\",\"choices\":[{\"index\":0,"
    "\"delta\":{\"role\":\"assistant\",\"content\":\"Hal\"},\"finish_reason\":null}],\"usage\":null}\n\n"
    "data: {\"id\":\"c1\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\"lo\"},"
    "\"finish_reason\":null}],\"usage\":null}\n\n"
    "data: {\"id\":\"c1\",\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}],"
    "\"usage\":null}\n\n"
    "data: {\"id\":\"c1\",\"object\":\"chat.completion.chunk\",\"choices\":[],\"usage\":{"
    "\"prompt_tokens\":12,\"completion_tokens\":2,\"total_tokens\":14,"
    "\"prompt_tokens_details\":{\"cached_tokens\":0,\"audio_tokens\":0},"
    "\"completion_tokens_details\":{\"reasoning_tokens\":0}}}\n\n";

static void test_usage_chunk(void)
{
    static char body[2048];
    unsigned seed;
    harness h;
    snprintf(body, sizeof body, "%sdata: [DONE]\n\n", usage_stream);
    run_one(&h, body, 1, 0, req_simple); /* one byte at a time */
    CHECK(has(&h.log, "T=Hallo"));
    CHECK(has(&h.log, "usage\t12\t2"));
    CHECK(count(&h.log, "usage\t") == 1);
    CHECK(has(&h.log, "done ok\tend"));
    CHECK(count(&h.log, "done ") == 1);
    CHECK(!has(&h.log, "BAD"));
    h_done(&h);
    for (seed = 1; seed <= 16; seed++) {
        run_one(&h, body, 23, seed * 104729u, req_simple);
        CHECK(has(&h.log, "T=Hallo") && has(&h.log, "usage\t12\t2") &&
              has(&h.log, "done ok\tend"));
        h_done(&h);
    }
    /* the usage chunk is not the terminal marker: EOF after it still fails */
    expect_fail(usage_stream, req_simple,
                "done fail\tunavailable\topenai-chat protocol error: stream ended before [DONE]");
    /* content after finish_reason is still refused, usage chunk or not */
    expect_fail("data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
                "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"x\"}}],"
                "\"usage\":{\"prompt_tokens\":1,\"completion_tokens\":1}}\n\n"
                "data: [DONE]\n\n",
                req_simple, "done fail\tunavailable\topenai-chat protocol error: content after finish_reason");
    /* tool calls, then the usage chunk, then [DONE] */
    snprintf(body, sizeof body,
             "%s"
             "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,"
             "\"function\":{\"arguments\":\"{}\"}}]}}],\"usage\":null}\n\n"
             "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"tool_calls\"}],"
             "\"usage\":null}\n\n"
             "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":5,\"completion_tokens\":9}}\n\n"
             "data: [DONE]\n\n",
             tool_stream_head);
    run_one(&h, body, 11, 7, req_tools);
    CHECK(has(&h.log, "tool\tcall_A\tset_view\t{}"));
    CHECK(has(&h.log, "usage\t5\t9"));
    CHECK(has(&h.log, "done ok\ttool"));
    CHECK(strstr(h.log.buf, "tool\tcall_A") < strstr(h.log.buf, "usage\t5\t9"));
    h_done(&h);
}

/* MiniMax and Moonshot OpenAI-compatible presets (canlin#51, Langertha MiniMax.pm and
 * Moonshot.pm): usable now, request to {base}/chat/completions with a bearer key. */
static void test_openai_compatible_presets(void)
{
    static const struct {
        const char *id, *url, *model;
    } cases[] = {{"minimax-openai", "https://api.minimax.io/v1/chat/completions", "MiniMax-M3"},
                 {"moonshot-openai", "https://api.moonshot.ai/v1/chat/completions", "kimi-k3"}};
    size_t i;
    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        const canlin_llm_preset *pre = canlin_llm_preset_find(cases[i].id);
        canlin_llm_profile_config pc;
        const canlin_llm_fixture_record *rec;
        char model[64];
        harness h;
        CHECK(pre != NULL);
        if (pre == NULL)
            continue;
        CHECK(pre->dialect == CANLIN_LLM_DIALECT_OPENAI_CHAT);
        CHECK(canlin_llm_dialect_available(pre->dialect));
        CHECK(pre->auth == CANLIN_LLM_AUTH_BEARER && pre->key == CANLIN_LLM_KEY_REQUIRED);
        CHECK(pre->tools && pre->streaming && !pre->local && !pre->stream_usage);
        h_init(&h, 0);
        memset(&pc, 0, sizeof pc);
        pc.id = "p";
        pc.preset = cases[i].id;
        CHECK(canlin_llm_profile_create(h.llm, &pc) == CANLIN_LLM_OK);
        CHECK(canlin_llm_profile_set_key(h.llm, "p", KEY, strlen(KEY)) == CANLIN_LLM_OK);
        push_sse(&h, 200, text_stream, 0, 0);
        h_load(&h, 0, req_tools, 1);
        h_run(&h);
        CHECK(has(&h.log, "done ok\tend"));
        rec = canlin_llm_fixture_record_at(h.fx, 0);
        CHECK(rec && strcmp(rec->url, cases[i].url) == 0);
        CHECK(rec && strstr(rec->headers, "Authorization: Bearer " KEY "\n"));
        snprintf(model, sizeof model, "{\"model\":\"%s\",\"stream\":true,\"messages\"",
                 cases[i].model);
        CHECK(rec && strncmp(rec->body, model, strlen(model)) == 0);
        h_done(&h);
    }
    /* the Anthropic-compatible entries are available since canlin#39 */
    CHECK(canlin_llm_dialect_available(canlin_llm_preset_find("minimax")->dialect));
    CHECK(canlin_llm_dialect_available(canlin_llm_preset_find("moonshot")->dialect));
}

static void test_origin(void)
{
    char o[128];
    CHECK(canlin_llm_url_origin("https://API.Example.com/v1", o, sizeof o) > 0 &&
          strcmp(o, "https://api.example.com:443") == 0);
    CHECK(canlin_llm_url_origin("http://localhost:11434/v1", o, sizeof o) > 0 &&
          strcmp(o, "http://localhost:11434") == 0);
    CHECK(canlin_llm_url_origin("http://[::1]/x", o, sizeof o) > 0 &&
          strcmp(o, "http://[::1]:80") == 0);
    CHECK(canlin_llm_url_origin("https://a@b/", o, sizeof o) < 0);
    CHECK(canlin_llm_url_origin("ftp://a/", o, sizeof o) < 0);
    CHECK(canlin_llm_url_origin("https://a:0/", o, sizeof o) < 0);
    CHECK(canlin_llm_url_origin("https://a:99999/", o, sizeof o) < 0);
    CHECK(canlin_llm_url_origin("https:///x", o, sizeof o) < 0);
}

/* ---- anthropic-messages (canlin#39; spec §2.1, §2.3, §9) ---- */

/* One SSE event as the Messages API frames it: event name line, then one data line. */
#define AEV(name, json) "event: " name "\ndata: " json "\n\n"
#define AM_START AEV("message_start", "{\"type\":\"message_start\",\"message\":{\"id\":\"msg_1\"," \
    "\"type\":\"message\",\"role\":\"assistant\",\"content\":[],\"model\":\"m-1\",\"stop_reason\":null," \
    "\"stop_sequence\":null,\"usage\":{\"input_tokens\":7,\"output_tokens\":1}}}")
#define AM_TEXT_START(i) AEV("content_block_start", "{\"type\":\"content_block_start\",\"index\":" #i \
    ",\"content_block\":{\"type\":\"text\",\"text\":\"\"}}")
#define AM_TEXT(i, t) AEV("content_block_delta", "{\"type\":\"content_block_delta\",\"index\":" #i \
    ",\"delta\":{\"type\":\"text_delta\",\"text\":\"" t "\"}}")
#define AM_TOOL_START(i, id) AEV("content_block_start", "{\"type\":\"content_block_start\",\"index\":" #i \
    ",\"content_block\":{\"type\":\"tool_use\",\"id\":\"" id "\",\"name\":\"set_view\",\"input\":{}}}")
#define AM_JSON(i, j) AEV("content_block_delta", "{\"type\":\"content_block_delta\",\"index\":" #i \
    ",\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"" j "\"}}")
#define AM_STOP_BLOCK(i) AEV("content_block_stop", "{\"type\":\"content_block_stop\",\"index\":" #i "}")
#define AM_DELTA(reason) AEV("message_delta", "{\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"" \
    reason "\",\"stop_sequence\":null},\"usage\":{\"output_tokens\":3}}")
#define AM_MSTOP AEV("message_stop", "{\"type\":\"message_stop\"}")

/* One profile "p" on the anthropic preset at a test origin, key KEY. */
static void h_profile_am(harness *h)
{
    canlin_llm_profile_config pc;
    memset(&pc, 0, sizeof pc);
    pc.id = "p";
    pc.preset = "anthropic";
    pc.base_url = "https://llm.example";
    pc.model = "m-1";
    CHECK(canlin_llm_profile_create(h->llm, &pc) == CANLIN_LLM_OK);
    CHECK(canlin_llm_profile_set_key(h->llm, "p", KEY, strlen(KEY)) == CANLIN_LLM_OK);
}

static void run_one_am(harness *h, const char *body, size_t chunk, unsigned seed,
                       const char *setup)
{
    h_init(h, 0);
    h_profile_am(h);
    push_sse(h, 200, body, chunk, seed);
    h_load(h, 0, setup, 1);
    h_run(h);
}

static void expect_fail_am(const char *body, const char *setup, const char *line)
{
    harness h;
    run_one_am(&h, body, 5, 11, setup);
    if (!has(&h.log, line))
        fprintf(stderr, "expected '%s' in log:\n%s\n", line, h.log.buf);
    CHECK(has(&h.log, line));
    CHECK(count(&h.log, "done ") == 1);
    h_done(&h);
}

/* Text stream with ping, CRLF and CR line ends, a comment, a key order with "type" last
 * and escapes/multi-byte text. */
static const char am_text_stream[] =
    ": comment\r\n"
    AM_START
    AM_TEXT_START(0)
    AEV("ping", "{\"type\": \"ping\"}")
    "event: content_block_delta\r\ndata: {\"index\":0,\"delta\":{\"text\":\"Gr\\u00fc\xc3\x9f"
    "e \",\"type\":\"text_delta\"},\"type\":\"content_block_delta\"}\r\n\r\n"
    "event: content_block_delta\rdata: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":"
    "{\"type\":\"text_delta\",\"text\":\"\xf0\x9f\x91\x8b\\n\\\"ok\\\"\"}}\r\r"
    AM_STOP_BLOCK(0)
    AM_DELTA("end_turn")
    AM_MSTOP;

static void check_am_text_ok(harness *h)
{
    CHECK(has(&h->log, "req ok"));
    CHECK(has(&h->log, text_expect));
    CHECK(has(&h->log, "usage\t7\t3"));
    CHECK(has(&h->log, "done ok\tend"));
    CHECK(count(&h->log, "done ") == 1);
    CHECK(count(&h->log, "usage\t") == 1);
    CHECK(!has(&h->log, "BAD"));
}

static void test_am_text_every_split(void)
{
    size_t n = strlen(am_text_stream), k;
    harness h;
    run_one_am(&h, am_text_stream, 1, 0, req_simple);
    check_am_text_ok(&h);
    h_done(&h);
    for (k = 2; k < n; k++) {
        run_one_am(&h, am_text_stream, k, 0, req_simple);
        check_am_text_ok(&h);
        h_done(&h);
        if (failures)
            break;
    }
    for (k = 1; k <= 16; k++) { /* random chunk sizes */
        run_one_am(&h, am_text_stream, 23, (unsigned)k * 104729u, req_simple);
        check_am_text_ok(&h);
        h_done(&h);
    }
}

static void test_am_tool_many_deltas(void)
{
    static const char args[] = "{\"title\":\"Einkaufsliste\",\"widgets\":[{\"type\":\"heading\","
                               "\"text\":\"Was fehlt? \\u00e4\"}]}";
    char *body = malloc(65536), *p, esc[8];
    size_t i;
    unsigned seed;
    p = body + sprintf(body, "%s", AM_START AM_TOOL_START(0, "toolu_A"));
    for (i = 0; args[i]; i++) { /* one argument byte per input_json_delta, JSON-escaped */
        if (args[i] == '"' || args[i] == '\\')
            snprintf(esc, sizeof esc, "\\%c", args[i]);
        else
            snprintf(esc, sizeof esc, "%c", args[i]);
        p += sprintf(p, AM_JSON(0, "%s"), esc);
    }
    sprintf(p, "%s", AM_STOP_BLOCK(0) AM_DELTA("tool_use") AM_MSTOP);
    for (seed = 0; seed <= 4; seed++) {
        harness h;
        run_one_am(&h, body, seed ? 31 : 13, seed * 7919u, req_tools);
        CHECK(has(&h.log, "tool\ttoolu_A\tset_view\t{\"title\":\"Einkaufsliste\",\"widgets\":[{\"type\":\"heading\",\"text\":\"Was fehlt? \\u00e4\"}]}"));
        CHECK(has(&h.log, "done ok\ttool"));
        CHECK(count(&h.log, "tool\t") == 1);
        h_done(&h);
    }
    free(body);
}

static void test_am_parallel_tools(void)
{
    static const char body[] =
        AM_START
        AM_TEXT_START(0) AM_TEXT(0, "Moment.") AM_STOP_BLOCK(0)
        AM_TOOL_START(1, "a") AM_JSON(1, "{\\\"wid") AM_JSON(1, "gets\\\":[]}") AM_STOP_BLOCK(1)
        AM_TOOL_START(2, "b") AM_JSON(2, "{\\\"x\\\"") AM_JSON(2, ":1}") AM_STOP_BLOCK(2)
        /* a tool without input streams no fragment: its input is {} */
        AM_TOOL_START(3, "c") AM_STOP_BLOCK(3)
        AM_DELTA("tool_use") AM_MSTOP;
    harness h;
    const char *a, *b, *c, *t;
    run_one_am(&h, body, 7, 3, req_tools);
    CHECK(has(&h.log, "T=Moment."));
    t = strstr(h.log.buf, "frag\t7");
    a = strstr(h.log.buf, "tool\ta\tset_view\t{\"widgets\":[]}");
    b = strstr(h.log.buf, "tool\tb\tset_view\t{\"x\":1}");
    c = strstr(h.log.buf, "tool\tc\tset_view\t{}");
    CHECK(a && b && c && t && t < a && a < b && b < c); /* stream order */
    CHECK(has(&h.log, "done ok\ttool\n"));
    h_done(&h);
}

static void test_am_length(void)
{
    harness h;
    run_one_am(&h, AM_START AM_TEXT_START(0) AM_TEXT(0, "abc") AM_STOP_BLOCK(0)
                   AM_DELTA("max_tokens") AM_MSTOP,
               4, 0, req_simple);
    CHECK(has(&h.log, "T=abc"));
    CHECK(has(&h.log, "done ok\tlength\t0"));
    h_done(&h);
    /* stop_sequence ends the turn normally; a later usage-only message_delta is fine */
    run_one_am(&h, AM_START AM_TEXT_START(0) AM_TEXT(0, "abc") AM_STOP_BLOCK(0)
                   AM_DELTA("stop_sequence")
                   AEV("message_delta", "{\"type\":\"message_delta\",\"delta\":{\"stop_reason\":null},"
                                        "\"usage\":{\"output_tokens\":4}}") AM_MSTOP,
               4, 0, req_simple);
    CHECK(has(&h.log, "done ok\tend"));
    CHECK(has(&h.log, "usage\t7\t4"));
    h_done(&h);
}

/* Length stop with an unfinished tool call (spec §2.3, decided 2026-09-23). */
static const char am_len_mixed[] =
    AM_START
    AM_TEXT_START(0) AM_TEXT(0, "Moment.") AM_STOP_BLOCK(0)
    AM_TOOL_START(1, "a") AM_JSON(1, "{\\\"wid") AM_JSON(1, "gets\\\":[]}") AM_STOP_BLOCK(1)
    AM_TOOL_START(2, "b") AM_JSON(2, "{\\\"widgets\\\":[{") AM_STOP_BLOCK(2)
    AM_DELTA("max_tokens") AM_MSTOP;

static void test_am_length_tools(void)
{
    /* mixed: complete call delivered at its block stop, the unfinished one dropped */
    every_split(am_len_mixed, 1, req_tools, check_len_mixed);
    /* an unfinished tool block still open at the max_tokens message_delta */
    every_split(AM_START AM_TEXT_START(0) AM_TEXT(0, "abc") AM_STOP_BLOCK(0)
                AM_TOOL_START(1, "a") AM_JSON(1, "{\\\"wid")
                AM_DELTA("max_tokens") AM_MSTOP,
                1, req_tools, check_len_only_unfinished);
    /* unfinished with no input at all, then a ping and a usage-only message_delta */
    every_split(AM_START AM_TEXT_START(0) AM_TEXT(0, "abc") AM_STOP_BLOCK(0)
                AEV("content_block_start", "{\"type\":\"content_block_start\",\"index\":1,"
                    "\"content_block\":{\"type\":\"tool_use\",\"id\":\"a\",\"name\":\"set_view\","
                    "\"input\":{\"x\":1}}}")
                AM_STOP_BLOCK(1) AEV("ping", "{\"type\":\"ping\"}")
                AEV("message_delta", "{\"type\":\"message_delta\",\"delta\":{\"stop_reason\":null},"
                                     "\"usage\":{\"output_tokens\":4}}")
                AM_DELTA("max_tokens") AM_MSTOP,
                1, req_tools, check_len_only_unfinished);
    /* all calls complete at max_tokens: delivered, nothing dropped (was a failure before
     * canlin#59: "tool_use block without stop_reason tool_use") */
    every_split(AM_START AM_TOOL_START(0, "a") AM_STOP_BLOCK(0) AM_DELTA("max_tokens") AM_MSTOP,
                1, req_tools, check_len_complete);
    /* an unfinished call followed by anything but the max_tokens stop is a failure */
    expect_fail_am(AM_START AM_TOOL_START(0, "a") AM_JSON(0, "{\\\"a") AM_STOP_BLOCK(0)
                   AM_DELTA("end_turn") AM_MSTOP,
                   req_tools, "anthropic-messages protocol error: tool call arguments are not one complete JSON object");
    expect_fail_am(AM_START AM_TOOL_START(0, "a") AM_JSON(0, "{\\\"a") AM_STOP_BLOCK(0)
                   AM_TEXT_START(1) AM_TEXT(1, "x") AM_STOP_BLOCK(1) AM_DELTA("max_tokens") AM_MSTOP,
                   req_tools, "anthropic-messages protocol error: tool call arguments are not one complete JSON object");
    expect_fail_am(AM_START AM_TOOL_START(0, "a") AM_JSON(0, "{\\\"a") AM_STOP_BLOCK(0),
                   req_tools, "anthropic-messages protocol error: tool call arguments are not one complete JSON object");
    expect_fail_am(AM_START AM_TOOL_START(0, "a") AM_JSON(0, "{\\\"a") AM_STOP_BLOCK(0)
                   AM_DELTA("refusal") AM_MSTOP,
                   req_tools, "done fail\tunavailable\tprovider stopped: refusal");
    /* max_tokens but no message_stop: still a failure */
    expect_fail_am(AM_START AM_TOOL_START(0, "a") AM_JSON(0, "{\\\"a") AM_STOP_BLOCK(0)
                   AM_DELTA("max_tokens"),
                   req_tools, "anthropic-messages protocol error: stream ended before message_stop");
    /* an open tool block at another stop, or an open text block at max_tokens, fails */
    expect_fail_am(AM_START AM_TOOL_START(0, "a") AM_JSON(0, "{\\\"a") AM_DELTA("end_turn") AM_MSTOP,
                   req_tools, "anthropic-messages protocol error: message_delta while a content block is open");
    expect_fail_am(AM_START AM_TEXT_START(0) AM_TEXT(0, "x") AM_DELTA("max_tokens") AM_MSTOP,
                   req_tools, "anthropic-messages protocol error: message_delta while a content block is open");
}

static void test_am_failures(void)
{
    /* error event mid-stream: text before it stays delivered, the code follows the type */
    expect_fail_am(AM_START AM_TEXT_START(0) AM_TEXT(0, "part")
                   AEV("error", "{\"type\":\"error\",\"error\":{\"type\":\"overloaded_error\","
                                "\"message\":\"Overloaded\"}}"),
                   req_simple, "done fail\tunavailable\tprovider error: Overloaded");
    expect_fail_am(AM_START AM_TEXT_START(0) AM_TEXT(0, "part")
                   AEV("error", "{\"type\":\"error\",\"error\":{\"type\":\"overloaded_error\","
                                "\"message\":\"Overloaded\"}}"),
                   req_simple, "T=part");
    expect_fail_am(AEV("error", "{\"type\":\"error\",\"error\":{\"type\":\"authentication_error\","
                                "\"message\":\"invalid x-api-key\"}}"),
                   req_simple, "done fail\tdenied\tprovider error: invalid x-api-key");
    expect_fail_am(AEV("error", "{\"type\":\"error\",\"error\":{\"type\":\"not_found_error\","
                                "\"message\":\"model: m-1\"}}"),
                   req_simple, "done fail\tunsupported\tprovider error: model: m-1");
    expect_fail_am(AEV("error", "not json"), req_simple,
                   "done fail\tunavailable\tprovider error event");
    /* EOF after stop_reason but before message_stop */
    expect_fail_am(AM_START AM_TEXT_START(0) AM_TEXT(0, "x") AM_STOP_BLOCK(0) AM_DELTA("end_turn"),
                   req_simple, "done fail\tunavailable\tanthropic-messages protocol error: stream ended before message_stop");
    /* EOF with nothing terminal */
    expect_fail_am(AM_START AM_TEXT_START(0) AM_TEXT(0, "x"),
                   req_simple, "done fail\tunavailable\tanthropic-messages protocol error: stream ended without stop_reason");
    /* incomplete final SSE record */
    expect_fail_am(AM_START AM_TEXT_START(0) AM_TEXT(0, "x") AM_STOP_BLOCK(0) AM_DELTA("end_turn")
                   "event: message_stop\ndata: {\"type\":\"messa",
                   req_simple, "done fail\tunavailable\tanthropic-messages protocol error: SSE framing");
    /* message_stop without a stop_reason */
    expect_fail_am(AM_START AM_TEXT_START(0) AM_TEXT(0, "x") AM_STOP_BLOCK(0) AM_MSTOP,
                   req_simple, "anthropic-messages protocol error: message_stop without stop_reason");
    /* malformed JSON, invalid UTF-8, lone surrogate */
    expect_fail_am(AM_START AEV("content_block_start", "{\"type\":\"content_block_start\",\"index\":0,"),
                   req_simple, "anthropic-messages protocol error: malformed JSON event");
    expect_fail_am(AM_START AM_TEXT_START(0) AM_TEXT(0, "ok \xc3\x28"),
                   req_simple, "anthropic-messages protocol error: malformed JSON event");
    expect_fail_am(AM_START AM_TEXT_START(0) AM_TEXT(0, "\\ud800"),
                   req_simple, "done fail\tunavailable\tanthropic-messages protocol error: malformed JSON event");
    /* structural violations */
    expect_fail_am(AM_TEXT_START(0) AM_TEXT(0, "x"), req_simple,
                   "anthropic-messages protocol error: content before message_start");
    expect_fail_am(AM_START AM_TEXT_START(0) AM_TEXT(1, "x"), req_simple,
                   "anthropic-messages protocol error: content_block_delta for a block that is not open");
    expect_fail_am(AM_START AM_TEXT_START(0) AM_TEXT_START(1), req_simple,
                   "anthropic-messages protocol error: content_block_start while a block is open");
    expect_fail_am(AM_START AM_TEXT_START(0) AM_STOP_BLOCK(0) AM_TEXT_START(0), req_simple,
                   "anthropic-messages protocol error: content_block_start without a new increasing index");
    expect_fail_am(AM_START AM_TEXT_START(0) AM_DELTA("end_turn"), req_simple,
                   "anthropic-messages protocol error: message_delta while a content block is open");
    expect_fail_am(AM_START AM_TEXT_START(0) AM_STOP_BLOCK(0) AM_DELTA("end_turn") AM_TEXT_START(1),
                   req_simple, "anthropic-messages protocol error: content after stop_reason");
    expect_fail_am(AM_START "event: content_block_stop\ndata: {\"type\":\"content_block_start\",\"index\":0,"
                   "\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n",
                   req_simple, "anthropic-messages protocol error: SSE event name differs");
    expect_fail_am(AM_START AM_TOOL_START(0, "a") AM_TEXT(0, "x"), req_tools,
                   "anthropic-messages protocol error: text_delta outside a text block");
    expect_fail_am(AM_START AM_TEXT_START(0) AM_STOP_BLOCK(0) AM_DELTA("tool_use") AM_MSTOP, req_tools,
                   "anthropic-messages protocol error: stop_reason tool_use without a tool call");
    expect_fail_am(AM_START AM_TOOL_START(0, "a") AM_JSON(0, "{}") AM_STOP_BLOCK(0) AM_DELTA("end_turn") AM_MSTOP,
                   req_tools, "anthropic-messages protocol error: tool_use block without stop_reason tool_use");
    expect_fail_am(AM_START AM_TEXT_START(0) AM_STOP_BLOCK(0) AM_DELTA("refusal") AM_MSTOP, req_simple,
                   "done fail\tunavailable\tprovider stopped: refusal");
    /* tool input that is not one complete JSON object */
    expect_fail_am(AM_START AM_TOOL_START(0, "a") AM_JSON(0, "{\\\"a\\\":") AM_STOP_BLOCK(0)
                   AM_DELTA("tool_use") AM_MSTOP,
                   req_tools, "anthropic-messages protocol error: tool call arguments are not one complete JSON object");
    expect_fail_am(AM_START AM_TOOL_START(0, "a") AM_JSON(0, "[1]") AM_STOP_BLOCK(0)
                   AM_DELTA("tool_use") AM_MSTOP,
                   req_tools, "anthropic-messages protocol error: tool call arguments are not one complete JSON object");
    expect_fail_am(AM_START AM_TOOL_START(0, "a") AM_JSON(0, "{} {}") AM_STOP_BLOCK(0)
                   AM_DELTA("tool_use") AM_MSTOP,
                   req_tools, "anthropic-messages protocol error: tool call arguments are not one complete JSON object");
    /* tool_use without a name */
    expect_fail_am(AM_START AEV("content_block_start", "{\"type\":\"content_block_start\",\"index\":0,"
                                "\"content_block\":{\"type\":\"tool_use\",\"id\":\"a\",\"input\":{}}}"),
                   req_tools, "anthropic-messages protocol error: tool_use block without a name");
}

/* A tool whose block never stops is never delivered, whatever else arrived. */
static void test_am_no_partial_tool(void)
{
    harness h;
    run_one_am(&h, AM_START AM_TOOL_START(0, "a") AM_JSON(0, "{}"), 3, 0, req_tools);
    CHECK(!has(&h.log, "tool\t"));
    CHECK(has(&h.log, "done fail\tunavailable"));
    h_done(&h);
    /* the block closed, the stream then broke: the tool event exists, done says not ok
     * (the assistant discards it, spec §2.3) */
    run_one_am(&h, AM_START AM_TOOL_START(0, "a") AM_JSON(0, "{}") AM_STOP_BLOCK(0), 3, 0, req_tools);
    CHECK(has(&h.log, "tool\ta\tset_view\t{}"));
    CHECK(has(&h.log, "done fail\tunavailable"));
    CHECK(!has(&h.log, "done ok"));
    h_done(&h);
}

static void test_am_http_status(void)
{
    static const struct {
        int status;
        const char *type, *line;
    } cases[] = {{401, "authentication_error", "done fail\tdenied\tprovider answered HTTP 401"},
                 {404, "not_found_error", "done fail\tunsupported\tprovider answered HTTP 404"},
                 {429, "rate_limit_error", "done fail\tunavailable\tprovider answered HTTP 429"},
                 {500, "api_error", "done fail\tunavailable\tprovider answered HTTP 500"},
                 {529, "overloaded_error", "done fail\tunavailable\tprovider answered HTTP 529"}};
    size_t i;
    char body[256];
    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        harness h;
        canlin_llm_fixture_response r;
        h_init(&h, 0);
        h_profile_am(&h);
        memset(&r, 0, sizeof r);
        r.status = cases[i].status;
        r.content_type = "application/json";
        /* an Anthropic error body echoing the key must not reach Lua or logs */
        snprintf(body, sizeof body,
                 "{\"type\":\"error\",\"error\":{\"type\":\"%s\",\"message\":\"x-api-key " KEY "\"}}",
                 cases[i].type);
        r.body = body;
        r.body_len = strlen(body);
        r.chunk = 9;
        CHECK(canlin_llm_fixture_push(h.fx, &r) == CANLIN_LLM_OK);
        h_load(&h, 0, req_simple, 1);
        h_run(&h);
        CHECK(has(&h.log, cases[i].line));
        CHECK(!has(&h.log, "frag"));
        h_done(&h);
    }
}

static void test_am_oversize(void)
{
    char *body = malloc(CANLIN_LLM_TEXT_MAX + 400000), *p;
    size_t total = 0;
    int i;
    harness h;
    char frag[4097];
    memset(frag, 'a', 4096);
    frag[4096] = 0;

    /* text beyond CANLIN_LLM_TEXT_MAX */
    p = body + sprintf(body, "%s", AM_START AM_TEXT_START(0));
    while (total <= CANLIN_LLM_TEXT_MAX) {
        p += sprintf(p, AM_TEXT(0, "%s"), frag);
        total += 4096;
    }
    sprintf(p, "%s", AM_STOP_BLOCK(0) AM_DELTA("end_turn") AM_MSTOP);
    run_one_am(&h, body, 4000, 0, req_simple);
    CHECK(has(&h.log, "done fail\tfull\tresponse text exceeds CANLIN_LLM_TEXT_MAX"));
    CHECK(has(&h.log, "textlen\t262144"));
    h_done(&h);

    /* tool arguments beyond CANLIN_LLM_TOOL_ARGS_MAX */
    p = body + sprintf(body, "%s", AM_START AM_TOOL_START(0, "a"));
    total = 0;
    while (total <= CANLIN_LLM_TOOL_ARGS_MAX) {
        p += sprintf(p, AM_JSON(0, "%s"), frag);
        total += 4096;
    }
    sprintf(p, "%s", AM_STOP_BLOCK(0) AM_DELTA("tool_use") AM_MSTOP);
    run_one_am(&h, body, 5000, 0, req_tools);
    CHECK(has(&h.log, "done fail\tfull\ttool arguments exceed CANLIN_LLM_TOOL_ARGS_MAX"));
    CHECK(!has(&h.log, "tool\t"));
    h_done(&h);

    /* tool_use blocks beyond CANLIN_LLM_TOOL_CALLS_MAX: the first ones were delivered as
     * complete blocks, the response still fails (nothing may act on them) */
    p = body + sprintf(body, "%s", AM_START);
    for (i = 0; i <= (int)CANLIN_LLM_TOOL_CALLS_MAX; i++)
        p += sprintf(p, AM_TOOL_START(%d, "c%d") AM_STOP_BLOCK(%d), i, i, i);
    sprintf(p, "%s", AM_DELTA("tool_use") AM_MSTOP);
    run_one_am(&h, body, 64, 0, req_tools);
    CHECK(has(&h.log, "done fail\tfull\tmore tool calls than CANLIN_LLM_TOOL_CALLS_MAX"));
    CHECK(count(&h.log, "tool\t") == CANLIN_LLM_TOOL_CALLS_MAX);
    h_done(&h);

    /* tool id beyond CANLIN_LLM_TOOL_ID_MAX */
    p = body + sprintf(body, "%s", AM_START "event: content_block_start\ndata: {\"type\":"
                                            "\"content_block_start\",\"index\":0,\"content_block\":"
                                            "{\"type\":\"tool_use\",\"name\":\"set_view\",\"id\":\"");
    memset(p, 'i', CANLIN_LLM_TOOL_ID_MAX + 1);
    p += CANLIN_LLM_TOOL_ID_MAX + 1;
    sprintf(p, "\"}}\n\n");
    run_one_am(&h, body, 64, 0, req_tools);
    CHECK(has(&h.log, "done fail\tfull\ttool call id exceeds CANLIN_LLM_TOOL_ID_MAX"));
    h_done(&h);

    /* one SSE line beyond CANLIN_LLM_SSE_LINE_MAX */
    p = body + sprintf(body, "%s", AM_START AM_TEXT_START(0) "event: content_block_delta\n"
                       "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":"
                       "\"text_delta\",\"text\":\"");
    memset(p, 'b', CANLIN_LLM_SSE_LINE_MAX);
    p += CANLIN_LLM_SSE_LINE_MAX;
    sprintf(p, "\"}}\n\n");
    run_one_am(&h, body, 8192, 0, req_simple);
    CHECK(has(&h.log, "done fail\tfull\tSSE record exceeds a limit"));
    h_done(&h);
    free(body);
}

static void test_am_golden_request(void)
{
    static const char setup[] =
        "go({messages = {\n"
        "  {role = 'system', text = 'Be brief.'},\n"
        "  {role = 'user', text = 'Hallo \"Welt\"\\n\\t\\1 \xc3\xa4'},\n"
        "  {role = 'assistant', text = 'Sure.', tool_calls = {\n"
        "    {id = 'c1', name = 'set_view', args = '{\"widgets\":[]}'},\n"
        "    {id = 'c2', name = 'set_view', args = ' {\"a\" : 1} '}}},\n"
        "  {role = 'tool', tool_call_id = 'c1', text = '{\"revision\":1}'},\n"
        "  {role = 'tool', tool_call_id = 'c2', text = 'ok'},\n"
        "  {role = 'system', text = 'Second rule.'},\n"
        "  {role = 'user', text = 'Next?'},\n"
        "  {role = 'assistant', text = '', tool_calls = {{id = 'c3', name = 'set_view', args = '{}'}}},\n"
        "  {role = 'tool', tool_call_id = 'c3', text = 'done'},\n"
        "  {role = 'assistant', text = 'Done.'},\n"
        "}, tools = {'set_view'}, max_tokens = 100})";
    static const char golden_head[] =
        "{\"model\":\"m-1\",\"max_tokens\":100,\"stream\":true,"
        "\"system\":\"Be brief.\\n\\nSecond rule.\",\"messages\":["
        "{\"role\":\"user\",\"content\":\"Hallo \\\"Welt\\\"\\n\\t\\u0001 \xc3\xa4\"},"
        "{\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"Sure.\"},"
        "{\"type\":\"tool_use\",\"id\":\"c1\",\"name\":\"set_view\",\"input\":{\"widgets\":[]}},"
        "{\"type\":\"tool_use\",\"id\":\"c2\",\"name\":\"set_view\",\"input\": {\"a\" : 1} }]},"
        "{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"c1\","
        "\"content\":\"{\\\"revision\\\":1}\"},"
        "{\"type\":\"tool_result\",\"tool_use_id\":\"c2\",\"content\":\"ok\"},"
        "{\"type\":\"text\",\"text\":\"Next?\"}]},"
        "{\"role\":\"assistant\",\"content\":[{\"type\":\"tool_use\",\"id\":\"c3\",\"name\":\"set_view\","
        "\"input\":{}}]},"
        "{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"c3\",\"content\":\"done\"}]},"
        "{\"role\":\"assistant\",\"content\":\"Done.\"}],"
        "\"tools\":[{\"name\":\"set_view\",\"description\":";
    static const char golden_tail[] = ",\"required\":[\"widgets\"]}}]}";
    harness h;
    const canlin_llm_fixture_record *rec;

    run_one_am(&h, am_text_stream, 0, 0, setup);
    CHECK(has(&h.log, "done ok\tend"));
    CHECK(canlin_llm_fixture_records(h.fx) == 1);
    rec = canlin_llm_fixture_record_at(h.fx, 0);
    CHECK(rec && strcmp(rec->method, "POST") == 0);
    CHECK(rec && strcmp(rec->url, "https://llm.example/v1/messages") == 0);
    CHECK(rec && strstr(rec->headers, "Content-Type: application/json\n"));
    CHECK(rec && strstr(rec->headers, "Accept: text/event-stream\n"));
    CHECK(rec && strstr(rec->headers, "anthropic-version: 2023-06-01\n"));
    CHECK(rec && strstr(rec->headers, "x-api-key: " KEY "\n"));
    CHECK(rec && !strstr(rec->headers, "Authorization"));
    if (rec && strncmp(rec->body, golden_head, strlen(golden_head)) != 0)
        fprintf(stderr, "body: %s\n", rec->body);
    CHECK(rec && strncmp(rec->body, golden_head, strlen(golden_head)) == 0);
    CHECK(rec && json_ok(rec->body, rec->body_len));
    CHECK(rec && rec->body_len > strlen(golden_tail) &&
          strcmp(rec->body + rec->body_len - strlen(golden_tail), golden_tail) == 0);
    h_done(&h);

    /* default max_tokens, merged user turns, no tools; system only goes as the user turn */
    h_init(&h, 0);
    h_profile_am(&h);
    push_sse(&h, 200, am_text_stream, 0, 0);
    push_sse(&h, 200, am_text_stream, 0, 0);
    h_load(&h, 0,
           "MULTI = true\n"
           "go({messages = {{role = 'user', text = 'a'}, {role = 'user', text = 'b'}}})\n"
           "go({messages = {{role = 'system', text = 'S1'}, {role = 'system', text = 'S2'}}})\n", 1);
    h_run(&h);
    CHECK(canlin_llm_fixture_records(h.fx) == 2);
    rec = canlin_llm_fixture_record_at(h.fx, 0);
    CHECK(rec && strcmp(rec->body, "{\"model\":\"m-1\",\"max_tokens\":1024,\"stream\":true,"
                                   "\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"text\","
                                   "\"text\":\"a\"},{\"type\":\"text\",\"text\":\"b\"}]}]}") == 0);
    rec = canlin_llm_fixture_record_at(h.fx, 1);
    CHECK(rec && strcmp(rec->body, "{\"model\":\"m-1\",\"max_tokens\":1024,\"stream\":true,"
                                   "\"messages\":[{\"role\":\"user\",\"content\":\"S1\\n\\nS2\"}]}") == 0);
    CHECK(count(&h.log, "done ok\tend") == 2);
    h_done(&h);

    /* caller args are embedded raw, so anything but one JSON object is refused unsent */
    run_one_am(&h, am_text_stream, 0, 0,
               "local function a(x) return go({messages = {{role = 'user', text = 'q'},\n"
               "  {role = 'assistant', tool_calls = {{id = 'i', name = 'set_view', args = x}}}}}) end\n"
               "a('{\"a\":1}], \"model\": \"x')\n"
               "a('')\n"
               "a('[1]')\n"
               "a('{} {}')\n"
               "a('{\"a\":\"\\255\"}')\n");
    CHECK(count(&h.log, "req\tinvalid\ttool call args are not one JSON object") == 5);
    CHECK(canlin_llm_fixture_records(h.fx) == 0);
    h_done(&h);
}

static void test_am_cancel_late(void)
{
    static const canlin_module_event ev_cancel = {"cancel", NULL};
    harness h;
    int k;
    h_init(&h, 0);
    h_profile_am(&h);
    push_sse(&h, 200, am_text_stream, 40, 0);
    h_load(&h, 0, req_simple, 1);
    CHECK(canlin_llm_fixture_step(h.fx) == 1);
    CHECK(canlin_llm_fixture_step(h.fx) == 1);
    CHECK(canlin_ev_post(h.ev, canlin_module_gen(h.m[0]), CANLIN_MODULE_SOURCE_EVENT, 0, 0, 0,
                         (void *)&ev_cancel, 0, NULL) == CANLIN_EV_OK);
    for (k = 0; k < 16; k++) { /* text events queue up behind "cancel" */
        canlin_llm_fixture_step(h.fx);
        canlin_llm_pump(h.llm);
    }
    h_run(&h);
    CHECK(has(&h.log, "cancel\tcancelled"));
    CHECK(!has(&h.log, "frag"));
    CHECK(!has(&h.log, "usage"));
    CHECK(!has(&h.log, "done "));
    CHECK(canlin_llm_fixture_open_streams(h.fx) == 0);
    CHECK(canlin_llm_inflight(h.llm) == 0);
    h_done(&h);
}

static void test_am_credentials(void)
{
    harness h;
    canlin_llm_fixture_response r;
    const canlin_llm_fixture_record *a, *b;
    canlin_llm_profile_config pc;

    /* cross-origin redirect: x-api-key dropped, the version header is not a secret */
    h_init(&h, 0);
    h_profile_am(&h);
    memset(&r, 0, sizeof r);
    r.status = 200;
    r.body = am_text_stream;
    r.body_len = strlen(am_text_stream);
    r.redirect_to = "https://evil.example/v1/messages";
    CHECK(canlin_llm_fixture_push(h.fx, &r) == CANLIN_LLM_OK);
    h_load(&h, 0, req_simple, 1);
    h_run(&h);
    CHECK(canlin_llm_fixture_records(h.fx) == 2);
    a = canlin_llm_fixture_record_at(h.fx, 0);
    b = canlin_llm_fixture_record_at(h.fx, 1);
    CHECK(a && strstr(a->headers, "x-api-key: " KEY "\n"));
    CHECK(b && strcmp(b->url, "https://evil.example/v1/messages") == 0);
    CHECK(b && !strstr(b->headers, "x-api-key"));
    CHECK(b && !strstr(b->headers, KEY));
    CHECK(b && strstr(b->headers, "anthropic-version: 2023-06-01\n"));
    h_done(&h);

    /* an error event quoting key material is withheld */
    h_init(&h, 0);
    h_profile_am(&h);
    push_sse(&h, 200, AEV("error", "{\"type\":\"error\",\"error\":{\"type\":\"authentication_error\","
                                   "\"message\":\"bad key " KEY "\"}}"), 0, 0);
    h_load(&h, 0, req_simple, 1);
    h_run(&h);
    CHECK(has(&h.log, "done fail\tdenied\t[message withheld"));
    h_done(&h);

    /* presets: minimax's base carries its path, the dialect appends /v1/messages */
    h_init(&h, 0);
    memset(&pc, 0, sizeof pc);
    pc.id = "p";
    pc.preset = "minimax";
    CHECK(canlin_llm_profile_create(h.llm, &pc) == CANLIN_LLM_OK);
    CHECK(canlin_llm_profile_set_key(h.llm, "p", KEY, strlen(KEY)) == CANLIN_LLM_OK);
    pc.id = "mk";
    pc.preset = "moonshot";
    CHECK(canlin_llm_profile_create(h.llm, &pc) == CANLIN_LLM_OK);
    /* a custom local anthropic-messages profile with an explicit key (the
     * lmstudio-anthropic preset and its dummy key: test_lmstudio_anthropic) */
    memset(&pc, 0, sizeof pc);
    pc.id = "lms";
    pc.preset = "custom";
    pc.dialect = CANLIN_LLM_DIALECT_ANTHROPIC_MESSAGES;
    pc.auth = CANLIN_LLM_AUTH_X_API_KEY;
    pc.base_url = "http://localhost:1234";
    pc.model = "default";
    pc.local = 1;
    CHECK(canlin_llm_profile_create(h.llm, &pc) == CANLIN_LLM_OK);
    CHECK(canlin_llm_profile_set_key(h.llm, "lms", "lmstudio", 8) == CANLIN_LLM_OK);
    push_sse(&h, 200, am_text_stream, 0, 0);
    push_sse(&h, 200, am_text_stream, 0, 0);
    h_load(&h, 0, "MULTI = true\n"
                  "go({messages = {{role = 'user', text = 'hi'}}}, 'p')\n"
                  "go({messages = {{role = 'user', text = 'hi'}}}, 'lms')\n", 1);
    h_run(&h);
    CHECK(canlin_llm_fixture_records(h.fx) == 2);
    a = canlin_llm_fixture_record_at(h.fx, 0);
    b = canlin_llm_fixture_record_at(h.fx, 1);
    CHECK(a && strcmp(a->url, "https://api.minimax.io/anthropic/v1/messages") == 0);
    CHECK(a && strstr(a->body, "\"model\":\"MiniMax-M3\""));
    CHECK(a && strstr(a->headers, "x-api-key: " KEY "\n"));
    CHECK(b && strcmp(b->url, "http://localhost:1234/v1/messages") == 0);
    CHECK(b && strstr(b->headers, "x-api-key: lmstudio\n"));
    CHECK(count(&h.log, "done ok\tend") == 2);
    h_done(&h);
}

/* lmstudio-anthropic (canlin#59, spec §3; Langertha LMStudioAnthropic.pm): without a
 * user key the preset's dummy "lmstudio" goes as x-api-key, bound to the profile origin;
 * a user key replaces it; a cross-origin redirect drops it like a credential. */
static void run_lms(harness *h, int key, const char *redirect)
{
    canlin_llm_fixture_response r;
    canlin_llm_profile_config pc;
    h_init(h, 0);
    memset(&pc, 0, sizeof pc);
    pc.id = "p";
    pc.preset = "lmstudio-anthropic"; /* plain http to loopback: a local preset */
    CHECK(canlin_llm_profile_create(h->llm, &pc) == CANLIN_LLM_OK);
    CHECK(canlin_llm_profile_has_key(h->llm, "p") == 0); /* the dummy is not a key */
    if (key == 1)
        CHECK(canlin_llm_profile_set_key(h->llm, "p", KEY, strlen(KEY)) == CANLIN_LLM_OK);
    if (key == 2) { /* set, then cleared: back to the dummy */
        CHECK(canlin_llm_profile_set_key(h->llm, "p", KEY, strlen(KEY)) == CANLIN_LLM_OK);
        CHECK(canlin_llm_profile_clear_key(h->llm, "p") == CANLIN_LLM_OK);
    }
    memset(&r, 0, sizeof r);
    r.status = 200;
    r.body = am_text_stream;
    r.body_len = strlen(am_text_stream);
    r.redirect_to = redirect;
    CHECK(canlin_llm_fixture_push(h->fx, &r) == CANLIN_LLM_OK);
    h_load(h, 0, req_simple, 1);
    h_run(h);
    CHECK(has(&h->log, "done ok\tend"));
}

static void test_lmstudio_anthropic(void)
{
    const canlin_llm_preset *pre = canlin_llm_preset_find("lmstudio-anthropic");
    const canlin_llm_fixture_record *a, *b;
    harness h;
    size_t i;

    CHECK(pre != NULL);
    if (pre == NULL)
        return;
    CHECK(pre->dialect == CANLIN_LLM_DIALECT_ANTHROPIC_MESSAGES);
    CHECK(strcmp(pre->base_url, "http://localhost:1234") == 0);
    CHECK(pre->auth == CANLIN_LLM_AUTH_X_API_KEY && pre->key == CANLIN_LLM_KEY_OPTIONAL);
    CHECK(pre->local == 1 && pre->stream_usage == 0 && pre->tools == 1);
    CHECK(strcmp(pre->default_model, "default") == 0);
    CHECK(pre->dummy_key && strcmp(pre->dummy_key, "lmstudio") == 0);
    for (i = 0; i < canlin_llm_preset_count(); i++) /* no other preset has a dummy */
        if (canlin_llm_preset_at(i) != pre)
            CHECK(canlin_llm_preset_at(i)->dummy_key == NULL);

    /* no user key: the dummy is sent to the preset's origin */
    run_lms(&h, 0, NULL);
    CHECK(canlin_llm_fixture_records(h.fx) == 1);
    a = canlin_llm_fixture_record_at(h.fx, 0);
    CHECK(a && strcmp(a->url, "http://localhost:1234/v1/messages") == 0);
    CHECK(a && strstr(a->body, "\"model\":\"default\""));
    CHECK(a && strstr(a->headers, "x-api-key: lmstudio\n"));
    CHECK(a && count(&(logbuf){a->headers, 0}, "x-api-key") == 1);
    CHECK(a && strstr(a->headers, "anthropic-version: 2023-06-01\n"));
    h_done(&h);

    /* a user key replaces the dummy */
    run_lms(&h, 1, NULL);
    a = canlin_llm_fixture_record_at(h.fx, 0);
    CHECK(a && strstr(a->headers, "x-api-key: " KEY "\n"));
    CHECK(a && !strstr(a->headers, "lmstudio"));
    CHECK(a && count(&(logbuf){a->headers, 0}, "x-api-key") == 1);
    h_done(&h);

    /* the key cleared again: back to the dummy */
    run_lms(&h, 2, NULL);
    a = canlin_llm_fixture_record_at(h.fx, 0);
    CHECK(a && strstr(a->headers, "x-api-key: lmstudio\n"));
    CHECK(a && !strstr(a->headers, KEY));
    h_done(&h);

    /* cross-origin redirect (another port is another origin): the dummy is not sent on */
    run_lms(&h, 0, "http://localhost:1235/v1/messages");
    CHECK(canlin_llm_fixture_records(h.fx) == 2);
    a = canlin_llm_fixture_record_at(h.fx, 0);
    b = canlin_llm_fixture_record_at(h.fx, 1);
    CHECK(a && strstr(a->headers, "x-api-key: lmstudio\n"));
    CHECK(b && strcmp(b->url, "http://localhost:1235/v1/messages") == 0);
    CHECK(b && !strstr(b->headers, "x-api-key"));
    CHECK(b && !strstr(b->headers, "lmstudio"));
    h_done(&h);
    run_lms(&h, 0, "https://evil.example/v1/messages");
    b = canlin_llm_fixture_record_at(h.fx, 1);
    CHECK(b && !strstr(b->headers, "x-api-key") && !strstr(b->headers, "lmstudio"));
    h_done(&h);
    /* same-origin redirect keeps it */
    run_lms(&h, 0, "http://localhost:1234/v1/messages?x=1");
    b = canlin_llm_fixture_record_at(h.fx, 1);
    CHECK(b && strstr(b->headers, "x-api-key: lmstudio\n"));
    h_done(&h);
}

int main(void)
{
    test_origin();
    test_golden_request();
    test_text_every_split();
    test_text_random_large();
    test_tool_many_deltas();
    test_two_parallel_tools();
    test_failures();
    test_no_partial_tool();
    test_length_tools();
    test_http_status();
    test_oversize();
    test_cancel_late();
    test_credentials();
    test_https_rule();
    test_not_assistant_and_inflight();
    test_idle_timeout();
    test_stream_usage_flag();
    test_usage_chunk();
    test_openai_compatible_presets();
    test_am_golden_request();
    test_am_text_every_split();
    test_am_tool_many_deltas();
    test_am_parallel_tools();
    test_am_length();
    test_am_length_tools();
    test_am_failures();
    test_am_no_partial_tool();
    test_am_http_status();
    test_am_oversize();
    test_am_cancel_late();
    test_am_credentials();
    test_lmstudio_anthropic();
    printf("llm: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
