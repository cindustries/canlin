/* openai-chat dialect (LLM providers spec §2.1, §2.3).
 *
 * Request: POST {base}/chat/completions, "stream": true, tools as tools[].function.
 * Response: SSE `data:` JSON chunks and `data: [DONE]`. Each chunk is parsed as one JSON
 * document by the push tokenizer; its effects (text, tool-call deltas, finish_reason,
 * usage, error) are collected first and applied only once the whole document parsed,
 * so nothing from a malformed chunk reaches the module. Tool calls are assembled from
 * index-based deltas and delivered only at finish_reason "tool_calls", complete and with
 * arguments that parse as one JSON object. Success needs finish_reason then [DONE].
 * With stream_options.include_usage (preset flag) the provider sends one more chunk
 * between finish_reason and [DONE]: "choices": [] and the usage object; it carries no
 * content, so it is accepted after finish_reason and only emits usage.
 */
#include "llm_int.h"
#include "canlin/json.h"
#include "canlin/sse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- request ---- */

static void role_text(llm_jw *w, const char *role, const llm_msg *m)
{
    llm_jw_lit(w, "{\"role\":\"");
    llm_jw_lit(w, role);
    llm_jw_lit(w, "\",\"content\":");
    llm_jw_str(w, m->text.s, m->text.n);
}

static void oa_build(llm_jw *w, const llm_build *b)
{
    size_t i, k;

    llm_jw_lit(w, "{\"model\":");
    llm_jw_str(w, b->model, strlen(b->model));
    llm_jw_lit(w, ",\"stream\":true");
    if (b->stream_usage)
        llm_jw_lit(w, ",\"stream_options\":{\"include_usage\":true}");
    if (b->max_tokens) {
        llm_jw_lit(w, ",\"max_tokens\":");
        llm_jw_uint(w, b->max_tokens);
    }
    llm_jw_lit(w, ",\"messages\":[");
    for (i = 0; i < b->nmsgs; i++) {
        const llm_msg *m = &b->msgs[i];
        if (i)
            llm_jw_raw(w, ",", 1);
        switch (m->role) {
        case LLM_ROLE_SYSTEM:
            role_text(w, "system", m);
            break;
        case LLM_ROLE_USER:
            role_text(w, "user", m);
            break;
        case LLM_ROLE_TOOL:
            llm_jw_lit(w, "{\"role\":\"tool\",\"tool_call_id\":");
            llm_jw_str(w, m->tool_call_id.s, m->tool_call_id.n);
            llm_jw_lit(w, ",\"content\":");
            llm_jw_str(w, m->text.s, m->text.n);
            break;
        default: /* assistant */
            llm_jw_lit(w, "{\"role\":\"assistant\",\"content\":");
            if (m->has_text)
                llm_jw_str(w, m->text.s, m->text.n);
            else
                llm_jw_lit(w, "null");
            if (m->ncalls) {
                llm_jw_lit(w, ",\"tool_calls\":[");
                for (k = 0; k < m->ncalls; k++) {
                    const llm_call *c = &m->calls[k];
                    if (k)
                        llm_jw_raw(w, ",", 1);
                    llm_jw_lit(w, "{\"id\":");
                    llm_jw_str(w, c->id.s, c->id.n);
                    llm_jw_lit(w, ",\"type\":\"function\",\"function\":{\"name\":");
                    llm_jw_str(w, c->name.s, c->name.n);
                    llm_jw_lit(w, ",\"arguments\":");
                    llm_jw_str(w, c->args.s, c->args.n);
                    llm_jw_lit(w, "}}");
                }
                llm_jw_raw(w, "]", 1);
            }
            break;
        }
        llm_jw_raw(w, "}", 1);
    }
    llm_jw_raw(w, "]", 1);
    if (b->ntools) {
        llm_jw_lit(w, ",\"tools\":[");
        for (i = 0; i < b->ntools; i++) {
            const llm_tool *t = b->tools[i];
            if (i)
                llm_jw_raw(w, ",", 1);
            llm_jw_lit(w, "{\"type\":\"function\",\"function\":{\"name\":");
            llm_jw_str(w, t->name, strlen(t->name));
            llm_jw_lit(w, ",\"description\":");
            llm_jw_str(w, t->description, strlen(t->description));
            llm_jw_lit(w, ",\"parameters\":");
            llm_jw_lit(w, t->schema_json); /* core data, valid JSON */
            llm_jw_lit(w, "}}");
        }
        llm_jw_raw(w, "]", 1);
    }
    llm_jw_raw(w, "}", 1);
}

/* ---- response ---- */

enum { TG_NONE = 0, TG_ROOT, TG_CHOICES, TG_CHOICE, TG_DELTA, TG_TCALLS, TG_TCALL, TG_FUNC,
       TG_USAGE, TG_ERROR };

enum { K_OTHER = 0, K_CHOICES, K_DELTA, K_CONTENT, K_TOOL_CALLS, K_INDEX, K_ID, K_FUNCTION,
       K_NAME, K_ARGUMENTS, K_FINISH, K_USAGE, K_PROMPT, K_COMPLETION, K_ERROR, K_MESSAGE };

static const struct {
    const char *s;
    int k;
} keys[] = {{"choices", K_CHOICES},        {"delta", K_DELTA},
            {"content", K_CONTENT},        {"tool_calls", K_TOOL_CALLS},
            {"index", K_INDEX},            {"id", K_ID},
            {"function", K_FUNCTION},      {"name", K_NAME},
            {"arguments", K_ARGUMENTS},    {"finish_reason", K_FINISH},
            {"usage", K_USAGE},            {"prompt_tokens", K_PROMPT},
            {"completion_tokens", K_COMPLETION}, {"error", K_ERROR},
            {"message", K_MESSAGE},        {NULL, 0}};

/* A string collected from the current document: offset/length into the arena. */
typedef struct span {
    size_t off, len;
    int set;
} span;

/* One tool_calls[] element of the current document. */
typedef struct tdelta {
    long index; /* -1 = absent: use the array position */
    long pos;
    span id, name, args;
} tdelta;

/* An assembled tool call across documents. */
typedef struct tslot {
    int used;
    char id[CANLIN_LLM_TOOL_ID_MAX];
    size_t idn;
    char name[CANLIN_LLM_TOOL_NAME_MAX];
    size_t namen;
    char *args;
    size_t argsn, argscap;
} tslot;

typedef struct oa {
    llm_req *r;
    canlin_sse sse;
    void *ssemem;
    canlin_json js;
    char jbuf[CANLIN_LLM_JSON_STRING_MAX];

    /* per document */
    char *arena; /* CANLIN_LLM_SSE_DATA_MAX: decoded strings never exceed the data */
    size_t arena_len;
    int tag[CANLIN_LLM_JSON_DEPTH + 2];
    int key[CANLIN_LLM_JSON_DEPTH + 2];
    long pos[CANLIN_LLM_JSON_DEPTH + 2];
    span text, finish, errmsg;
    int has_error;
    int64_t usage_in, usage_out;
    int has_usage;
    tdelta td[CANLIN_LLM_TOOL_CALLS_MAX];
    size_t ntd;
    int doc_err; /* "full" overflow detected in the callback */

    /* per response */
    int finished;      /* finish_reason seen */
    int done;          /* [DONE] or terminal */
    tslot slot[CANLIN_LLM_TOOL_CALLS_MAX];
} oa;

static int key_id(const char *s, size_t n)
{
    int i;
    for (i = 0; keys[i].s; i++)
        if (strlen(keys[i].s) == n && memcmp(keys[i].s, s, n) == 0)
            return keys[i].k;
    return K_OTHER;
}

static int keep(oa *o, span *sp, const char *s, size_t n)
{
    if (n > CANLIN_LLM_SSE_DATA_MAX - o->arena_len)
        return -1; /* cannot happen: decoded strings are shorter than the data */
    memcpy(o->arena + o->arena_len, s, n);
    sp->off = o->arena_len;
    sp->len = n;
    sp->set = 1;
    o->arena_len += n;
    return 0;
}

static int parse_long(const char *s, size_t n, long *out)
{
    long v = 0;
    size_t i;
    if (n == 0 || n > 9)
        return -1;
    for (i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9')
            return -1;
        v = v * 10 + (s[i] - '0');
    }
    *out = v;
    return 0;
}

/* Token callback: record the effects of one chunk document. */
static int on_tok(void *ud, const canlin_json_token *t)
{
    oa *o = ud;
    unsigned d = t->depth;
    int parent, k, is_begin;
    long mypos;

    if (d > CANLIN_LLM_JSON_DEPTH)
        return 1; /* the tokenizer limit makes this unreachable */
    if (t->type == CANLIN_JSON_KEY) {
        o->key[d] = key_id(t->ptr, t->len);
        return 0;
    }
    if (t->type == CANLIN_JSON_OBJ_END || t->type == CANLIN_JSON_ARR_END) {
        if (o->tag[d + 1] == TG_TCALL && o->ntd < CANLIN_LLM_TOOL_CALLS_MAX)
            o->ntd++; /* element complete */
        o->tag[d + 1] = TG_NONE;
        return 0;
    }
    is_begin = t->type == CANLIN_JSON_OBJ_BEGIN || t->type == CANLIN_JSON_ARR_BEGIN;
    if (d == 0) {
        if (t->type != CANLIN_JSON_OBJ_BEGIN)
            return 1; /* a chunk must be an object */
        o->tag[1] = TG_ROOT;
        o->key[1] = K_OTHER;
        return 0;
    }
    parent = o->tag[d];
    k = o->key[d];
    mypos = o->pos[d]++; /* array element position (meaningless inside objects) */
    if (is_begin) {
        int nt = TG_NONE;
        if (parent == TG_ROOT && k == K_CHOICES && t->type == CANLIN_JSON_ARR_BEGIN)
            nt = TG_CHOICES;
        else if (parent == TG_ROOT && k == K_USAGE && t->type == CANLIN_JSON_OBJ_BEGIN)
            nt = TG_USAGE;
        else if (parent == TG_ROOT && k == K_ERROR) {
            nt = TG_ERROR;
            o->has_error = 1;
        } else if (parent == TG_CHOICES && mypos == 0 && t->type == CANLIN_JSON_OBJ_BEGIN)
            nt = TG_CHOICE;
        else if (parent == TG_CHOICE && k == K_DELTA && t->type == CANLIN_JSON_OBJ_BEGIN)
            nt = TG_DELTA;
        else if (parent == TG_DELTA && k == K_TOOL_CALLS && t->type == CANLIN_JSON_ARR_BEGIN)
            nt = TG_TCALLS;
        else if (parent == TG_TCALLS && t->type == CANLIN_JSON_OBJ_BEGIN) {
            if (o->ntd >= CANLIN_LLM_TOOL_CALLS_MAX) {
                o->doc_err = 1;
                return 1;
            }
            nt = TG_TCALL;
            memset(&o->td[o->ntd], 0, sizeof o->td[0]);
            o->td[o->ntd].index = -1;
            o->td[o->ntd].pos = mypos;
        } else if (parent == TG_TCALL && k == K_FUNCTION && t->type == CANLIN_JSON_OBJ_BEGIN)
            nt = TG_FUNC;
        o->tag[d + 1] = nt;
        o->key[d + 1] = K_OTHER;
        o->pos[d + 1] = 0;
        return 0;
    }
    /* scalar value */
    switch (parent) {
    case TG_ROOT:
        if (k == K_ERROR && t->type == CANLIN_JSON_STRING) {
            o->has_error = 1;
            return keep(o, &o->errmsg, t->ptr, t->len);
        }
        break;
    case TG_CHOICE:
        if (k == K_FINISH && t->type == CANLIN_JSON_STRING)
            return keep(o, &o->finish, t->ptr, t->len);
        break;
    case TG_DELTA:
        if (k == K_CONTENT && t->type == CANLIN_JSON_STRING)
            return keep(o, &o->text, t->ptr, t->len);
        break;
    case TG_TCALL: {
        tdelta *td = &o->td[o->ntd];
        if (k == K_INDEX && t->type == CANLIN_JSON_NUMBER) {
            if (parse_long(t->ptr, t->len, &td->index) != 0)
                td->index = (long)CANLIN_LLM_TOOL_CALLS_MAX; /* out of range: full */
        } else if (k == K_ID && t->type == CANLIN_JSON_STRING)
            return keep(o, &td->id, t->ptr, t->len);
        break;
    }
    case TG_FUNC: {
        tdelta *td = &o->td[o->ntd];
        if (k == K_NAME && t->type == CANLIN_JSON_STRING)
            return keep(o, &td->name, t->ptr, t->len);
        if (k == K_ARGUMENTS && t->type == CANLIN_JSON_STRING)
            return keep(o, &td->args, t->ptr, t->len);
        break;
    }
    case TG_USAGE:
        if ((k == K_PROMPT || k == K_COMPLETION) && t->type == CANLIN_JSON_NUMBER) {
            long v;
            if (parse_long(t->ptr, t->len, &v) == 0) {
                if (k == K_PROMPT)
                    o->usage_in = v;
                else
                    o->usage_out = v;
                o->has_usage = 1;
            }
        }
        break;
    case TG_ERROR:
        if (k == K_MESSAGE && t->type == CANLIN_JSON_STRING)
            return keep(o, &o->errmsg, t->ptr, t->len);
        break;
    default:
        break;
    }
    return 0;
}

static int span_is(const oa *o, const span *sp, const char *lit)
{
    size_t n = strlen(lit);
    return sp->set && sp->len == n && memcmp(o->arena + sp->off, lit, n) == 0;
}

static void fail_full(oa *o, const char *what)
{
    o->done = 1;
    llm_fail(o->r, "full", what);
}

static void fail_proto(oa *o, const char *what)
{
    char msg[2 * CANLIN_LLM_MSG_MAX];
    snprintf(msg, sizeof msg, "openai-chat protocol error: %s", what);
    o->done = 1;
    llm_fail(o->r, "unavailable", msg);
}

static int append_args(tslot *s, const char *p, size_t n)
{
    if (n > CANLIN_LLM_TOOL_ARGS_MAX - s->argsn)
        return -1;
    if (s->argsn + n > s->argscap) {
        size_t cap = s->argscap ? s->argscap : 256;
        char *nb;
        while (cap < s->argsn + n)
            cap *= 2;
        if (cap > CANLIN_LLM_TOOL_ARGS_MAX)
            cap = CANLIN_LLM_TOOL_ARGS_MAX;
        nb = realloc(s->args, cap);
        if (nb == NULL)
            return -2;
        s->args = nb;
        s->argscap = cap;
    }
    memcpy(s->args + s->argsn, p, n);
    s->argsn += n;
    return 0;
}

static int merge_delta(oa *o, const tdelta *td)
{
    long i = td->index >= 0 ? td->index : td->pos;
    tslot *s;
    int rc;
    if (i < 0 || i >= (long)CANLIN_LLM_TOOL_CALLS_MAX) {
        fail_full(o, "more tool calls than CANLIN_LLM_TOOL_CALLS_MAX");
        return -1;
    }
    s = &o->slot[i];
    s->used = 1;
    if (td->id.set && td->id.len) {
        if (s->idn && (s->idn != td->id.len || memcmp(s->id, o->arena + td->id.off, s->idn))) {
            fail_proto(o, "tool call id changed within one call");
            return -1;
        }
        if (td->id.len > sizeof s->id) {
            fail_full(o, "tool call id exceeds CANLIN_LLM_TOOL_ID_MAX");
            return -1;
        }
        memcpy(s->id, o->arena + td->id.off, td->id.len);
        s->idn = td->id.len;
    }
    if (td->name.set) {
        if (td->name.len > sizeof s->name - s->namen) {
            fail_full(o, "tool name exceeds CANLIN_LLM_TOOL_NAME_MAX");
            return -1;
        }
        memcpy(s->name + s->namen, o->arena + td->name.off, td->name.len);
        s->namen += td->name.len;
    }
    if (td->args.set && td->args.len) {
        rc = append_args(s, o->arena + td->args.off, td->args.len);
        if (rc == -1) {
            fail_full(o, "tool arguments exceed CANLIN_LLM_TOOL_ARGS_MAX");
            return -1;
        }
        if (rc != 0) {
            o->done = 1;
            llm_fail(o->r, "unavailable", "out of memory assembling tool arguments");
            return -1;
        }
    }
    return 0;
}

/* Arguments must be exactly one complete JSON object. */
static int args_tok(void *ud, const canlin_json_token *t)
{
    int *first = ud;
    if (*first == 0)
        *first = t->type;
    return 0;
}

/* buf: the chunk parser's token buffer, idle once a document has been parsed. */
static int args_valid(char *buf, size_t buflen, const char *p, size_t n)
{
    canlin_json js;
    canlin_json_limits lim;
    int first = 0;
    lim.max_depth = CANLIN_LLM_JSON_DEPTH;
    lim.max_string = CANLIN_LLM_JSON_STRING_MAX;
    lim.max_number = CANLIN_LLM_JSON_NUMBER_MAX;
    lim.max_total = CANLIN_LLM_TOOL_ARGS_MAX;
    if (canlin_json_init(&js, &lim, buf, buflen, args_tok, &first) != CANLIN_JSON_OK)
        return 0;
    if (canlin_json_feed(&js, p, n) != CANLIN_JSON_OK || canlin_json_finish(&js) != CANLIN_JSON_OK)
        return 0;
    return first == CANLIN_JSON_OBJ_BEGIN;
}

static void emit_tools(oa *o)
{
    size_t i, n = 0;
    for (i = 0; i < CANLIN_LLM_TOOL_CALLS_MAX; i++) {
        tslot *s = &o->slot[i];
        char gen[24];
        const char *id = s->id;
        size_t idn = s->idn;
        if (!s->used)
            continue;
        if (s->namen == 0 || !llm_utf8_valid(s->name, s->namen)) {
            fail_proto(o, "tool call without a name");
            return;
        }
        if (!args_valid(o->jbuf, sizeof o->jbuf, s->args, s->argsn)) {
            fail_proto(o, "tool call arguments are not one complete JSON object");
            return;
        }
        if (idn == 0) { /* some local servers omit ids: synthesise a stable one */
            idn = (size_t)snprintf(gen, sizeof gen, "call_%u", (unsigned)i);
            id = gen;
        }
        if (llm_emit_tool(o->r, id, idn, s->name, s->namen, s->args ? s->args : "",
                          s->argsn) != 0)
            return;
        n++;
    }
    if (n == 0)
        fail_proto(o, "finish_reason tool_calls without a tool call");
}

static int any_tool(const oa *o)
{
    size_t i;
    for (i = 0; i < CANLIN_LLM_TOOL_CALLS_MAX; i++)
        if (o->slot[i].used)
            return 1;
    return 0;
}

static void json_fail(oa *o, int rc)
{
    char msg[CANLIN_LLM_MSG_MAX];
    if (rc == CANLIN_JSON_E_ABORTED && o->doc_err) {
        fail_full(o, "more tool calls than CANLIN_LLM_TOOL_CALLS_MAX");
        return;
    }
    if (rc == CANLIN_JSON_E_STRING_LEN || rc == CANLIN_JSON_E_DEPTH ||
        rc == CANLIN_JSON_E_TOTAL || rc == CANLIN_JSON_E_NUMBER_LEN) {
        snprintf(msg, sizeof msg, "stream chunk exceeds a JSON limit: %s",
                 canlin_json_strerror(rc));
        fail_full(o, msg);
        return;
    }
    snprintf(msg, sizeof msg, "malformed JSON chunk: %s", canlin_json_strerror(rc));
    fail_proto(o, msg);
}

static void on_data(oa *o, const char *data, size_t n)
{
    canlin_json_limits lim;
    int rc;
    size_t i;

    memset(o->tag, 0, sizeof o->tag);
    memset(o->key, 0, sizeof o->key);
    memset(o->pos, 0, sizeof o->pos);
    memset(&o->text, 0, sizeof o->text);
    memset(&o->finish, 0, sizeof o->finish);
    memset(&o->errmsg, 0, sizeof o->errmsg);
    o->arena_len = 0;
    o->has_error = o->has_usage = o->doc_err = 0;
    o->usage_in = o->usage_out = 0;
    o->ntd = 0;

    lim.max_depth = CANLIN_LLM_JSON_DEPTH;
    lim.max_string = CANLIN_LLM_JSON_STRING_MAX;
    lim.max_number = CANLIN_LLM_JSON_NUMBER_MAX;
    lim.max_total = CANLIN_LLM_SSE_DATA_MAX;
    canlin_json_init(&o->js, &lim, o->jbuf, sizeof o->jbuf, on_tok, o);
    rc = canlin_json_feed(&o->js, data, n);
    if (rc == CANLIN_JSON_OK)
        rc = canlin_json_finish(&o->js);
    if (rc != CANLIN_JSON_OK) {
        json_fail(o, rc);
        return;
    }
    if (o->has_error) {
        char msg[CANLIN_LLM_MSG_MAX];
        if (o->errmsg.set)
            snprintf(msg, sizeof msg, "provider error: %.*s", (int)o->errmsg.len,
                     o->arena + o->errmsg.off);
        else
            snprintf(msg, sizeof msg, "provider error");
        o->done = 1;
        llm_fail(o->r, "unavailable", msg); /* llm_fail sanitises and redacts */
        return;
    }
    if (o->finished && (o->text.len || o->ntd)) {
        fail_proto(o, "content after finish_reason");
        return;
    }
    if (o->text.set && o->text.len) {
        if (!llm_utf8_valid(o->arena + o->text.off, o->text.len)) { /* D5 */
            fail_proto(o, "text is not valid UTF-8");
            return;
        }
        if (llm_emit_text(o->r, o->arena + o->text.off, o->text.len) != 0) {
            o->done = 1;
            return;
        }
    }
    for (i = 0; i < o->ntd; i++)
        if (merge_delta(o, &o->td[i]) != 0)
            return;
    if (o->has_usage && llm_emit_usage(o->r, o->usage_in, o->usage_out) != 0) {
        o->done = 1;
        return;
    }
    if (o->finish.set) {
        o->finished = 1;
        if (span_is(o, &o->finish, "tool_calls")) {
            emit_tools(o);
        } else if (any_tool(o)) {
            fail_proto(o, "tool call deltas without finish_reason tool_calls");
        } else if (span_is(o, &o->finish, "stop")) {
        } else if (span_is(o, &o->finish, "length")) {
        } else {
            char msg[CANLIN_LLM_MSG_MAX];
            snprintf(msg, sizeof msg, "provider stopped: %.*s",
                     (int)(o->finish.len > 32 ? 32 : o->finish.len), o->arena + o->finish.off);
            o->done = 1;
            llm_fail(o->r, "unavailable", msg);
            return;
        }
        if (!llm_is_terminal(o->r)) {
            /* stop kind for [DONE]: 1 end, 2 tool, 3 length */
            o->finished = span_is(o, &o->finish, "tool_calls") ? 2
                          : span_is(o, &o->finish, "length")   ? 3
                                                               : 1;
        }
    }
}

static int on_event(void *ud, const canlin_sse_event *ev)
{
    oa *o = ud;
    if (o->done || llm_is_terminal(o->r))
        return 1;
    if (ev->type_len == 5 && memcmp(ev->type, "error", 5) == 0) {
        fail_proto(o, "SSE error event");
        return 1;
    }
    if (!(ev->type_len == 7 && memcmp(ev->type, "message", 7) == 0))
        return 0; /* other event types carry nothing for this dialect */
    if (ev->data_len == 0)
        return 0;
    if (ev->data_len == 6 && memcmp(ev->data, "[DONE]", 6) == 0) {
        o->done = 1;
        if (!o->finished)
            fail_proto(o, "[DONE] without finish_reason");
        else
            llm_succeed(o->r, o->finished == 2 ? "tool" : o->finished == 3 ? "length" : "end");
        return 1;
    }
    on_data(o, ev->data, ev->data_len);
    return (o->done || llm_is_terminal(o->r)) ? 1 : 0;
}

static void sse_fail(oa *o, int rc)
{
    char msg[CANLIN_LLM_MSG_MAX];
    if (rc == CANLIN_SSE_E_LINE || rc == CANLIN_SSE_E_DATA) {
        snprintf(msg, sizeof msg, "SSE record exceeds a limit: %s", canlin_sse_strerror(rc));
        fail_full(o, msg);
        return;
    }
    snprintf(msg, sizeof msg, "SSE framing: %s", canlin_sse_strerror(rc));
    fail_proto(o, msg);
}

static void *oa_begin(llm_req *r)
{
    oa *o = calloc(1, sizeof *o);
    size_t mem = CANLIN_SSE_MEM(CANLIN_LLM_SSE_LINE_MAX, CANLIN_LLM_SSE_DATA_MAX);
    if (o == NULL)
        return NULL;
    o->r = r;
    o->ssemem = malloc(mem);
    o->arena = malloc(CANLIN_LLM_SSE_DATA_MAX);
    if (o->ssemem == NULL || o->arena == NULL ||
        canlin_sse_init(&o->sse, o->ssemem, mem, CANLIN_LLM_SSE_LINE_MAX,
                        CANLIN_LLM_SSE_DATA_MAX, on_event, o) != CANLIN_SSE_OK) {
        free(o->ssemem);
        free(o->arena);
        free(o);
        return NULL;
    }
    return o;
}

static void oa_feed(void *st, const void *buf, size_t n)
{
    oa *o = st;
    int rc;
    if (o->done || llm_is_terminal(o->r))
        return;
    rc = canlin_sse_feed(&o->sse, buf, n);
    if (rc != CANLIN_SSE_OK && !o->done && !llm_is_terminal(o->r))
        sse_fail(o, rc);
}

static void oa_eof(void *st)
{
    oa *o = st;
    int rc;
    if (o->done || llm_is_terminal(o->r))
        return;
    rc = canlin_sse_finish(&o->sse);
    if (o->done || llm_is_terminal(o->r))
        return;
    if (rc != CANLIN_SSE_OK) {
        sse_fail(o, rc);
        return;
    }
    fail_proto(o, o->finished ? "stream ended before [DONE]"
                              : "stream ended without finish_reason and [DONE]");
}

static void oa_end(void *st)
{
    oa *o = st;
    size_t i;
    if (o == NULL)
        return;
    for (i = 0; i < CANLIN_LLM_TOOL_CALLS_MAX; i++)
        free(o->slot[i].args);
    free(o->ssemem);
    free(o->arena);
    free(o);
}

const llm_dialect llm_dialect_openai_chat = {
    CANLIN_LLM_DIALECT_OPENAI_CHAT, "openai-chat", "/chat/completions", oa_build, NULL,
    oa_begin, oa_feed, oa_eof, oa_end};
