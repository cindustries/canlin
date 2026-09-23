/* CanLin LLM — internal seams between the request core (llm.c), the dialect adapters
 * (openai_chat.c, anthropic_messages.c) and the bounded JSON writer. */
#ifndef CANLIN_LLM_INT_H
#define CANLIN_LLM_INT_H

#include "canlin/llm.h"

#include <stddef.h>
#include <stdint.h>

/* ---- bounded JSON writer (jsonw.c) ---- */

/* JW_EINVAL: a dialect refused caller data it would have to embed as raw JSON. */
enum { JW_OK = 0, JW_EFULL = -1, JW_ENOMEM = -2, JW_EUTF8 = -3, JW_EINVAL = -4 };

typedef struct llm_jw {
    char *buf;
    size_t len, cap, max;
    int err; /* sticky */
} llm_jw;

void llm_jw_init(llm_jw *w, size_t max);
void llm_jw_free(llm_jw *w);
void llm_jw_raw(llm_jw *w, const char *s, size_t n);
void llm_jw_lit(llm_jw *w, const char *s); /* NUL-terminated raw text */
/* JSON string with escapes; invalid UTF-8 is a sticky JW_EUTF8. */
void llm_jw_str(llm_jw *w, const char *s, size_t n);
/* The escaped string body without quotes (to join several spans into one string). */
void llm_jw_esc(llm_jw *w, const char *s, size_t n);
void llm_jw_uint(llm_jw *w, unsigned long v);

/* 1 iff s[0..n) is well-formed UTF-8 (no overlongs, surrogates or > U+10FFFF). */
int llm_utf8_valid(const char *s, size_t n);

/* 1 iff p[0..n) is exactly one complete JSON object within the CANLIN_LLM_JSON_* limits
 * and max_total bytes. buf: token storage of CANLIN_LLM_JSON_STRING_MAX bytes. */
int llm_json_object_valid(char *buf, size_t buflen, const char *p, size_t n, size_t max_total);

/* ---- normalised request (spec §4) ---- */

enum { LLM_ROLE_SYSTEM = 1, LLM_ROLE_USER, LLM_ROLE_ASSISTANT, LLM_ROLE_TOOL };

typedef struct llm_str {
    const char *s;
    size_t n;
} llm_str;

typedef struct llm_call {
    llm_str id, name, args;
} llm_call;

typedef struct llm_msg {
    int role;
    int has_text;
    llm_str text;
    llm_str tool_call_id;       /* role tool */
    const llm_call *calls;      /* role assistant */
    size_t ncalls;
} llm_msg;

/* A core-provided tool (spec §4): name, description and JSON schema are core data. */
typedef struct llm_tool {
    const char *name, *description, *schema_json;
} llm_tool;

const llm_tool *llm_tool_find(const char *name, size_t len);

typedef struct llm_build {
    const char *model;
    const llm_msg *msgs;
    size_t nmsgs;
    const llm_tool *const *tools;
    size_t ntools;
    unsigned long max_tokens;   /* 0 = not sent */
    int stream_usage;           /* openai-chat: ask for the usage chunk (preset flag) */
} llm_build;

/* ---- request core services used by dialects (llm.c) ---- */

typedef struct llm_req llm_req;

/* Each returns 0, or -1 when the request is already terminal (caller stops parsing). */
int llm_emit_text(llm_req *r, const char *s, size_t n);
int llm_emit_tool(llm_req *r, const char *id, size_t idn, const char *name, size_t namen,
                  const char *args, size_t argsn);
int llm_emit_usage(llm_req *r, int64_t input, int64_t output);
/* Terminal outcomes. stop: "end" | "tool" | "length". dropped_tools: unfinished tool
 * calls dropped at the length stop (spec §2.3); reported only with "length", must be 0
 * otherwise. code: core API §3.1 string. */
void llm_succeed(llm_req *r, const char *stop, unsigned dropped_tools);
void llm_fail(llm_req *r, const char *code, const char *msg);
int llm_is_terminal(const llm_req *r);

/* ---- dialect vtable ---- */

typedef struct llm_dialect {
    canlin_llm_dialect id;
    const char *name;
    const char *path;                /* suffix appended to the base URL */
    /* Serialise the request body into w (bounded by CANLIN_LLM_REQUEST_MAX). */
    void (*build)(llm_jw *w, const llm_build *b);
    /* Extra non-secret headers, NULL-name terminated (e.g. anthropic-version). */
    const canlin_llm_header *headers;
    /* Response side: state is created per request after a 2xx status. */
    void *(*begin)(llm_req *r);
    void (*feed)(void *st, const void *buf, size_t n);
    void (*eof)(void *st);           /* transport end: validate the terminal state */
    void (*end)(void *st);           /* free */
} llm_dialect;

extern const llm_dialect llm_dialect_openai_chat;
extern const llm_dialect llm_dialect_anthropic_messages;

#endif
