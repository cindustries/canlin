/* Assistant response gate contract tests (LLM providers spec §2.3, decided 2026-09-23,
 * canlin#51): a tool call is acted on only after `done` with ok = true; if the stream
 * fails after a tool event, the call is discarded and the response is marked partial.
 *
 * End to end: fixture transport -> openai-chat dialect -> canlin.llm events in a real
 * sandboxed assistant module -> the gate -> the assistant view. The module forwards its
 * events to the gate through test-only functions installed next to canlin.llm by the
 * trusted assistant hook (no product Lua binding for the gate exists yet). Plus direct
 * C checks of the gate's state machine. Fake clock, no network. Exits 0 on success. */
#include "canlin/assistant_response.h"
#include "canlin/assistant_view.h"
#include "canlin/llm.h"
#include "canlin/module.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

static int failures, checks;

#define CHECK(c)                                                                   \
    do {                                                                           \
        checks++;                                                                  \
        if (!(c)) {                                                                \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c);  \
            failures++;                                                            \
        }                                                                          \
    } while (0)

#define LOGMAX (1u << 16)
#define ARGS "{\"widgets\":[{\"type\":\"heading\",\"text\":\"Hi\"}]}"

/* ---- harness ---- */

typedef struct harness {
    canlin_ev_fake_clock clock;
    canlin_ev *ev;
    canlin_llm *llm;
    canlin_llm_fixture *fx;
    canlin_module *m;
    canlin_assistant_view *av;
    canlin_assistant_response *gate;
    char log[LOGMAX];
    size_t loglen;
} harness;

static harness *cur; /* the gate the test functions forward to */
static int use_anthropic; /* h_init: profile on the anthropic preset instead of openai */

static void log_fn(void *ud, const char *msg, size_t len)
{
    harness *h = ud;
    if (len + 2 > LOGMAX - h->loglen)
        return;
    memcpy(h->log + h->loglen, msg, len);
    h->loglen += len;
    h->log[h->loglen++] = '\n';
    h->log[h->loglen] = 0;
}

static int has(const harness *h, const char *needle)
{
    return strstr(h->log, needle) != NULL;
}

static void on_deliver(void *ud, canlin_ev *ev, const canlin_ev_delivery *d)
{
    harness *h = ud;
    (void)ev;
    if (h->m && canlin_module_gen(h->m) == d->gen)
        canlin_module_deliver(h->m, d);
}

/* canlin.gate_text(n), canlin.gate_tool(id, name, args), canlin.gate_done(ok, code, msg):
 * forward the module's llm events verbatim; each returns the gate status. */
static int l_gate_text(lua_State *L)
{
    lua_pushinteger(L, canlin_assistant_response_text(cur->gate,
                                                      (size_t)luaL_checkinteger(L, 1)));
    return 1;
}

static int l_gate_tool(lua_State *L)
{
    size_t il, nl, al;
    const char *id = luaL_checklstring(L, 1, &il);
    const char *name = luaL_checklstring(L, 2, &nl);
    const char *args = luaL_checklstring(L, 3, &al);
    lua_pushinteger(L, canlin_assistant_response_tool(cur->gate, id, il, name, nl, args, al));
    return 1;
}

static int l_gate_done(lua_State *L)
{
    int ok = lua_toboolean(L, 1);
    lua_pushinteger(L, canlin_assistant_response_finish(cur->gate, cur->av, ok,
                                                        luaL_optstring(L, 2, NULL),
                                                        luaL_optstring(L, 3, NULL)));
    return 1;
}

/* canlin.gate_length(dropped_tools): done ok with result.stop = "length" */
static int l_gate_length(lua_State *L)
{
    lua_pushinteger(L, canlin_assistant_response_finish_length(
                           cur->gate, cur->av, (unsigned)luaL_checkinteger(L, 1)));
    return 1;
}

static int install(lua_State *L, canlin_module *m, void *ud)
{
    canlin_llm_install(L, m, ud);
    lua_pushcfunction(L, l_gate_length);
    lua_setfield(L, -2, "gate_length");
    lua_pushcfunction(L, l_gate_text);
    lua_setfield(L, -2, "gate_text");
    lua_pushcfunction(L, l_gate_tool);
    lua_setfield(L, -2, "gate_tool");
    lua_pushcfunction(L, l_gate_done);
    lua_setfield(L, -2, "gate_done");
    return 0;
}

static const char mod_src[] =
    "local M = {}\n"
    "function M.init()\n"
    "  local h, code, msg = canlin.llm.request('p', {messages = {{role = 'user', text = 'hi'}},\n"
    "                                               tools = {'set_view'}})\n"
    "  print('req', h or code, msg)\n"
    "end\n"
    "function M.event(name, p)\n"
    "  if name == 'llm' then\n"
    "    if p.kind == 'text' then print('gate_text', canlin.gate_text(#p.text))\n"
    "    elseif p.kind == 'tool' then print('tool', p.id, p.name)\n"
    "      print('gate_tool', canlin.gate_tool(p.id, p.name, p.args))\n"
    "    end\n"
    "  elseif name == 'done' then\n"
    "    if p.ok then print('done ok', p.result.stop) else print('done fail', p.code) end\n"
    "    if p.ok and p.result.stop == 'length' then\n"
    "      print('gate_length', canlin.gate_length(p.result.dropped_tools))\n"
    "    else print('gate_done', canlin.gate_done(p.ok, p.code, p.message)) end\n"
    "  end\n"
    "end\n"
    "return M\n";

static void h_init(harness *h)
{
    canlin_ev_clock c;
    canlin_ev_callbacks cb;
    canlin_llm_config lc;
    canlin_llm_profile_config pc;
    memset(h, 0, sizeof *h);
    c = canlin_ev_fake_clock_bind(&h->clock);
    cb.deliver = on_deliver;
    h->ev = canlin_ev_new(&c, &cb, h);
    h->fx = canlin_llm_fixture_new();
    memset(&lc, 0, sizeof lc);
    lc.transport = canlin_llm_fixture_transport(h->fx);
    h->llm = canlin_llm_new(h->ev, &lc);
    h->av = canlin_assistant_view_new();
    h->gate = canlin_assistant_response_new();
    if (!h->ev || !h->fx || !h->llm || !h->av || !h->gate) {
        fprintf(stderr, "harness setup failed\n");
        exit(1);
    }
    memset(&pc, 0, sizeof pc);
    pc.id = "p";
    pc.preset = use_anthropic ? "anthropic" : "openai";
    pc.base_url = use_anthropic ? "https://llm.example" : "https://llm.example/v1";
    pc.model = "m-1";
    CHECK(canlin_llm_profile_create(h->llm, &pc) == CANLIN_LLM_OK);
    CHECK(canlin_llm_profile_set_key(h->llm, "p", "sk-dummy", 8) == CANLIN_LLM_OK);
    cur = h;
}

static void h_done(harness *h)
{
    canlin_module_free(h->m);
    canlin_llm_free(h->llm);
    CHECK(canlin_ev_free(h->ev) == CANLIN_EV_OK);
    canlin_llm_fixture_free(h->fx);
    canlin_assistant_response_free(h->gate);
    canlin_assistant_view_free(h->av);
    cur = NULL;
}

static void h_run(harness *h)
{
    int idle = 0, k;
    for (k = 0; k < 100000 && idle < 3; k++) {
        canlin_ev_turn_result tr;
        unsigned made = canlin_llm_fixture_step(h->fx);
        canlin_llm_pump(h->llm);
        CHECK(canlin_ev_turn(h->ev, &tr) == CANLIN_EV_OK);
        idle = (made == 0 && !tr.more && canlin_llm_inflight(h->llm) == 0) ? idle + 1 : 0;
    }
    CHECK(idle >= 3);
}

/* Push `body` (fault as given), load the module, open turn 1 in view and gate, run. */
static void run_stream(harness *h, const char *body, canlin_llm_fixture_fault fault,
                       size_t fault_at, size_t chunk)
{
    canlin_llm_fixture_response r;
    canlin_module_source s;
    canlin_sandbox_config cfg;
    int err = 1;
    h_init(h);
    memset(&r, 0, sizeof r);
    r.status = 200;
    r.body = body;
    r.body_len = strlen(body);
    r.chunk = chunk;
    r.fault = fault;
    r.fault_at = fault_at;
    CHECK(canlin_llm_fixture_push(h->fx, &r) == CANLIN_LLM_OK);
    memset(&s, 0, sizeof s);
    memset(&cfg, 0, sizeof cfg);
    s.name = "assistant";
    s.version = "1";
    s.main = mod_src;
    s.main_len = strlen(mod_src);
    s.assistant = install;
    s.assistant_ud = h->llm;
    cfg.log = log_fn;
    cfg.ud = h;
    h->m = canlin_module_load(h->ev, &s, &cfg, &err);
    CHECK(err == CANLIN_MODULE_OK);
    /* The request is admitted in init; its events arrive only in h_run. */
    canlin_assistant_view_set_scope(h->av, canlin_module_gen(h->m), 1);
    CHECK(canlin_assistant_response_begin(h->gate, canlin_module_gen(h->m), 1) == CANLIN_AV_OK);
    h_run(h);
}

/* ---- fixtures: one complete set_view call, then the terminal part ---- */

static const char head[] =
    "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"Moment.\"}}]}\n\n"
    "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_A\","
    "\"type\":\"function\",\"function\":{\"name\":\"set_view\",\"arguments\":"
    "\"{\\\"widgets\\\":[{\\\"type\\\":\\\"heading\\\",\\\"text\\\":\\\"Hi\\\"}]}\"}}]}}]}\n\n"
    "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n\n";
static const char tail_done[] = "data: [DONE]\n\n";
static const char tail_error[] =
    "data: {\"error\":{\"message\":\"overloaded\",\"type\":\"server_error\"}}\n\n";

static char body_buf[4096];

static const char *body(const char *tail)
{
    snprintf(body_buf, sizeof body_buf, "%s%s", head, tail);
    return body_buf;
}

/* The tool event reached the module, but the response failed: nothing acted on. */
static void check_discarded(harness *h, const char *what)
{
    const canlin_ar_outcome *o = canlin_assistant_response_outcome(h->gate);
    if (!has(h, "tool\tcall_A\tset_view") || !has(h, "done fail"))
        fprintf(stderr, "%s: log:\n%s\n", what, h->log);
    CHECK(has(h, "tool\tcall_A\tset_view")); /* the call was delivered as an event ... */
    CHECK(has(h, "gate_tool\t0"));
    CHECK(has(h, "done fail\tunavailable"));
    CHECK(has(h, "gate_done\t0"));
    CHECK(canlin_assistant_view_revision(h->av) == 0); /* ... and never acted on */
    CHECK(canlin_assistant_view_model(h->av) == NULL);
    CHECK(o->state == CANLIN_AR_FAILED);
    CHECK(o->partial == 1);
    CHECK(o->text_bytes == 7);
    CHECK(o->calls == 1 && o->discarded == 1 && o->dispatched == 0);
    CHECK(strcmp(o->code, "unavailable") == 0);
    CHECK(o->message[0] != 0);
    CHECK(canlin_assistant_response_ncalls(h->gate) == 0);
}

static void test_eof_before_done(void)
{
    static harness h;
    run_stream(&h, body(""), CANLIN_LLM_FX_NONE, 0, 9);
    check_discarded(&h, "eof");
    CHECK(strstr(canlin_assistant_response_outcome(h.gate)->message, "before [DONE]") != NULL);
    h_done(&h);
}

static void test_error_after_tool(void)
{
    static harness h;
    run_stream(&h, body(tail_error), CANLIN_LLM_FX_NONE, 0, 9);
    check_discarded(&h, "error");
    CHECK(strstr(canlin_assistant_response_outcome(h.gate)->message, "overloaded") != NULL);
    h_done(&h);
}

static void test_transport_error_after_tool(void)
{
    static harness h;
    /* reset inside "data: [DONE]": the finish chunk (and the tool event) got through */
    run_stream(&h, body(tail_done), CANLIN_LLM_FX_ERROR_AT, strlen(head) + 8, 9);
    check_discarded(&h, "transport");
    CHECK(strstr(canlin_assistant_response_outcome(h.gate)->message, "transport") != NULL);
    h_done(&h);
}

static void test_complete_acts_once(void)
{
    static harness h;
    const canlin_ar_outcome *o;
    const canlin_ar_call *c;
    run_stream(&h, body(tail_done), CANLIN_LLM_FX_NONE, 0, 9);
    o = canlin_assistant_response_outcome(h.gate);
    CHECK(has(&h, "done ok\ttool"));
    CHECK(has(&h, "gate_done\t0"));
    CHECK(o->state == CANLIN_AR_COMPLETE && o->partial == 0);
    CHECK(o->calls == 1 && o->dispatched == 1 && o->discarded == 0);
    CHECK(canlin_assistant_view_revision(h.av) == 1);
    CHECK(canlin_assistant_view_model(h.av) != NULL);
    c = canlin_assistant_response_call(h.gate, 0);
    CHECK(c && c->dispatched && c->res.status == CANLIN_AV_OK && c->res.revision == 1);
    CHECK(c && c->res.duplicate == 0);
    CHECK(c && c->id_len == 6 && memcmp(c->id, "call_A", 6) == 0);
    /* a repeated done or a late tool event acts on nothing */
    CHECK(canlin_assistant_response_finish(h.gate, h.av, 1, NULL, NULL) == CANLIN_AV_ESTALE);
    CHECK(canlin_assistant_response_tool(h.gate, "call_B", 6, "set_view", 8, ARGS,
                                         strlen(ARGS)) == CANLIN_AV_ESTALE);
    CHECK(canlin_assistant_response_cancel(h.gate) == CANLIN_AV_ESTALE);
    CHECK(canlin_assistant_view_revision(h.av) == 1);
    CHECK(canlin_assistant_response_outcome(h.gate)->state == CANLIN_AR_COMPLETE);
    h_done(&h);
}

/* ---- length stop with an unfinished tool call (LLM providers §2.3, canlin#59) ---- */

/* openai-chat: complete call_A, then call_B cut off by finish_reason "length". */
static const char oa_length[] =
    "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"Moment.\"}}]}\n\n"
    "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_A\","
    "\"type\":\"function\",\"function\":{\"name\":\"set_view\",\"arguments\":"
    "\"{\\\"widgets\\\":[{\\\"type\\\":\\\"heading\\\",\\\"text\\\":\\\"Hi\\\"}]}\"}}]}}]}\n\n"
    "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":1,\"id\":\"call_B\","
    "\"type\":\"function\",\"function\":{\"name\":\"set_view\",\"arguments\":"
    "\"{\\\"widgets\\\":[{\\\"type\"}}]}}]}\n\n"
    "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"length\"}]}\n\n"
    "data: [DONE]\n\n";

/* anthropic-messages: the same, call_B's block stopped with incomplete input. */
#define AEV(name, json) "event: " name "\ndata: " json "\n\n"
static const char am_length[] =
    AEV("message_start", "{\"type\":\"message_start\",\"message\":{\"id\":\"msg_1\","
        "\"usage\":{\"input_tokens\":7,\"output_tokens\":1}}}")
    AEV("content_block_start", "{\"type\":\"content_block_start\",\"index\":0,"
        "\"content_block\":{\"type\":\"text\",\"text\":\"\"}}")
    AEV("content_block_delta", "{\"type\":\"content_block_delta\",\"index\":0,"
        "\"delta\":{\"type\":\"text_delta\",\"text\":\"Moment.\"}}")
    AEV("content_block_stop", "{\"type\":\"content_block_stop\",\"index\":0}")
    AEV("content_block_start", "{\"type\":\"content_block_start\",\"index\":1,"
        "\"content_block\":{\"type\":\"tool_use\",\"id\":\"call_A\",\"name\":\"set_view\",\"input\":{}}}")
    AEV("content_block_delta", "{\"type\":\"content_block_delta\",\"index\":1,\"delta\":"
        "{\"type\":\"input_json_delta\",\"partial_json\":"
        "\"{\\\"widgets\\\":[{\\\"type\\\":\\\"heading\\\",\\\"text\\\":\\\"Hi\\\"}]}\"}}")
    AEV("content_block_stop", "{\"type\":\"content_block_stop\",\"index\":1}")
    AEV("content_block_start", "{\"type\":\"content_block_start\",\"index\":2,"
        "\"content_block\":{\"type\":\"tool_use\",\"id\":\"call_B\",\"name\":\"set_view\",\"input\":{}}}")
    AEV("content_block_delta", "{\"type\":\"content_block_delta\",\"index\":2,\"delta\":"
        "{\"type\":\"input_json_delta\",\"partial_json\":\"{\\\"widgets\\\":[\"}}")
    AEV("content_block_stop", "{\"type\":\"content_block_stop\",\"index\":2}")
    AEV("message_delta", "{\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"max_tokens\"},"
        "\"usage\":{\"output_tokens\":9}}")
    AEV("message_stop", "{\"type\":\"message_stop\"}");

/* The complete call acts after done ok; the dropped one is counted, never received. */
static void check_length(harness *h, const char *what)
{
    const canlin_ar_outcome *o = canlin_assistant_response_outcome(h->gate);
    const canlin_ar_call *c = canlin_assistant_response_call(h->gate, 0);
    if (!has(h, "gate_length\t0"))
        fprintf(stderr, "%s: log:\n%s\n", what, h->log);
    CHECK(has(h, "tool\tcall_A\tset_view"));
    CHECK(!has(h, "call_B"));
    CHECK(has(h, "done ok\tlength"));
    CHECK(has(h, "gate_length\t0"));
    CHECK(o->state == CANLIN_AR_COMPLETE && o->partial == 0);
    CHECK(o->truncated == 1 && o->dropped == 1);
    CHECK(o->calls == 1 && o->dispatched == 1 && o->discarded == 0);
    CHECK(o->text_bytes == 7);
    CHECK(canlin_assistant_response_ncalls(h->gate) == 1);
    CHECK(c && c->dispatched && c->res.status == CANLIN_AV_OK && c->res.revision == 1);
    CHECK(canlin_assistant_view_revision(h->av) == 1);
    CHECK(canlin_assistant_view_model(h->av) != NULL);
    /* exactly once */
    CHECK(canlin_assistant_response_finish_length(h->gate, h->av, 1) == CANLIN_AV_ESTALE);
    CHECK(canlin_assistant_response_finish(h->gate, h->av, 1, NULL, NULL) == CANLIN_AV_ESTALE);
    CHECK(canlin_assistant_view_revision(h->av) == 1);
}

static void test_length_drops_unfinished(void)
{
    static harness h;
    size_t k;
    for (k = 1; k <= 64 && !failures; k += 7) {
        use_anthropic = 0;
        run_stream(&h, oa_length, CANLIN_LLM_FX_NONE, 0, k);
        check_length(&h, "openai-chat length");
        h_done(&h);
        use_anthropic = 1;
        run_stream(&h, am_length, CANLIN_LLM_FX_NONE, 0, k);
        check_length(&h, "anthropic-messages length");
        h_done(&h);
    }
    use_anthropic = 0;
    /* the length stop that never reached [DONE] is still a failure: nothing acts */
    run_stream(&h, oa_length, CANLIN_LLM_FX_EOF_AT, strlen(oa_length) - 8, 9);
    CHECK(has(&h, "tool\tcall_A\tset_view") && has(&h, "done fail\tunavailable"));
    CHECK(canlin_assistant_response_outcome(h.gate)->state == CANLIN_AR_FAILED);
    CHECK(canlin_assistant_response_outcome(h.gate)->discarded == 1);
    CHECK(canlin_assistant_response_outcome(h.gate)->truncated == 0);
    CHECK(canlin_assistant_view_revision(h.av) == 0);
    h_done(&h);
}

/* ---- the gate alone ---- */

static void test_gate_unit(void)
{
    canlin_assistant_view *av = canlin_assistant_view_new();
    canlin_assistant_response *g = canlin_assistant_response_new();
    const canlin_ar_outcome *o = canlin_assistant_response_outcome(g);
    const canlin_ar_call *c;
    char big[64];
    unsigned i;

    /* nothing open */
    CHECK(o->state == CANLIN_AR_IDLE);
    CHECK(canlin_assistant_response_tool(g, "a", 1, "set_view", 8, ARGS, strlen(ARGS)) ==
          CANLIN_AV_ESTALE);
    CHECK(canlin_assistant_response_finish(g, av, 1, NULL, NULL) == CANLIN_AV_ESTALE);
    CHECK(canlin_assistant_response_begin(g, 7, 0) == CANLIN_AV_EARG);

    /* cancel after a held call: discarded, partial */
    canlin_assistant_view_set_scope(av, 7, 1);
    CHECK(canlin_assistant_response_begin(g, 7, 1) == CANLIN_AV_OK);
    CHECK(canlin_assistant_response_tool(g, "a", 1, "set_view", 8, ARGS, strlen(ARGS)) ==
          CANLIN_AV_OK);
    CHECK(canlin_assistant_response_ncalls(g) == 1);
    CHECK(canlin_assistant_view_revision(av) == 0); /* held, not acted on */
    CHECK(canlin_assistant_response_cancel(g) == CANLIN_AV_OK);
    CHECK(o->state == CANLIN_AR_CANCELLED && o->partial && o->discarded == 1);
    CHECK(strcmp(o->code, "cancelled") == 0);
    CHECK(canlin_assistant_response_finish(g, av, 1, NULL, NULL) == CANLIN_AV_ESTALE);
    CHECK(canlin_assistant_view_revision(av) == 0);

    /* failed with nothing received: not partial */
    CHECK(canlin_assistant_response_begin(g, 7, 1) == CANLIN_AV_OK);
    CHECK(canlin_assistant_response_finish(g, av, 0, "denied", "HTTP 401") == CANLIN_AV_OK);
    CHECK(o->state == CANLIN_AR_FAILED && !o->partial && o->discarded == 0);
    CHECK(strcmp(o->code, "denied") == 0 && strcmp(o->message, "HTTP 401") == 0);

    /* ok without a view while calls are held: refused, still open, then taken */
    CHECK(canlin_assistant_response_begin(g, 7, 1) == CANLIN_AV_OK);
    CHECK(canlin_assistant_response_text(g, 3) == CANLIN_AV_OK);
    CHECK(canlin_assistant_response_tool(g, "a", 1, "set_view", 8, ARGS, strlen(ARGS)) ==
          CANLIN_AV_OK);
    CHECK(canlin_assistant_response_tool(g, "b", 1, "shell", 5, "{}", 2) == CANLIN_AV_OK);
    CHECK(canlin_assistant_response_tool(g, "c", 1, "set_view", 8, "{\"x\":1}", 7) ==
          CANLIN_AV_OK);
    CHECK(canlin_assistant_response_finish(g, NULL, 1, NULL, NULL) == CANLIN_AV_EARG);
    CHECK(o->state == CANLIN_AR_OPEN);
    CHECK(canlin_assistant_response_finish(g, av, 1, NULL, NULL) == CANLIN_AV_OK);
    CHECK(o->state == CANLIN_AR_COMPLETE && o->calls == 3 && o->dispatched == 2);
    c = canlin_assistant_response_call(g, 0);
    CHECK(c && c->res.status == CANLIN_AV_OK && c->res.revision == 1);
    c = canlin_assistant_response_call(g, 1); /* unknown tool: error result, not dispatched */
    CHECK(c && !c->dispatched && c->res.status == CANLIN_AV_EINVALID);
    c = canlin_assistant_response_call(g, 2); /* invalid view: error result, view kept */
    CHECK(c && c->dispatched && c->res.status == CANLIN_AV_EINVALID);
    CHECK(canlin_assistant_response_call(g, 3) == NULL);
    CHECK(canlin_assistant_view_revision(av) == 1);

    /* a response of an old scope gets "stale" from the view and changes nothing */
    canlin_assistant_view_set_scope(av, 7, 2);
    CHECK(canlin_assistant_response_begin(g, 7, 1) == CANLIN_AV_OK);
    CHECK(canlin_assistant_response_tool(g, "z", 1, "set_view", 8, ARGS, strlen(ARGS)) ==
          CANLIN_AV_OK);
    CHECK(canlin_assistant_response_finish(g, av, 1, NULL, NULL) == CANLIN_AV_OK);
    c = canlin_assistant_response_call(g, 0);
    CHECK(c && c->res.status == CANLIN_AV_ESTALE);
    CHECK(canlin_assistant_view_revision(av) == 1);

    /* one call too many fails the whole response: no subset is applied */
    CHECK(canlin_assistant_response_begin(g, 7, 2) == CANLIN_AV_OK);
    for (i = 0; i < CANLIN_LLM_TOOL_CALLS_MAX; i++) {
        snprintf(big, sizeof big, "c%u", i);
        CHECK(canlin_assistant_response_tool(g, big, strlen(big), "set_view", 8, ARGS,
                                             strlen(ARGS)) == CANLIN_AV_OK);
    }
    CHECK(canlin_assistant_response_tool(g, "x", 1, "set_view", 8, ARGS, strlen(ARGS)) ==
          CANLIN_AV_EFULL);
    CHECK(o->state == CANLIN_AR_FAILED && strcmp(o->code, "full") == 0);
    CHECK(o->calls == CANLIN_LLM_TOOL_CALLS_MAX + 1 && o->discarded == CANLIN_LLM_TOOL_CALLS_MAX);
    CHECK(canlin_assistant_response_finish(g, av, 1, NULL, NULL) == CANLIN_AV_ESTALE);
    CHECK(canlin_assistant_view_revision(av) == 1);

    /* length stop: held calls act like done ok, truncated and dropped recorded; a
     * normal finish resets both */
    canlin_assistant_view_set_scope(av, 7, 3);
    CHECK(canlin_assistant_response_begin(g, 7, 3) == CANLIN_AV_OK);
    CHECK(o->truncated == 0 && o->dropped == 0);
    CHECK(canlin_assistant_response_tool(g, "a", 1, "set_view", 8, ARGS, strlen(ARGS)) ==
          CANLIN_AV_OK);
    CHECK(canlin_assistant_response_finish_length(g, NULL, 2) == CANLIN_AV_EARG); /* view */
    CHECK(o->state == CANLIN_AR_OPEN && o->truncated == 0);
    CHECK(canlin_assistant_response_finish_length(g, av, 2) == CANLIN_AV_OK);
    CHECK(o->state == CANLIN_AR_COMPLETE && o->truncated == 1 && o->dropped == 2);
    CHECK(o->dispatched == 1 && o->discarded == 0 && o->partial == 0);
    CHECK(canlin_assistant_view_revision(av) == 2);
    CHECK(canlin_assistant_response_finish_length(g, av, 1) == CANLIN_AV_ESTALE);
    CHECK(o->dropped == 2);
    /* nothing held, nothing dropped: still a truncated success */
    CHECK(canlin_assistant_response_begin(g, 7, 3) == CANLIN_AV_OK);
    CHECK(canlin_assistant_response_finish_length(g, NULL, 0) == CANLIN_AV_OK);
    CHECK(o->state == CANLIN_AR_COMPLETE && o->truncated == 1 && o->dropped == 0);
    CHECK(canlin_assistant_response_begin(g, 7, 3) == CANLIN_AV_OK);
    CHECK(o->truncated == 0 && o->dropped == 0);
    /* cancelled: a later length finish acts on nothing */
    CHECK(canlin_assistant_response_tool(g, "a", 1, "set_view", 8, ARGS, strlen(ARGS)) ==
          CANLIN_AV_OK);
    CHECK(canlin_assistant_response_cancel(g) == CANLIN_AV_OK);
    CHECK(canlin_assistant_response_finish_length(g, av, 1) == CANLIN_AV_ESTALE);
    CHECK(o->truncated == 0 && canlin_assistant_view_revision(av) == 2);

    /* bad arguments */
    CHECK(canlin_assistant_response_begin(g, 7, 2) == CANLIN_AV_OK);
    CHECK(canlin_assistant_response_tool(g, NULL, 0, "set_view", 8, "{}", 2) == CANLIN_AV_EARG);
    CHECK(canlin_assistant_response_tool(g, "a", 1, "", 0, "{}", 2) == CANLIN_AV_EARG);
    CHECK(o->state == CANLIN_AR_OPEN && o->calls == 0);

    canlin_assistant_response_free(g);
    canlin_assistant_view_free(av);
}

int main(void)
{
    test_gate_unit();
    test_complete_acts_once();
    test_eof_before_done();
    test_error_after_tool();
    test_transport_error_after_tool();
    test_length_drops_unfinished();
    printf("assistant response: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
