/* CanLin LLM providers: presets, profiles, session credentials, transport seam and the
 * assistant-only Lua surface canlin.llm.
 *
 * Contract: docs/superpowers/specs/2026-09-23-canlin-llm-providers-design.md (§1–§8),
 * LLM/UI design §3–§4, core API §3, §5. Owner-thread only. Every function here is
 * trusted C: the settings surface and the assistant loader use it; modules never do.
 *
 * Credential rule: a key given to canlin_llm_profile_set_key() lives only in core
 * memory owned by the profile, bound to that profile's base-URL origin and auth scheme.
 * It is handed to a transport only as a canlin_llm_auth (pointer, never copied by the
 * core) and injected by the transport only for a URL whose origin equals the bound one.
 * It never enters a Lua value, a log line or an error message; it is zeroised when it is
 * replaced, cleared, its profile changes endpoint or is deleted, and on canlin_llm_free.
 */
#ifndef CANLIN_LLM_H
#define CANLIN_LLM_H

#include <stddef.h>
#include <stdint.h>

#include "canlin/ev.h"
#include "canlin/llm_limits.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    CANLIN_LLM_OK = 0,
    CANLIN_LLM_EINVAL = -1,      /* bad argument, id or URL */
    CANLIN_LLM_ENOMEM = -2,
    CANLIN_LLM_EFULL = -3,       /* CANLIN_LLM_PROFILES_MAX reached */
    CANLIN_LLM_EEXIST = -4,      /* profile id already used */
    CANLIN_LLM_ENOENT = -5,      /* unknown profile or preset */
    CANLIN_LLM_EINSECURE = -6,   /* plain http outside the local/dev rule (spec §5) */
    CANLIN_LLM_EUNSUPPORTED = -7 /* dialect or auth scheme not available */
};

typedef enum canlin_llm_dialect {
    CANLIN_LLM_DIALECT_OPENAI_CHAT = 1,
    CANLIN_LLM_DIALECT_ANTHROPIC_MESSAGES = 2 /* canlin#39: not implemented yet */
} canlin_llm_dialect;

typedef enum canlin_llm_auth_scheme {
    CANLIN_LLM_AUTH_NONE = 0,
    CANLIN_LLM_AUTH_BEARER = 1,   /* Authorization: Bearer <key> */
    CANLIN_LLM_AUTH_X_API_KEY = 2 /* x-api-key: <key> (+ anthropic-version) */
} canlin_llm_auth_scheme;

typedef enum canlin_llm_key_need {
    CANLIN_LLM_KEY_NONE = 0,
    CANLIN_LLM_KEY_OPTIONAL = 1,
    CANLIN_LLM_KEY_REQUIRED = 2
} canlin_llm_key_need;

/* ---- presets (spec §3): data compiled into the binary ---- */

typedef struct canlin_llm_preset {
    const char *id, *title;
    canlin_llm_dialect dialect;
    const char *base_url;       /* full base incl. version path */
    canlin_llm_auth_scheme auth;
    canlin_llm_key_need key;
    const char *default_model;  /* "" = the user must choose */
    int tools, streaming;       /* configured capability facts */
    int local;                  /* loopback preset: plain http allowed, no key needed */
} canlin_llm_preset;

size_t canlin_llm_preset_count(void);
const canlin_llm_preset *canlin_llm_preset_at(size_t i);
const canlin_llm_preset *canlin_llm_preset_find(const char *id);
/* 1 when the preset's dialect is implemented in this build (spec §2.2). */
int canlin_llm_dialect_available(canlin_llm_dialect d);

/* ---- transport seam (spec §8) ---- */

typedef struct canlin_llm_header {
    const char *name, *value; /* NUL-terminated */
} canlin_llm_header;

/* The credential binding handed to a transport. `secret` points into the profile's key
 * storage and stays valid until the stream is closed; a transport must not copy it
 * anywhere but into the outgoing request, and injects it only when
 * canlin_llm_auth_applies(auth, url) says so — for the first URL and every redirect. */
typedef struct canlin_llm_auth {
    const char *origin;  /* bound origin, "scheme://host:port" (normalised) */
    const char *header;  /* "Authorization" or "x-api-key" */
    const char *prefix;  /* "Bearer " or "" */
    const char *secret;
    size_t secret_len;
} canlin_llm_auth;

typedef struct canlin_llm_http_request {
    const char *method;                /* "POST" / "GET" */
    const char *url;                   /* absolute */
    const canlin_llm_header *headers;  /* non-secret headers */
    size_t nheaders;
    const char *body;
    size_t body_len;
    const canlin_llm_auth *auth;       /* NULL = no credential */
} canlin_llm_http_request;

/* Callbacks a transport makes on the owner thread, never from inside canlin_ev_turn's
 * delivery of Lua and never from inside open(). Order: status, header*, body*, then
 * exactly one of end/error. After close() returns no callback is made for that stream. */
typedef struct canlin_llm_stream_cb {
    void (*status)(void *ud, int http_status);
    void (*header)(void *ud, const char *name, const char *value);
    void (*body)(void *ud, const void *buf, size_t len);
    void (*end)(void *ud);
    void (*error)(void *ud, const char *msg); /* network/TLS/transport, no secrets */
} canlin_llm_stream_cb;

typedef struct canlin_llm_transport {
    /* Start a request. Everything in *req (auth included) stays valid until close().
     * Returns a stream or NULL (*msg set to static diagnostic text). */
    void *(*open)(void *ctx, const canlin_llm_http_request *req,
                  const canlin_llm_stream_cb *cb, void *ud, const char **msg);
    /* Stop and release a stream (after end/error, or to cancel). Idempotent per stream;
     * allowed from inside that stream's callbacks. */
    void (*close)(void *ctx, void *stream);
    void *ctx;
} canlin_llm_transport;

/* Normalised origin "scheme://host:port" of an http(s) URL (lowercase scheme/host,
 * default port made explicit). Userinfo is rejected. Returns length or -1. */
int canlin_llm_url_origin(const char *url, char *out, size_t outlen);
/* 1 iff a credential bound to auth->origin may be sent to `url`. */
int canlin_llm_auth_applies(const canlin_llm_auth *auth, const char *url);

/* ---- fixture transport (tests; spec §8, §9) ---- */

typedef enum canlin_llm_fixture_fault {
    CANLIN_LLM_FX_NONE = 0,
    CANLIN_LLM_FX_ERROR_AT,  /* transport error after fault_at body bytes */
    CANLIN_LLM_FX_EOF_AT     /* clean end after fault_at body bytes (truncation) */
} canlin_llm_fixture_fault;

typedef struct canlin_llm_fixture_response {
    int status;                /* HTTP status */
    const char *content_type;  /* NULL = "text/event-stream" */
    const char *body;
    size_t body_len;
    size_t chunk;              /* bytes per body callback; 0 = whole body at once */
    unsigned seed;             /* nonzero: random chunk sizes in [1, chunk] */
    const char *redirect_to;   /* follow one redirect to this URL before responding */
    canlin_llm_fixture_fault fault;
    size_t fault_at;
} canlin_llm_fixture_response;

/* One outgoing request as the fixture saw it (after redirect handling). `headers` is
 * "Name: value\n" lines, auth included when it was injected. */
typedef struct canlin_llm_fixture_record {
    char *method, *url, *headers, *body;
    size_t body_len;
} canlin_llm_fixture_record;

typedef struct canlin_llm_fixture canlin_llm_fixture;

canlin_llm_fixture *canlin_llm_fixture_new(void);
void canlin_llm_fixture_free(canlin_llm_fixture *fx);
canlin_llm_transport canlin_llm_fixture_transport(canlin_llm_fixture *fx);
/* Queue the response for the next opened stream (FIFO). Strings are copied. A stream
 * opened with no scripted response fails with a transport error. */
int canlin_llm_fixture_push(canlin_llm_fixture *fx, const canlin_llm_fixture_response *r);
/* Make at most one callback on every open stream. Returns the number made (0 = idle). */
unsigned canlin_llm_fixture_step(canlin_llm_fixture *fx);
size_t canlin_llm_fixture_records(const canlin_llm_fixture *fx);
const canlin_llm_fixture_record *canlin_llm_fixture_record_at(const canlin_llm_fixture *fx,
                                                              size_t i);
unsigned canlin_llm_fixture_open_streams(const canlin_llm_fixture *fx);

/* ---- context, profiles and credentials ---- */

typedef struct canlin_llm canlin_llm;

typedef void (*canlin_llm_log_fn)(void *ud, const char *msg, size_t len);

typedef struct canlin_llm_config {
    canlin_llm_transport transport;
    int allow_insecure_dev;   /* explicit local-development setting (spec §5) */
    canlin_llm_log_fn log;    /* diagnostics; never carries a credential */
    void *log_ud;
} canlin_llm_config;

/* The context must outlive every module it was installed into. */
canlin_llm *canlin_llm_new(canlin_ev *ev, const canlin_llm_config *cfg);
/* Aborts remaining requests (their ops complete as failures) and zeroises every key. */
void canlin_llm_free(canlin_llm *llm);

typedef struct canlin_llm_profile_config {
    const char *id;         /* [a-z0-9_-]{1,32} */
    const char *title;      /* NULL = preset title / id */
    const char *preset;     /* preset id, or "custom" */
    const char *base_url;   /* NULL = preset base; required for custom */
    const char *model;      /* NULL = preset default; required when that is empty */
    /* custom only: */
    canlin_llm_dialect dialect;
    canlin_llm_auth_scheme auth;
    int local;              /* loopback endpoint: plain http allowed */
    int tools;              /* tool support claimed by the user */
} canlin_llm_profile_config;

int canlin_llm_profile_create(canlin_llm *llm, const canlin_llm_profile_config *cfg);
/* Replaces the whole configuration (id unchanged). Aborts the profile's in-flight
 * requests; a changed origin or auth scheme drops (zeroises) the stored key. */
int canlin_llm_profile_update(canlin_llm *llm, const canlin_llm_profile_config *cfg);
/* Aborts in-flight requests, zeroises the key, removes the profile. */
int canlin_llm_profile_delete(canlin_llm *llm, const char *id);
/* Session-only key (spec §5): copied into core memory, bound to the profile's current
 * origin and auth scheme. EUNSUPPORTED when the profile's auth scheme is none. */
int canlin_llm_profile_set_key(canlin_llm *llm, const char *id, const void *key, size_t len);
int canlin_llm_profile_clear_key(canlin_llm *llm, const char *id);
int canlin_llm_profile_has_key(const canlin_llm *llm, const char *id);

/* Retry posting stream events refused by a full queue, complete finished requests and
 * enforce CANLIN_LLM_IDLE_TIMEOUT_MS. Call once per loop iteration; transports' own
 * callbacks already pump. */
void canlin_llm_pump(canlin_llm *llm);
/* Live requests (all modules). */
unsigned canlin_llm_inflight(const canlin_llm *llm);

/* ---- Lua surface (spec §4) ----
 * The assistant hook for canlin_module_source.assistant: installs canlin.llm into the
 * `canlin` table on top of the stack. `ud` is the canlin_llm context. */
struct lua_State;
struct canlin_module;
int canlin_llm_install(struct lua_State *L, struct canlin_module *m, void *ud);

#ifdef __cplusplus
}
#endif

#endif
