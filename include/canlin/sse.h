/* Bounded, push-fed Server-Sent Events (text/event-stream) parser.
 *
 * Wire format per WHATWG HTML "event stream interpretation": LF, CRLF and lone CR
 * line endings (CRLF may be split across feeds), one leading UTF-8 BOM stripped,
 * ':' comments, fields event/data/id/retry, field without colon, one leading space
 * stripped from the value, multi-line data joined with '\n'.
 *
 * Deviations, all deliberate:
 *  - bytes are passed through unchanged; no UTF-8 decoding/replacement happens here
 *    (the consumer's JSON layer validates UTF-8);
 *  - a line longer than max_line or assembled data longer than max_data is a sticky
 *    error, never truncation;
 *  - EOF inside a partial line or partial event (a non-comment line after the last
 *    blank line) makes canlin_sse_finish() return CANLIN_SSE_E_INCOMPLETE instead of
 *    silently discarding it.
 *
 * No allocation: all storage is the caller's buffer given to canlin_sse_init().
 * One owner thread; the callback must not re-enter the same parser.
 */
#ifndef CANLIN_SSE_H
#define CANLIN_SSE_H

#include <stddef.h>

enum {
    CANLIN_SSE_OK = 0,
    CANLIN_SSE_E_ARG = -1,        /* bad init arguments / buffer too small */
    CANLIN_SSE_E_LINE = -2,       /* line longer than max_line */
    CANLIN_SSE_E_DATA = -3,       /* assembled data longer than max_data */
    CANLIN_SSE_E_INCOMPLETE = -4, /* EOF inside a line or an undispatched event */
    CANLIN_SSE_E_ABORTED = -5,    /* callback returned nonzero */
    CANLIN_SSE_E_STATE = -6       /* feed/finish after finish */
};

typedef struct canlin_sse_event {
    const char *type; /* "message" when no event field was given */
    size_t type_len;
    const char *data; /* data lines joined with '\n', no trailing '\n' */
    size_t data_len;
    const char *id;   /* last event ID after this event (may be empty) */
    size_t id_len;
} canlin_sse_event;

/* Pointers are valid only during the call. Return nonzero to abort parsing. */
typedef int (*canlin_sse_cb)(void *ud, const canlin_sse_event *ev);

typedef struct canlin_sse {
    char *line, *data, *type, *idbuf, *id;
    size_t line_len, data_len, type_len, idbuf_len, id_len;
    size_t max_line, max_data;
    unsigned long retry_ms; /* last valid retry field; retry_set says if any */
    int retry_set;
    int bom;     /* 0..2 BOM bytes matched so far, 3 = BOM phase over */
    int cr;      /* previous byte was CR: swallow a following LF */
    int pending; /* non-comment field line seen since the last blank line */
    int finished;
    int err;     /* sticky error */
    canlin_sse_cb cb;
    void *ud;
} canlin_sse;

/* Bytes of caller memory needed for the given limits. */
#define CANLIN_SSE_MEM(max_line, max_data) (4u * (size_t)(max_line) + (size_t)(max_data) + 1u)

int canlin_sse_init(canlin_sse *p, void *mem, size_t memlen, size_t max_line,
                    size_t max_data, canlin_sse_cb cb, void *ud);
/* Any split is allowed, including one byte at a time. Returns OK or sticky error. */
int canlin_sse_feed(canlin_sse *p, const void *buf, size_t len);
/* End of stream. OK only if no partial line/event is pending. */
int canlin_sse_finish(canlin_sse *p);
/* Committed last event ID (the reconnection "Last-Event-ID" value). */
const char *canlin_sse_last_id(const canlin_sse *p, size_t *len);
const char *canlin_sse_strerror(int err);

#endif
