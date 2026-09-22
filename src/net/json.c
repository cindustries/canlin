#include "canlin/json.h"

#include <string.h>

/* grammar states */
enum { G_VALUE, G_ARR_FIRST, G_OBJ_FIRST, G_KEY, G_COLON, G_NEXT, G_DONE };
/* lexical states */
enum { L_NONE, L_STR, L_ESC, L_UHEX, L_SUR_BS, L_SUR_U, L_NUM, L_LIT };
/* number states */
enum { N_MINUS, N_ZERO, N_INT, N_FRAC0, N_FRAC, N_EXP0, N_EXPS, N_EXP };

static const char *const lits[3] = { "true", "false", "null" };
static const int lit_tok[3] = { CANLIN_JSON_TRUE, CANLIN_JSON_FALSE, CANLIN_JSON_NULL };

int canlin_json_init(canlin_json *p, const canlin_json_limits *lim, void *buf,
                     size_t buflen, canlin_json_cb cb, void *ud)
{
    if (!p)
        return CANLIN_JSON_E_ARG;
    memset(p, 0, sizeof *p);
    p->err = CANLIN_JSON_E_ARG;
    if (!lim || !buf || !cb || lim->max_depth < 1 || lim->max_depth > CANLIN_JSON_DEPTH_CAP ||
        lim->max_string > buflen || lim->max_number > buflen || lim->max_number < 1 ||
        lim->max_total < 1)
        return CANLIN_JSON_E_ARG;
    p->lim = *lim;
    p->buf = buf;
    p->gs = G_VALUE;
    p->ls = L_NONE;
    p->cb = cb;
    p->ud = ud;
    p->err = CANLIN_JSON_OK;
    return CANLIN_JSON_OK;
}

static int emit(canlin_json *p, int type, const char *ptr, size_t len)
{
    canlin_json_token t;
    t.type = type;
    t.ptr = ptr;
    t.len = len;
    t.depth = p->depth;
    return p->cb(p->ud, &t) ? CANLIN_JSON_E_ABORTED : CANLIN_JSON_OK;
}

static int top_is_obj(const canlin_json *p)
{
    unsigned i = p->depth - 1;
    return (p->stack[i >> 3] >> (i & 7)) & 1;
}

static void value_done(canlin_json *p)
{
    p->gs = p->depth ? G_NEXT : G_DONE;
}

static int push(canlin_json *p, int obj)
{
    unsigned i = p->depth;
    int rc;

    if (i >= p->lim.max_depth)
        return CANLIN_JSON_E_DEPTH;
    rc = emit(p, obj ? CANLIN_JSON_OBJ_BEGIN : CANLIN_JSON_ARR_BEGIN, NULL, 0);
    if (obj)
        p->stack[i >> 3] |= (unsigned char)(1u << (i & 7));
    else
        p->stack[i >> 3] &= (unsigned char)~(1u << (i & 7));
    p->depth++;
    p->gs = obj ? G_OBJ_FIRST : G_ARR_FIRST;
    return rc;
}

static int pop(canlin_json *p)
{
    int obj = top_is_obj(p);
    p->depth--;
    value_done(p);
    return emit(p, obj ? CANLIN_JSON_OBJ_END : CANLIN_JSON_ARR_END, NULL, 0);
}

static int put_str(canlin_json *p, unsigned char c)
{
    if (p->len >= p->lim.max_string)
        return CANLIN_JSON_E_STRING_LEN;
    p->buf[p->len++] = (char)c;
    return CANLIN_JSON_OK;
}

static int put_cp(canlin_json *p, unsigned long cp)
{
    unsigned char u[4];
    size_t n, i;
    int rc;

    if (cp < 0x80) {
        u[0] = (unsigned char)cp;
        n = 1;
    } else if (cp < 0x800) {
        u[0] = (unsigned char)(0xC0 | (cp >> 6));
        u[1] = (unsigned char)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        u[0] = (unsigned char)(0xE0 | (cp >> 12));
        u[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        u[2] = (unsigned char)(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        u[0] = (unsigned char)(0xF0 | (cp >> 18));
        u[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
        u[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        u[3] = (unsigned char)(0x80 | (cp & 0x3F));
        n = 4;
    }
    for (i = 0; i < n; i++)
        if ((rc = put_str(p, u[i])) != 0)
            return rc;
    return CANLIN_JSON_OK;
}

static int num_terminal(int ns)
{
    return ns == N_ZERO || ns == N_INT || ns == N_FRAC || ns == N_EXP;
}

static int end_number(canlin_json *p)
{
    p->ls = L_NONE;
    value_done(p);
    return emit(p, CANLIN_JSON_NUMBER, p->buf, p->len);
}

static int hexval(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int start_value(canlin_json *p, unsigned char c)
{
    switch (c) {
    case '{': return push(p, 1);
    case '[': return push(p, 0);
    case '"':
        p->ls = L_STR;
        p->is_key = 0;
        p->len = 0;
        return CANLIN_JSON_OK;
    case 't': p->lit = 0; break;
    case 'f': p->lit = 1; break;
    case 'n': p->lit = 2; break;
    default:
        if (c == '-' || (c >= '0' && c <= '9')) {
            p->ls = L_NUM;
            p->len = 0;
            p->ns = c == '-' ? N_MINUS : c == '0' ? N_ZERO : N_INT;
            p->buf[p->len++] = (char)c; /* max_number >= 1 */
            return CANLIN_JSON_OK;
        }
        return CANLIN_JSON_E_SYNTAX;
    }
    p->ls = L_LIT;
    p->litpos = 1;
    return CANLIN_JSON_OK;
}

static int grammar(canlin_json *p, unsigned char c)
{
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
        return CANLIN_JSON_OK;
    switch (p->gs) {
    case G_DONE:
        return CANLIN_JSON_E_TRAILING;
    case G_COLON:
        if (c != ':')
            return CANLIN_JSON_E_SYNTAX;
        p->gs = G_VALUE;
        return CANLIN_JSON_OK;
    case G_NEXT:
        if (c == ',') {
            p->gs = top_is_obj(p) ? G_KEY : G_VALUE;
            return CANLIN_JSON_OK;
        }
        if ((c == '}' && top_is_obj(p)) || (c == ']' && !top_is_obj(p)))
            return pop(p);
        return CANLIN_JSON_E_SYNTAX;
    case G_OBJ_FIRST:
        if (c == '}')
            return pop(p);
        /* fall through */
    case G_KEY:
        if (c != '"')
            return CANLIN_JSON_E_SYNTAX;
        p->ls = L_STR;
        p->is_key = 1;
        p->len = 0;
        return CANLIN_JSON_OK;
    case G_ARR_FIRST:
        if (c == ']')
            return pop(p);
        /* fall through */
    default:
        return start_value(p, c);
    }
}

static int string_byte(canlin_json *p, unsigned char c)
{
    if (p->u8need) {
        if (c < p->u8lo || c > p->u8hi)
            return CANLIN_JSON_E_UTF8;
        p->u8lo = 0x80;
        p->u8hi = 0xBF;
        p->u8need--;
        return put_str(p, c);
    }
    if (c == '"') {
        p->ls = L_NONE;
        if (p->is_key) {
            p->gs = G_COLON;
            return emit(p, CANLIN_JSON_KEY, p->buf, p->len);
        }
        value_done(p);
        return emit(p, CANLIN_JSON_STRING, p->buf, p->len);
    }
    if (c == '\\') {
        p->ls = L_ESC;
        return CANLIN_JSON_OK;
    }
    if (c < 0x20)
        return CANLIN_JSON_E_STRING;
    if (c >= 0x80) {
        p->u8lo = 0x80;
        p->u8hi = 0xBF;
        if (c >= 0xC2 && c <= 0xDF) {
            p->u8need = 1;
        } else if (c >= 0xE0 && c <= 0xEF) {
            p->u8need = 2;
            if (c == 0xE0) p->u8lo = 0xA0;      /* overlong */
            if (c == 0xED) p->u8hi = 0x9F;      /* surrogates */
        } else if (c >= 0xF0 && c <= 0xF4) {
            p->u8need = 3;
            if (c == 0xF0) p->u8lo = 0x90;      /* overlong */
            if (c == 0xF4) p->u8hi = 0x8F;      /* > U+10FFFF */
        } else {
            return CANLIN_JSON_E_UTF8;          /* 80-C1, F5-FF */
        }
    }
    return put_str(p, c);
}

static int step(canlin_json *p, unsigned char c)
{
    int rc, v;

    switch (p->ls) {
    case L_STR:
        return string_byte(p, c);
    case L_ESC:
        p->ls = L_STR;
        switch (c) {
        case '"': return put_str(p, '"');
        case '\\': return put_str(p, '\\');
        case '/': return put_str(p, '/');
        case 'b': return put_str(p, '\b');
        case 'f': return put_str(p, '\f');
        case 'n': return put_str(p, '\n');
        case 'r': return put_str(p, '\r');
        case 't': return put_str(p, '\t');
        case 'u':
            p->ls = L_UHEX;
            p->uhex = 0;
            p->ucp = 0;
            return CANLIN_JSON_OK;
        }
        return CANLIN_JSON_E_ESCAPE;
    case L_UHEX:
        if ((v = hexval(c)) < 0)
            return CANLIN_JSON_E_ESCAPE;
        p->ucp = (p->ucp << 4) | (unsigned long)v;
        if (++p->uhex < 4)
            return CANLIN_JSON_OK;
        if (p->hisur) {
            unsigned long hi = p->hisur;
            p->hisur = 0;
            if (p->ucp < 0xDC00 || p->ucp > 0xDFFF)
                return CANLIN_JSON_E_ESCAPE;
            p->ls = L_STR;
            return put_cp(p, 0x10000 + ((hi - 0xD800) << 10) + (p->ucp - 0xDC00));
        }
        if (p->ucp >= 0xD800 && p->ucp <= 0xDBFF) {
            p->hisur = p->ucp;
            p->ls = L_SUR_BS;
            return CANLIN_JSON_OK;
        }
        if (p->ucp >= 0xDC00 && p->ucp <= 0xDFFF)
            return CANLIN_JSON_E_ESCAPE;
        p->ls = L_STR;
        return put_cp(p, p->ucp);
    case L_SUR_BS:
        if (c != '\\')
            return CANLIN_JSON_E_ESCAPE;
        p->ls = L_SUR_U;
        return CANLIN_JSON_OK;
    case L_SUR_U:
        if (c != 'u')
            return CANLIN_JSON_E_ESCAPE;
        p->ls = L_UHEX;
        p->uhex = 0;
        p->ucp = 0;
        return CANLIN_JSON_OK;
    case L_NUM: {
        int d = c >= '0' && c <= '9', ns = -1;
        switch (p->ns) {
        case N_MINUS: if (d) ns = c == '0' ? N_ZERO : N_INT; break;
        case N_ZERO:
            if (c == '.') ns = N_FRAC0;
            else if (c == 'e' || c == 'E') ns = N_EXP0;
            break;
        case N_INT:
            if (d) ns = N_INT;
            else if (c == '.') ns = N_FRAC0;
            else if (c == 'e' || c == 'E') ns = N_EXP0;
            break;
        case N_FRAC0: if (d) ns = N_FRAC; break;
        case N_FRAC:
            if (d) ns = N_FRAC;
            else if (c == 'e' || c == 'E') ns = N_EXP0;
            break;
        case N_EXP0:
            if (c == '+' || c == '-') ns = N_EXPS;
            else if (d) ns = N_EXP;
            break;
        case N_EXPS: if (d) ns = N_EXP; break;
        case N_EXP: if (d) ns = N_EXP; break;
        }
        if (ns >= 0) {
            if (p->len >= p->lim.max_number)
                return CANLIN_JSON_E_NUMBER_LEN;
            p->buf[p->len++] = (char)c;
            p->ns = ns;
            return CANLIN_JSON_OK;
        }
        if (!num_terminal(p->ns))
            return CANLIN_JSON_E_NUMBER;
        if ((rc = end_number(p)) != 0)
            return rc;
        return grammar(p, c); /* the delimiter belongs to the grammar */
    }
    case L_LIT:
        if (c != (unsigned char)lits[p->lit][p->litpos])
            return CANLIN_JSON_E_SYNTAX;
        if (lits[p->lit][++p->litpos])
            return CANLIN_JSON_OK;
        p->ls = L_NONE;
        value_done(p);
        return emit(p, lit_tok[p->lit], NULL, 0);
    default:
        return grammar(p, c);
    }
}

int canlin_json_feed(canlin_json *p, const void *data, size_t len)
{
    const unsigned char *b = data;
    size_t i;
    int rc;

    if (p->err)
        return p->err;
    if (p->finished)
        return p->err = CANLIN_JSON_E_STATE;
    for (i = 0; i < len; i++) {
        if (p->total >= p->lim.max_total) {
            p->total++;
            return p->err = CANLIN_JSON_E_TOTAL;
        }
        p->total++;
        if ((rc = step(p, b[i])) != 0)
            return p->err = rc;
    }
    return CANLIN_JSON_OK;
}

int canlin_json_finish(canlin_json *p)
{
    int rc;

    if (p->err)
        return p->err;
    if (p->finished)
        return p->err = CANLIN_JSON_E_STATE;
    p->finished = 1;
    if (p->ls == L_NUM && num_terminal(p->ns)) {
        if ((rc = end_number(p)) != 0)
            return p->err = rc;
    }
    if (p->ls != L_NONE || p->gs != G_DONE)
        return p->err = CANLIN_JSON_E_INCOMPLETE;
    return CANLIN_JSON_OK;
}

const char *canlin_json_strerror(int err)
{
    switch (err) {
    case CANLIN_JSON_OK: return "ok";
    case CANLIN_JSON_E_ARG: return "invalid argument";
    case CANLIN_JSON_E_SYNTAX: return "JSON syntax error";
    case CANLIN_JSON_E_TRAILING: return "trailing data after JSON value";
    case CANLIN_JSON_E_NUMBER: return "malformed JSON number";
    case CANLIN_JSON_E_STRING: return "control character in JSON string";
    case CANLIN_JSON_E_ESCAPE: return "invalid JSON escape";
    case CANLIN_JSON_E_UTF8: return "invalid UTF-8";
    case CANLIN_JSON_E_DEPTH: return "JSON nesting too deep";
    case CANLIN_JSON_E_STRING_LEN: return "JSON string too long";
    case CANLIN_JSON_E_NUMBER_LEN: return "JSON number too long";
    case CANLIN_JSON_E_TOTAL: return "JSON document too large";
    case CANLIN_JSON_E_INCOMPLETE: return "JSON document truncated";
    case CANLIN_JSON_E_ABORTED: return "aborted by callback";
    case CANLIN_JSON_E_STATE: return "JSON parser already finished";
    }
    return "unknown JSON error";
}
