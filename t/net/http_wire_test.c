/* HTTP wire primitives (HTTP transport spec §6, §9): the response parser fed at every
 * split point (and byte by byte, and with a consumer that pauses after every body
 * callback), the request writer and URL handling. Exit 0 = pass. */
#include "canlin/http_wire.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

/* ---- parser harness ---- */

typedef struct run_log {
    char *s;
    size_t n, cap;
    int heads;
    int pause;    /* body callback asks to pause every time */
    int stop;     /* head callback asks to stop */
} run_log;

static void lput(run_log *l, const char *s, size_t n)
{
    if (l->n + n + 1 > l->cap) {
        size_t cap = (l->cap + n + 1) * 2;
        char *p = realloc(l->s, cap);
        if (p == NULL) {
            fprintf(stderr, "oom\n");
            exit(2);
        }
        l->s = p;
        l->cap = cap;
    }
    memcpy(l->s + l->n, s, n);
    l->n += n;
    l->s[l->n] = 0;
}

static int on_head(void *ud, int status, const canlin_http_field *f, size_t n)
{
    run_log *l = ud;
    char b[32];
    size_t i;
    l->heads++;
    snprintf(b, sizeof b, "S%d", status);
    lput(l, b, strlen(b));
    for (i = 0; i < n; i++) {
        lput(l, "|", 1);
        lput(l, f[i].name, strlen(f[i].name));
        lput(l, "=", 1);
        lput(l, f[i].value, strlen(f[i].value));
    }
    lput(l, "#", 1);
    return l->stop;
}

static int on_body(void *ud, const char *buf, size_t n)
{
    run_log *l = ud;
    lput(l, buf, n);
    return l->pause;
}

static const canlin_http_parser_cb pcb = {on_head, on_body};
static char pmem[CANLIN_HTTP_PARSER_MEM];
static const char *last_why;

/* Feed `in` cut at the sorted offsets cuts[0..ncut); eof: signal EOF at the end.
 * Returns the final parser result; *log gets the callback log. */
static int run(const char *in, size_t n, const size_t *cuts, size_t ncut, int eof, int pause,
               int stop, run_log *log)
{
    canlin_http_parser p;
    size_t start = 0, k, used;
    int rc = CANLIN_HTTP_P_MORE;

    memset(log, 0, sizeof *log);
    lput(log, "", 0);
    log->pause = pause;
    log->stop = stop;
    canlin_http_parser_init(&p, pmem, 0, &pcb, log);
    for (k = 0; k <= ncut; k++) {
        size_t end = k < ncut ? cuts[k] : n;
        while (start < end) {
            rc = canlin_http_parser_feed(&p, in + start, end - start, &used);
            last_why = canlin_http_parser_why(&p);
            if (rc < 0)
                return rc;
            if (rc == CANLIN_HTTP_P_DONE || rc == CANLIN_HTTP_P_STOPPED)
                return rc; /* rest ignored */
            if (used == 0 && rc == CANLIN_HTTP_P_PAUSED) {
                fprintf(stderr, "paused without progress\n");
                return -99;
            }
            start += used;
        }
    }
    if (eof)
        rc = canlin_http_parser_eof(&p);
    last_why = canlin_http_parser_why(&p);
    return rc;
}

typedef struct vec {
    const char *name;
    const char *in;
    size_t len;  /* 0 = strlen */
    int eof;     /* EOF after the input */
    int want;    /* final result */
    const char *log; /* expected callback log, NULL = don't care */
    const char *why; /* expected error reason, NULL = don't care */
} vec;

#define CHUNKED "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"

static const vec vecs[] = {
    {"content-length", "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 5\r\n\r\nhelloXYZ",
     0, 0, CANLIN_HTTP_P_DONE, "S200|Content-Type=text/plain|Content-Length=5#hello", NULL},
    {"content-length zero", "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n", 0, 0,
     CANLIN_HTTP_P_DONE, "S200|Content-Length=0#", NULL},
    {"duplicate equal content-length", "HTTP/1.1 200 OK\r\nContent-Length: 2\r\ncontent-length: 2\r\n\r\nab",
     0, 0, CANLIN_HTTP_P_DONE, "S200|Content-Length=2|content-length=2#ab", NULL},
    {"chunked + ext + trailers", CHUNKED "5;ext=1\r\nhello\r\nA \r\n, world!!!\r\n0\r\nX-Trailer: t\r\n\r\nJUNK",
     0, 0, CANLIN_HTTP_P_DONE, "S200|Transfer-Encoding=chunked#hello, world!!!", NULL},
    {"chunked uppercase hex, LF only", "HTTP/1.1 200 OK\nTransfer-Encoding: Chunked\n\nB\nhello world\n0\n\n",
     0, 0, CANLIN_HTTP_P_DONE, "S200|Transfer-Encoding=Chunked#hello world", NULL},
    {"chunked leading zeros", CHUNKED "00000003\r\nabc\r\n0\r\n\r\n", 0, 0, CANLIN_HTTP_P_DONE,
     "S200|Transfer-Encoding=chunked#abc", NULL},
    {"close-delimited", "HTTP/1.0 200 OK\r\nX: y\r\n\r\nabc", 0, 1, CANLIN_HTTP_P_DONE, "S200|X=y#abc", NULL},
    {"close-delimited empty", "HTTP/1.1 200 OK\r\n\r\n", 0, 1, CANLIN_HTTP_P_DONE, "S200#", NULL},
    {"1xx skipped", "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 103 Early Hints\r\nLink: </a>\r\n\r\n"
     "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok", 0, 0, CANLIN_HTTP_P_DONE, "S200|Content-Length=2#ok", NULL},
    {"204 no body", "HTTP/1.1 204 No Content\r\nX: 1\r\n\r\n", 0, 0, CANLIN_HTTP_P_DONE, "S204|X=1#", NULL},
    {"304 no body", "HTTP/1.1 304 Not Modified\r\nContent-Length: 10\r\n\r\n", 0, 0,
     CANLIN_HTTP_P_DONE, "S304|Content-Length=10#", NULL},
    {"status without reason", "HTTP/1.1 404\r\nContent-Length: 1\r\n\r\nx", 0, 0, CANLIN_HTTP_P_DONE,
     "S404|Content-Length=1#x", NULL},
    {"OWS trimmed, empty value", "HTTP/1.1 200 OK\r\nX-A:   v w \t\r\nX-B:\r\nContent-Length: 0\r\n\r\n",
     0, 0, CANLIN_HTTP_P_DONE, "S200|X-A=v w|X-B=|Content-Length=0#", NULL},
    {"obs-text value", "HTTP/1.1 200 OK\r\nX: gr\xc3\xbc\xc3\x9f\r\nContent-Length: 0\r\n\r\n", 0, 0,
     CANLIN_HTTP_P_DONE, "S200|X=gr\xc3\xbc\xc3\x9f|Content-Length=0#", NULL},
    {"identity content-encoding", "HTTP/1.1 200 OK\r\nContent-Encoding: identity\r\nContent-Length: 0\r\n\r\n",
     0, 0, CANLIN_HTTP_P_DONE, NULL, NULL},
    /* errors */
    {"obs-fold", "HTTP/1.1 200 OK\r\nX: a\r\n  b\r\nContent-Length: 0\r\n\r\n", 0, 0,
     CANLIN_HTTP_P_EPROTOCOL, "", "obsolete line folding"},
    {"obs-fold tab", "HTTP/1.1 200 OK\r\nX: a\r\n\tb\r\n\r\n", 0, 0, CANLIN_HTTP_P_EPROTOCOL, "", "obsolete line folding"},
    {"both framing headers", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Length: 3\r\n\r\n0\r\n\r\n",
     0, 0, CANLIN_HTTP_P_EPROTOCOL, "", "both transfer-encoding and content-length"},
    {"conflicting content-length", "HTTP/1.1 200 OK\r\nContent-Length: 3\r\nContent-Length: 4\r\n\r\nabcd",
     0, 0, CANLIN_HTTP_P_EPROTOCOL, "", "conflicting content-length"},
    {"content-length list", "HTTP/1.1 200 OK\r\nContent-Length: 3, 3\r\n\r\nabc", 0, 0,
     CANLIN_HTTP_P_EPROTOCOL, "", "malformed content-length"},
    {"content-length sign", "HTTP/1.1 200 OK\r\nContent-Length: +3\r\n\r\nabc", 0, 0,
     CANLIN_HTTP_P_EPROTOCOL, "", NULL},
    {"content-length overflow", "HTTP/1.1 200 OK\r\nContent-Length: 99999999999999999999\r\n\r\n", 0, 0,
     CANLIN_HTTP_P_EPROTOCOL, "", NULL},
    {"gzip content-encoding", "HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Length: 3\r\n\r\nabc",
     0, 0, CANLIN_HTTP_P_EPROTOCOL, "", "unsupported content-encoding"},
    {"gzip transfer-encoding", "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n0\r\n\r\n",
     0, 0, CANLIN_HTTP_P_EPROTOCOL, "", "unsupported transfer-encoding"},
    {"two transfer-encodings", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: chunked\r\n\r\n",
     0, 0, CANLIN_HTTP_P_EPROTOCOL, "", NULL},
    {"bad chunk size non-hex", CHUNKED "zz\r\nab\r\n0\r\n\r\n", 0, 0, CANLIN_HTTP_P_EPROTOCOL,
     "S200|Transfer-Encoding=chunked#", "bad chunk size"},
    {"bad chunk size empty", CHUNKED "\r\nab\r\n", 0, 0, CANLIN_HTTP_P_EPROTOCOL, NULL, "bad chunk size"},
    {"bad chunk size trailing junk", CHUNKED "5x\r\nhello\r\n", 0, 0, CANLIN_HTTP_P_EPROTOCOL, NULL, "bad chunk size"},
    {"bad chunk size too many digits", CHUNKED "100000000\r\n", 0, 0, CANLIN_HTTP_P_EPROTOCOL, NULL, "bad chunk size"},
    {"bad chunk size negative", CHUNKED "-1\r\n", 0, 0, CANLIN_HTTP_P_EPROTOCOL, NULL, NULL},
    {"missing CRLF after chunk", CHUNKED "5\r\nhelloX\r\n0\r\n\r\n", 0, 0, CANLIN_HTTP_P_EPROTOCOL,
     "S200|Transfer-Encoding=chunked#hello", "missing CRLF after chunk"},
    {"EOF mid-chunk", CHUNKED "5\r\nhel", 0, 1, CANLIN_HTTP_P_EEOF, "S200|Transfer-Encoding=chunked#hel", "eof inside a chunked body"},
    {"EOF before last chunk", CHUNKED "3\r\nabc\r\n", 0, 1, CANLIN_HTTP_P_EEOF,
     "S200|Transfer-Encoding=chunked#abc", NULL},
    {"EOF in trailers", CHUNKED "0\r\nX: 1\r\n", 0, 1, CANLIN_HTTP_P_EEOF, NULL, NULL},
    {"EOF mid content-length", "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nabc", 0, 1, CANLIN_HTTP_P_EEOF,
     "S200|Content-Length=10#abc", "eof before content-length bytes"},
    {"EOF mid head", "HTTP/1.1 200 OK\r\nContent-Le", 0, 1, CANLIN_HTTP_P_EEOF, "", NULL},
    {"EOF before anything", "", 0, 1, CANLIN_HTTP_P_EEOF, "", NULL},
    {"101 switch", "HTTP/1.1 101 Switching Protocols\r\nUpgrade: x\r\n\r\n", 0, 0, CANLIN_HTTP_P_EPROTOCOL, "", "unexpected protocol switch"},
    {"HTTP/2 status line", "HTTP/2 200 OK\r\n\r\n", 0, 0, CANLIN_HTTP_P_EPROTOCOL, "", NULL},
    {"not HTTP", "ICY 200 OK\r\n\r\n", 0, 0, CANLIN_HTTP_P_EPROTOCOL, "", NULL},
    {"status 600", "HTTP/1.1 600 X\r\n\r\n", 0, 0, CANLIN_HTTP_P_EPROTOCOL, "", NULL},
    {"space before colon", "HTTP/1.1 200 OK\r\nX : a\r\n\r\n", 0, 0, CANLIN_HTTP_P_EPROTOCOL, "", NULL},
    {"no colon", "HTTP/1.1 200 OK\r\nXa\r\n\r\n", 0, 0, CANLIN_HTTP_P_EPROTOCOL, "", NULL},
    {"bare CR", "HTTP/1.1 200 OK\r\nX: a\rb\r\n\r\n", 0, 0, CANLIN_HTTP_P_EPROTOCOL, "", "bare CR in line"},
    {"NUL in value", "HTTP/1.1 200 OK\r\nX: a\0b\r\n\r\n", sizeof "HTTP/1.1 200 OK\r\nX: a\0b\r\n\r\n" - 1, 0, CANLIN_HTTP_P_EPROTOCOL, "", "NUL in line"},
    {"control in value", "HTTP/1.1 200 OK\r\nX: a\x01" "b\r\n\r\n", 0, 0, CANLIN_HTTP_P_EPROTOCOL, "", NULL},
};

static void check_vec(const vec *v)
{
    size_t n = v->len ? v->len : strlen(v->in), k, cut;
    run_log ref, l;
    int rc, rc2, pause;

    rc = run(v->in, n, NULL, 0, v->eof, 0, 0, &ref);
    CHECK(rc == v->want, "%s: whole feed rc %d want %d", v->name, rc, v->want);
    if (v->log)
        CHECK(strcmp(ref.s, v->log) == 0, "%s: log '%s' want '%s'", v->name, ref.s, v->log);
    if (v->why)
        CHECK(strcmp(last_why, v->why) == 0, "%s: why '%s' want '%s'", v->name, last_why, v->why);
    for (pause = 0; pause <= 1; pause++) {
        /* two pieces at every split point */
        for (k = 1; k < n; k++) {
            cut = k;
            rc2 = run(v->in, n, &cut, 1, v->eof, pause, 0, &l);
            CHECK(rc2 == rc && strcmp(l.s, ref.s) == 0,
                  "%s: split %u pause %d: rc %d log '%s' (whole: %d '%s')", v->name, (unsigned)k,
                  pause, rc2, l.s, rc, ref.s);
            free(l.s);
            if (failures > 20)
                exit(1);
        }
        /* byte by byte */
        {
            size_t *cuts = malloc((n ? n : 1) * sizeof *cuts);
            for (k = 1; k < n; k++)
                cuts[k - 1] = k;
            rc2 = run(v->in, n, cuts, n ? n - 1 : 0, v->eof, pause, 0, &l);
            CHECK(rc2 == rc && strcmp(l.s, ref.s) == 0, "%s: bytewise pause %d: rc %d log '%s'",
                  v->name, pause, rc2, l.s);
            free(l.s);
            free(cuts);
        }
    }
    free(ref.s);
}

/* Large inputs: whole, byte by byte and 300 pseudo-random split sets. */
static void check_big(const char *name, const char *in, size_t n, int eof, int want)
{
    run_log ref, l;
    size_t *cuts = malloc(n * sizeof *cuts), k, nc;
    unsigned x = 12345;
    int rc, rc2, it;

    rc = run(in, n, NULL, 0, eof, 0, 0, &ref);
    CHECK(rc == want, "%s: rc %d want %d", name, rc, want);
    for (k = 1; k < n; k++)
        cuts[k - 1] = k;
    rc2 = run(in, n, cuts, n - 1, eof, 1, 0, &l);
    CHECK(rc2 == rc && strcmp(l.s, ref.s) == 0, "%s: bytewise", name);
    free(l.s);
    for (it = 0; it < 300; it++) {
        size_t pos = 0;
        nc = 0;
        for (;;) {
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            pos += 1 + x % 4096;
            if (pos >= n)
                break;
            cuts[nc++] = pos;
        }
        rc2 = run(in, n, cuts, nc, eof, it & 1, 0, &l);
        CHECK(rc2 == rc && strcmp(l.s, ref.s) == 0, "%s: random split %d", name, it);
        free(l.s);
    }
    free(ref.s);
    free(cuts);
}

static size_t addf(char *p, const char *s)
{
    size_t n = strlen(s);
    memcpy(p, s, n);
    return n;
}

static void test_limits(void)
{
    char *b = malloc(256 * 1024);
    size_t n, i, vl;
    char line[64];

    /* a header line exactly at CANLIN_NET_HEADER_LINE_MAX bytes: accepted */
    n = addf(b, "HTTP/1.1 200 OK\r\nX: ");
    vl = CANLIN_NET_HEADER_LINE_MAX - 3;
    memset(b + n, 'v', vl);
    n += vl;
    n += addf(b + n, "\r\nContent-Length: 0\r\n\r\n");
    check_big("line at bound", b, n, 0, CANLIN_HTTP_P_DONE);
    /* LF-only too */
    n = addf(b, "HTTP/1.1 200 OK\nX: ");
    memset(b + n, 'v', vl);
    n += vl;
    n += addf(b + n, "\nContent-Length: 0\n\n");
    check_big("line at bound LF", b, n, 0, CANLIN_HTTP_P_DONE);
    /* one byte over */
    n = addf(b, "HTTP/1.1 200 OK\r\nX: ");
    memset(b + n, 'v', vl + 1);
    n += vl + 1;
    n += addf(b + n, "\r\nContent-Length: 0\r\n\r\n");
    check_big("line over bound", b, n, 0, CANLIN_HTTP_P_ELIMIT);

    /* header count: CANLIN_NET_HEADERS_MAX ok, one more refused */
    n = addf(b, "HTTP/1.1 200 OK\r\n");
    for (i = 0; i < CANLIN_NET_HEADERS_MAX - 1; i++) {
        snprintf(line, sizeof line, "X-%u: %u\r\n", (unsigned)i, (unsigned)i);
        n += addf(b + n, line);
    }
    n += addf(b + n, "Content-Length: 0\r\n\r\n");
    check_big("headers at bound", b, n, 0, CANLIN_HTTP_P_DONE);
    n = addf(b, "HTTP/1.1 200 OK\r\n");
    for (i = 0; i < CANLIN_NET_HEADERS_MAX; i++) {
        snprintf(line, sizeof line, "X-%u: %u\r\n", (unsigned)i, (unsigned)i);
        n += addf(b + n, line);
    }
    n += addf(b + n, "Content-Length: 0\r\n\r\n");
    check_big("headers over bound", b, n, 0, CANLIN_HTTP_P_ELIMIT);

    /* total head bytes: five 7000-byte lines (each under the line bound) exceed 32 KiB */
    n = addf(b, "HTTP/1.1 200 OK\r\n");
    for (i = 0; i < 5; i++) {
        snprintf(line, sizeof line, "X-%u: ", (unsigned)i);
        n += addf(b + n, line);
        memset(b + n, 'a', 7000);
        n += 7000;
        n += addf(b + n, "\r\n");
    }
    n += addf(b + n, "Content-Length: 0\r\n\r\n");
    check_big("head total over bound", b, n, 0, CANLIN_HTTP_P_ELIMIT);
    /* four such lines fit */
    n = addf(b, "HTTP/1.1 200 OK\r\n");
    for (i = 0; i < 4; i++) {
        snprintf(line, sizeof line, "X-%u: ", (unsigned)i);
        n += addf(b + n, line);
        memset(b + n, 'a', 7000);
        n += 7000;
        n += addf(b + n, "\r\n");
    }
    n += addf(b + n, "Content-Length: 0\r\n\r\n");
    check_big("head total under bound", b, n, 0, CANLIN_HTTP_P_DONE);

    /* trailers: count and total bound apply too */
    n = addf(b, CHUNKED "1\r\na\r\n0\r\n");
    for (i = 0; i <= CANLIN_NET_HEADERS_MAX; i++) {
        snprintf(line, sizeof line, "T-%u: %u\r\n", (unsigned)i, (unsigned)i);
        n += addf(b + n, line);
    }
    n += addf(b + n, "\r\n");
    check_big("trailers over count", b, n, 0, CANLIN_HTTP_P_ELIMIT);
    n = addf(b, CHUNKED "1\r\na\r\n0\r\n");
    for (i = 0; i < 5; i++) {
        n += addf(b + n, "T: ");
        memset(b + n, 't', 7000);
        n += 7000;
        n += addf(b + n, "\r\n");
    }
    n += addf(b + n, "\r\n");
    check_big("trailers over total", b, n, 0, CANLIN_HTTP_P_ELIMIT);

    /* chunk-size line with a long extension is bounded by the line bound */
    n = addf(b, CHUNKED "1;");
    memset(b + n, 'e', CANLIN_NET_HEADER_LINE_MAX);
    n += CANLIN_NET_HEADER_LINE_MAX;
    n += addf(b + n, "\r\na\r\n0\r\n\r\n");
    check_big("chunk ext over line bound", b, n, 0, CANLIN_HTTP_P_ELIMIT);

    /* interim responses: CANLIN_NET_INTERIM_MAX skipped, one more refused */
    n = 0;
    for (i = 0; i < CANLIN_NET_INTERIM_MAX; i++)
        n += addf(b + n, "HTTP/1.1 100 Continue\r\n\r\n");
    n += addf(b + n, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
    check_big("interim at bound", b, n, 0, CANLIN_HTTP_P_DONE);
    n = 0;
    for (i = 0; i <= CANLIN_NET_INTERIM_MAX; i++)
        n += addf(b + n, "HTTP/1.1 100 Continue\r\n\r\n");
    n += addf(b + n, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
    check_big("interim over bound", b, n, 0, CANLIN_HTTP_P_ELIMIT);

    /* a long chunked body in many chunks, delivered intact at random splits */
    {
        size_t want = 0;
        run_log l;
        n = addf(b, CHUNKED);
        for (i = 0; i < 300; i++) {
            size_t c = 1 + (i * 37) % 200, k;
            snprintf(line, sizeof line, "%x\r\n", (unsigned)c);
            n += addf(b + n, line);
            for (k = 0; k < c; k++)
                b[n + k] = (char)('a' + (want + k) % 26);
            n += c;
            want += c;
            n += addf(b + n, "\r\n");
        }
        n += addf(b + n, "0\r\n\r\n");
        check_big("many chunks", b, n, 0, CANLIN_HTTP_P_DONE);
        run(b, n, NULL, 0, 0, 0, 0, &l);
        CHECK(l.n == strlen("S200|Transfer-Encoding=chunked#") + want, "many chunks body length");
        for (i = 0; i < want; i++)
            if (l.s[strlen("S200|Transfer-Encoding=chunked#") + i] != (char)('a' + i % 26)) {
                CHECK(0, "many chunks body byte %u", (unsigned)i);
                break;
            }
        free(l.s);
    }
    free(b);
}

static void test_stop_and_sticky(void)
{
    static const char r[] = "HTTP/1.1 302 Found\r\nLocation: /x\r\nContent-Length: 3\r\n\r\nabc";
    run_log l;
    canlin_http_parser p;
    size_t used;
    int rc = run(r, strlen(r), NULL, 0, 0, 0, 1, &l);
    CHECK(rc == CANLIN_HTTP_P_STOPPED, "head stop rc %d", rc);
    CHECK(strcmp(l.s, "S302|Location=/x|Content-Length=3#") == 0, "stop log '%s'", l.s);
    free(l.s);
    /* errors are sticky and carry a reason */
    memset(&l, 0, sizeof l);
    lput(&l, "", 0);
    canlin_http_parser_init(&p, pmem, 0, &pcb, &l);
    rc = canlin_http_parser_feed(&p, "HTTP/1.1 200 OK\r\nX: a\r\n b\r\n", 29, &used);
    CHECK(rc == CANLIN_HTTP_P_EPROTOCOL, "obs-fold rc %d", rc);
    CHECK(strcmp(canlin_http_parser_why(&p), "obsolete line folding") == 0, "why '%s'",
          canlin_http_parser_why(&p));
    CHECK(canlin_http_parser_feed(&p, "\r\n", 2, &used) == CANLIN_HTTP_P_EPROTOCOL, "sticky");
    CHECK(canlin_http_parser_eof(&p) == CANLIN_HTTP_P_EPROTOCOL, "sticky eof");
    free(l.s);
    /* HEAD request: no body whatever Content-Length says */
    memset(&l, 0, sizeof l);
    lput(&l, "", 0);
    canlin_http_parser_init(&p, pmem, 1, &pcb, &l);
    rc = canlin_http_parser_feed(&p, "HTTP/1.1 200 OK\r\nContent-Length: 9\r\n\r\n", 38, &used);
    CHECK(rc == CANLIN_HTTP_P_DONE && used == 38, "HEAD rc %d used %u", rc, (unsigned)used);
    free(l.s);
}

/* ---- URLs ---- */

static void test_urls(void)
{
    static const struct {
        const char *url, *origin, *host, *path;
        unsigned port;
        int ipv6, loop;
    } ok[] = {
        {"https://api.example.com/v1/chat?x=1#frag", "https://api.example.com:443", "api.example.com",
         "/v1/chat?x=1", 443, 0, 0},
        {"HTTP://LocalHost:8080", "http://localhost:8080", "localhost", "/", 8080, 0, 1},
        {"http://127.0.0.1:11434/v1", "http://127.0.0.1:11434", "127.0.0.1", "/v1", 11434, 0, 1},
        {"http://[::1]:9/x", "http://[::1]:9", "::1", "/x", 9, 1, 1},
        {"https://[2001:DB8::1]/", "https://[2001:db8::1]:443", "2001:db8::1", "/", 443, 1, 0},
        {"https://h?q", "https://h:443", "h", "/?q", 443, 0, 0},
        {"http://127.1.2.3/", "http://127.1.2.3:80", "127.1.2.3", "/", 80, 0, 1},
        {"http://127.example/", "http://127.example:80", "127.example", "/", 80, 0, 0},
        {"http://127.0.0.1.evil/", "http://127.0.0.1.evil:80", "127.0.0.1.evil", "/", 80, 0, 0},
    };
    static const char *const bad[] = {
        "ftp://x/", "https:/x", "https://", "https://user@h/", "https://h:0/", "https://h:65536/",
        "https://h:/", "https://h:1a/", "https://h x/", "https://h/ p", "https://h%41/",
        "https://[::1/", "https://[g::1]/", "https://h\\@evil/", "https://h/\x7f", "https://h\xc3\xa4/",
        "https://under_score/", "//h/", "", NULL};
    canlin_http_url u, b, r;
    size_t i;
    for (i = 0; i < sizeof ok / sizeof ok[0]; i++) {
        int rc = canlin_http_url_parse(ok[i].url, &u);
        CHECK(rc == 0, "parse %s", ok[i].url);
        if (rc)
            continue;
        CHECK(strcmp(u.origin, ok[i].origin) == 0, "%s origin %s", ok[i].url, u.origin);
        CHECK(strcmp(u.host, ok[i].host) == 0, "%s host %s", ok[i].url, u.host);
        CHECK(strcmp(u.path, ok[i].path) == 0, "%s path %s", ok[i].url, u.path);
        CHECK(u.port == ok[i].port && u.ipv6 == ok[i].ipv6, "%s port/ipv6", ok[i].url);
        CHECK(canlin_http_url_loopback(&u) == ok[i].loop, "%s loopback", ok[i].url);
    }
    for (i = 0; bad[i]; i++)
        CHECK(canlin_http_url_parse(bad[i], &u) != 0, "accepted bad url '%s'", bad[i]);
    CHECK(canlin_http_url_parse("https://h:443/a/b", &b) == 0, "base");
    CHECK(canlin_http_url_resolve(&b, "/c?d", &r) == 0 && strcmp(r.path, "/c?d") == 0 &&
              strcmp(r.origin, "https://h:443") == 0, "resolve abs path");
    CHECK(canlin_http_url_resolve(&b, "//other:8443/z", &r) == 0 &&
              strcmp(r.origin, "https://other:8443") == 0 && strcmp(r.path, "/z") == 0,
          "resolve scheme-relative");
    CHECK(canlin_http_url_resolve(&b, "http://x/y", &r) == 0 && strcmp(r.origin, "http://x:80") == 0,
          "resolve absolute");
    CHECK(canlin_http_url_resolve(&b, "c/d", &r) != 0, "relative path refused");
    CHECK(canlin_http_url_resolve(&b, "", &r) != 0, "empty refused");
    CHECK(canlin_http_url_resolve(&b, "//user@x/", &r) != 0, "userinfo refused");
}

/* ---- request writer ---- */

static void test_writer(void)
{
    static const char secret[] = "sk-secret-123";
    canlin_http_cred cred = {"https://api.example.com:443", "Authorization", "Bearer ", secret, 13};
    canlin_http_header hs[2] = {{"Content-Type", "application/json"}, {"Accept", "text/event-stream"}};
    canlin_http_header bad[1];
    canlin_http_url u;
    char out[1024];
    size_t len = 0, i;
    int rc;
    static const char want1[] =
        "POST /v1/chat/completions HTTP/1.1\r\nHost: api.example.com\r\n"
        "User-Agent: CanLin/0.1\r\nAccept-Encoding: identity\r\nConnection: close\r\n"
        "Content-Length: 42\r\nContent-Type: application/json\r\nAccept: text/event-stream\r\n"
        "Authorization: Bearer sk-secret-123\r\n\r\n";
    static const char want2[] =
        "GET / HTTP/1.1\r\nHost: [::1]:8443\r\nUser-Agent: CanLin/0.1\r\n"
        "Accept-Encoding: identity\r\nConnection: close\r\n\r\n";
    static const char *const reserved[] = {"Host", "content-length", "Transfer-Encoding",
                                           "Connection", "Accept-Encoding", "user-agent", "TE",
                                           "authorization", NULL};

    CHECK(canlin_http_url_parse("https://api.example.com/v1/chat/completions", &u) == 0, "url");
    CHECK(canlin_http_check_request("POST", hs, 2, &cred) == 0, "check ok");
    rc = canlin_http_write_request(out, sizeof out, &len, "POST", &u, hs, 2, 1, 42, &cred,
                                   "CanLin/0.1");
    CHECK(rc == 0 && len == strlen(want1) && memcmp(out, want1, len) == 0, "golden POST: %.*s",
          (int)len, out);
    /* other origin (port differs): no credential on the wire */
    CHECK(canlin_http_url_parse("https://api.example.com:8443/x", &u) == 0, "url2");
    CHECK(!canlin_http_cred_applies(&cred, &u), "cred must not apply to another port");
    memset(out, 0, sizeof out);
    rc = canlin_http_write_request(out, sizeof out, &len, "POST", &u, hs, 2, 1, 1, &cred, "CanLin/0.1");
    CHECK(rc == 0 && strstr(out, "Authorization") == NULL && strstr(out, "sk-") == NULL,
          "credential leaked to another origin");
    CHECK(canlin_http_url_parse("http://api.example.com/", &u) == 0, "url3");
    CHECK(!canlin_http_cred_applies(&cred, &u), "cred must not apply to http");
    CHECK(canlin_http_url_parse("https://[::1]:8443", &u) == 0, "url4");
    rc = canlin_http_write_request(out, sizeof out, &len, "GET", &u, NULL, 0, 0, 0, NULL, "CanLin/0.1");
    CHECK(rc == 0 && len == strlen(want2) && memcmp(out, want2, len) == 0, "golden GET: %.*s",
          (int)len, out);
    /* too small: refused and the buffer holds no part of the secret */
    CHECK(canlin_http_url_parse("https://api.example.com/", &u) == 0, "url5");
    memset(out, 'x', sizeof out);
    rc = canlin_http_write_request(out, 200, &len, "POST", &u, hs, 2, 1, 1, &cred, "CanLin/0.1");
    CHECK(rc == CANLIN_HTTP_W_ELIMIT, "limit rc %d", rc);
    for (i = 0; i < 200; i++)
        if (out[i] != 0) {
            CHECK(0, "buffer not zeroised at %u", (unsigned)i);
            break;
        }
    /* header validation */
    bad[0].name = "X-A";
    bad[0].value = "a\r\nInjected: 1";
    CHECK(canlin_http_check_request("GET", bad, 1, NULL) == CANLIN_HTTP_W_EINVAL, "CRLF in value");
    bad[0].value = "a\nb";
    CHECK(canlin_http_check_request("GET", bad, 1, NULL) == CANLIN_HTTP_W_EINVAL, "LF in value");
    bad[0].value = "a\rb";
    CHECK(canlin_http_check_request("GET", bad, 1, NULL) == CANLIN_HTTP_W_EINVAL, "CR in value");
    bad[0].name = "X A";
    bad[0].value = "v";
    CHECK(canlin_http_check_request("GET", bad, 1, NULL) == CANLIN_HTTP_W_EINVAL, "space in name");
    bad[0].name = "X:A";
    CHECK(canlin_http_check_request("GET", bad, 1, NULL) == CANLIN_HTTP_W_EINVAL, "colon in name");
    bad[0].name = "";
    CHECK(canlin_http_check_request("GET", bad, 1, NULL) == CANLIN_HTTP_W_EINVAL, "empty name");
    for (i = 0; reserved[i]; i++) {
        bad[0].name = reserved[i];
        CHECK(canlin_http_check_request("GET", bad, 1, &cred) == CANLIN_HTTP_W_EINVAL,
              "reserved '%s' accepted", reserved[i]);
    }
    CHECK(canlin_http_check_request("GE T", NULL, 0, NULL) == CANLIN_HTTP_W_EINVAL, "method");
    CHECK(canlin_http_check_request("GET\r\n", NULL, 0, NULL) == CANLIN_HTTP_W_EINVAL, "method CRLF");
    {
        canlin_http_cred c2 = cred;
        c2.secret = "a\r\nb";
        c2.secret_len = 4;
        CHECK(canlin_http_check_request("GET", NULL, 0, &c2) == CANLIN_HTTP_W_EINVAL, "CRLF in secret");
        c2 = cred;
        c2.header = "Bad Header";
        CHECK(canlin_http_check_request("GET", NULL, 0, &c2) == CANLIN_HTTP_W_EINVAL, "cred header name");
    }
}

int main(void)
{
    size_t i;
    for (i = 0; i < sizeof vecs / sizeof vecs[0]; i++)
        check_vec(&vecs[i]);
    test_limits();
    test_stop_and_sticky();
    test_urls();
    test_writer();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("http_wire_test: ok (%u vectors at every split)\n", (unsigned)(sizeof vecs / sizeof vecs[0]));
    return 0;
}
