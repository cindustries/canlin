/* JSON tokenizer: table vectors at every split point, limits, UTF-8, fuzz. Exit 0 = pass. */
#include "canlin/json.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

typedef struct { char buf[8192]; size_t len; int overflow; int abort_on_null; } logbuf;

static void lput(logbuf *l, const char *s, size_t n)
{
    if (n > sizeof l->buf - 1 - l->len) { l->overflow = 1; return; }
    memcpy(l->buf + l->len, s, n);
    l->len += n;
    l->buf[l->len] = 0;
}

/* bytes < 0x20 are logged as \HH so NUL and controls stay comparable */
static void lbytes(logbuf *l, const char *s, size_t n)
{
    char hex[4];
    size_t i;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20) {
            snprintf(hex, sizeof hex, "\\%02X", c);
            lput(l, hex, 3);
        } else {
            lput(l, s + i, 1);
        }
    }
}

static int on_tok(void *ud, const canlin_json_token *t)
{
    logbuf *l = ud;
    char d[16];

    if (l->len)
        lput(l, " ", 1);
    switch (t->type) {
    case CANLIN_JSON_OBJ_BEGIN: lput(l, "{", 1); break;
    case CANLIN_JSON_OBJ_END: lput(l, "}", 1); break;
    case CANLIN_JSON_ARR_BEGIN: lput(l, "[", 1); break;
    case CANLIN_JSON_ARR_END: lput(l, "]", 1); break;
    case CANLIN_JSON_KEY: lput(l, "k:", 2); lbytes(l, t->ptr, t->len); break;
    case CANLIN_JSON_STRING: lput(l, "s:", 2); lbytes(l, t->ptr, t->len); break;
    case CANLIN_JSON_NUMBER: lput(l, "n:", 2); lbytes(l, t->ptr, t->len); break;
    case CANLIN_JSON_TRUE: lput(l, "T", 1); break;
    case CANLIN_JSON_FALSE: lput(l, "F", 1); break;
    case CANLIN_JSON_NULL: lput(l, "N", 1); break;
    default: lput(l, "?", 1);
    }
    if (l->abort_on_null == 2) { /* depth logging mode */
        snprintf(d, sizeof d, "@%u", t->depth);
        lput(l, d, strlen(d));
    }
    return l->abort_on_null == 1 && t->type == CANLIN_JSON_NULL;
}

static char tokbuf[256];
static const canlin_json_limits deflim = { 32, 200, 40, 4096 };

static int run(const char *in, size_t n, const canlin_json_limits *lim, const size_t *cuts,
               int ncut, int bytewise, logbuf *l, int mode)
{
    canlin_json p;
    size_t pos = 0;
    int i, rc;

    memset(l, 0, sizeof *l);
    l->abort_on_null = mode;
    if (canlin_json_init(&p, lim, tokbuf, sizeof tokbuf, on_tok, l))
        return 100;
    rc = 0;
    if (bytewise) {
        for (pos = 0; pos < n && !rc; pos++)
            rc = canlin_json_feed(&p, in + pos, 1);
    } else {
        for (i = 0; i <= ncut && !rc; i++) {
            size_t end = i < ncut ? cuts[i] : n;
            rc = canlin_json_feed(&p, in + pos, end - pos);
            pos = end;
        }
    }
    if (!rc)
        rc = canlin_json_finish(&p);
    else
        CHECK(canlin_json_finish(&p) == rc, "error not sticky");
    return rc;
}

struct vec {
    const char *in;
    size_t n;
    const char *log; /* NULL: only check consistency across splits */
    int rc;
    canlin_json_limits lim;
};

#define V(s, log, rc) { s, sizeof(s) - 1, log, rc, { 32, 200, 40, 4096 } }
#define VL(s, log, rc, d, ms, mn, mt) { s, sizeof(s) - 1, log, rc, { d, ms, mn, mt } }
#define E(s, rc) V(s, NULL, CANLIN_JSON_E_##rc)

static const struct vec vecs[] = {
    V("{}", "{ }", 0),
    V("[]", "[ ]", 0),
    V(" \t\r\n[1, -0.5e+3, 0, 2E7, -0, 0.25, 1e-2, \"a\"] \n",
      "[ n:1 n:-0.5e+3 n:0 n:2E7 n:-0 n:0.25 n:1e-2 s:a ]", 0),
    V("{\"k\":[true,false,null],\"o\":{},\"e\":[]}", "{ k:k [ T F N ] k:o { } k:e [ ] }", 0),
    V("123", "n:123", 0),
    V("-7 ", "n:-7", 0),
    V("0", "n:0", 0),
    V("\"x\"", "s:x", 0),
    V("\"\"", "s:", 0),
    V("true", "T", 0),
    V("null", "N", 0),
    V("[[[]],{\"a\":{\"b\":[1]}}]", "[ [ [ ] ] { k:a { k:b [ n:1 ] } } ]", 0),
    V("\"\\\"\\\\\\/\\b\\f\\n\\r\\t\"", "s:\"\\/\\08\\0C\\0A\\0D\\09", 0),
    V("\"\\u00e9\\u20AC\\ud83d\\ude00\\u0041\"", "s:\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80" "A", 0),
    V("\"\\u0000\\u001f\\u007F\\u0080\\u07ff\\u0800\\uffff\"",
      "s:\\00\\1F\x7F\xC2\x80\xDF\xBF\xE0\xA0\x80\xEF\xBF\xBF", 0),
    V("\"\\udbff\\udfff\"", "s:\xF4\x8F\xBF\xBF", 0),
    V("\"\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80\xF4\x8F\xBF\xBF\xED\x9F\xBF\xEE\x80\x80\"",
      "s:\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80\xF4\x8F\xBF\xBF\xED\x9F\xBF\xEE\x80\x80", 0),
    V("{\"\xC3\xA4\":\"\\u00e4\"}", "{ k:\xC3\xA4 s:\xC3\xA4 }", 0),
    /* truncation */
    E("", INCOMPLETE), E("   ", INCOMPLETE), E("[", INCOMPLETE), E("{\"a\"", INCOMPLETE),
    E("{\"a\":", INCOMPLETE), E("{\"a\":1", INCOMPLETE), E("{\"a\":1,", INCOMPLETE),
    E("[1,", INCOMPLETE), E("\"abc", INCOMPLETE), E("-", INCOMPLETE), E("1.", INCOMPLETE),
    E("1e", INCOMPLETE), E("1e-", INCOMPLETE), E("tru", INCOMPLETE), E("n", INCOMPLETE),
    E("\"\\", INCOMPLETE), E("\"\\u12", INCOMPLETE), E("\"\\ud83d", INCOMPLETE),
    E("\"\\ud83d\\", INCOMPLETE), E("\"\\ud83d\\u", INCOMPLETE), E("\"\xC3", INCOMPLETE),
    E("\"\xF0\x9F\x98", INCOMPLETE), E("[[[1]]", INCOMPLETE),
    /* syntax */
    E("[1,]", SYNTAX), E("{,}", SYNTAX), E("{\"a\" 1}", SYNTAX), E("[1 2]", SYNTAX),
    E("{\"a\":1,}", SYNTAX), E("{1:2}", SYNTAX), E("[}", SYNTAX), E("{]", SYNTAX),
    E("{\"a\":1]", SYNTAX), E("[1}", SYNTAX), E("nul ", SYNTAX), E("True", SYNTAX),
    E(".5", SYNTAX), E("+1", SYNTAX), E("[01]", SYNTAX), E("\xC3\xA9", SYNTAX),
    E("\xEF\xBB\xBF{}", SYNTAX), E("{\"a\"::1}", SYNTAX), E("[,1]", SYNTAX), E("'a'", SYNTAX),
    E("\x0b[]", SYNTAX), E("[1,,2]", SYNTAX), E("{\"a\":}", SYNTAX),
    /* trailing garbage */
    E("01", TRAILING), E("1 2", TRAILING), E("{} x", TRAILING), E("truex", TRAILING),
    E("[]]", TRAILING), E("{}{}", TRAILING), E("\"a\"\"b\"", TRAILING), E("null\0", TRAILING),
    /* numbers */
    E("1.e5", NUMBER), E("-a", NUMBER), E("[1e+]", NUMBER), E("[-]", NUMBER), E("1.x", NUMBER),
    E("-.5", NUMBER), E("1e++2", NUMBER),
    /* strings, escapes */
    E("\"a\x01\"", STRING), E("\"\n\"", STRING), E("\"\t\"", STRING),
    E("\"\\x\"", ESCAPE), E("\"\\U0041\"", ESCAPE), E("\"\\u12G4\"", ESCAPE),
    E("\"\\udc00\"", ESCAPE), E("\"\\ud800x\"", ESCAPE), E("\"\\ud800\\u0041\"", ESCAPE),
    E("\"\\ud800\\n\"", ESCAPE), E("\"\\ud800\\ud800\"", ESCAPE), E("\"\\ud800\"", ESCAPE),
    E("\"\\'\"", ESCAPE),
    /* UTF-8 */
    E("\"\xC0\xAF\"", UTF8), E("\"\xC1\xBF\"", UTF8), E("\"\xE0\x80\xAF\"", UTF8),
    E("\"\xE0\x9F\xBF\"", UTF8), E("\"\xED\xA0\x80\"", UTF8), E("\"\xED\xBF\xBF\"", UTF8),
    E("\"\xF0\x8F\xBF\xBF\"", UTF8), E("\"\xF4\x90\x80\x80\"", UTF8),
    E("\"\xF5\x80\x80\x80\"", UTF8), E("\"\xFF\"", UTF8), E("\"\x80\"", UTF8),
    E("\"\xBF\"", UTF8), E("\"\xC3(\"", UTF8), E("\"\xF0\x9F\x98\"", UTF8),
    E("\"\xE2\x82\"", UTF8), E("{\"\xC3\":1}", UTF8),
    /* limits: depth */
    VL("[[1]]", "[ [ n:1 ] ]", 0, 2, 200, 40, 4096),
    VL("[[[1]]]", NULL, CANLIN_JSON_E_DEPTH, 2, 200, 40, 4096),
    VL("{\"a\":{\"b\":{}}}", NULL, CANLIN_JSON_E_DEPTH, 2, 200, 40, 4096),
    VL("[1]", "[ n:1 ]", 0, 1, 200, 40, 4096),
    VL("[[", NULL, CANLIN_JSON_E_DEPTH, 1, 200, 40, 4096),
    /* limits: string bytes (decoded) */
    VL("\"abc\"", "s:abc", 0, 4, 3, 3, 4096),
    VL("\"abcd\"", NULL, CANLIN_JSON_E_STRING_LEN, 4, 3, 3, 4096),
    VL("\"abcd", NULL, CANLIN_JSON_E_STRING_LEN, 4, 3, 3, 4096),
    VL("{\"abcd\":1}", NULL, CANLIN_JSON_E_STRING_LEN, 4, 3, 3, 4096),
    VL("{\"abc\":1}", "{ k:abc n:1 }", 0, 4, 3, 3, 4096),
    VL("\"\\u20ac\"", "s:\xE2\x82\xAC", 0, 4, 3, 3, 4096),
    VL("\"a\\u20ac\"", NULL, CANLIN_JSON_E_STRING_LEN, 4, 3, 3, 4096),
    VL("\"\xF0\x9F\x98\x80\"", NULL, CANLIN_JSON_E_STRING_LEN, 4, 3, 3, 4096),
    VL("\"\\ud83d\\ude00\"", NULL, CANLIN_JSON_E_STRING_LEN, 4, 3, 3, 4096),
    VL("\"\\\\\\\\\\\\\"", "s:\\\\\\", 0, 4, 3, 3, 4096),
    /* limits: number chars */
    VL("123", "n:123", 0, 4, 3, 3, 4096),
    VL("1234", NULL, CANLIN_JSON_E_NUMBER_LEN, 4, 3, 3, 4096),
    VL("[-1.5]", NULL, CANLIN_JSON_E_NUMBER_LEN, 4, 3, 3, 4096),
    VL("[1e5]", "[ n:1e5 ]", 0, 4, 3, 3, 4096),
    /* limits: total bytes, whitespace included */
    VL("[1,2]", "[ n:1 n:2 ]", 0, 4, 3, 3, 5),
    VL("[1,2] ", NULL, CANLIN_JSON_E_TOTAL, 4, 3, 3, 5),
    VL("[1, 2]", NULL, CANLIN_JSON_E_TOTAL, 4, 3, 3, 5),
    VL("\"abc\"", "s:abc", 0, 4, 3, 3, 5),
    /* callback abort */
    V("[1,null,2]", NULL, CANLIN_JSON_E_ABORTED),
};

static void check_vec(const struct vec *v, int idx)
{
    logbuf ref, l;
    size_t cuts[2], i, j;
    int rc, mode = v->rc == CANLIN_JSON_E_ABORTED;

    rc = run(v->in, v->n, &v->lim, NULL, 0, 0, &ref, mode);
    CHECK(rc == v->rc, "vec %d whole: rc %d (%s) want %d", idx, rc, canlin_json_strerror(rc), v->rc);
    if (v->log)
        CHECK(!strcmp(ref.buf, v->log), "vec %d: log [%s] want [%s]", idx, ref.buf, v->log);
    rc = run(v->in, v->n, &v->lim, NULL, 0, 1, &l, mode);
    CHECK(rc == v->rc && !strcmp(l.buf, ref.buf), "vec %d bytewise: rc %d log [%s]", idx, rc, l.buf);
    for (i = 0; i <= v->n; i++) {
        cuts[0] = i;
        rc = run(v->in, v->n, &v->lim, cuts, 1, 0, &l, mode);
        CHECK(rc == v->rc && !strcmp(l.buf, ref.buf), "vec %d split %zu: rc %d", idx, i, rc);
        for (j = i; j <= v->n; j++) {
            cuts[1] = j;
            rc = run(v->in, v->n, &v->lim, cuts, 2, 0, &l, mode);
            CHECK(rc == v->rc && !strcmp(l.buf, ref.buf), "vec %d split %zu/%zu: rc %d",
                  idx, i, j, rc);
        }
    }
}

static void test_misc(void)
{
    static const char doc[] = "{\"a\":[1,{\"b\":null}],\"c\":\"d\"}";
    canlin_json p;
    canlin_json_limits lim = deflim;
    logbuf l;
    int rc;

    rc = run(doc, sizeof doc - 1, &deflim, NULL, 0, 0, &l, 2);
    CHECK(rc == 0 && !strcmp(l.buf, "{@0 k:a@1 [@1 n:1@2 {@2 k:b@3 N@3 }@2 ]@1 k:c@1 s:d@1 }@0"),
          "depth log [%s]", l.buf);

    CHECK(canlin_json_init(&p, &deflim, tokbuf, sizeof tokbuf, on_tok, &l) == 0, "init");
    CHECK(canlin_json_feed(&p, "[1]", 3) == 0 && canlin_json_finish(&p) == 0, "doc");
    CHECK(canlin_json_feed(&p, " ", 1) == CANLIN_JSON_E_STATE, "feed after finish");
    CHECK(canlin_json_finish(&p) == CANLIN_JSON_E_STATE, "finish twice");

    CHECK(canlin_json_init(&p, &deflim, tokbuf, 10, on_tok, &l) == CANLIN_JSON_E_ARG, "small buf");
    CHECK(canlin_json_feed(&p, "1", 1) == CANLIN_JSON_E_ARG, "feed after failed init");
    lim.max_depth = CANLIN_JSON_DEPTH_CAP + 1;
    CHECK(canlin_json_init(&p, &lim, tokbuf, sizeof tokbuf, on_tok, &l) == CANLIN_JSON_E_ARG, "depth cap");
    lim.max_depth = 0;
    CHECK(canlin_json_init(&p, &lim, tokbuf, sizeof tokbuf, on_tok, &l) == CANLIN_JSON_E_ARG, "depth 0");

    /* error offset: p.total - 1 is the offending byte */
    canlin_json_init(&p, &deflim, tokbuf, sizeof tokbuf, on_tok, &l);
    rc = canlin_json_feed(&p, "[1,2,x]", 7);
    CHECK(rc == CANLIN_JSON_E_SYNTAX && p.total - 1 == 5, "error offset %zu", p.total - 1);

    /* deep nesting at the cap */
    {
        static char deep[2 * CANLIN_JSON_DEPTH_CAP + 2];
        unsigned i;
        lim = deflim;
        lim.max_depth = CANLIN_JSON_DEPTH_CAP;
        lim.max_total = sizeof deep;
        for (i = 0; i < CANLIN_JSON_DEPTH_CAP; i++) {
            deep[i] = '[';
            deep[2 * CANLIN_JSON_DEPTH_CAP - 1 - i] = ']';
        }
        canlin_json_init(&p, &lim, tokbuf, sizeof tokbuf, on_tok, &l);
        memset(&l, 0, sizeof l);
        CHECK(canlin_json_feed(&p, deep, 2 * CANLIN_JSON_DEPTH_CAP) == 0 &&
              canlin_json_finish(&p) == 0, "depth cap nesting");
        deep[CANLIN_JSON_DEPTH_CAP] = '[';
        canlin_json_init(&p, &lim, tokbuf, sizeof tokbuf, on_tok, &l);
        memset(&l, 0, sizeof l);
        CHECK(canlin_json_feed(&p, deep, CANLIN_JSON_DEPTH_CAP + 1) == CANLIN_JSON_E_DEPTH,
              "depth cap + 1");
        /* mixed object/array stack bits across byte boundaries */
        for (i = 0; i < 20; i++) {
            deep[4 * i] = '{'; deep[4 * i + 1] = '"'; deep[4 * i + 2] = '"'; deep[4 * i + 3] = ':';
        }
        memcpy(deep + 80, "[[[1]]]", 7);
        for (i = 0; i < 20; i++)
            deep[87 + i] = '}';
        canlin_json_init(&p, &lim, tokbuf, sizeof tokbuf, on_tok, &l);
        memset(&l, 0, sizeof l);
        CHECK(canlin_json_feed(&p, deep, 107) == 0 && canlin_json_finish(&p) == 0, "mixed stack");
        deep[106] = ']';
        canlin_json_init(&p, &lim, tokbuf, sizeof tokbuf, on_tok, &l);
        memset(&l, 0, sizeof l);
        CHECK(canlin_json_feed(&p, deep, 107) == CANLIN_JSON_E_SYNTAX, "mismatched close");
    }
}

static unsigned long rng = 0x9E3779B9ul;
static unsigned rnd(unsigned n)
{
    rng ^= rng << 13; rng &= 0xFFFFFFFFul;
    rng ^= rng >> 17;
    rng ^= rng << 5; rng &= 0xFFFFFFFFul;
    return (unsigned)(rng % n);
}

static void fuzz(void)
{
    static const char alpha[] = "{}[]\",:0123456789.eE+-truefalsn \\u\xC3\xA9\xED\xF0\x9F\x80\xBF";
    char in[200];
    logbuf ref, l;
    size_t cuts[8], n;
    canlin_json_limits lim;
    int it, k, rc0, rc, ncut, nok = 0;

    for (it = 0; it < 30000; it++) {
        const struct vec *v = &vecs[rnd(sizeof vecs / sizeof vecs[0])];
        if (rnd(4)) {
            n = v->n;
            memcpy(in, v->in, n);
            for (k = rnd(4); k >= 0; k--) {
                unsigned op = rnd(4);
                if (op == 0 && n) in[rnd((unsigned)n)] = alpha[rnd(sizeof alpha - 1)];
                else if (op == 1 && n + 8 < sizeof in) {
                    /* splice a random slice of another vector */
                    const struct vec *w = &vecs[rnd(sizeof vecs / sizeof vecs[0])];
                    size_t at = rnd((unsigned)n + 1), from, len;
                    if (!w->n) continue;
                    from = rnd((unsigned)w->n);
                    len = 1 + rnd(8);
                    if (len > w->n - from) len = w->n - from;
                    memmove(in + at + len, in + at, n - at);
                    memcpy(in + at, w->in + from, len);
                    n += len;
                } else if (op == 2 && n) n = rnd((unsigned)n);
                else if (op == 3 && n < sizeof in) {
                    size_t at = rnd((unsigned)n + 1);
                    memmove(in + at + 1, in + at, n - at);
                    in[at] = (char)rnd(256);
                    n++;
                }
            }
        } else {
            n = rnd(sizeof in);
            for (k = 0; k < (int)n; k++) in[k] = alpha[rnd(sizeof alpha - 1)];
        }
        lim.max_depth = 1 + rnd(6);
        lim.max_string = rnd(12);
        lim.max_number = 1 + rnd(10);
        lim.max_total = 1 + rnd(220);
        rc0 = run(in, n, &lim, NULL, 0, 0, &ref, 0);
        CHECK(!ref.overflow && rc0 <= 0 && rc0 > CANLIN_JSON_E_ABORTED, "fuzz %d rc %d", it, rc0);
        nok += rc0 == 0;
        rc = run(in, n, &lim, NULL, 0, 1, &l, 0);
        CHECK(rc == rc0 && !strcmp(l.buf, ref.buf), "fuzz %d bytewise differs", it);
        ncut = (int)rnd(8);
        for (k = 0; k < ncut; k++) {
            size_t lo = k ? cuts[k - 1] : 0;
            cuts[k] = lo + rnd((unsigned)(n - lo) + 1);
        }
        rc = run(in, n, &lim, cuts, ncut, 0, &l, 0);
        CHECK(rc == rc0 && !strcmp(l.buf, ref.buf), "fuzz %d chunked differs", it);
    }
    CHECK(nok > 100, "fuzz produced too few valid documents (%d)", nok);
}

int main(void)
{
    size_t i;
    for (i = 0; i < sizeof vecs / sizeof vecs[0]; i++)
        check_vec(&vecs[i], (int)i);
    test_misc();
    fuzz();
    if (failures) {
        fprintf(stderr, "json_test: %d failures\n", failures);
        return 1;
    }
    printf("json_test: %zu vectors (all split points, 2- and 3-chunk, bytewise), "
           "misc tests, 30000 fuzz iterations: ok\n", sizeof vecs / sizeof vecs[0]);
    return 0;
}
