/* HTTP/1.1 wire primitives of the native transport (HTTP transport spec §6): URL
 * parsing and origins, the request writer and the incremental response parser.
 * No I/O, no allocation, no global state; owner thread only.
 *
 * Contract: docs/superpowers/specs/2026-09-23-canlin-http-transport-design.md.
 * Limits: include/canlin/net_limits.h.
 */
#ifndef CANLIN_HTTP_WIRE_H
#define CANLIN_HTTP_WIRE_H

#include <stddef.h>
#include <stdint.h>

#include "canlin/net_limits.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- URLs ---- */

typedef struct canlin_http_url {
    int https;
    int ipv6;                             /* host is an IPv6 literal (no brackets here) */
    char host[CANLIN_NET_HOST_MAX + 1];   /* lowercase; for DNS and SNI */
    unsigned port;                        /* explicit or the scheme default */
    char path[CANLIN_NET_URL_MAX + 1];    /* origin-form request target, "/" at least */
    /* "scheme://host:port", byte-identical to canlin_llm_url_origin() for the same URL
     * (IPv6 in brackets, default port explicit). */
    char origin[CANLIN_NET_HOST_MAX + 32];
} canlin_http_url;

/* Parse an absolute http(s) URL. Userinfo, percent-encoded or non-ASCII hosts, spaces
 * and control bytes are rejected; a fragment is dropped. Returns 0 or -1. */
int canlin_http_url_parse(const char *url, canlin_http_url *out);
/* Resolve a Location value against `base`: an absolute http(s) URL, "//authority/..."
 * or "/path". Other relative forms are refused (-1). */
int canlin_http_url_resolve(const canlin_http_url *base, const char *loc, canlin_http_url *out);
/* 1 iff the host is localhost, 127.0.0.0/8 or ::1. */
int canlin_http_url_loopback(const canlin_http_url *u);

/* ---- request writer ---- */

typedef struct canlin_http_header {
    const char *name, *value; /* NUL-terminated */
} canlin_http_header;

/* Credential binding (spec §2): injected as "<header>: <prefix><secret>" only when the
 * request URL's origin equals `origin`. `secret` is not NUL-terminated. */
typedef struct canlin_http_cred {
    const char *origin, *header, *prefix;
    const char *secret;
    size_t secret_len;
} canlin_http_cred;

enum {
    CANLIN_HTTP_W_OK = 0,
    CANLIN_HTTP_W_EINVAL = -1, /* bad method, header name/value (CR, LF, NUL) or reserved name */
    CANLIN_HTTP_W_ELIMIT = -2  /* request head exceeds the buffer */
};

/* Validate a method token and the caller's headers: RFC 9110 token names, values
 * without CR/LF/NUL/other controls, and none of the names the transport owns (Host,
 * Content-Length, Transfer-Encoding, Connection, Accept-Encoding, User-Agent, TE,
 * Upgrade, Expect, Trailer) nor the credential header name (if cred). */
int canlin_http_check_request(const char *method, const canlin_http_header *h, size_t nh,
                              const canlin_http_cred *cred);
/* 1 iff cred is non-NULL and applies to u (origin equality). */
int canlin_http_cred_applies(const canlin_http_cred *cred, const canlin_http_url *u);

/* Write the request head (spec §6): request line, Host, User-Agent, Accept-Encoding:
 * identity, Connection: close, Content-Length when `send_length`, the caller's
 * headers, the credential only if it applies to u, and the blank line. The body is
 * not copied. Arguments must have passed canlin_http_check_request(). *len = bytes. */
int canlin_http_write_request(char *out, size_t cap, size_t *len, const char *method,
                              const canlin_http_url *u, const canlin_http_header *h,
                              size_t nh, int send_length, size_t body_len,
                              const canlin_http_cred *cred, const char *user_agent);

/* ---- response parser ---- */

typedef struct canlin_http_field {
    const char *name, *value; /* NUL-terminated, value without surrounding OWS */
} canlin_http_field;

typedef struct canlin_http_parser_cb {
    /* The final (non-1xx) response head, framing already validated. Pointers are valid
     * only during the call. Return nonzero to stop parsing (e.g. to follow a redirect). */
    int (*head)(void *ud, int status, const canlin_http_field *f, size_t n);
    /* Body bytes. Return nonzero to pause: feed() returns right after these bytes. */
    int (*body)(void *ud, const char *buf, size_t n);
} canlin_http_parser_cb;

enum {
    CANLIN_HTTP_P_MORE = 0,       /* all input consumed, message not complete */
    CANLIN_HTTP_P_PAUSED = 1,     /* body callback asked to pause */
    CANLIN_HTTP_P_DONE = 2,       /* message complete (framing satisfied) */
    CANLIN_HTTP_P_STOPPED = 3,    /* head callback returned nonzero */
    CANLIN_HTTP_P_EPROTOCOL = -1, /* malformed or forbidden (obs-fold, both framings, gzip ...) */
    CANLIN_HTTP_P_ELIMIT = -2,    /* line/count/total header bound or interim count */
    CANLIN_HTTP_P_EEOF = -3       /* EOF before the framing says the body is complete */
};

/* Bytes of caller memory a parser needs. */
#define CANLIN_HTTP_PARSER_MEM ((size_t)CANLIN_NET_HEADER_BYTES_MAX + 8u)

typedef struct canlin_http_parser {
    char *buf;            /* CANLIN_HTTP_PARSER_MEM bytes: head fields, current line */
    size_t used;          /* bytes of completed fields in buf */
    size_t line;          /* bytes of the current line (after used) */
    size_t head_bytes;    /* wire bytes of the current head block (or trailer block) */
    unsigned nfields, interim;
    canlin_http_field f[CANLIN_NET_HEADERS_MAX];
    int state, status, err, no_body;
    int chunked, has_len;
    uint64_t len, left;   /* Content-Length / bytes left of the current chunk or body */
    const char *why;      /* static detail of the sticky error */
    canlin_http_parser_cb cb;
    void *ud;
} canlin_http_parser;

/* no_body: the request was HEAD (no body follows whatever the headers say). */
void canlin_http_parser_init(canlin_http_parser *p, void *mem, int no_body,
                             const canlin_http_parser_cb *cb, void *ud);
/* Feed any split of the response bytes. *used = bytes consumed (the rest must be fed
 * again after a pause; after DONE/STOPPED the rest is ignored). Errors are sticky. */
int canlin_http_parser_feed(canlin_http_parser *p, const char *buf, size_t n, size_t *used);
/* Transport EOF: DONE for a close-delimited body or a finished message, else EEOF. */
int canlin_http_parser_eof(canlin_http_parser *p);
/* Static detail text of the sticky error ("obsolete line folding", ...). */
const char *canlin_http_parser_why(const canlin_http_parser *p);

#ifdef __cplusplus
}
#endif

#endif
