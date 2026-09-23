/* HTTP/1.1 wire primitives (HTTP transport spec §6). See include/canlin/http_wire.h. */
#include "canlin/http_wire.h"

#include <stdio.h>
#include <string.h>

/* ---- helpers ---- */

static char lc(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static int ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (lc(*a) != lc(*b))
            return 0;
    return *a == *b;
}

/* RFC 9110 tchar */
static int tchar(unsigned char c)
{
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
        return 1;
    return c && strchr("!#$%&'*+-.^_`|~", c) != NULL;
}

static int token(const char *s)
{
    if (s == NULL || *s == 0)
        return 0;
    for (; *s; s++)
        if (!tchar((unsigned char)*s))
            return 0;
    return 1;
}

/* Field value: visible ASCII, SP, HTAB, obs-text; no CR, LF, NUL or other controls. */
static int field_value(const char *s, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if ((c < 0x20 && c != '\t') || c == 0x7f)
            return 0;
    }
    return 1;
}

/* ---- URLs ---- */

static int parse_authority(const char *a, const char *e, int https, canlin_http_url *u)
{
    const char *hs, *he, *p;
    unsigned long port;
    size_t hl, k;
    int n;

    if (a == e)
        return -1;
    if (*a == '[') {
        hs = a + 1;
        for (he = hs; he < e && *he != ']'; he++)
            ;
        if (he == e)
            return -1;
        p = he + 1;
        u->ipv6 = 1;
    } else {
        hs = a;
        for (he = a; he < e && *he != ':'; he++)
            ;
        p = he;
        u->ipv6 = 0;
    }
    hl = (size_t)(he - hs);
    if (hl == 0 || hl > CANLIN_NET_HOST_MAX)
        return -1;
    for (k = 0; k < hl; k++) {
        char c = lc(hs[k]);
        int ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.';
        if (u->ipv6)
            ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || c == ':' || c == '.';
        else if (c == '-')
            ok = 1;
        if (!ok)
            return -1;
        u->host[k] = c;
    }
    u->host[hl] = 0;
    if (p < e) {
        if (*p != ':' || p + 1 == e || e - p > 6)
            return -1;
        port = 0;
        for (p++; p < e; p++) {
            if (*p < '0' || *p > '9')
                return -1;
            port = port * 10 + (unsigned long)(*p - '0');
        }
        if (port == 0 || port > 65535)
            return -1;
    } else
        port = https ? 443 : 80;
    u->https = https;
    u->port = (unsigned)port;
    n = snprintf(u->origin, sizeof u->origin, "%s://%s%s%s:%u", https ? "https" : "http",
                 u->ipv6 ? "[" : "", u->host, u->ipv6 ? "]" : "", u->port);
    return (n < 0 || (size_t)n >= sizeof u->origin) ? -1 : 0;
}

/* Path + query from s (up to '#' or end) into u->path; "/" when empty. */
static int set_path(const char *s, canlin_http_url *u)
{
    size_t n = 0;
    if (*s != '/')
        u->path[n++] = '/';
    for (; *s && *s != '#'; s++) {
        unsigned char c = (unsigned char)*s;
        if (c <= 0x20 || c >= 0x7f || c == '\\')
            return -1;
        if (n >= CANLIN_NET_URL_MAX)
            return -1;
        u->path[n++] = (char)c;
    }
    u->path[n] = 0;
    return 0;
}

static int scheme_prefix(const char *s, int *https)
{
    static const char h[] = "http";
    size_t i;
    for (i = 0; i < 4; i++)
        if (lc(s[i]) != h[i])
            return 0;
    if (lc(s[4]) == 's' && s[5] == ':' && s[6] == '/' && s[7] == '/') {
        *https = 1;
        return 8;
    }
    if (s[4] == ':' && s[5] == '/' && s[6] == '/') {
        *https = 0;
        return 7;
    }
    return 0;
}

static int parse_after_scheme(const char *a, int https, canlin_http_url *u)
{
    const char *e;
    for (e = a; *e && *e != '/' && *e != '?' && *e != '#'; e++)
        if (*e == '@' || *e == '\\' || (unsigned char)*e <= 0x20 || (unsigned char)*e >= 0x7f ||
            *e == '%')
            return -1;
    if (parse_authority(a, e, https, u) != 0)
        return -1;
    return set_path(e, u);
}

int canlin_http_url_parse(const char *url, canlin_http_url *out)
{
    int https, k;
    if (url == NULL || out == NULL || strlen(url) > CANLIN_NET_URL_MAX)
        return -1;
    memset(out, 0, sizeof *out);
    k = scheme_prefix(url, &https);
    if (k == 0)
        return -1;
    return parse_after_scheme(url + k, https, out);
}

int canlin_http_url_resolve(const canlin_http_url *base, const char *loc, canlin_http_url *out)
{
    canlin_http_url t;
    int https;
    if (base == NULL || loc == NULL || out == NULL || strlen(loc) > CANLIN_NET_URL_MAX)
        return -1;
    if (scheme_prefix(loc, &https))
        return canlin_http_url_parse(loc, out);
    memset(&t, 0, sizeof t);
    if (loc[0] == '/' && loc[1] == '/') {
        if (parse_after_scheme(loc + 2, base->https, &t) != 0)
            return -1;
    } else if (loc[0] == '/') {
        t = *base;
        if (set_path(loc, &t) != 0)
            return -1;
    } else
        return -1;
    *out = t;
    return 0;
}

int canlin_http_url_loopback(const canlin_http_url *u)
{
    const char *h = u->host;
    unsigned d, dots = 0, v;
    if (strcmp(h, "localhost") == 0)
        return 1;
    if (u->ipv6)
        return strcmp(h, "::1") == 0;
    if (strncmp(h, "127.", 4) != 0)
        return 0;
    /* strict dotted quad */
    for (h += 4, dots = 1; *h; dots++) {
        for (d = 0, v = 0; *h >= '0' && *h <= '9'; h++, d++)
            v = v * 10 + (unsigned)(*h - '0');
        if (d == 0 || d > 3 || v > 255)
            return 0;
        if (*h == '.')
            h++;
        else if (*h)
            return 0;
    }
    return dots == 4;
}

/* ---- request writer ---- */

static const char *const reserved[] = {"host", "content-length", "transfer-encoding",
                                       "connection", "accept-encoding", "user-agent",
                                       "te", "upgrade", "expect", "trailer", "keep-alive",
                                       "proxy-connection"};

int canlin_http_check_request(const char *method, const canlin_http_header *h, size_t nh,
                              const canlin_http_cred *cred)
{
    size_t i, k;
    if (!token(method) || nh > CANLIN_NET_REQ_HEADERS_MAX || (nh && h == NULL))
        return CANLIN_HTTP_W_EINVAL;
    for (i = 0; i < nh; i++) {
        if (!token(h[i].name) || h[i].value == NULL ||
            !field_value(h[i].value, strlen(h[i].value)))
            return CANLIN_HTTP_W_EINVAL;
        for (k = 0; k < sizeof reserved / sizeof reserved[0]; k++)
            if (ieq(h[i].name, reserved[k]))
                return CANLIN_HTTP_W_EINVAL;
        if (cred && cred->header && ieq(h[i].name, cred->header))
            return CANLIN_HTTP_W_EINVAL;
    }
    if (cred) {
        if (!token(cred->header) || cred->origin == NULL || cred->prefix == NULL ||
            !field_value(cred->prefix, strlen(cred->prefix)) ||
            (cred->secret_len && cred->secret == NULL) ||
            !field_value(cred->secret, cred->secret_len))
            return CANLIN_HTTP_W_EINVAL;
    }
    return CANLIN_HTTP_W_OK;
}

int canlin_http_cred_applies(const canlin_http_cred *cred, const canlin_http_url *u)
{
    return cred && cred->origin && u && strcmp(cred->origin, u->origin) == 0;
}

typedef struct wbuf {
    char *p;
    size_t n, cap;
    int full;
} wbuf;

static void put(wbuf *w, const char *s, size_t n)
{
    if (w->full || n > w->cap - w->n) {
        w->full = 1;
        return;
    }
    memcpy(w->p + w->n, s, n);
    w->n += n;
}

static void puts0(wbuf *w, const char *s)
{
    put(w, s, strlen(s));
}

int canlin_http_write_request(char *out, size_t cap, size_t *len, const char *method,
                              const canlin_http_url *u, const canlin_http_header *h,
                              size_t nh, int send_length, size_t body_len,
                              const canlin_http_cred *cred, const char *user_agent)
{
    wbuf w;
    char num[32];
    size_t i;

    w.p = out;
    w.n = 0;
    w.cap = cap;
    w.full = 0;
    puts0(&w, method);
    put(&w, " ", 1);
    puts0(&w, u->path);
    puts0(&w, " HTTP/1.1\r\nHost: ");
    if (u->ipv6)
        put(&w, "[", 1);
    puts0(&w, u->host);
    if (u->ipv6)
        put(&w, "]", 1);
    if (u->port != (u->https ? 443u : 80u)) {
        snprintf(num, sizeof num, ":%u", u->port);
        puts0(&w, num);
    }
    puts0(&w, "\r\nUser-Agent: ");
    puts0(&w, user_agent);
    puts0(&w, "\r\nAccept-Encoding: identity\r\nConnection: close\r\n");
    if (send_length) {
        snprintf(num, sizeof num, "Content-Length: %lu\r\n", (unsigned long)body_len);
        puts0(&w, num);
    }
    for (i = 0; i < nh; i++) {
        puts0(&w, h[i].name);
        put(&w, ": ", 2);
        puts0(&w, h[i].value);
        put(&w, "\r\n", 2);
    }
    if (canlin_http_cred_applies(cred, u)) {
        puts0(&w, cred->header);
        put(&w, ": ", 2);
        puts0(&w, cred->prefix);
        put(&w, cred->secret, cred->secret_len);
        put(&w, "\r\n", 2);
    }
    put(&w, "\r\n", 2);
    if (w.full) {
        memset(out, 0, cap); /* may hold part of the secret */
        return CANLIN_HTTP_W_ELIMIT;
    }
    *len = w.n;
    return CANLIN_HTTP_W_OK;
}

/* ---- response parser ---- */

enum {
    PS_STATUS = 0, /* status line */
    PS_HEADER,     /* header lines */
    PS_BODY_LEN,   /* Content-Length body */
    PS_BODY_CLOSE, /* close-delimited body */
    PS_CHUNK_SIZE, /* chunk-size line */
    PS_CHUNK_DATA,
    PS_CHUNK_CR,   /* CRLF after chunk data */
    PS_CHUNK_LF,
    PS_TRAILER,    /* trailer lines, discarded */
    PS_DONE,
    PS_STOPPED,
    PS_ERROR
};

void canlin_http_parser_init(canlin_http_parser *p, void *mem, int no_body,
                             const canlin_http_parser_cb *cb, void *ud)
{
    memset(p, 0, sizeof *p);
    p->buf = mem;
    p->no_body = no_body;
    p->cb = *cb;
    p->ud = ud;
    p->state = PS_STATUS;
}

static int fail(canlin_http_parser *p, int err, const char *why)
{
    p->state = PS_ERROR;
    p->err = err;
    p->why = why;
    return err;
}

const char *canlin_http_parser_why(const canlin_http_parser *p)
{
    return p->why ? p->why : "";
}

static void reset_head(canlin_http_parser *p)
{
    p->used = 0;
    p->line = 0;
    p->head_bytes = 0;
    p->nfields = 0;
}

static int parse_status_line(canlin_http_parser *p, const char *s, size_t n)
{
    int st;
    if (n < 12 || memcmp(s, "HTTP/1.", 7) != 0 || s[7] < '0' || s[7] > '9' || s[8] != ' ')
        return fail(p, CANLIN_HTTP_P_EPROTOCOL, "malformed status line");
    if (s[9] < '1' || s[9] > '5' || s[10] < '0' || s[10] > '9' || s[11] < '0' || s[11] > '9')
        return fail(p, CANLIN_HTTP_P_EPROTOCOL, "malformed status code");
    if (n > 12 && s[12] != ' ')
        return fail(p, CANLIN_HTTP_P_EPROTOCOL, "malformed status line");
    if (!field_value(s, n))
        return fail(p, CANLIN_HTTP_P_EPROTOCOL, "control byte in status line");
    st = (s[9] - '0') * 100 + (s[10] - '0') * 10 + (s[11] - '0');
    p->status = st;
    return 0;
}

/* One header (or trailer) line s[0..n) at p->buf + p->used; stores name\0value\0. */
static int parse_field(canlin_http_parser *p, char *s, size_t n, int keep)
{
    size_t c, vs, ve;
    if (s[0] == ' ' || s[0] == '\t')
        return fail(p, CANLIN_HTTP_P_EPROTOCOL, "obsolete line folding");
    for (c = 0; c < n && s[c] != ':'; c++)
        if (!tchar((unsigned char)s[c]))
            return fail(p, CANLIN_HTTP_P_EPROTOCOL, "malformed header name");
    if (c == 0 || c == n)
        return fail(p, CANLIN_HTTP_P_EPROTOCOL, "malformed header line");
    for (vs = c + 1; vs < n && (s[vs] == ' ' || s[vs] == '\t'); vs++)
        ;
    for (ve = n; ve > vs && (s[ve - 1] == ' ' || s[ve - 1] == '\t'); ve--)
        ;
    if (!field_value(s + vs, ve - vs))
        return fail(p, CANLIN_HTTP_P_EPROTOCOL, "control byte in header value");
    if (++p->nfields > CANLIN_NET_HEADERS_MAX)
        return fail(p, CANLIN_HTTP_P_ELIMIT, "too many header fields");
    if (!keep)
        return 0;
    s[c] = 0;
    memmove(s + c + 1, s + vs, ve - vs);
    s[c + 1 + (ve - vs)] = 0;
    p->f[p->nfields - 1].name = s;
    p->f[p->nfields - 1].value = s + c + 1;
    p->used += c + 1 + (ve - vs) + 1;
    return 0;
}

static int parse_uint(const char *v, uint64_t *out)
{
    uint64_t x = 0;
    if (*v == 0)
        return -1;
    for (; *v; v++) {
        if (*v < '0' || *v > '9')
            return -1;
        if (x > (UINT64_MAX - 9) / 10)
            return -1;
        x = x * 10 + (uint64_t)(*v - '0');
    }
    *out = x;
    return 0;
}

/* Head complete: validate framing, then hand the final head to the consumer. */
static int head_done(canlin_http_parser *p)
{
    size_t i;
    int te = 0, cl = 0;
    uint64_t v;

    if (p->status < 200) {
        if (p->status == 101)
            return fail(p, CANLIN_HTTP_P_EPROTOCOL, "unexpected protocol switch");
        if (++p->interim > CANLIN_NET_INTERIM_MAX)
            return fail(p, CANLIN_HTTP_P_ELIMIT, "too many interim responses");
        reset_head(p);
        p->state = PS_STATUS;
        return 0;
    }
    p->chunked = 0;
    p->has_len = 0;
    for (i = 0; i < p->nfields; i++) {
        const char *n = p->f[i].name, *val = p->f[i].value;
        if (ieq(n, "transfer-encoding")) {
            if (te++ || !ieq(val, "chunked"))
                return fail(p, CANLIN_HTTP_P_EPROTOCOL, "unsupported transfer-encoding");
            p->chunked = 1;
        } else if (ieq(n, "content-length")) {
            if (parse_uint(val, &v) != 0)
                return fail(p, CANLIN_HTTP_P_EPROTOCOL, "malformed content-length");
            if (cl++ && v != p->len)
                return fail(p, CANLIN_HTTP_P_EPROTOCOL, "conflicting content-length");
            p->len = v;
            p->has_len = 1;
        } else if (ieq(n, "content-encoding")) {
            if (!ieq(val, "identity"))
                return fail(p, CANLIN_HTTP_P_EPROTOCOL, "unsupported content-encoding");
        }
    }
    if (te && cl)
        return fail(p, CANLIN_HTTP_P_EPROTOCOL, "both transfer-encoding and content-length");
    if (p->cb.head(p->ud, p->status, p->f, p->nfields)) {
        p->state = PS_STOPPED;
        return CANLIN_HTTP_P_STOPPED;
    }
    reset_head(p); /* the buffer now serves chunk-size and trailer lines */
    if (p->no_body || p->status == 204 || p->status == 304)
        p->state = PS_DONE;
    else if (p->chunked)
        p->state = PS_CHUNK_SIZE;
    else if (p->has_len) {
        p->left = p->len;
        p->state = p->len ? PS_BODY_LEN : PS_DONE;
    } else
        p->state = PS_BODY_CLOSE;
    return 0;
}

static int chunk_size_line(canlin_http_parser *p, const char *s, size_t n)
{
    size_t i, d = 0;
    uint64_t v = 0;
    for (i = 0; i < n; i++, d++) {
        char c = lc(s[i]);
        if (c >= '0' && c <= '9')
            v = v * 16 + (uint64_t)(c - '0');
        else if (c >= 'a' && c <= 'f')
            v = v * 16 + (uint64_t)(c - 'a' + 10);
        else
            break;
        if (d >= CANLIN_NET_CHUNK_HEX_MAX)
            return fail(p, CANLIN_HTTP_P_EPROTOCOL, "bad chunk size");
    }
    if (d == 0)
        return fail(p, CANLIN_HTTP_P_EPROTOCOL, "bad chunk size");
    for (; i < n && (s[i] == ' ' || s[i] == '\t'); i++)
        ;
    if (i < n && s[i] != ';')
        return fail(p, CANLIN_HTTP_P_EPROTOCOL, "bad chunk size");
    if (!field_value(s, n))
        return fail(p, CANLIN_HTTP_P_EPROTOCOL, "control byte in chunk extension");
    p->left = v;
    p->state = v ? PS_CHUNK_DATA : PS_TRAILER;
    p->head_bytes = 0;
    p->nfields = 0;
    return 0;
}

/* A complete line (terminator stripped) in the line area. */
static int line_done(canlin_http_parser *p)
{
    char *s = p->buf + p->used;
    size_t n = p->line, i;
    int rc = 0;

    for (i = 0; i < n; i++)
        if (s[i] == '\r' || s[i] == 0)
            return fail(p, CANLIN_HTTP_P_EPROTOCOL, s[i] ? "bare CR in line" : "NUL in line");
    p->line = 0;
    switch (p->state) {
    case PS_STATUS:
        rc = parse_status_line(p, s, n);
        if (rc == 0)
            p->state = PS_HEADER;
        return rc;
    case PS_HEADER:
        if (n == 0)
            return head_done(p);
        return parse_field(p, s, n, 1);
    case PS_CHUNK_SIZE:
        return chunk_size_line(p, s, n);
    case PS_TRAILER:
        if (n == 0) {
            p->state = PS_DONE;
            return 0;
        }
        return parse_field(p, s, n, 0);
    default:
        return fail(p, CANLIN_HTTP_P_EPROTOCOL, "internal parser state");
    }
}

static int line_state(int st)
{
    return st == PS_STATUS || st == PS_HEADER || st == PS_CHUNK_SIZE || st == PS_TRAILER;
}

int canlin_http_parser_feed(canlin_http_parser *p, const char *buf, size_t n, size_t *used)
{
    size_t i = 0;
    int rc;

    *used = 0;
    if (p->state == PS_ERROR)
        return p->err;
    while (i < n) {
        if (p->state == PS_DONE) {
            *used = i;
            return CANLIN_HTTP_P_DONE;
        }
        if (p->state == PS_STOPPED) {
            *used = i;
            return CANLIN_HTTP_P_STOPPED;
        }
        if (line_state(p->state)) {
            char c = buf[i++];
            /* Head bytes are bounded per head/trailer block; chunk-size lines only by
             * the line bound. */
            if (p->state != PS_CHUNK_SIZE && ++p->head_bytes > CANLIN_NET_HEADER_BYTES_MAX)
                return fail(p, CANLIN_HTTP_P_ELIMIT, "header block too large");
            if (c == '\n') {
                if (p->line && p->buf[p->used + p->line - 1] == '\r')
                    p->line--;
                rc = line_done(p);
                if (rc == CANLIN_HTTP_P_STOPPED) {
                    *used = i;
                    return rc;
                }
                if (rc < 0)
                    return rc;
                continue;
            }
            /* The bound is on the line without its terminator: at the bound only the
             * CR of a CRLF may follow. */
            if (p->line > CANLIN_NET_HEADER_LINE_MAX ||
                (p->line == CANLIN_NET_HEADER_LINE_MAX && c != '\r'))
                return fail(p, CANLIN_HTTP_P_ELIMIT, "header line too long");
            if (p->used + p->line >= CANLIN_HTTP_PARSER_MEM - 2)
                return fail(p, CANLIN_HTTP_P_ELIMIT, "header block too large");
            p->buf[p->used + p->line++] = c;
            continue;
        }
        switch (p->state) {
        case PS_BODY_LEN:
        case PS_CHUNK_DATA:
        case PS_BODY_CLOSE: {
            size_t k = n - i;
            int pause;
            if (p->state != PS_BODY_CLOSE && (uint64_t)k > p->left)
                k = (size_t)p->left;
            if (p->state != PS_BODY_CLOSE) {
                p->left -= k;
                if (p->left == 0)
                    p->state = p->state == PS_BODY_LEN ? PS_DONE : PS_CHUNK_CR;
            }
            pause = p->cb.body(p->ud, buf + i, k);
            i += k;
            if (pause) {
                *used = i;
                return p->state == PS_DONE ? CANLIN_HTTP_P_DONE : CANLIN_HTTP_P_PAUSED;
            }
            break;
        }
        case PS_CHUNK_CR:
            if (buf[i] == '\r')
                p->state = PS_CHUNK_LF;
            else if (buf[i] == '\n')
                p->state = PS_CHUNK_SIZE;
            else
                return fail(p, CANLIN_HTTP_P_EPROTOCOL, "missing CRLF after chunk");
            i++;
            break;
        case PS_CHUNK_LF:
            if (buf[i] != '\n')
                return fail(p, CANLIN_HTTP_P_EPROTOCOL, "missing CRLF after chunk");
            p->state = PS_CHUNK_SIZE;
            i++;
            break;
        default:
            return fail(p, CANLIN_HTTP_P_EPROTOCOL, "internal parser state");
        }
    }
    *used = i;
    if (p->state == PS_DONE)
        return CANLIN_HTTP_P_DONE;
    if (p->state == PS_STOPPED)
        return CANLIN_HTTP_P_STOPPED;
    return CANLIN_HTTP_P_MORE;
}

int canlin_http_parser_needs_eof(const canlin_http_parser *p)
{
    return p->state == PS_BODY_CLOSE;
}

int canlin_http_parser_eof(canlin_http_parser *p)
{
    switch (p->state) {
    case PS_ERROR:
        return p->err;
    case PS_DONE:
    case PS_STOPPED:
        return CANLIN_HTTP_P_DONE;
    case PS_BODY_CLOSE:
        p->state = PS_DONE;
        return CANLIN_HTTP_P_DONE;
    case PS_STATUS:
    case PS_HEADER:
        return fail(p, CANLIN_HTTP_P_EEOF, "eof before the end of the response head");
    case PS_BODY_LEN:
        return fail(p, CANLIN_HTTP_P_EEOF, "eof before content-length bytes");
    default:
        return fail(p, CANLIN_HTTP_P_EEOF, "eof inside a chunked body");
    }
}
