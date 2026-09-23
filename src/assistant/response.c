/* CanLin assistant response gate. See include/canlin/assistant_response.h for the
 * contract (LLM providers spec §2.3: tool calls act only after done ok = true).
 *
 * Held calls live in one heap block each (id, name, args back to back), bounded by the
 * LLM limits, so a response holds at most CANLIN_LLM_TOOL_CALLS_MAX *
 * (CANLIN_LLM_TOOL_ID_MAX + CANLIN_LLM_TOOL_NAME_MAX + CANLIN_LLM_TOOL_ARGS_MAX) bytes.
 */
#include "canlin/assistant_response.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct canlin_assistant_response {
    uint64_t gen, turn;
    canlin_ar_outcome out;
    canlin_ar_call calls[CANLIN_LLM_TOOL_CALLS_MAX];
    char *mem[CANLIN_LLM_TOOL_CALLS_MAX];
    unsigned ncalls;
};

canlin_assistant_response *canlin_assistant_response_new(void)
{
    return calloc(1, sizeof(canlin_assistant_response));
}

static void drop_calls(canlin_assistant_response *r)
{
    unsigned i;
    for (i = 0; i < r->ncalls; i++) {
        free(r->mem[i]);
        r->mem[i] = NULL;
    }
    memset(r->calls, 0, sizeof r->calls);
    r->ncalls = 0;
}

void canlin_assistant_response_free(canlin_assistant_response *r)
{
    if (r == NULL)
        return;
    drop_calls(r);
    free(r);
}

int canlin_assistant_response_begin(canlin_assistant_response *r, uint64_t gen,
                                    uint64_t turn)
{
    if (r == NULL || turn == 0)
        return CANLIN_AV_EARG;
    drop_calls(r);
    memset(&r->out, 0, sizeof r->out);
    r->gen = gen;
    r->turn = turn;
    r->out.state = CANLIN_AR_OPEN;
    return CANLIN_AV_OK;
}

static void copy_bounded(char *dst, size_t cap, const char *src)
{
    size_t n = src ? strlen(src) : 0;
    if (n >= cap)
        n = cap - 1;
    if (n)
        memcpy(dst, src, n);
    dst[n] = 0;
}

/* Close the response unsuccessfully: every held call is discarded unexecuted. */
static void close_failed(canlin_assistant_response *r, canlin_ar_state st, const char *code,
                         const char *msg)
{
    r->out.state = st;
    r->out.discarded = r->ncalls;
    r->out.dispatched = 0;
    r->out.partial = r->out.text_bytes > 0 || r->out.calls > 0;
    copy_bounded(r->out.code, sizeof r->out.code, code);
    copy_bounded(r->out.message, sizeof r->out.message, msg);
    drop_calls(r);
}

int canlin_assistant_response_text(canlin_assistant_response *r, size_t n)
{
    if (r == NULL)
        return CANLIN_AV_EARG;
    if (r->out.state != CANLIN_AR_OPEN)
        return CANLIN_AV_ESTALE;
    r->out.text_bytes += n;
    return CANLIN_AV_OK;
}

int canlin_assistant_response_tool(canlin_assistant_response *r, const char *id,
                                   size_t id_len, const char *name, size_t name_len,
                                   const char *args, size_t args_len)
{
    canlin_ar_call *c;
    char *m;
    if (r == NULL)
        return CANLIN_AV_EARG;
    if (r->out.state != CANLIN_AR_OPEN)
        return CANLIN_AV_ESTALE; /* late: never held, never acted on */
    if (id == NULL || id_len == 0 || name == NULL || name_len == 0 ||
        (args == NULL && args_len))
        return CANLIN_AV_EARG;
    r->out.calls++;
    if (r->ncalls >= CANLIN_LLM_TOOL_CALLS_MAX || id_len > CANLIN_LLM_TOOL_ID_MAX ||
        name_len > CANLIN_LLM_TOOL_NAME_MAX || args_len > CANLIN_LLM_TOOL_ARGS_MAX) {
        close_failed(r, CANLIN_AR_FAILED, "full", "tool call exceeds an assistant bound");
        return CANLIN_AV_EFULL;
    }
    m = malloc(id_len + name_len + args_len + 3);
    if (m == NULL) {
        close_failed(r, CANLIN_AR_FAILED, "unavailable", "out of memory holding a tool call");
        return CANLIN_AV_ENOMEM;
    }
    c = &r->calls[r->ncalls];
    memset(c, 0, sizeof *c);
    memcpy(m, id, id_len);
    m[id_len] = 0;
    memcpy(m + id_len + 1, name, name_len);
    m[id_len + 1 + name_len] = 0;
    if (args_len)
        memcpy(m + id_len + name_len + 2, args, args_len);
    m[id_len + name_len + 2 + args_len] = 0;
    c->id = m;
    c->id_len = id_len;
    c->name = m + id_len + 1;
    c->name_len = name_len;
    c->args = m + id_len + name_len + 2;
    c->args_len = args_len;
    r->mem[r->ncalls++] = m;
    return CANLIN_AV_OK;
}

static void unknown_tool(canlin_av_result *res)
{
    memset(res, 0, sizeof *res);
    res->status = CANLIN_AV_EINVALID;
    res->err.status = CANLIN_AV_EINVALID;
    snprintf(res->err.message, sizeof res->err.message, "unknown tool");
}

int canlin_assistant_response_finish(canlin_assistant_response *r,
                                     canlin_assistant_view *av, int ok, const char *code,
                                     const char *message)
{
    unsigned i;
    if (r == NULL)
        return CANLIN_AV_EARG;
    if (r->out.state != CANLIN_AR_OPEN)
        return CANLIN_AV_ESTALE; /* exactly once: a repeated done acts on nothing */
    if (!ok) {
        close_failed(r, CANLIN_AR_FAILED, code ? code : "unavailable", message);
        return CANLIN_AV_OK;
    }
    if (r->ncalls && av == NULL)
        return CANLIN_AV_EARG; /* still open: the caller can retry with its view */
    r->out.state = CANLIN_AR_COMPLETE;
    for (i = 0; i < r->ncalls; i++) {
        canlin_ar_call *c = &r->calls[i];
        if (c->name_len == 8 && memcmp(c->name, "set_view", 8) == 0) {
            canlin_av_call call;
            call.gen = r->gen;
            call.turn = r->turn;
            call.id = c->id;
            call.id_len = c->id_len;
            canlin_assistant_view_set_view(av, &call, c->args, c->args_len, &c->res);
            c->dispatched = 1;
            r->out.dispatched++;
        } else
            unknown_tool(&c->res); /* only core tools exist; nothing acts on it */
    }
    return CANLIN_AV_OK;
}

int canlin_assistant_response_finish_length(canlin_assistant_response *r,
                                            canlin_assistant_view *av, unsigned dropped_tools)
{
    int rc = canlin_assistant_response_finish(r, av, 1, NULL, NULL);
    if (rc == CANLIN_AV_OK) {
        r->out.truncated = 1;
        r->out.dropped = dropped_tools;
    }
    return rc;
}

int canlin_assistant_response_cancel(canlin_assistant_response *r)
{
    if (r == NULL)
        return CANLIN_AV_EARG;
    if (r->out.state != CANLIN_AR_OPEN)
        return CANLIN_AV_ESTALE;
    close_failed(r, CANLIN_AR_CANCELLED, "cancelled", "cancelled");
    return CANLIN_AV_OK;
}

const canlin_ar_outcome *canlin_assistant_response_outcome(const canlin_assistant_response *r)
{
    return r ? &r->out : NULL;
}

unsigned canlin_assistant_response_ncalls(const canlin_assistant_response *r)
{
    return r ? r->ncalls : 0;
}

const canlin_ar_call *canlin_assistant_response_call(const canlin_assistant_response *r,
                                                     unsigned i)
{
    return r && i < r->ncalls ? &r->calls[i] : NULL;
}
