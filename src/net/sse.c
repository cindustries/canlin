#include "canlin/sse.h"

#include <string.h>

static const unsigned char bom_bytes[3] = { 0xEF, 0xBB, 0xBF };

int canlin_sse_init(canlin_sse *p, void *mem, size_t memlen, size_t max_line,
                    size_t max_data, canlin_sse_cb cb, void *ud)
{
    char *m = mem;

    if (!p)
        return CANLIN_SSE_E_ARG;
    memset(p, 0, sizeof *p);
    p->err = CANLIN_SSE_E_ARG;
    if (!m || !cb || max_line == 0 || max_data == 0)
        return CANLIN_SSE_E_ARG;
    /* overflow-safe CANLIN_SSE_MEM(max_line, max_data) <= memlen */
    if (max_line > memlen / 4 || max_data >= memlen - 4 * max_line)
        return CANLIN_SSE_E_ARG;
    p->line = m;
    p->type = m + max_line;
    p->idbuf = m + 2 * max_line;
    p->id = m + 3 * max_line;
    p->data = m + 4 * max_line; /* max_data + 1: room for the joining '\n' */
    p->max_line = max_line;
    p->max_data = max_data;
    p->cb = cb;
    p->ud = ud;
    p->err = CANLIN_SSE_OK;
    return CANLIN_SSE_OK;
}

static int dispatch(canlin_sse *p)
{
    canlin_sse_event ev;

    /* WHATWG: the last event ID is committed even if no event is dispatched. */
    memcpy(p->id, p->idbuf, p->idbuf_len);
    p->id_len = p->idbuf_len;
    p->pending = 0;
    if (p->data_len == 0) {
        p->type_len = 0;
        return CANLIN_SSE_OK;
    }
    ev.data = p->data;
    ev.data_len = p->data_len - 1; /* strip the final '\n' */
    if (p->type_len) {
        ev.type = p->type;
        ev.type_len = p->type_len;
    } else {
        ev.type = "message";
        ev.type_len = 7;
    }
    ev.id = p->id;
    ev.id_len = p->id_len;
    p->data_len = 0;
    p->type_len = 0;
    if (p->cb(p->ud, &ev))
        return CANLIN_SSE_E_ABORTED;
    return CANLIN_SSE_OK;
}

static int field(canlin_sse *p, const char *name, size_t nlen, const char *v, size_t vlen)
{
    size_t i;
    unsigned long r;

    p->pending = 1;
    if (nlen == 5 && !memcmp(name, "event", 5)) {
        memcpy(p->type, v, vlen);
        p->type_len = vlen;
    } else if (nlen == 4 && !memcmp(name, "data", 4)) {
        if (vlen > p->max_data - p->data_len || p->data_len > p->max_data)
            return CANLIN_SSE_E_DATA;
        memcpy(p->data + p->data_len, v, vlen);
        p->data_len += vlen;
        p->data[p->data_len++] = '\n';
    } else if (nlen == 2 && !memcmp(name, "id", 2)) {
        if (!memchr(v, 0, vlen)) {
            memcpy(p->idbuf, v, vlen);
            p->idbuf_len = vlen;
        }
    } else if (nlen == 5 && !memcmp(name, "retry", 5)) {
        if (vlen == 0)
            return CANLIN_SSE_OK;
        r = 0;
        for (i = 0; i < vlen; i++) {
            unsigned d = (unsigned char)v[i] - '0';
            if (d > 9 || r > (0xFFFFFFFFul - d) / 10)
                return CANLIN_SSE_OK; /* not ASCII digits / out of range: ignored */
            r = r * 10 + d;
        }
        p->retry_ms = r;
        p->retry_set = 1;
    }
    /* unknown fields are ignored per WHATWG */
    return CANLIN_SSE_OK;
}

static int line_end(canlin_sse *p)
{
    size_t len = p->line_len;
    const char *colon;

    p->line_len = 0;
    if (len == 0)
        return dispatch(p);
    if (p->line[0] == ':')
        return CANLIN_SSE_OK; /* comment */
    colon = memchr(p->line, ':', len);
    if (colon) {
        size_t nlen = (size_t)(colon - p->line);
        const char *v = colon + 1;
        size_t vlen = len - nlen - 1;
        if (vlen && *v == ' ') {
            v++;
            vlen--;
        }
        return field(p, p->line, nlen, v, vlen);
    }
    return field(p, p->line, len, "", 0);
}

static int byte(canlin_sse *p, unsigned char c)
{
    if (p->cr) {
        p->cr = 0;
        if (c == '\n')
            return CANLIN_SSE_OK;
    }
    if (c == '\r') {
        p->cr = 1;
        return line_end(p);
    }
    if (c == '\n')
        return line_end(p);
    if (p->line_len >= p->max_line)
        return CANLIN_SSE_E_LINE;
    p->line[p->line_len++] = (char)c;
    return CANLIN_SSE_OK;
}

int canlin_sse_feed(canlin_sse *p, const void *buf, size_t len)
{
    const unsigned char *b = buf;
    size_t i;
    int rc;

    if (p->err)
        return p->err;
    if (p->finished)
        return p->err = CANLIN_SSE_E_STATE;
    for (i = 0; i < len; i++) {
        unsigned char c = b[i];
        if (p->bom < 3) {
            if (c == bom_bytes[p->bom]) {
                p->bom++;
                continue;
            }
            /* not a BOM: replay the matched prefix as ordinary bytes */
            {
                int k, n = p->bom;
                p->bom = 3;
                for (k = 0; k < n; k++)
                    if ((rc = byte(p, bom_bytes[k])) != 0)
                        return p->err = rc;
            }
        }
        if ((rc = byte(p, c)) != 0)
            return p->err = rc;
    }
    return CANLIN_SSE_OK;
}

int canlin_sse_finish(canlin_sse *p)
{
    if (p->err)
        return p->err;
    if (p->finished)
        return p->err = CANLIN_SSE_E_STATE;
    p->finished = 1;
    if (p->line_len || p->pending || (p->bom > 0 && p->bom < 3))
        return p->err = CANLIN_SSE_E_INCOMPLETE;
    return CANLIN_SSE_OK;
}

const char *canlin_sse_last_id(const canlin_sse *p, size_t *len)
{
    *len = p->id_len;
    return p->id;
}

const char *canlin_sse_strerror(int err)
{
    switch (err) {
    case CANLIN_SSE_OK: return "ok";
    case CANLIN_SSE_E_ARG: return "invalid argument";
    case CANLIN_SSE_E_LINE: return "SSE line too long";
    case CANLIN_SSE_E_DATA: return "SSE event data too large";
    case CANLIN_SSE_E_INCOMPLETE: return "SSE stream ended inside an event";
    case CANLIN_SSE_E_ABORTED: return "aborted by callback";
    case CANLIN_SSE_E_STATE: return "SSE parser already finished";
    }
    return "unknown SSE error";
}
