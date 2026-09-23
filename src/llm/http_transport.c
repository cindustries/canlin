/* canlin_llm_transport over canlin_http, see include/canlin/llm_http.h. Owner thread. */
#include "canlin/llm_http.h"

#include <stdlib.h>
#include <string.h>

typedef struct lh_stream {
    canlin_http *h;
    canlin_http_stream *s;
    canlin_llm_stream_cb cb;
    void *ud;
    canlin_http_cred cred; /* points at the caller's auth strings; never copies the secret */
    canlin_http_header hdr[CANLIN_NET_REQ_HEADERS_MAX];
} lh_stream;

/* Each wrapper makes its callback last: the LLM core may close (and so free) the stream
 * from inside it. */
static void a_status(void *ud, int st)
{
    lh_stream *a = ud;
    a->cb.status(a->ud, st);
}

static void a_header(void *ud, const char *name, const char *value)
{
    lh_stream *a = ud;
    a->cb.header(a->ud, name, value);
}

static int a_body(void *ud, const void *buf, size_t len)
{
    lh_stream *a = ud;
    a->cb.body(a->ud, buf, len);
    return 0;
}

static void a_end(void *ud)
{
    lh_stream *a = ud;
    a->cb.end(a->ud);
}

static void a_error(void *ud, const char *msg)
{
    lh_stream *a = ud;
    a->cb.error(a->ud, msg);
}

static const canlin_http_cb acb = {a_status, a_header, a_body, a_end, a_error};

static void *lh_open(void *ctx, const canlin_llm_http_request *req,
                     const canlin_llm_stream_cb *cb, void *ud, const char **msg)
{
    canlin_http_request hr;
    lh_stream *a;
    size_t i;

    if (req->nheaders > CANLIN_NET_REQ_HEADERS_MAX) {
        *msg = "too many request headers";
        return NULL;
    }
    a = calloc(1, sizeof *a);
    if (a == NULL) {
        *msg = "out of memory";
        return NULL;
    }
    a->h = ctx;
    a->cb = *cb;
    a->ud = ud;
    for (i = 0; i < req->nheaders; i++) {
        a->hdr[i].name = req->headers[i].name;
        a->hdr[i].value = req->headers[i].value;
    }
    memset(&hr, 0, sizeof hr);
    hr.method = req->method;
    hr.url = req->url;
    hr.headers = a->hdr;
    hr.nheaders = req->nheaders;
    hr.body = req->body;
    hr.body_len = req->body_len;
    if (req->auth) {
        /* canlin_llm_auth and canlin_http_cred are the same binding (origin normalised
         * identically, t/llm/test_llm_http checks it). */
        a->cred.origin = req->auth->origin;
        a->cred.header = req->auth->header;
        a->cred.prefix = req->auth->prefix;
        a->cred.secret = req->auth->secret;
        a->cred.secret_len = req->auth->secret_len;
        hr.cred = &a->cred;
    }
    a->s = canlin_http_open(a->h, &hr, &acb, a, msg);
    if (a->s == NULL) {
        memset(a, 0, sizeof *a);
        free(a);
        return NULL;
    }
    return a;
}

static void lh_close(void *ctx, void *stream)
{
    lh_stream *a = stream;
    (void)ctx;
    if (a == NULL)
        return;
    canlin_http_close(a->h, a->s);
    memset(a, 0, sizeof *a);
    free(a);
}

canlin_llm_transport canlin_llm_http_transport(canlin_http *h)
{
    canlin_llm_transport t;
    t.open = lh_open;
    t.close = lh_close;
    t.ctx = h;
    return t;
}
