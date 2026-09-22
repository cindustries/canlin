/* Bounded, push-fed streaming JSON (RFC 8259) tokenizer and validator.
 *
 * Exactly one top-level value, optionally surrounded by JSON whitespace; anything
 * after it is CANLIN_JSON_E_TRAILING. Input can be split anywhere, including inside
 * UTF-8 sequences, escapes and \uXXXX surrogate pairs. Tokens are emitted in input
 * order through the callback; strings and keys are delivered whole and decoded
 * (escapes resolved, UTF-8), numbers as their validated source text.
 *
 * Strict choices: raw UTF-8 must be well formed (no overlongs, no encoded surrogates,
 * nothing above U+10FFFF); \u escapes must form valid scalar values (lone or
 * mispaired surrogates are rejected); raw control bytes inside strings are rejected;
 * a BOM is not accepted. \u0000 is accepted and yields a NUL byte in the
 * length-delimited string.
 *
 * All limits are checked per byte during assembly. Errors are sticky; tokens emitted
 * before an error must be discarded by the consumer. The byte offset of an error is
 * p->total - 1. No allocation: token storage is the caller's buffer.
 */
#ifndef CANLIN_JSON_H
#define CANLIN_JSON_H

#include <stddef.h>

enum {
    CANLIN_JSON_OK = 0,
    CANLIN_JSON_E_ARG = -1,
    CANLIN_JSON_E_SYNTAX = -2,     /* unexpected byte */
    CANLIN_JSON_E_TRAILING = -3,   /* non-whitespace after the top-level value */
    CANLIN_JSON_E_NUMBER = -4,     /* malformed number */
    CANLIN_JSON_E_STRING = -5,     /* raw control byte in string */
    CANLIN_JSON_E_ESCAPE = -6,     /* bad escape or invalid \u surrogate use */
    CANLIN_JSON_E_UTF8 = -7,       /* malformed UTF-8 */
    CANLIN_JSON_E_DEPTH = -8,      /* nesting deeper than max_depth */
    CANLIN_JSON_E_STRING_LEN = -9, /* decoded string/key longer than max_string */
    CANLIN_JSON_E_NUMBER_LEN = -10,/* number text longer than max_number */
    CANLIN_JSON_E_TOTAL = -11,     /* more than max_total input bytes */
    CANLIN_JSON_E_INCOMPLETE = -12,/* finish() before the value was complete */
    CANLIN_JSON_E_ABORTED = -13,   /* callback returned nonzero */
    CANLIN_JSON_E_STATE = -14      /* feed/finish after finish */
};

enum canlin_json_type {
    CANLIN_JSON_OBJ_BEGIN = 1,
    CANLIN_JSON_OBJ_END,
    CANLIN_JSON_ARR_BEGIN,
    CANLIN_JSON_ARR_END,
    CANLIN_JSON_KEY,
    CANLIN_JSON_STRING,
    CANLIN_JSON_NUMBER,
    CANLIN_JSON_TRUE,
    CANLIN_JSON_FALSE,
    CANLIN_JSON_NULL
};

typedef struct canlin_json_token {
    int type;
    const char *ptr; /* KEY/STRING: decoded bytes; NUMBER: source text; else NULL */
    size_t len;
    unsigned depth;  /* enclosing containers (a container's own BEGIN/END excluded) */
} canlin_json_token;

/* Pointers are valid only during the call. Return nonzero to abort parsing. */
typedef int (*canlin_json_cb)(void *ud, const canlin_json_token *t);

#define CANLIN_JSON_DEPTH_CAP 256u

typedef struct canlin_json_limits {
    unsigned max_depth; /* 1..CANLIN_JSON_DEPTH_CAP */
    size_t max_string;  /* decoded bytes per key/string */
    size_t max_number;  /* characters per number */
    size_t max_total;   /* input bytes per document, whitespace included */
} canlin_json_limits;

typedef struct canlin_json {
    canlin_json_limits lim;
    char *buf;
    size_t len;
    size_t total;
    unsigned char stack[CANLIN_JSON_DEPTH_CAP / 8]; /* bit set = object */
    unsigned depth;
    int gs, ls, ns;             /* grammar, lexical and number state */
    int is_key;
    unsigned u8need;
    unsigned char u8lo, u8hi;
    unsigned long ucp, hisur;
    unsigned uhex;
    int lit;
    unsigned litpos;
    int finished;
    int err;
    canlin_json_cb cb;
    void *ud;
} canlin_json;

/* buf must hold max(max_string, max_number) bytes. */
int canlin_json_init(canlin_json *p, const canlin_json_limits *lim, void *buf,
                     size_t buflen, canlin_json_cb cb, void *ud);
int canlin_json_feed(canlin_json *p, const void *data, size_t len);
/* OK only when exactly one complete value was seen; else E_INCOMPLETE or the error. */
int canlin_json_finish(canlin_json *p);
const char *canlin_json_strerror(int err);

#endif
