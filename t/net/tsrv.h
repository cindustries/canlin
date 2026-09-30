/* Loopback test server for the native transport tests (HTTP transport spec §9).
 *
 * A forked child serves listeners on 127.0.0.1: plain HTTP, TLS with each fixture
 * certificate (t/net/fixtures), a "stall" listener that accepts and never answers and a
 * "backlog" listener whose accept queue is kept full (connect stays pending). The child uses
 * the same Mbed TLS build as the client (server side) and reports every request it read,
 * verbatim, to the parent, so tests can check what went over the wire.
 *
 * Built-in routes (any listener): /ok, /echo, /chunked, /sse-bytes (SSE sent one byte
 * per write), /close-delim, /close-noclose (TLS: no close_notify), /r?c=CODE&l=LOCATION,
 * /chain/N, /stall-body, /early-close, /gzip, /big. Scripted responses (tsrv_script)
 * take precedence. Everything else is 404. */
#ifndef CANLIN_T_TSRV_H
#define CANLIN_T_TSRV_H

#include <stddef.h>

enum {
    TS_PLAIN = 0,
    TS_EC,       /* localhost-ec leaf from the test CA */
    TS_RSA,      /* localhost-rsa leaf from the test CA */
    TS_WRONG,    /* wrong-host leaf */
    TS_EXPIRED,  /* expired leaf */
    TS_SELF,     /* self-signed */
    TS_OTHER,    /* leaf from an untrusted CA */
    TS_STALL,    /* accepts, never reads or writes */
    TS_BACKLOG,  /* accept queue kept full (also on Linux 3.4): connects stay pending */
    TS_N
};

#define TSRV_BIG_LEN 200000u
#define TSRV_SSE_BYTES "event: a\ndata: {\"x\":1}\n\n: comment\ndata: two\ndata: lines\n\ndata: [DONE]\n\n"

typedef struct tsrv tsrv;

/* Fork the server. NULL on failure. */
tsrv *tsrv_start(void);
void tsrv_stop(tsrv *s);
unsigned tsrv_port(const tsrv *s, int listener);
/* Register a raw response (status line, headers, body) for `path` (request target incl.
 * query), sent in pieces of `piece` bytes (0 = at once), `delay_us` between pieces;
 * no_notify: close TLS without close_notify. Synchronous: registered on return. */
int tsrv_script(tsrv *s, const char *path, const char *resp, size_t resp_len, size_t piece,
                unsigned delay_us, int no_notify);
/* Collect request reports the child has written so far (nonblocking). */
void tsrv_pump(tsrv *s);
/* Wait (real time, at most ms) until at least n reports arrived. Returns the count. */
size_t tsrv_wait(tsrv *s, size_t n, int ms);
size_t tsrv_count(const tsrv *s);
/* Report i: "L=<listener>\n" followed by the raw request head and body. */
const char *tsrv_req(const tsrv *s, size_t i, size_t *len);
void tsrv_clear(tsrv *s);
/* Write the test CA to a temporary file; returns its path (static buffer). */
const char *tsrv_ca_file(void);
const char *tsrv_other_ca_file(void);

#endif
