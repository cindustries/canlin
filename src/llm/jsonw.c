/* Bounded JSON writer and UTF-8 validation for request bodies (spec §7
 * CANLIN_LLM_REQUEST_MAX). Errors are sticky; the buffer never exceeds w->max bytes. */
#include "llm_int.h"
#include "canlin/json.h"

#include <stdlib.h>
#include <string.h>

void llm_jw_init(llm_jw *w, size_t max)
{
    memset(w, 0, sizeof *w);
    w->max = max;
}

void llm_jw_free(llm_jw *w)
{
    free(w->buf);
    memset(w, 0, sizeof *w);
}

static int reserve(llm_jw *w, size_t n)
{
    size_t cap;
    char *nb;
    if (w->err)
        return -1;
    if (n > w->max - w->len) {
        w->err = JW_EFULL;
        return -1;
    }
    if (w->len + n <= w->cap)
        return 0;
    cap = w->cap ? w->cap : 1024;
    while (cap < w->len + n)
        cap = cap > w->max / 2 ? w->max : cap * 2;
    nb = realloc(w->buf, cap);
    if (nb == NULL) {
        w->err = JW_ENOMEM;
        return -1;
    }
    w->buf = nb;
    w->cap = cap;
    return 0;
}

void llm_jw_raw(llm_jw *w, const char *s, size_t n)
{
    if (reserve(w, n) == 0) {
        memcpy(w->buf + w->len, s, n);
        w->len += n;
    }
}

void llm_jw_lit(llm_jw *w, const char *s)
{
    llm_jw_raw(w, s, strlen(s));
}

void llm_jw_uint(llm_jw *w, unsigned long v)
{
    char tmp[24];
    size_t k = sizeof tmp;
    do {
        tmp[--k] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v);
    llm_jw_raw(w, tmp + k, sizeof tmp - k);
}

/* Length of the well-formed UTF-8 sequence at s (n bytes left), 0 if malformed. */
static size_t u8seq(const unsigned char *s, size_t n)
{
    unsigned char c = s[0];
    size_t len, k;
    unsigned char lo = 0x80, hi = 0xbf;
    if (c < 0x80)
        return 1;
    if (c >= 0xc2 && c <= 0xdf)
        len = 2;
    else if (c >= 0xe0 && c <= 0xef) {
        len = 3;
        if (c == 0xe0)
            lo = 0xa0;
        else if (c == 0xed)
            hi = 0x9f;
    } else if (c >= 0xf0 && c <= 0xf4) {
        len = 4;
        if (c == 0xf0)
            lo = 0x90;
        else if (c == 0xf4)
            hi = 0x8f;
    } else
        return 0;
    if (n < len)
        return 0;
    if (s[1] < lo || s[1] > hi)
        return 0;
    for (k = 2; k < len; k++)
        if (s[k] < 0x80 || s[k] > 0xbf)
            return 0;
    return len;
}

int llm_utf8_valid(const char *s, size_t n)
{
    const unsigned char *p = (const unsigned char *)s;
    size_t i = 0, l;
    while (i < n) {
        l = u8seq(p + i, n - i);
        if (l == 0)
            return 0;
        i += l;
    }
    return 1;
}

void llm_jw_esc(llm_jw *w, const char *s, size_t n)
{
    static const char hex[] = "0123456789abcdef";
    const unsigned char *p = (const unsigned char *)s;
    size_t i = 0, l;

    while (i < n && !w->err) {
        unsigned char c = p[i];
        char esc[6];
        if (c >= 0x80) {
            l = u8seq(p + i, n - i);
            if (l == 0) {
                w->err = JW_EUTF8;
                return;
            }
            llm_jw_raw(w, s + i, l);
            i += l;
            continue;
        }
        if (c == '"' || c == '\\') {
            esc[0] = '\\';
            esc[1] = (char)c;
            llm_jw_raw(w, esc, 2);
        } else if (c == '\n') {
            llm_jw_raw(w, "\\n", 2);
        } else if (c == '\r') {
            llm_jw_raw(w, "\\r", 2);
        } else if (c == '\t') {
            llm_jw_raw(w, "\\t", 2);
        } else if (c < 0x20 || c == 0x7f) {
            esc[0] = '\\';
            esc[1] = 'u';
            esc[2] = '0';
            esc[3] = '0';
            esc[4] = hex[c >> 4];
            esc[5] = hex[c & 15];
            llm_jw_raw(w, esc, 6);
        } else {
            llm_jw_raw(w, s + i, 1);
        }
        i++;
    }
}

void llm_jw_str(llm_jw *w, const char *s, size_t n)
{
    llm_jw_raw(w, "\"", 1);
    llm_jw_esc(w, s, n);
    llm_jw_raw(w, "\"", 1);
}

static int first_tok(void *ud, const canlin_json_token *t)
{
    int *first = ud;
    if (*first == 0)
        *first = t->type;
    return 0;
}

int llm_json_object_valid(char *buf, size_t buflen, const char *p, size_t n, size_t max_total)
{
    canlin_json js;
    canlin_json_limits lim;
    int first = 0;
    lim.max_depth = CANLIN_LLM_JSON_DEPTH;
    lim.max_string = CANLIN_LLM_JSON_STRING_MAX;
    lim.max_number = CANLIN_LLM_JSON_NUMBER_MAX;
    lim.max_total = max_total;
    if (canlin_json_init(&js, &lim, buf, buflen, first_tok, &first) != CANLIN_JSON_OK)
        return 0;
    if (canlin_json_feed(&js, p, n) != CANLIN_JSON_OK || canlin_json_finish(&js) != CANLIN_JSON_OK)
        return 0;
    return first == CANLIN_JSON_OBJ_BEGIN;
}
