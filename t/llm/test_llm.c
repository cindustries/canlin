/* LLM providers contract tests (LLM providers spec §2–§9, core API §3, §5): the real
 * sandbox, scheduler, module layer and openai-chat dialect against the fixture
 * transport, fake clock, no network, no key of any real provider. Exits 0 on success. */
#include "canlin/llm.h"
#include "canlin/module.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* Assistant module: `setup` runs in init (it sets REQ and may call request), events are
 * printed. Every payload field is printed, so check_no_key covers them. */
static const char mod_tpl[] =
    "local M = {}\n"
    "H = nil\n"
    "local T = ''\n"
    "local function show(v, pre)\n"
    "  if type(v) ~= 'table' then print(pre, tostring(v)) return end\n"
    "  local ks = {} for k in pairs(v) do ks[#ks + 1] = tostring(k) end table.sort(ks)\n"
    "  for _, k in ipairs(ks) do show(v[k] == nil and v[tonumber(k)] or v[k], pre .. '.' .. k) end\n"
    "end\n"
    "function go(req, pid)\n"
    "  local h, code, msg = canlin.llm.request(pid or 'p', req)\n"
    "  if h then H = h print('req ok') else print('req', code, msg) end\n"
    "  return h\n"
    "end\n"
    "function M.init()\n"
    "  print('llm', type(canlin.llm))\n"
    "  if canlin.llm then show(canlin.llm.profiles(), 'prof') end\n"
    "%s\n"
    "end\n"
    "function M.event(name, p)\n"
    "  if name == 'cancel' then print('cancel', canlin.cancel(H)) return end\n"
    "  if name == 'llm' then\n"
    "    if p.handle ~= H and not MULTI then print('BADHANDLE') end\n"
    "    if p.kind == 'text' then T = T .. p.text print('frag', #p.text)\n"
    "    elseif p.kind == 'tool' then print('tool', p.id, p.name, p.args)\n"
    "    elseif p.kind == 'usage' then print('usage', p.input, p.output)\n"
    "    else print('BADKIND') end\n"
    "    return\n"
    "  end\n"
    "  if name == 'done' then\n"
    "    if #T <= 512 then print('T=' .. T) end\n"
    "    print('textlen', #T)\n"
    "    if p.ok then print('done ok', p.result.stop) else print('done fail', p.code, p.message) end\n"
    "    T = ''\n"
    "  end\n"
    "end\n"
    "return M\n";

static const char req_simple[] =
    "go({messages = {{role = 'user', text = 'hi'}}})";
static const char req_tools[] =
    "go({messages = {{role = 'user', text = 'hi'}}, tools = {'set_view'}})";

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

static const char text_stream[] =
    ": keep-alive comment\r\n"
    "data: {\"id\":\"c1\",\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\","
    "\"content\":\"\"}}]}\r\n\r\n"
    "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"Gr\\u00fc\xc3\x9f"
    "e \"}}]}\n\n"
    "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"\xf0\x9f\x91\x8b\\n\\\"ok\\\"\"},"
    "\"finish_reason\":null}]}\r\r"
    "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}],"
    "\"usage\":{\"prompt_tokens\":7,\"completion_tokens\":3}}\n\n"
    "data: [DONE]\n\n";

static const char text_expect[] = "T=Gr\xc3\xbc\xc3\x9f" "e \xf0\x9f\x91\x8b\n\"ok\"";

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
        CHECK(has(&h.log, "done ok\tlength"));
        CHECK(!has(&h.log, "BAD"));
        h_done(&h);
    }
    free(body);
}

static const char tool_stream_head[] =
    "data: {\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\",\"tool_calls\":"
    "[{\"index\":0,\"id\":\"call_A\",\"type\":\"function\",\"function\":{\"name\":\"set_view\","
    "\"arguments\":\"\"}}]}}]}\n\n";

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
    static const char body[] =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"Moment.\"}}]}\n\n"
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":["
        "{\"index\":0,\"id\":\"a\",\"function\":{\"name\":\"set_view\",\"arguments\":\"{\\\"wid\"}},"
        "{\"index\":1,\"id\":\"b\",\"function\":{\"name\":\"set_view\",\"arguments\":\"{\\\"x\"}}]}}]}\n\n"
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":["
        "{\"index\":1,\"function\":{\"arguments\":\"\\\":1}\"}}]}}]}\n\n"
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":["
        "{\"index\":0,\"function\":{\"arguments\":\"gets\\\":[]}\"}}]}}]}\n\n"
        "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n\n"
        "data: [DONE]\n\n";
    harness h;
    const char *a, *b, *t;
    run_one(&h, body, 7, 3, req_tools);
    CHECK(has(&h.log, "T=Moment."));
    a = strstr(h.log.buf, "tool\ta\tset_view\t{\"widgets\":[]}");
    b = strstr(h.log.buf, "tool\tb\tset_view\t{\"x\":1}");
    t = strstr(h.log.buf, "frag\t7");
    CHECK(a && b && t && t < a && a < b); /* text first, then tools in index order */
    CHECK(has(&h.log, "done ok\ttool"));
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
        "{\"model\":\"m-1\",\"stream\":true,\"max_tokens\":100,\"messages\":["
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
    pc.preset = "anthropic"; /* listed, dialect not implemented yet (canlin#39) */
    pc.base_url = NULL;
    pc.model = NULL;
    CHECK(canlin_llm_profile_create(h.llm, &pc) == CANLIN_LLM_EUNSUPPORTED);
    CHECK(canlin_llm_preset_find("anthropic") != NULL);
    CHECK(!canlin_llm_dialect_available(canlin_llm_preset_find("moonshot")->dialect));
    CHECK(canlin_llm_dialect_available(canlin_llm_preset_find("groq")->dialect));
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
    test_http_status();
    test_oversize();
    test_cancel_late();
    test_credentials();
    test_https_rule();
    test_not_assistant_and_inflight();
    test_idle_timeout();
    printf("llm: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
