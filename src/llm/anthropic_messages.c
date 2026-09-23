/* anthropic-messages dialect (LLM providers spec §2.1, §2.3).
 *
 * Request: POST {base}/v1/messages, "stream": true, header anthropic-version, the
 * credential as x-api-key (llm.c, bound to the profile origin). System messages become
 * the top-level "system" string (joined with a blank line, as Langertha does); the
 * others become alternating user/assistant messages: consecutive messages of one side are
 * merged, tool results are tool_result blocks placed before any text of their user
 * message, assistant tool calls are tool_use blocks whose "input" is the caller's args
 * embedded raw after checking they are exactly one JSON object.
 *
 * Response: SSE events whose data is one JSON document with a "type". As in openai-chat,
 * a document's effects are collected first and applied only once it parsed whole.
 * Content blocks are strictly sequential (one open block, increasing index). A tool_use
 * block is delivered at its content_block_stop, only when its accumulated input_json_delta
 * fragments parse as one complete JSON object. Success needs message_stop after a
 * message_delta with stop_reason; everything else ends as a failure.
 *
 * Length stop (spec §2.3, decided 2026-09-23): a tool_use block whose input is not one
 * complete JSON object at its content_block_stop is held back as unfinished. It is dropped
 * and counted in the result's dropped_tools only when the next thing is the message_delta
 * with stop_reason "max_tokens"; anything else after it (another block, another stop
 * reason, EOF) fails the stream as before. A tool_use block still open at the max_tokens
 * message_delta is dropped the same way. Tool calls completed before are delivered.
 */
#include "llm_int.h"
#include "canlin/json.h"
#include "canlin/sse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Messages API version sent with every request (Langertha AnthropicCompatible.pm). */
#define AM_VERSION "2023-06-01"

static const canlin_llm_header am_headers[] = {{"anthropic-version", AM_VERSION},
                                               {NULL, NULL}};

/* ---- request ---- */

static int user_side(int role)
{
    return role == LLM_ROLE_USER || role == LLM_ROLE_TOOL;
}

/* All system texts as one JSON string, separated by a blank line. */
static void system_text(llm_jw *w, const llm_build *b)
{
    size_t i;
    int first = 1;
    llm_jw_raw(w, "\"", 1);
    for (i = 0; i < b->nmsgs; i++) {
        if (b->msgs[i].role != LLM_ROLE_SYSTEM)
            continue;
        if (!first)
            llm_jw_raw(w, "\\n\\n", 4);
        llm_jw_esc(w, b->msgs[i].text.s, b->msgs[i].text.n);
        first = 0;
    }
    llm_jw_raw(w, "\"", 1);
}

static void sep(llm_jw *w, size_t *n)
{
    if ((*n)++)
        llm_jw_raw(w, ",", 1);
}

static void text_block(llm_jw *w, const llm_str *t)
{
    llm_jw_lit(w, "{\"type\":\"text\",\"text\":");
    llm_jw_str(w, t->s, t->n);
    llm_jw_raw(w, "}", 1);
}

static void tool_use_block(llm_jw *w, const llm_call *c, char *vbuf)
{
    /* args go into the body unquoted: anything but one JSON object would let the caller
     * reshape the request, so it is refused. */
    if (!llm_json_object_valid(vbuf, CANLIN_LLM_JSON_STRING_MAX, c->args.s, c->args.n,
                               CANLIN_LLM_TOOL_ARGS_MAX)) {
        if (!w->err)
            w->err = JW_EINVAL;
        return;
    }
    llm_jw_lit(w, "{\"type\":\"tool_use\",\"id\":");
    llm_jw_str(w, c->id.s, c->id.n);
    llm_jw_lit(w, ",\"name\":");
    llm_jw_str(w, c->name.s, c->name.n);
    llm_jw_lit(w, ",\"input\":");
    llm_jw_raw(w, c->args.s, c->args.n);
    llm_jw_raw(w, "}", 1);
}

/* One API message from msgs[a..e): one side, system messages inside skipped. */
static void run(llm_jw *w, const llm_build *b, size_t a, size_t e, char *vbuf)
{
    int user = user_side(b->msgs[a].role);
    const llm_msg *only = NULL;
    size_t i, k, nmsg = 0, nb = 0;

    for (i = a; i < e; i++)
        if (b->msgs[i].role != LLM_ROLE_SYSTEM) {
            nmsg++;
            only = &b->msgs[i];
        }
    llm_jw_lit(w, user ? "{\"role\":\"user\",\"content\":" : "{\"role\":\"assistant\",\"content\":");
    if (nmsg == 1 && only->role != LLM_ROLE_TOOL && only->ncalls == 0) {
        llm_jw_str(w, only->text.s, only->text.n);
        llm_jw_raw(w, "}", 1);
        return;
    }
    llm_jw_raw(w, "[", 1);
    if (user) {
        /* tool_result blocks come first in their user message, text after them */
        for (i = a; i < e; i++) {
            const llm_msg *m = &b->msgs[i];
            if (m->role != LLM_ROLE_TOOL)
                continue;
            sep(w, &nb);
            llm_jw_lit(w, "{\"type\":\"tool_result\",\"tool_use_id\":");
            llm_jw_str(w, m->tool_call_id.s, m->tool_call_id.n);
            llm_jw_lit(w, ",\"content\":");
            llm_jw_str(w, m->text.s, m->text.n);
            llm_jw_raw(w, "}", 1);
        }
        for (i = a; i < e; i++)
            if (b->msgs[i].role == LLM_ROLE_USER && b->msgs[i].text.n) {
                sep(w, &nb);
                text_block(w, &b->msgs[i].text);
            }
    } else {
        for (i = a; i < e; i++) {
            const llm_msg *m = &b->msgs[i];
            if (m->role != LLM_ROLE_ASSISTANT)
                continue;
            if (m->has_text && m->text.n) { /* the API refuses empty text blocks */
                sep(w, &nb);
                text_block(w, &m->text);
            }
            for (k = 0; k < m->ncalls; k++) {
                sep(w, &nb);
                tool_use_block(w, &m->calls[k], vbuf);
            }
        }
    }
    if (nb == 0) { /* only empty texts: keep the message, the provider judges it */
        llm_str empty = {"", 0};
        text_block(w, &empty);
    }
    llm_jw_raw(w, "]}", 2);
}

static void am_build(llm_jw *w, const llm_build *b)
{
    size_t i, j, nsys = 0, n = 0, ncalls = 0;
    char *vbuf = NULL;

    for (i = 0; i < b->nmsgs; i++) {
        if (b->msgs[i].role == LLM_ROLE_SYSTEM)
            nsys++;
        ncalls += b->msgs[i].ncalls;
    }
    if (ncalls) {
        vbuf = malloc(CANLIN_LLM_JSON_STRING_MAX);
        if (vbuf == NULL) {
            w->err = JW_ENOMEM;
            return;
        }
    }
    llm_jw_lit(w, "{\"model\":");
    llm_jw_str(w, b->model, strlen(b->model));
    llm_jw_lit(w, ",\"max_tokens\":"); /* required by the Messages API */
    llm_jw_uint(w, b->max_tokens ? b->max_tokens : CANLIN_LLM_ANTHROPIC_MAX_TOKENS_DEFAULT);
    llm_jw_lit(w, ",\"stream\":true");
    if (nsys && nsys < b->nmsgs) {
        llm_jw_lit(w, ",\"system\":");
        system_text(w, b);
    }
    llm_jw_lit(w, ",\"messages\":[");
    if (nsys == b->nmsgs) { /* system only: sent as the user turn (Langertha) */
        llm_jw_lit(w, "{\"role\":\"user\",\"content\":");
        system_text(w, b);
        llm_jw_raw(w, "}", 1);
    } else {
        for (i = 0; i < b->nmsgs && b->msgs[i].role == LLM_ROLE_SYSTEM; i++)
            ;
        while (i < b->nmsgs) {
            int side = user_side(b->msgs[i].role);
            for (j = i + 1; j < b->nmsgs; j++)
                if (b->msgs[j].role != LLM_ROLE_SYSTEM && user_side(b->msgs[j].role) != side)
                    break;
            sep(w, &n);
            run(w, b, i, j, vbuf);
            i = j;
        }
    }
    llm_jw_raw(w, "]", 1);
    if (b->ntools) {
        llm_jw_lit(w, ",\"tools\":[");
        for (i = 0; i < b->ntools; i++) {
            const llm_tool *t = b->tools[i];
            if (i)
                llm_jw_raw(w, ",", 1);
            llm_jw_lit(w, "{\"name\":");
            llm_jw_str(w, t->name, strlen(t->name));
            llm_jw_lit(w, ",\"description\":");
            llm_jw_str(w, t->description, strlen(t->description));
            llm_jw_lit(w, ",\"input_schema\":");
            llm_jw_lit(w, t->schema_json); /* core data, valid JSON */
            llm_jw_raw(w, "}", 1);
        }
        llm_jw_raw(w, "]", 1);
    }
    llm_jw_raw(w, "}", 1);
    free(vbuf);
}

/* ---- response ---- */

enum { TG_NONE = 0, TG_ROOT, TG_MESSAGE, TG_MUSAGE, TG_BLOCK, TG_BINPUT, TG_DELTA, TG_USAGE,
       TG_ERROR };

enum { K_OTHER = 0, K_TYPE, K_INDEX, K_MESSAGE, K_USAGE, K_INPUT_TOKENS, K_OUTPUT_TOKENS,
       K_CONTENT_BLOCK, K_ID, K_NAME, K_TEXT, K_INPUT, K_DELTA, K_PARTIAL_JSON, K_STOP_REASON,
       K_ERROR };

static const struct {
    const char *s;
    int k;
} keys[] = {{"type", K_TYPE},
            {"index", K_INDEX},
            {"message", K_MESSAGE},
            {"usage", K_USAGE},
            {"input_tokens", K_INPUT_TOKENS},
            {"output_tokens", K_OUTPUT_TOKENS},
            {"content_block", K_CONTENT_BLOCK},
            {"id", K_ID},
            {"name", K_NAME},
            {"text", K_TEXT},
            {"input", K_INPUT},
            {"delta", K_DELTA},
            {"partial_json", K_PARTIAL_JSON},
            {"stop_reason", K_STOP_REASON},
            {"error", K_ERROR},
            {NULL, 0}};

typedef struct span {
    size_t off, len;
    int set;
} span;

/* Open block kinds. */
enum { BK_NONE = 0, BK_TEXT, BK_TOOL, BK_OTHER };
/* Stop kinds, as openai-chat: 1 end, 2 tool, 3 length. */
enum { ST_NONE = 0, ST_END, ST_TOOL, ST_LENGTH };

typedef struct am {
    llm_req *r;
    canlin_sse sse;
    void *ssemem;
    canlin_json js;
    char jbuf[CANLIN_LLM_JSON_STRING_MAX];

    /* per document */
    char *arena; /* CANLIN_LLM_SSE_DATA_MAX */
    size_t arena_len;
    int tag[CANLIN_LLM_JSON_DEPTH + 2];
    int key[CANLIN_LLM_JSON_DEPTH + 2];
    span type, btype, bid, bname, btext, dtype, dtext, djson, stop, etype, emsg;
    long index; /* -1 = absent */
    int has_index, binput_nonempty;
    int64_t in_tok, out_tok;
    int has_in, has_out;

    /* per response */
    int started;       /* message_start seen */
    int stopped;       /* ST_* from message_delta.stop_reason */
    int done;          /* terminal */
    int open;          /* BK_* */
    long open_index, last_index;
    size_t tools_started, tools_done;
    int64_t usage_in;
    int has_usage_in;
    /* the open tool_use block */
    char tid[CANLIN_LLM_TOOL_ID_MAX];
    size_t tidn;
    char tname[CANLIN_LLM_TOOL_NAME_MAX];
    size_t tnamen;
    int tinput_empty; /* content_block_start carried "input": {} (or none) */
    int unfinished;   /* a stopped tool_use block with incomplete input, held back */
    unsigned dropped; /* unfinished tool calls dropped at the max_tokens stop */
    char *args;
    size_t argsn, argscap;
} am;

static int key_id(const char *s, size_t n)
{
    int i;
    for (i = 0; keys[i].s; i++)
        if (strlen(keys[i].s) == n && memcmp(keys[i].s, s, n) == 0)
            return keys[i].k;
    return K_OTHER;
}

static int keep(am *o, span *sp, const char *s, size_t n)
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

/* usage.input_tokens / output_tokens (message_start's message.usage or message_delta's) */
static void tokens(am *o, int k, const canlin_json_token *t)
{
    long v;
    if (t->type != CANLIN_JSON_NUMBER || parse_long(t->ptr, t->len, &v) != 0)
        return;
    if (k == K_INPUT_TOKENS) {
        o->in_tok = v;
        o->has_in = 1;
    } else if (k == K_OUTPUT_TOKENS) {
        o->out_tok = v;
        o->has_out = 1;
    }
}

/* Token callback: record the effects of one event document. */
static int on_tok(void *ud, const canlin_json_token *t)
{
    am *o = ud;
    unsigned d = t->depth;
    int parent, k;

    if (d > CANLIN_LLM_JSON_DEPTH)
        return 1; /* the tokenizer limit makes this unreachable */
    if (t->type == CANLIN_JSON_KEY) {
        o->key[d] = key_id(t->ptr, t->len);
        if (o->tag[d] == TG_BINPUT)
            o->binput_nonempty = 1;
        return 0;
    }
    if (t->type == CANLIN_JSON_OBJ_END || t->type == CANLIN_JSON_ARR_END) {
        o->tag[d + 1] = TG_NONE;
        return 0;
    }
    if (d == 0) {
        if (t->type != CANLIN_JSON_OBJ_BEGIN)
            return 1; /* an event document must be an object */
        o->tag[1] = TG_ROOT;
        o->key[1] = K_OTHER;
        return 0;
    }
    parent = o->tag[d];
    k = o->key[d];
    if (t->type == CANLIN_JSON_OBJ_BEGIN || t->type == CANLIN_JSON_ARR_BEGIN) {
        int nt = TG_NONE, obj = t->type == CANLIN_JSON_OBJ_BEGIN;
        if (parent == TG_ROOT && obj) {
            nt = k == K_MESSAGE         ? TG_MESSAGE
                 : k == K_CONTENT_BLOCK ? TG_BLOCK
                 : k == K_DELTA         ? TG_DELTA
                 : k == K_USAGE         ? TG_USAGE
                 : k == K_ERROR         ? TG_ERROR
                                        : TG_NONE;
        } else if (parent == TG_MESSAGE && k == K_USAGE && obj)
            nt = TG_MUSAGE;
        else if (parent == TG_BLOCK && k == K_INPUT) {
            if (obj)
                nt = TG_BINPUT;
            else
                o->binput_nonempty = 1; /* not an object: never "{}" */
        }
        o->tag[d + 1] = nt;
        o->key[d + 1] = K_OTHER;
        return 0;
    }
    /* scalar value */
    switch (parent) {
    case TG_ROOT:
        if (k == K_TYPE && t->type == CANLIN_JSON_STRING)
            return keep(o, &o->type, t->ptr, t->len);
        if (k == K_INDEX && t->type == CANLIN_JSON_NUMBER) {
            o->has_index = 1;
            if (parse_long(t->ptr, t->len, &o->index) != 0)
                o->index = -1; /* not a small non-negative integer: rejected below */
        }
        break;
    case TG_MUSAGE:
    case TG_USAGE:
        tokens(o, k, t);
        break;
    case TG_BLOCK:
        if (t->type != CANLIN_JSON_STRING) {
            if (k == K_INPUT)
                o->binput_nonempty = 1;
            break;
        }
        if (k == K_TYPE)
            return keep(o, &o->btype, t->ptr, t->len);
        if (k == K_ID)
            return keep(o, &o->bid, t->ptr, t->len);
        if (k == K_NAME)
            return keep(o, &o->bname, t->ptr, t->len);
        if (k == K_TEXT)
            return keep(o, &o->btext, t->ptr, t->len);
        if (k == K_INPUT)
            o->binput_nonempty = 1;
        break;
    case TG_DELTA:
        if (t->type != CANLIN_JSON_STRING)
            break;
        if (k == K_TYPE)
            return keep(o, &o->dtype, t->ptr, t->len);
        if (k == K_TEXT)
            return keep(o, &o->dtext, t->ptr, t->len);
        if (k == K_PARTIAL_JSON)
            return keep(o, &o->djson, t->ptr, t->len);
        if (k == K_STOP_REASON)
            return keep(o, &o->stop, t->ptr, t->len);
        break;
    case TG_ERROR:
        if (t->type != CANLIN_JSON_STRING)
            break;
        if (k == K_TYPE)
            return keep(o, &o->etype, t->ptr, t->len);
        if (k == K_MESSAGE)
            return keep(o, &o->emsg, t->ptr, t->len);
        break;
    default:
        break;
    }
    return 0;
}

static int span_is(const am *o, const span *sp, const char *lit)
{
    size_t n = strlen(lit);
    return sp->set && sp->len == n && memcmp(o->arena + sp->off, lit, n) == 0;
}

static void fail_full(am *o, const char *what)
{
    o->done = 1;
    llm_fail(o->r, "full", what);
}

static void fail_proto(am *o, const char *what)
{
    char msg[2 * CANLIN_LLM_MSG_MAX];
    snprintf(msg, sizeof msg, "anthropic-messages protocol error: %s", what);
    o->done = 1;
    llm_fail(o->r, "unavailable", msg);
}

static void json_fail(am *o, int rc)
{
    char msg[CANLIN_LLM_MSG_MAX];
    if (rc == CANLIN_JSON_E_STRING_LEN || rc == CANLIN_JSON_E_DEPTH ||
        rc == CANLIN_JSON_E_TOTAL || rc == CANLIN_JSON_E_NUMBER_LEN) {
        snprintf(msg, sizeof msg, "stream event exceeds a JSON limit: %s",
                 canlin_json_strerror(rc));
        fail_full(o, msg);
        return;
    }
    snprintf(msg, sizeof msg, "malformed JSON event: %s", canlin_json_strerror(rc));
    fail_proto(o, msg);
}

/* error event: core API §3.1 code from the provider's error type (spec §4). */
static void provider_error(am *o)
{
    char msg[CANLIN_LLM_MSG_MAX];
    const char *code = "unavailable";
    if (span_is(o, &o->etype, "authentication_error") || span_is(o, &o->etype, "permission_error"))
        code = "denied";
    else if (span_is(o, &o->etype, "not_found_error") ||
             span_is(o, &o->etype, "invalid_request_error"))
        code = "unsupported";
    else if (span_is(o, &o->etype, "request_too_large"))
        code = "full";
    if (o->emsg.set)
        snprintf(msg, sizeof msg, "provider error: %.*s", (int)o->emsg.len,
                 o->arena + o->emsg.off);
    else
        snprintf(msg, sizeof msg, "provider error");
    o->done = 1;
    llm_fail(o->r, code, msg); /* llm_fail sanitises and redacts */
}

static int emit_text(am *o, const span *sp)
{
    if (!sp->set || sp->len == 0)
        return 0;
    if (!llm_utf8_valid(o->arena + sp->off, sp->len)) { /* D5 */
        fail_proto(o, "text is not valid UTF-8");
        return -1;
    }
    if (llm_emit_text(o->r, o->arena + sp->off, sp->len) != 0) {
        o->done = 1;
        return -1;
    }
    return 0;
}

static int append_args(am *o, const char *p, size_t n)
{
    if (n > CANLIN_LLM_TOOL_ARGS_MAX - o->argsn)
        return -1;
    if (o->argsn + n > o->argscap) {
        size_t cap = o->argscap ? o->argscap : 256;
        char *nb;
        while (cap < o->argsn + n)
            cap *= 2;
        if (cap > CANLIN_LLM_TOOL_ARGS_MAX)
            cap = CANLIN_LLM_TOOL_ARGS_MAX;
        nb = realloc(o->args, cap);
        if (nb == NULL)
            return -2;
        o->args = nb;
        o->argscap = cap;
    }
    memcpy(o->args + o->argsn, p, n);
    o->argsn += n;
    return 0;
}

static void unfinished_fail(am *o)
{
    fail_proto(o, "tool call arguments are not one complete JSON object");
}

static void block_start(am *o)
{
    if (o->unfinished) { /* only the length stop may follow an unfinished tool call */
        unfinished_fail(o);
        return;
    }
    if (o->open != BK_NONE) {
        fail_proto(o, "content_block_start while a block is open");
        return;
    }
    if (!o->has_index || o->index < 0 || o->index <= o->last_index) {
        fail_proto(o, "content_block_start without a new increasing index");
        return;
    }
    o->last_index = o->open_index = o->index;
    if (span_is(o, &o->btype, "text")) {
        o->open = BK_TEXT;
        emit_text(o, &o->btext);
    } else if (span_is(o, &o->btype, "tool_use")) {
        if (o->tools_started >= CANLIN_LLM_TOOL_CALLS_MAX) {
            fail_full(o, "more tool calls than CANLIN_LLM_TOOL_CALLS_MAX");
            return;
        }
        if (!o->bname.set || o->bname.len == 0 ||
            !llm_utf8_valid(o->arena + o->bname.off, o->bname.len)) {
            fail_proto(o, "tool_use block without a name");
            return;
        }
        if (!o->bid.set || o->bid.len == 0) {
            fail_proto(o, "tool_use block without an id");
            return;
        }
        if (o->bname.len > sizeof o->tname) {
            fail_full(o, "tool name exceeds CANLIN_LLM_TOOL_NAME_MAX");
            return;
        }
        if (o->bid.len > sizeof o->tid) {
            fail_full(o, "tool call id exceeds CANLIN_LLM_TOOL_ID_MAX");
            return;
        }
        o->tools_started++;
        o->open = BK_TOOL;
        memcpy(o->tid, o->arena + o->bid.off, o->bid.len);
        o->tidn = o->bid.len;
        memcpy(o->tname, o->arena + o->bname.off, o->bname.len);
        o->tnamen = o->bname.len;
        o->tinput_empty = !o->binput_nonempty;
        o->argsn = 0;
    } else if (o->btype.set) {
        o->open = BK_OTHER; /* e.g. thinking: never requested, its deltas are skipped */
    } else {
        fail_proto(o, "content_block_start without a block type");
    }
}

static int open_block_matches(am *o, const char *ev)
{
    char msg[80];
    if (o->open == BK_NONE || !o->has_index || o->index != o->open_index) {
        snprintf(msg, sizeof msg, "%s for a block that is not open", ev);
        fail_proto(o, msg);
        return 0;
    }
    return 1;
}

static void block_delta(am *o)
{
    int rc;
    if (!open_block_matches(o, "content_block_delta"))
        return;
    if (span_is(o, &o->dtype, "text_delta")) {
        if (o->open != BK_TEXT) {
            fail_proto(o, "text_delta outside a text block");
            return;
        }
        emit_text(o, &o->dtext);
    } else if (span_is(o, &o->dtype, "input_json_delta")) {
        if (o->open != BK_TOOL) {
            fail_proto(o, "input_json_delta outside a tool_use block");
            return;
        }
        if (!o->djson.set || o->djson.len == 0)
            return;
        rc = append_args(o, o->arena + o->djson.off, o->djson.len);
        if (rc == -1)
            fail_full(o, "tool arguments exceed CANLIN_LLM_TOOL_ARGS_MAX");
        else if (rc != 0) {
            o->done = 1;
            llm_fail(o->r, "unavailable", "out of memory assembling tool arguments");
        }
    } else if (!o->dtype.set) {
        fail_proto(o, "content_block_delta without a delta type");
    }
    /* other delta types (thinking, signature, citations) carry nothing for v1 */
}

static void block_stop(am *o)
{
    const char *args = o->args;
    size_t argsn = o->argsn;
    int kind = o->open;
    if (!open_block_matches(o, "content_block_stop"))
        return;
    o->open = BK_NONE;
    if (kind != BK_TOOL)
        return;
    if (argsn == 0 && o->tinput_empty) { /* a tool without input streams no fragments */
        args = "{}";
        argsn = 2;
    }
    if (!llm_json_object_valid(o->jbuf, sizeof o->jbuf, args ? args : "", argsn,
                               CANLIN_LLM_TOOL_ARGS_MAX)) {
        o->unfinished = 1; /* dropped at a max_tokens stop, else a failure */
        return;
    }
    if (llm_emit_tool(o->r, o->tid, o->tidn, o->tname, o->tnamen, args, argsn) != 0) {
        o->done = 1;
        return;
    }
    o->tools_done++;
}

static void message_delta(am *o)
{
    int st = ST_NONE;
    if (!o->started) {
        fail_proto(o, "message_delta before message_start");
        return;
    }
    if (o->stop.set) {
        if (span_is(o, &o->stop, "end_turn") || span_is(o, &o->stop, "stop_sequence"))
            st = ST_END;
        else if (span_is(o, &o->stop, "tool_use"))
            st = ST_TOOL;
        else if (span_is(o, &o->stop, "max_tokens"))
            st = ST_LENGTH;
    }
    if (o->open != BK_NONE) {
        /* only a tool call cut off by the length stop may still be open (spec §2.3) */
        if (!(o->open == BK_TOOL && st == ST_LENGTH && !o->stopped)) {
            fail_proto(o, "message_delta while a content block is open");
            return;
        }
        o->open = BK_NONE;
        o->unfinished = 1;
    }
    if (o->has_in) {
        o->usage_in = o->in_tok;
        o->has_usage_in = 1;
    }
    if (o->has_out && llm_emit_usage(o->r, o->has_usage_in ? o->usage_in : 0, o->out_tok) != 0) {
        o->done = 1;
        return;
    }
    if (!o->stop.set)
        return; /* stop_reason null: usage only */
    if (o->stopped) {
        fail_proto(o, "second stop_reason");
        return;
    }
    if (st == ST_NONE) {
        char msg[CANLIN_LLM_MSG_MAX];
        snprintf(msg, sizeof msg, "provider stopped: %.*s",
                 (int)(o->stop.len > 32 ? 32 : o->stop.len), o->arena + o->stop.off);
        o->done = 1;
        llm_fail(o->r, "unavailable", msg);
        return;
    }
    if (o->unfinished) {
        if (st != ST_LENGTH) {
            unfinished_fail(o);
            return;
        }
        o->unfinished = 0;
        o->dropped++;
    }
    if (st == ST_TOOL && o->tools_done == 0) {
        fail_proto(o, "stop_reason tool_use without a tool call");
        return;
    }
    if (st == ST_END && o->tools_done) {
        fail_proto(o, "tool_use block without stop_reason tool_use");
        return;
    }
    o->stopped = st;
}

static void on_data(am *o, const char *name, size_t namelen, const char *data, size_t n)
{
    canlin_json_limits lim;
    int rc;

    memset(o->tag, 0, sizeof o->tag);
    memset(o->key, 0, sizeof o->key);
    memset(&o->type, 0, sizeof o->type);
    memset(&o->btype, 0, sizeof o->btype);
    memset(&o->bid, 0, sizeof o->bid);
    memset(&o->bname, 0, sizeof o->bname);
    memset(&o->btext, 0, sizeof o->btext);
    memset(&o->dtype, 0, sizeof o->dtype);
    memset(&o->dtext, 0, sizeof o->dtext);
    memset(&o->djson, 0, sizeof o->djson);
    memset(&o->stop, 0, sizeof o->stop);
    memset(&o->etype, 0, sizeof o->etype);
    memset(&o->emsg, 0, sizeof o->emsg);
    o->arena_len = 0;
    o->index = -1;
    o->has_index = o->binput_nonempty = 0;
    o->in_tok = o->out_tok = 0;
    o->has_in = o->has_out = 0;

    lim.max_depth = CANLIN_LLM_JSON_DEPTH;
    lim.max_string = CANLIN_LLM_JSON_STRING_MAX;
    lim.max_number = CANLIN_LLM_JSON_NUMBER_MAX;
    lim.max_total = CANLIN_LLM_SSE_DATA_MAX;
    canlin_json_init(&o->js, &lim, o->jbuf, sizeof o->jbuf, on_tok, o);
    rc = canlin_json_feed(&o->js, data, n);
    if (rc == CANLIN_JSON_OK)
        rc = canlin_json_finish(&o->js);
    if (rc != CANLIN_JSON_OK) {
        if (namelen == 5 && memcmp(name, "error", 5) == 0) {
            o->done = 1;
            llm_fail(o->r, "unavailable", "provider error event");
            return;
        }
        json_fail(o, rc);
        return;
    }
    if (!o->type.set) {
        fail_proto(o, "event without a type");
        return;
    }
    /* The SSE event name, when given, must agree with the document's type. */
    if (!(namelen == 7 && memcmp(name, "message", 7) == 0) &&
        !(namelen == o->type.len && memcmp(name, o->arena + o->type.off, namelen) == 0)) {
        fail_proto(o, "SSE event name differs from the event type");
        return;
    }
    if (span_is(o, &o->type, "error")) {
        provider_error(o);
        return;
    }
    if (span_is(o, &o->type, "ping"))
        return;
    if (span_is(o, &o->type, "message_start")) {
        if (o->started) {
            fail_proto(o, "second message_start");
            return;
        }
        o->started = 1;
        if (o->has_in) {
            o->usage_in = o->in_tok;
            o->has_usage_in = 1;
        }
        return;
    }
    if (span_is(o, &o->type, "message_stop")) {
        o->done = 1;
        if (o->open != BK_NONE)
            fail_proto(o, "message_stop while a content block is open");
        else if (!o->stopped)
            fail_proto(o, "message_stop without stop_reason");
        else
            llm_succeed(o->r,
                        o->stopped == ST_TOOL     ? "tool"
                        : o->stopped == ST_LENGTH ? "length"
                                                  : "end",
                        o->stopped == ST_LENGTH ? o->dropped : 0);
        return;
    }
    if (span_is(o, &o->type, "message_delta")) {
        message_delta(o);
        return;
    }
    if (span_is(o, &o->type, "content_block_start") ||
        span_is(o, &o->type, "content_block_delta") ||
        span_is(o, &o->type, "content_block_stop")) {
        if (!o->started) {
            fail_proto(o, "content before message_start");
            return;
        }
        if (o->stopped) {
            fail_proto(o, "content after stop_reason");
            return;
        }
        if (span_is(o, &o->type, "content_block_start"))
            block_start(o);
        else if (span_is(o, &o->type, "content_block_delta"))
            block_delta(o);
        else
            block_stop(o);
        return;
    }
    /* Unknown event types are skipped (the Messages API may add some). */
}

static int on_event(void *ud, const canlin_sse_event *ev)
{
    am *o = ud;
    if (o->done || llm_is_terminal(o->r))
        return 1;
    if (ev->data_len == 0) {
        if (ev->type_len == 5 && memcmp(ev->type, "error", 5) == 0) {
            o->done = 1;
            llm_fail(o->r, "unavailable", "provider error event");
            return 1;
        }
        return 0;
    }
    on_data(o, ev->type, ev->type_len, ev->data, ev->data_len);
    return (o->done || llm_is_terminal(o->r)) ? 1 : 0;
}

static void sse_fail(am *o, int rc)
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

static void *am_begin(llm_req *r)
{
    am *o = calloc(1, sizeof *o);
    size_t mem = CANLIN_SSE_MEM(CANLIN_LLM_SSE_LINE_MAX, CANLIN_LLM_SSE_DATA_MAX);
    if (o == NULL)
        return NULL;
    o->r = r;
    o->last_index = o->open_index = -1;
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

static void am_feed(void *st, const void *buf, size_t n)
{
    am *o = st;
    int rc;
    if (o->done || llm_is_terminal(o->r))
        return;
    rc = canlin_sse_feed(&o->sse, buf, n);
    if (rc != CANLIN_SSE_OK && !o->done && !llm_is_terminal(o->r))
        sse_fail(o, rc);
}

static void am_eof(void *st)
{
    am *o = st;
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
    if (o->unfinished) {
        unfinished_fail(o);
        return;
    }
    fail_proto(o, o->stopped ? "stream ended before message_stop"
                             : "stream ended without stop_reason and message_stop");
}

static void am_end(void *st)
{
    am *o = st;
    if (o == NULL)
        return;
    free(o->args);
    free(o->ssemem);
    free(o->arena);
    free(o);
}

const llm_dialect llm_dialect_anthropic_messages = {
    CANLIN_LLM_DIALECT_ANTHROPIC_MESSAGES, "anthropic-messages", "/v1/messages", am_build,
    am_headers, am_begin, am_feed, am_eof, am_end};
