/* SSE parser: table vectors at every split point, limits, EOF, fuzz. Exit 0 = pass. */
#include "canlin/sse.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

typedef struct { char buf[8192]; size_t len; int overflow; } logbuf;

static void lput(logbuf *l, const char *s, size_t n)
{
    if (n > sizeof l->buf - 1 - l->len) { l->overflow = 1; return; }
    memcpy(l->buf + l->len, s, n);
    l->len += n;
    l->buf[l->len] = 0;
}

static int on_event(void *ud, const canlin_sse_event *ev)
{
    logbuf *l = ud;
    lput(l, ev->type, ev->type_len);
    lput(l, "|", 1);
    lput(l, ev->data, ev->data_len);
    lput(l, "|", 1);
    lput(l, ev->id, ev->id_len);
    lput(l, ";", 1);
    return ev->type_len == 4 && !memcmp(ev->type, "stop", 4);
}

#define MAXL 64
#define MAXD 128
static unsigned char mem[CANLIN_SSE_MEM(MAXL, MAXD)];

/* cuts: sorted split offsets (ncut of them); step 1 = byte by byte */
static int run(const char *in, size_t n, size_t ml, size_t md, const size_t *cuts,
               int ncut, int bytewise, logbuf *l, canlin_sse *out)
{
    canlin_sse p;
    size_t pos = 0;
    int i, rc;

    memset(l, 0, sizeof *l);
    rc = canlin_sse_init(&p, mem, CANLIN_SSE_MEM(ml, md), ml, md, on_event, l);
    if (rc)
        return 100;
    if (bytewise) {
        for (pos = 0; pos < n; pos++)
            if ((rc = canlin_sse_feed(&p, in + pos, 1)) != 0)
                break;
    } else {
        rc = 0;
        for (i = 0; i <= ncut && !rc; i++) {
            size_t end = i < ncut ? cuts[i] : n;
            rc = canlin_sse_feed(&p, in + pos, end - pos);
            pos = end;
        }
    }
    if (!rc)
        rc = canlin_sse_finish(&p);
    else
        CHECK(canlin_sse_finish(&p) == rc, "error not sticky");
    if (out)
        *out = p;
    return rc;
}

struct vec {
    const char *in;
    size_t n;
    const char *log;
    int rc;
    size_t ml, md;
};

#define V(s, log, rc) { s, sizeof(s) - 1, log, rc, MAXL, MAXD }
#define VL(s, log, rc, ml, md) { s, sizeof(s) - 1, log, rc, ml, md }

static const struct vec vecs[] = {
    V("data: hello\n\n", "message|hello|;", 0),
    V("data:a\r\ndata: b\r\n\r\n", "message|a\nb|;", 0),
    V("data: a\r\n\r\ndata: b\r\n\r\n", "message|a|;message|b|;", 0),
    V("data: x\rdata:y\r\r", "message|x\ny|;", 0),
    V("data: x\ndata:y\r\n\r", "message|x\ny|;", 0),
    V("\xEF\xBB\xBF" "data: z\n\n", "message|z|;", 0),
    V("\xEF\xBB\xBF\xEF\xBB\xBF" "data: z\n\n", "", 0), /* second BOM is not stripped */
    V("\xEF" "data: z\n\n", "", 0),                     /* partial BOM is field name */
    V(": hi\ndata: q\n: mid\n\n", "message|q|;", 0),
    V(":\n\n", "", 0),
    V("event: tool\nid: 7\ndata: {}\n\n", "tool|{}|7;", 0),
    V("id: 1\ndata: a\n\ndata: b\n\n", "message|a|1;message|b|1;", 0),
    V("id: 1\ndata: a\n\nid\ndata: b\n\n", "message|a|1;message|b|;", 0),
    V("data\n\n", "message||;", 0),
    V("data\ndata\n\n", "message|\n|;", 0),
    V("data:  two\n\n", "message| two|;", 0),
    V("data:\tt\n\n", "message|\tt|;", 0),
    V("data: a:b\n\n", "message|a:b|;", 0),
    V("event: x\n\ndata: y\n\n", "message|y|;", 0),
    V("event\ndata: y\n\n", "message|y|;", 0),
    V("retry: 1500\n\n", "", 0),
    V("foo: bar\ndata: 1\n\n", "message|1|;", 0),
    V("Data: 1\n\n", "", 0),
    V("id: 1\n\ndata: a\nid: x\0y\n\n", "message|a|1;", 0),
    V("data: \xF0\x9F\x98\x80\n\n", "message|\xF0\x9F\x98\x80|;", 0),
    V("\n\n\r\n\r", "", 0),
    V("", "", 0),
    V("\xEF\xBB\xBF", "", 0),
    V("data: a\n\n: trailing comment\n", "message|a|;", 0),
    /* callback abort */
    V("event: stop\ndata: s\n\ndata: t\n\n", "stop|s|;", CANLIN_SSE_E_ABORTED),
    /* limits: line <= 8, data <= 4 */
    VL("data: 12\n\n", "message|12|;", 0, 8, 4),
    VL("data: 123\n\n", "", CANLIN_SSE_E_LINE, 8, 4),
    VL(":2345678\n\n", "", 0, 8, 4),
    VL(":23456789\n\n", "", CANLIN_SSE_E_LINE, 8, 4),
    VL("data:1234\n\n", "message|1234|;", 0, 10, 4),
    VL("data:12345\n\n", "", CANLIN_SSE_E_DATA, 10, 4),
    VL("data: 12\ndata: 12\n\n", "", CANLIN_SSE_E_DATA, 10, 4),
    VL("data: 123\ndata\n\n", "message|123\n|;", 0, 10, 4),
    VL("data: 12\ndata: 1\n\n", "message|12\n1|;", 0, 10, 4),
    VL("data\ndata\ndata\ndata\ndata\n\n", "message|\n\n\n\n|;", 0, 10, 4),
    VL("data\ndata\ndata\ndata\ndata\ndata\n\n", "", CANLIN_SSE_E_DATA, 10, 4),
    /* truncation */
    V("data: a\n", "", CANLIN_SSE_E_INCOMPLETE),
    V("data: a", "", CANLIN_SSE_E_INCOMPLETE),
    V("data: a\n\ndata: b\r", "message|a|;", CANLIN_SSE_E_INCOMPLETE),
    V("data: a\n\n: c", "message|a|;", CANLIN_SSE_E_INCOMPLETE),
    V("event: x\n", "", CANLIN_SSE_E_INCOMPLETE),
    V("\xEF\xBB", "", CANLIN_SSE_E_INCOMPLETE),
};

static void check_vec(const struct vec *v, int idx)
{
    logbuf ref, l;
    size_t cuts[2], i, j;
    int rc;

    rc = run(v->in, v->n, v->ml, v->md, NULL, 0, 0, &ref, NULL);
    CHECK(rc == v->rc, "vec %d whole: rc %d want %d", idx, rc, v->rc);
    CHECK(!strcmp(ref.buf, v->log), "vec %d whole: log [%s] want [%s]", idx, ref.buf, v->log);
    rc = run(v->in, v->n, v->ml, v->md, NULL, 0, 1, &l, NULL);
    CHECK(rc == v->rc && !strcmp(l.buf, v->log), "vec %d bytewise: rc %d log [%s]", idx, rc, l.buf);
    for (i = 0; i <= v->n; i++) {
        cuts[0] = i;
        rc = run(v->in, v->n, v->ml, v->md, cuts, 1, 0, &l, NULL);
        CHECK(rc == v->rc && !strcmp(l.buf, v->log), "vec %d split %zu: rc %d log [%s]",
              idx, i, rc, l.buf);
        for (j = i; j <= v->n; j++) {
            cuts[1] = j;
            rc = run(v->in, v->n, v->ml, v->md, cuts, 2, 0, &l, NULL);
            CHECK(rc == v->rc && !strcmp(l.buf, v->log), "vec %d split %zu/%zu: rc %d",
                  idx, i, j, rc);
        }
    }
}

static void test_state(void)
{
    logbuf l;
    canlin_sse p;
    size_t len;
    const char *id;
    int rc;

    rc = run("retry: 1500\n\nretry: 15a\n\nretry:\n\n", strlen("retry: 1500\n\nretry: 15a\n\nretry:\n\n"), MAXL, MAXD, NULL, 0, 0, &l, &p);
    CHECK(rc == 0 && p.retry_set && p.retry_ms == 1500, "retry: rc %d set %d ms %lu",
          rc, p.retry_set, p.retry_ms);
    rc = run("retry: 4294967296\n\n", strlen("retry: 4294967296\n\n"), MAXL, MAXD, NULL, 0, 0, &l, &p);
    CHECK(rc == 0 && !p.retry_set, "retry overflow ignored");
    rc = run("retry: 4294967295\n\n", strlen("retry: 4294967295\n\n"), MAXL, MAXD, NULL, 0, 0, &l, &p);
    CHECK(rc == 0 && p.retry_set && p.retry_ms == 4294967295ul, "retry max");

    /* id without data is still committed at a blank line */
    rc = run("id: 5\n\n", strlen("id: 5\n\n"), MAXL, MAXD, NULL, 0, 0, &l, &p);
    id = canlin_sse_last_id(&p, &len);
    CHECK(rc == 0 && len == 1 && id[0] == '5' && l.len == 0, "id commit");
    /* uncommitted id at EOF */
    rc = run("id: 5\n\nid: 9\n", strlen("id: 5\n\nid: 9\n"), MAXL, MAXD, NULL, 0, 0, &l, &p);
    id = canlin_sse_last_id(&p, &len);
    CHECK(rc == CANLIN_SSE_E_INCOMPLETE && len == 1 && id[0] == '5', "id uncommitted");

    /* use after finish, bad init */
    CHECK(canlin_sse_init(&p, mem, sizeof mem, MAXL, MAXD, on_event, &l) == 0, "init");
    CHECK(canlin_sse_finish(&p) == 0, "finish empty");
    CHECK(canlin_sse_feed(&p, "x", 1) == CANLIN_SSE_E_STATE, "feed after finish");
    CHECK(canlin_sse_init(&p, mem, sizeof mem - 1, MAXL, MAXD, on_event, &l) == CANLIN_SSE_E_ARG,
          "short mem");
    CHECK(canlin_sse_feed(&p, "x", 1) == CANLIN_SSE_E_ARG, "feed after failed init");
    CHECK(canlin_sse_init(&p, mem, sizeof mem, 0, MAXD, on_event, &l) == CANLIN_SSE_E_ARG, "0 line");
    CHECK(canlin_sse_init(&p, mem, (size_t)-1, (size_t)-1 / 2, 8, on_event, &l) == CANLIN_SSE_E_ARG,
          "mem overflow");
}

static unsigned long rng = 0x2545F491ul;
static unsigned rnd(unsigned n)
{
    rng ^= rng << 13; rng &= 0xFFFFFFFFul;
    rng ^= rng >> 17;
    rng ^= rng << 5; rng &= 0xFFFFFFFFul;
    return (unsigned)(rng % n);
}

static void fuzz(void)
{
    static const char alpha[] = "data: eventidryx\r\n\r\n\n::\xEF\xBB\xBF\0";
    char in[160];
    logbuf ref, l;
    size_t cuts[8], n, ml, md;
    int it, k, rc0, rc, ncut;

    for (it = 0; it < 20000; it++) {
        const struct vec *v = &vecs[rnd(sizeof vecs / sizeof vecs[0])];
        if (rnd(3)) {
            n = v->n;
            memcpy(in, v->in, n);
            for (k = rnd(4); k >= 0; k--) {
                unsigned op = rnd(3);
                if (op == 0 && n) in[rnd((unsigned)n)] = alpha[rnd(sizeof alpha - 1)];
                else if (op == 1 && n < sizeof in) {
                    size_t at = rnd((unsigned)n + 1);
                    memmove(in + at + 1, in + at, n - at);
                    in[at] = rnd(8) ? alpha[rnd(sizeof alpha - 1)] : (char)rnd(256);
                    n++;
                } else if (n) n = rnd((unsigned)n);
            }
        } else {
            n = rnd(sizeof in);
            for (k = 0; k < (int)n; k++) in[k] = alpha[rnd(sizeof alpha - 1)];
        }
        ml = 1 + rnd(20);
        md = 1 + rnd(40);
        rc0 = run(in, n, ml, md, NULL, 0, 0, &ref, NULL);
        CHECK(!ref.overflow, "log overflow");
        rc = run(in, n, ml, md, NULL, 0, 1, &l, NULL);
        CHECK(rc == rc0 && !strcmp(l.buf, ref.buf), "fuzz %d bytewise differs", it);
        ncut = (int)rnd(8);
        for (k = 0; k < ncut; k++) {
            size_t lo = k ? cuts[k - 1] : 0;
            cuts[k] = lo + rnd((unsigned)(n - lo) + 1);
        }
        rc = run(in, n, ml, md, cuts, ncut, 0, &l, NULL);
        CHECK(rc == rc0 && !strcmp(l.buf, ref.buf), "fuzz %d chunked differs", it);
        CHECK(rc0 <= 0 && rc0 >= CANLIN_SSE_E_ABORTED, "fuzz %d odd rc %d", it, rc0);
    }
}

int main(void)
{
    size_t i;
    for (i = 0; i < sizeof vecs / sizeof vecs[0]; i++)
        check_vec(&vecs[i], (int)i);
    test_state();
    fuzz();
    if (failures) {
        fprintf(stderr, "sse_test: %d failures\n", failures);
        return 1;
    }
    printf("sse_test: %zu vectors (all split points, 2- and 3-chunk, bytewise), "
           "state tests, 20000 fuzz iterations: ok\n", sizeof vecs / sizeof vecs[0]);
    return 0;
}
