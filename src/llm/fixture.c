/* Fixture transport (LLM providers spec §8, §9): scripted responses, arbitrary chunk
 * splits, injected faults and a record of every outgoing request, including one
 * followed redirect. Credentials are injected per canlin_llm_auth_applies, exactly as a
 * real transport must. No sockets. Owner thread only. */
#include "canlin/llm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct fx_resp {
    canlin_llm_fixture_response r;
    char *ct, *body, *redir;
    struct fx_resp *next;
} fx_resp;

typedef struct fx_stream {
    canlin_llm_fixture *fx;
    fx_resp *resp;
    canlin_llm_stream_cb cb;
    void *ud;
    char *method, *body;
    size_t body_len;
    char *hdrs; /* non-secret header lines, for re-recording on redirect */
    const canlin_llm_auth *auth;
    int phase; /* 0 redirect, 1 status, 2 content-type, 3 body, 4 finished */
    size_t off;
    unsigned rng;
    int closed;
    struct fx_stream *next;
} fx_stream;

struct canlin_llm_fixture {
    fx_resp *qhead, *qtail;
    fx_stream *streams;
    canlin_llm_fixture_record *rec;
    size_t nrec, caprec;
};

static char *dupn(const char *s, size_t n)
{
    char *d = malloc(n + 1);
    if (d) {
        if (n)
            memcpy(d, s, n);
        d[n] = 0;
    }
    return d;
}

static char *dup0(const char *s)
{
    return s ? dupn(s, strlen(s)) : NULL;
}

canlin_llm_fixture *canlin_llm_fixture_new(void)
{
    return calloc(1, sizeof(canlin_llm_fixture));
}

static void resp_free(fx_resp *r)
{
    if (r == NULL)
        return;
    free(r->ct);
    free(r->body);
    free(r->redir);
    free(r);
}

static void stream_free(fx_stream *s)
{
    resp_free(s->resp);
    free(s->method);
    free(s->body);
    free(s->hdrs);
    free(s);
}

void canlin_llm_fixture_free(canlin_llm_fixture *fx)
{
    size_t i;
    if (fx == NULL)
        return;
    while (fx->qhead) {
        fx_resp *n = fx->qhead->next;
        resp_free(fx->qhead);
        fx->qhead = n;
    }
    while (fx->streams) {
        fx_stream *n = fx->streams->next;
        stream_free(fx->streams);
        fx->streams = n;
    }
    for (i = 0; i < fx->nrec; i++) {
        free(fx->rec[i].method);
        free(fx->rec[i].url);
        free(fx->rec[i].headers);
        free(fx->rec[i].body);
    }
    free(fx->rec);
    free(fx);
}

int canlin_llm_fixture_push(canlin_llm_fixture *fx, const canlin_llm_fixture_response *r)
{
    fx_resp *q = calloc(1, sizeof *q);
    if (q == NULL)
        return CANLIN_LLM_ENOMEM;
    q->r = *r;
    q->ct = dup0(r->content_type ? r->content_type : "text/event-stream");
    q->body = dupn(r->body ? r->body : "", r->body ? r->body_len : 0);
    q->redir = dup0(r->redirect_to);
    q->r.body_len = r->body ? r->body_len : 0;
    if (q->ct == NULL || q->body == NULL || (r->redirect_to && q->redir == NULL)) {
        resp_free(q);
        return CANLIN_LLM_ENOMEM;
    }
    if (fx->qtail)
        fx->qtail->next = q;
    else
        fx->qhead = q;
    fx->qtail = q;
    return CANLIN_LLM_OK;
}

/* Record one outgoing request; the credential is added only where it applies. */
static void record(canlin_llm_fixture *fx, const fx_stream *s, const char *url)
{
    canlin_llm_fixture_record *r;
    size_t hl = strlen(s->hdrs);
    int inject = canlin_llm_auth_applies(s->auth, url);
    char *h;

    if (fx->nrec == fx->caprec) {
        size_t cap = fx->caprec ? fx->caprec * 2 : 8;
        canlin_llm_fixture_record *nr = realloc(fx->rec, cap * sizeof *nr);
        if (nr == NULL)
            return;
        fx->rec = nr;
        fx->caprec = cap;
    }
    r = &fx->rec[fx->nrec];
    memset(r, 0, sizeof *r);
    h = malloc(hl + (inject ? strlen(s->auth->header) + strlen(s->auth->prefix) +
                                  s->auth->secret_len + 4
                            : 0) + 1);
    if (h == NULL)
        return;
    memcpy(h, s->hdrs, hl);
    if (inject) {
        size_t o = hl, n;
        n = strlen(s->auth->header);
        memcpy(h + o, s->auth->header, n);
        o += n;
        memcpy(h + o, ": ", 2);
        o += 2;
        n = strlen(s->auth->prefix);
        memcpy(h + o, s->auth->prefix, n);
        o += n;
        memcpy(h + o, s->auth->secret, s->auth->secret_len);
        o += s->auth->secret_len;
        h[o++] = '\n';
        hl = o;
    }
    h[hl] = 0;
    r->headers = h;
    r->method = dup0(s->method);
    r->url = dup0(url);
    r->body = dupn(s->body, s->body_len);
    r->body_len = s->body_len;
    fx->nrec++;
}

static void *fx_open(void *ctx, const canlin_llm_http_request *req, const canlin_llm_stream_cb *cb,
                     void *ud, const char **msg)
{
    canlin_llm_fixture *fx = ctx;
    fx_stream *s;
    size_t i, hl = 0;

    s = calloc(1, sizeof *s);
    if (s == NULL) {
        *msg = "fixture: out of memory";
        return NULL;
    }
    for (i = 0; i < req->nheaders; i++)
        hl += strlen(req->headers[i].name) + strlen(req->headers[i].value) + 3;
    s->hdrs = malloc(hl + 1);
    s->method = dup0(req->method);
    s->body = dupn(req->body, req->body_len);
    if (s->hdrs == NULL || s->method == NULL || s->body == NULL) {
        stream_free(s);
        *msg = "fixture: out of memory";
        return NULL;
    }
    s->hdrs[0] = 0;
    for (i = 0, hl = 0; i < req->nheaders; i++)
        hl += (size_t)sprintf(s->hdrs + hl, "%s: %s\n", req->headers[i].name,
                              req->headers[i].value);
    s->body_len = req->body_len;
    s->auth = req->auth;
    s->fx = fx;
    s->cb = *cb;
    s->ud = ud;
    record(fx, s, req->url);
    s->resp = fx->qhead;
    if (s->resp == NULL) {
        stream_free(s);
        *msg = "fixture: no scripted response";
        return NULL;
    }
    fx->qhead = s->resp->next;
    if (fx->qhead == NULL)
        fx->qtail = NULL;
    s->resp->next = NULL;
    s->rng = s->resp->r.seed;
    s->phase = s->resp->redir ? 0 : 1;
    s->next = fx->streams;
    fx->streams = s;
    return s;
}

static void fx_close(void *ctx, void *stream)
{
    (void)ctx;
    if (stream)
        ((fx_stream *)stream)->closed = 1; /* freed by the next step or fixture_free */
}

canlin_llm_transport canlin_llm_fixture_transport(canlin_llm_fixture *fx)
{
    canlin_llm_transport t;
    t.open = fx_open;
    t.close = fx_close;
    t.ctx = fx;
    return t;
}

static unsigned xr(unsigned *s)
{
    unsigned x = *s ? *s : 2463534242u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

/* One callback for s. Returns 1 if a callback was made. */
static int step_one(fx_stream *s)
{
    const canlin_llm_fixture_response *r = &s->resp->r;
    size_t n, end;

    switch (s->phase) {
    case 0:
        record(s->fx, s, s->resp->redir); /* follow: the credential only if it applies */
        s->phase = 1;
        return 0;
    case 1:
        s->phase = 2;
        s->cb.status(s->ud, r->status);
        return 1;
    case 2:
        s->phase = 3;
        s->cb.header(s->ud, "Content-Type", s->resp->ct);
        return 1;
    case 3:
        end = r->body_len;
        if (r->fault != CANLIN_LLM_FX_NONE && r->fault_at < end)
            end = r->fault_at;
        if (s->off >= end) {
            s->phase = 4;
            if (r->fault == CANLIN_LLM_FX_ERROR_AT)
                s->cb.error(s->ud, "fixture: connection reset");
            else
                s->cb.end(s->ud);
            return 1;
        }
        n = end - s->off;
        if (r->chunk) {
            size_t c = r->chunk;
            if (r->seed)
                c = 1 + xr(&s->rng) % r->chunk;
            if (n > c)
                n = c;
        }
        s->off += n;
        s->cb.body(s->ud, s->resp->body + s->off - n, n);
        return 1;
    default:
        return 0;
    }
}

unsigned canlin_llm_fixture_step(canlin_llm_fixture *fx)
{
    fx_stream *s, **pp;
    unsigned made = 0;
    for (s = fx->streams; s; s = s->next)
        if (!s->closed && s->phase < 4)
            made += (unsigned)step_one(s) + (s->phase == 1 ? 1u : 0u);
    for (pp = &fx->streams; *pp;) {
        s = *pp;
        if (s->closed) {
            *pp = s->next;
            stream_free(s);
        } else
            pp = &s->next;
    }
    return made;
}

size_t canlin_llm_fixture_records(const canlin_llm_fixture *fx)
{
    return fx->nrec;
}

const canlin_llm_fixture_record *canlin_llm_fixture_record_at(const canlin_llm_fixture *fx,
                                                              size_t i)
{
    return i < fx->nrec ? &fx->rec[i] : NULL;
}

unsigned canlin_llm_fixture_open_streams(const canlin_llm_fixture *fx)
{
    unsigned n = 0;
    const fx_stream *s;
    for (s = fx->streams; s; s = s->next)
        if (!s->closed)
            n++;
    return n;
}
