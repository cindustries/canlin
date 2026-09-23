/* CanLin LLM request core: presets, profiles, session credentials, request admission,
 * stream events and terminal completion. See include/canlin/llm.h for the contract.
 *
 * Event path: a dialect emits text/tool/usage into the request's pending FIFO; pump
 * posts the FIFO in order as "llm" module events (retrying when the scheduler queue is
 * full) and, once the request is terminal and the FIFO empty, completes the operation
 * with "done". Cancel retracts queued events (module.h: empty name) and never delivers.
 */
#include "llm_int.h"
#include "canlin/module.h"
#include "canlin/sandbox_limits.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

/* ---- presets (spec §3) ----
 * Base URLs and default models copied from Langertha (~/dev/langertha,
 * lib/Langertha/Engine/) at commit 4d2f42337ac87b812ed97c7d52f06221d30da80c
 * ($VERSION 0.503). Deviations: Mistral's engine URL has no version path, its chat path
 * is /v1/chat/completions, so the preset base carries /v1 (spec §3: base includes the
 * version path). Groq/OpenRouter/OllamaOpenAI require the user to choose a model there
 * ("" here), except ollama which takes Ollama.pm's default. anthropic, minimax and
 * moonshot use their Anthropic-compatible engines (Anthropic.pm, MiniMaxAnthropic.pm,
 * MoonshotAnthropic.pm) and stay unavailable until anthropic-messages exists
 * (canlin#39, spec §2.2). */
static const canlin_llm_preset presets[] = {
    {"openai", "OpenAI", CANLIN_LLM_DIALECT_OPENAI_CHAT, "https://api.openai.com/v1",
     CANLIN_LLM_AUTH_BEARER, CANLIN_LLM_KEY_REQUIRED, "gpt-5.6-terra", 1, 1, 0},
    {"anthropic", "Anthropic", CANLIN_LLM_DIALECT_ANTHROPIC_MESSAGES,
     "https://api.anthropic.com", CANLIN_LLM_AUTH_X_API_KEY, CANLIN_LLM_KEY_REQUIRED,
     "claude-sonnet-5", 1, 1, 0},
    {"groq", "Groq", CANLIN_LLM_DIALECT_OPENAI_CHAT, "https://api.groq.com/openai/v1",
     CANLIN_LLM_AUTH_BEARER, CANLIN_LLM_KEY_REQUIRED, "", 1, 1, 0},
    {"mistral", "Mistral", CANLIN_LLM_DIALECT_OPENAI_CHAT, "https://api.mistral.ai/v1",
     CANLIN_LLM_AUTH_BEARER, CANLIN_LLM_KEY_REQUIRED, "mistral-small-latest", 1, 1, 0},
    {"deepseek", "DeepSeek", CANLIN_LLM_DIALECT_OPENAI_CHAT, "https://api.deepseek.com",
     CANLIN_LLM_AUTH_BEARER, CANLIN_LLM_KEY_REQUIRED, "deepseek-flash", 1, 1, 0},
    {"openrouter", "OpenRouter", CANLIN_LLM_DIALECT_OPENAI_CHAT, "https://openrouter.ai/api/v1",
     CANLIN_LLM_AUTH_BEARER, CANLIN_LLM_KEY_REQUIRED, "", 1, 1, 0},
    {"xai", "xAI", CANLIN_LLM_DIALECT_OPENAI_CHAT, "https://api.x.ai/v1",
     CANLIN_LLM_AUTH_BEARER, CANLIN_LLM_KEY_REQUIRED, "grok-4.6", 1, 1, 0},
    {"cerebras", "Cerebras", CANLIN_LLM_DIALECT_OPENAI_CHAT, "https://api.cerebras.ai/v1",
     CANLIN_LLM_AUTH_BEARER, CANLIN_LLM_KEY_REQUIRED, "gpt-oss-120b", 1, 1, 0},
    {"minimax", "MiniMax", CANLIN_LLM_DIALECT_ANTHROPIC_MESSAGES,
     "https://api.minimax.io/anthropic", CANLIN_LLM_AUTH_X_API_KEY, CANLIN_LLM_KEY_REQUIRED,
     "MiniMax-M3", 1, 1, 0},
    {"moonshot", "Moonshot", CANLIN_LLM_DIALECT_ANTHROPIC_MESSAGES,
     "https://api.moonshot.ai/anthropic", CANLIN_LLM_AUTH_X_API_KEY, CANLIN_LLM_KEY_REQUIRED,
     "kimi-k3", 1, 1, 0},
    {"ollama", "Ollama", CANLIN_LLM_DIALECT_OPENAI_CHAT, "http://localhost:11434/v1",
     CANLIN_LLM_AUTH_BEARER, CANLIN_LLM_KEY_OPTIONAL, "llama3.3", 1, 1, 1},
    {"lmstudio", "LM Studio", CANLIN_LLM_DIALECT_OPENAI_CHAT, "http://localhost:1234/v1",
     CANLIN_LLM_AUTH_BEARER, CANLIN_LLM_KEY_OPTIONAL, "default", 1, 1, 1},
    {"vllm", "vLLM", CANLIN_LLM_DIALECT_OPENAI_CHAT, "http://localhost:8000/v1",
     CANLIN_LLM_AUTH_BEARER, CANLIN_LLM_KEY_OPTIONAL, "default", 1, 1, 1},
    {"llamacpp", "llama.cpp", CANLIN_LLM_DIALECT_OPENAI_CHAT, "http://localhost:8080/v1",
     CANLIN_LLM_AUTH_BEARER, CANLIN_LLM_KEY_OPTIONAL, "default", 1, 1, 1},
};

#define NPRESETS (sizeof presets / sizeof presets[0])

size_t canlin_llm_preset_count(void)
{
    return NPRESETS;
}

const canlin_llm_preset *canlin_llm_preset_at(size_t i)
{
    return i < NPRESETS ? &presets[i] : NULL;
}

const canlin_llm_preset *canlin_llm_preset_find(const char *id)
{
    size_t i;
    if (id == NULL)
        return NULL;
    for (i = 0; i < NPRESETS; i++)
        if (strcmp(presets[i].id, id) == 0)
            return &presets[i];
    return NULL;
}

static const llm_dialect *dialect_of(canlin_llm_dialect d)
{
    return d == CANLIN_LLM_DIALECT_OPENAI_CHAT ? &llm_dialect_openai_chat : NULL;
}

int canlin_llm_dialect_available(canlin_llm_dialect d)
{
    return dialect_of(d) != NULL;
}

/* ---- core-provided tools (spec §4) ----
 * set_view: schema stub after UI spec §7. The authoritative validation is the set_view
 * validator (canlin#20); this schema only tells the provider the shape. */
static const llm_tool tools[] = {
    {"set_view",
     "Replace the assistant view with a declarative flow description of allowlisted "
     "widgets. The only action is submit.",
     "{\"type\":\"object\",\"properties\":{\"title\":{\"type\":\"string\"},"
     "\"widgets\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{"
     "\"type\":{\"type\":\"string\",\"enum\":[\"heading\",\"text\",\"bubble\",\"field\","
     "\"area\",\"toggle\",\"choice\",\"meter\",\"separator\",\"section\",\"columns\","
     "\"graph\",\"button\"]},\"id\":{\"type\":\"string\"},\"text\":{\"type\":\"string\"},"
     "\"label\":{\"type\":\"string\"},\"value\":{},\"index\":{\"type\":\"integer\"},"
     "\"role\":{\"type\":\"string\",\"enum\":[\"user\",\"assistant\"]},"
     "\"options\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},"
     "\"widgets\":{\"type\":\"array\"},\"left\":{\"type\":\"array\"},"
     "\"right\":{\"type\":\"array\"},\"nodes\":{\"type\":\"array\"},"
     "\"edges\":{\"type\":\"array\"},"
     "\"action\":{\"type\":\"string\",\"enum\":[\"submit\"]}},"
     "\"required\":[\"type\"]}}},\"required\":[\"widgets\"]}"},
};

const llm_tool *llm_tool_find(const char *name, size_t len)
{
    size_t i;
    for (i = 0; i < sizeof tools / sizeof tools[0]; i++)
        if (strlen(tools[i].name) == len && memcmp(tools[i].name, name, len) == 0)
            return &tools[i];
    return NULL;
}

/* ---- URLs and origins (spec §5) ---- */

int canlin_llm_url_origin(const char *url, char *out, size_t outlen)
{
    const char *p, *a, *e, *hs, *he;
    int https;
    unsigned long port;
    char host[256];
    size_t hl, k;
    int n;

    if (url == NULL || out == NULL)
        return -1;
    if (strncmp(url, "https://", 8) == 0 || strncmp(url, "HTTPS://", 8) == 0) {
        https = 1;
        a = url + 8;
    } else if (strncmp(url, "http://", 7) == 0 || strncmp(url, "HTTP://", 7) == 0) {
        https = 0;
        a = url + 7;
    } else
        return -1;
    for (e = a; *e && *e != '/' && *e != '?' && *e != '#'; e++)
        if (*e == '@' || *e == '\\' || (unsigned char)*e <= 0x20 || (unsigned char)*e >= 0x7f)
            return -1; /* userinfo or junk in the authority */
    if (*a == '[') {
        hs = a;
        for (he = a; he < e && *he != ']'; he++)
            ;
        if (he == e)
            return -1;
        he++;
        p = he;
    } else {
        hs = a;
        for (he = a; he < e && *he != ':'; he++)
            ;
        p = he;
    }
    hl = (size_t)(he - hs);
    if (hl == 0 || hl >= sizeof host)
        return -1;
    for (k = 0; k < hl; k++) {
        char c = hs[k];
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-' ||
              c == '[' || c == ']' || c == ':'))
            return -1;
        host[k] = c;
    }
    host[hl] = 0;
    if (p < e) {
        if (*p != ':' || p + 1 == e || e - p > 6)
            return -1;
        port = 0;
        for (p++; p < e; p++) {
            if (*p < '0' || *p > '9')
                return -1;
            port = port * 10 + (unsigned long)(*p - '0');
        }
        if (port == 0 || port > 65535)
            return -1;
    } else
        port = https ? 443 : 80;
    n = snprintf(out, outlen, "%s://%s:%lu", https ? "https" : "http", host, port);
    if (n < 0 || (size_t)n >= outlen)
        return -1;
    return n;
}

int canlin_llm_auth_applies(const canlin_llm_auth *auth, const char *url)
{
    char o[CANLIN_LLM_URL_MAX + 16];
    if (auth == NULL || auth->origin == NULL || url == NULL)
        return 0;
    if (canlin_llm_url_origin(url, o, sizeof o) < 0)
        return 0;
    return strcmp(o, auth->origin) == 0;
}

static int origin_loopback(const char *origin)
{
    const char *h = strstr(origin, "://");
    if (h == NULL)
        return 0;
    h += 3;
    return strncmp(h, "localhost:", 10) == 0 || strncmp(h, "127.", 4) == 0 ||
           strncmp(h, "[::1]:", 6) == 0;
}

/* ---- context and profiles ---- */

typedef struct profile {
    int used;
    char id[CANLIN_LLM_ID_MAX + 1];
    char title[CANLIN_LLM_TITLE_MAX + 1];
    char base[CANLIN_LLM_URL_MAX + 1];
    char origin[CANLIN_LLM_URL_MAX + 16];
    char model[CANLIN_LLM_MODEL_MAX + 1];
    canlin_llm_dialect dialect;
    canlin_llm_auth_scheme auth;
    canlin_llm_key_need keyneed;
    int local, tools;
    /* session credential (spec §5): bound to origin + auth at set time */
    char *key;
    size_t keylen;
    char key_origin[CANLIN_LLM_URL_MAX + 16];
    canlin_llm_auth_scheme key_auth;
} profile;

typedef struct pev pev;

struct llm_req {
    canlin_llm *llm;
    llm_req *next;
    profile *prof; /* NULL once terminal */
    canlin_ev_gen gen;
    canlin_ev_handle h;
    const llm_dialect *d;
    void *dst;
    void *stream;
    char url[CANLIN_LLM_URL_MAX + 32];
    llm_jw body;
    canlin_llm_header hdrs[4];
    canlin_llm_auth auth;
    int status;
    canlin_ev_time last_byte;
    size_t text_total;
    /* outcome */
    int terminal, ok;
    const char *stop;
    const char *code;
    char msg[CANLIN_LLM_MSG_MAX];
    /* pending events, in order; pending text is materialised before any later event */
    char *ptext;
    size_t ptext_len, ptext_cap;
    pev *qhead, *qtail;
    pev *posted; /* in the scheduler queue: retracted on cancel */
};

struct canlin_llm {
    canlin_ev *ev;
    canlin_llm_config cfg;
    profile prof[CANLIN_LLM_PROFILES_MAX];
    llm_req *reqs;
};

static void llog(canlin_llm *llm, const char *fmt, const char *a, const char *b)
{
    char buf[CANLIN_LLM_MSG_MAX + 128];
    int n;
    if (llm->cfg.log == NULL)
        return;
    n = snprintf(buf, sizeof buf, fmt, a, b);
    if (n < 0)
        return;
    if ((size_t)n >= sizeof buf)
        n = (int)sizeof buf - 1;
    llm->cfg.log(llm->cfg.log_ud, buf, (size_t)n);
}

static void zeroise(void *p, size_t n)
{
    volatile unsigned char *v = p;
    while (n--)
        *v++ = 0;
}

static void key_drop(profile *p)
{
    if (p->key) {
        zeroise(p->key, p->keylen);
        free(p->key);
    }
    p->key = NULL;
    p->keylen = 0;
    p->key_origin[0] = 0;
}

canlin_llm *canlin_llm_new(canlin_ev *ev, const canlin_llm_config *cfg)
{
    canlin_llm *llm;
    if (ev == NULL || cfg == NULL || cfg->transport.open == NULL || cfg->transport.close == NULL)
        return NULL;
    llm = calloc(1, sizeof *llm);
    if (llm == NULL)
        return NULL;
    llm->ev = ev;
    llm->cfg = *cfg;
    return llm;
}

static profile *prof_find(const canlin_llm *llm, const char *id)
{
    unsigned i;
    if (id == NULL)
        return NULL;
    for (i = 0; i < CANLIN_LLM_PROFILES_MAX; i++)
        if (llm->prof[i].used && strcmp(llm->prof[i].id, id) == 0)
            return (profile *)&llm->prof[i];
    return NULL;
}

static int valid_id(const char *s)
{
    size_t n = 0;
    if (s == NULL)
        return 0;
    for (; s[n]; n++) {
        char c = s[n];
        if (n >= CANLIN_LLM_ID_MAX)
            return 0;
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return 0;
    }
    return n > 0;
}

static int copy_str(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);
    if (n >= cap || !llm_utf8_valid(src, n))
        return -1;
    memcpy(dst, src, n + 1);
    return 0;
}

/* Fill p (not yet visible) from cfg. */
static int prof_fill(canlin_llm *llm, profile *p, const canlin_llm_profile_config *cfg)
{
    const canlin_llm_preset *pre = NULL;
    const char *base, *model, *title;
    size_t n, k;

    if (!valid_id(cfg->id) || cfg->preset == NULL)
        return CANLIN_LLM_EINVAL;
    if (strcmp(cfg->preset, "custom") == 0) {
        if (cfg->dialect != CANLIN_LLM_DIALECT_OPENAI_CHAT &&
            cfg->dialect != CANLIN_LLM_DIALECT_ANTHROPIC_MESSAGES)
            return CANLIN_LLM_EINVAL;
        p->dialect = cfg->dialect;
        p->auth = cfg->auth;
        if (p->auth == CANLIN_LLM_AUTH_X_API_KEY
                ? p->dialect != CANLIN_LLM_DIALECT_ANTHROPIC_MESSAGES
                : (p->auth != CANLIN_LLM_AUTH_BEARER && p->auth != CANLIN_LLM_AUTH_NONE))
            return CANLIN_LLM_EUNSUPPORTED;
        p->keyneed = p->auth == CANLIN_LLM_AUTH_NONE ? CANLIN_LLM_KEY_NONE
                                                     : CANLIN_LLM_KEY_OPTIONAL;
        p->local = cfg->local != 0;
        p->tools = cfg->tools != 0;
        base = cfg->base_url;
        model = cfg->model;
        title = cfg->title ? cfg->title : cfg->id;
    } else {
        pre = canlin_llm_preset_find(cfg->preset);
        if (pre == NULL)
            return CANLIN_LLM_ENOENT;
        p->dialect = pre->dialect;
        p->auth = pre->auth;
        p->keyneed = pre->key;
        p->local = pre->local;
        p->tools = pre->tools;
        base = cfg->base_url ? cfg->base_url : pre->base_url;
        model = cfg->model ? cfg->model : pre->default_model;
        title = cfg->title ? cfg->title : pre->title;
    }
    if (!canlin_llm_dialect_available(p->dialect))
        return CANLIN_LLM_EUNSUPPORTED; /* spec §2.2: listed, not usable */
    if (base == NULL || model == NULL || model[0] == 0)
        return CANLIN_LLM_EINVAL;
    memcpy(p->id, cfg->id, strlen(cfg->id) + 1);
    if (copy_str(p->title, sizeof p->title, title) || copy_str(p->model, sizeof p->model, model) ||
        copy_str(p->base, sizeof p->base, base))
        return CANLIN_LLM_EINVAL;
    for (k = 0; p->model[k]; k++)
        if ((unsigned char)p->model[k] < 0x20 || p->model[k] == 0x7f)
            return CANLIN_LLM_EINVAL;
    n = strlen(p->base);
    while (n && p->base[n - 1] == '/')
        p->base[--n] = 0;
    for (k = 0; k < n; k++)
        if ((unsigned char)p->base[k] <= 0x20 || p->base[k] == 0x7f || p->base[k] == '?' ||
            p->base[k] == '#')
            return CANLIN_LLM_EINVAL;
    if (canlin_llm_url_origin(p->base, p->origin, sizeof p->origin) < 0)
        return CANLIN_LLM_EINVAL;
    if (strncmp(p->origin, "https://", 8) != 0 && !llm->cfg.allow_insecure_dev &&
        !(p->local && origin_loopback(p->origin)))
        return CANLIN_LLM_EINSECURE;
    return CANLIN_LLM_OK;
}

int canlin_llm_profile_create(canlin_llm *llm, const canlin_llm_profile_config *cfg)
{
    profile tmp, *slot = NULL;
    unsigned i;
    int rc;
    if (llm == NULL || cfg == NULL)
        return CANLIN_LLM_EINVAL;
    if (prof_find(llm, cfg->id))
        return CANLIN_LLM_EEXIST;
    for (i = 0; i < CANLIN_LLM_PROFILES_MAX && !slot; i++)
        if (!llm->prof[i].used)
            slot = &llm->prof[i];
    if (slot == NULL)
        return CANLIN_LLM_EFULL;
    memset(&tmp, 0, sizeof tmp);
    rc = prof_fill(llm, &tmp, cfg);
    if (rc != CANLIN_LLM_OK)
        return rc;
    tmp.used = 1;
    *slot = tmp;
    return CANLIN_LLM_OK;
}

static void req_abort_profile(canlin_llm *llm, profile *p, const char *why);

int canlin_llm_profile_update(canlin_llm *llm, const canlin_llm_profile_config *cfg)
{
    profile tmp, *p;
    int rc;
    if (llm == NULL || cfg == NULL)
        return CANLIN_LLM_EINVAL;
    p = prof_find(llm, cfg->id);
    if (p == NULL)
        return CANLIN_LLM_ENOENT;
    memset(&tmp, 0, sizeof tmp);
    rc = prof_fill(llm, &tmp, cfg);
    if (rc != CANLIN_LLM_OK)
        return rc;
    req_abort_profile(llm, p, "profile was reconfigured");
    tmp.used = 1;
    /* The key stays only while origin and auth scheme are unchanged (spec §5). */
    if (p->key && strcmp(p->key_origin, tmp.origin) == 0 && p->key_auth == tmp.auth) {
        tmp.key = p->key;
        tmp.keylen = p->keylen;
        memcpy(tmp.key_origin, p->key_origin, sizeof tmp.key_origin);
        tmp.key_auth = p->key_auth;
        p->key = NULL;
    }
    key_drop(p);
    *p = tmp;
    return CANLIN_LLM_OK;
}

int canlin_llm_profile_delete(canlin_llm *llm, const char *id)
{
    profile *p = llm ? prof_find(llm, id) : NULL;
    if (p == NULL)
        return CANLIN_LLM_ENOENT;
    req_abort_profile(llm, p, "profile was deleted");
    key_drop(p);
    zeroise(p, sizeof *p);
    return CANLIN_LLM_OK;
}

int canlin_llm_profile_set_key(canlin_llm *llm, const char *id, const void *key, size_t len)
{
    profile *p = llm ? prof_find(llm, id) : NULL;
    char *k;
    size_t i;
    if (p == NULL)
        return CANLIN_LLM_ENOENT;
    if (key == NULL || len == 0 || len > CANLIN_LLM_KEY_MAX)
        return CANLIN_LLM_EINVAL;
    for (i = 0; i < len; i++) /* header-safe bytes only: no CR/LF injection */
        if (((const unsigned char *)key)[i] <= 0x20 || ((const unsigned char *)key)[i] >= 0x7f)
            return CANLIN_LLM_EINVAL;
    if (p->auth == CANLIN_LLM_AUTH_NONE)
        return CANLIN_LLM_EUNSUPPORTED;
    k = malloc(len);
    if (k == NULL)
        return CANLIN_LLM_ENOMEM;
    memcpy(k, key, len);
    req_abort_profile(llm, p, "credential was replaced");
    key_drop(p);
    p->key = k;
    p->keylen = len;
    memcpy(p->key_origin, p->origin, sizeof p->key_origin);
    p->key_auth = p->auth;
    return CANLIN_LLM_OK;
}

int canlin_llm_profile_clear_key(canlin_llm *llm, const char *id)
{
    profile *p = llm ? prof_find(llm, id) : NULL;
    if (p == NULL)
        return CANLIN_LLM_ENOENT;
    req_abort_profile(llm, p, "credential was removed");
    key_drop(p);
    return CANLIN_LLM_OK;
}

int canlin_llm_profile_has_key(const canlin_llm *llm, const char *id)
{
    const profile *p = llm ? prof_find(llm, id) : NULL;
    return p != NULL && p->key != NULL;
}

/* ---- stream events ---- */

enum { PEV_TEXT = 1, PEV_TOOL, PEV_USAGE };

struct pev {
    canlin_module_event e;
    canlin_value root;
    canlin_value_pair p[5];
    llm_req *owner; /* while in the scheduler queue; NULL once detached */
    pev *prev, *next;
    size_t bytes;
    char data[1];
};

static void pev_unlink(pev *e)
{
    llm_req *r = e->owner;
    if (r == NULL)
        return;
    if (e->prev)
        e->prev->next = e->next;
    else
        r->posted = e->next;
    if (e->next)
        e->next->prev = e->prev;
    e->owner = NULL;
    e->prev = e->next = NULL;
}

static void pev_release(void *payload, size_t bytes)
{
    pev *e = payload;
    (void)bytes;
    pev_unlink(e);
    free(e);
}

static void setkv_str(canlin_value_pair *p, const char *k, const char *s, size_t n)
{
    p->key.type = CANLIN_VALUE_STR;
    p->key.s = k;
    p->key.len = strlen(k);
    p->val.type = CANLIN_VALUE_STR;
    p->val.s = s;
    p->val.len = n;
}

static void setkv_int(canlin_value_pair *p, const char *k, int64_t v)
{
    p->key.type = CANLIN_VALUE_STR;
    p->key.s = k;
    p->key.len = strlen(k);
    p->val.type = CANLIN_VALUE_INT;
    p->val.i = v;
}

static pev *pev_new(llm_req *r, const char *kind, size_t datalen)
{
    size_t bytes = sizeof(pev) + datalen;
    pev *e = calloc(1, bytes);
    if (e == NULL)
        return NULL;
    e->bytes = bytes;
    e->e.name = "llm";
    e->e.value = &e->root;
    e->root.type = CANLIN_VALUE_TABLE;
    e->root.pairs = e->p;
    setkv_int(&e->p[0], "handle", (int64_t)r->h);
    setkv_str(&e->p[1], "kind", kind, strlen(kind));
    e->root.n = 2;
    return e;
}

static void q_push(llm_req *r, pev *e)
{
    if (r->qtail)
        r->qtail->next = e;
    else
        r->qhead = e;
    r->qtail = e;
}

/* Split pending text into events of at most CANLIN_LLM_EVENT_TEXT_MAX bytes, cut at a
 * UTF-8 boundary (the pending text is valid UTF-8). */
static int materialise_text(llm_req *r)
{
    size_t off = 0;
    while (off < r->ptext_len) {
        size_t n = r->ptext_len - off;
        pev *e;
        if (n > CANLIN_LLM_EVENT_TEXT_MAX) {
            n = CANLIN_LLM_EVENT_TEXT_MAX;
            while (n > 0 && ((unsigned char)r->ptext[off + n] & 0xc0) == 0x80)
                n--;
        }
        e = pev_new(r, "text", n);
        if (e == NULL)
            return -1;
        memcpy(e->data, r->ptext + off, n);
        setkv_str(&e->p[2], "text", e->data, n);
        e->root.n = 3;
        q_push(r, e);
        off += n;
    }
    r->ptext_len = 0;
    return 0;
}

int llm_is_terminal(const llm_req *r)
{
    return r->terminal;
}

static void close_stream(llm_req *r)
{
    if (r->stream) {
        void *s = r->stream;
        r->stream = NULL;
        r->llm->cfg.transport.close(r->llm->cfg.transport.ctx, s);
    }
}

/* Terminal: stop reading and forget the profile (its key may be dropped after this). */
static void set_terminal(llm_req *r)
{
    r->terminal = 1;
    close_stream(r);
    r->prof = NULL;
}

/* Diagnostic text for Lua and logs: bounded, printable, valid UTF-8, and never holding
 * credential material (any 8-byte window of the key, or the whole key if shorter). */
static void sanitise(llm_req *r, const char *msg)
{
    const profile *p = r->prof;
    size_t n = strlen(msg), k;
    if (n >= sizeof r->msg)
        n = sizeof r->msg - 1;
    while (n && !llm_utf8_valid(msg, n))
        n--; /* cut a sequence split by the bound */
    memcpy(r->msg, msg, n);
    r->msg[n] = 0;
    for (k = 0; k < n; k++)
        if ((unsigned char)r->msg[k] < 0x20 || r->msg[k] == 0x7f)
            r->msg[k] = ' ';
    if (p && p->key) {
        size_t w = p->keylen < 8 ? p->keylen : 8, i, j;
        for (i = 0; i + w <= p->keylen; i++)
            for (j = 0; j + w <= n; j++)
                if (memcmp(r->msg + j, p->key + i, w) == 0) {
                    snprintf(r->msg, sizeof r->msg, "[message withheld: contained credential "
                                                    "material]");
                    return;
                }
    }
}

void llm_fail(llm_req *r, const char *code, const char *msg)
{
    if (r->terminal)
        return;
    r->ok = 0;
    r->code = code;
    sanitise(r, msg ? msg : "");
    set_terminal(r);
}

void llm_succeed(llm_req *r, const char *stop)
{
    if (r->terminal)
        return;
    r->ok = 1;
    r->stop = stop;
    set_terminal(r);
}

int llm_emit_text(llm_req *r, const char *s, size_t n)
{
    if (r->terminal)
        return -1;
    if (n > CANLIN_LLM_TEXT_MAX - r->text_total) {
        llm_fail(r, "full", "response text exceeds CANLIN_LLM_TEXT_MAX");
        return -1;
    }
    if (r->ptext_len + n > r->ptext_cap) {
        size_t cap = r->ptext_cap ? r->ptext_cap : 1024;
        char *nb;
        while (cap < r->ptext_len + n)
            cap *= 2;
        nb = realloc(r->ptext, cap);
        if (nb == NULL) {
            llm_fail(r, "unavailable", "out of memory buffering text");
            return -1;
        }
        r->ptext = nb;
        r->ptext_cap = cap;
    }
    memcpy(r->ptext + r->ptext_len, s, n);
    r->ptext_len += n;
    r->text_total += n;
    return 0;
}

int llm_emit_tool(llm_req *r, const char *id, size_t idn, const char *name, size_t namen,
                  const char *args, size_t argsn)
{
    pev *e;
    if (r->terminal)
        return -1;
    if (materialise_text(r) != 0 || (e = pev_new(r, "tool", idn + namen + argsn)) == NULL) {
        llm_fail(r, "unavailable", "out of memory queueing a tool call");
        return -1;
    }
    memcpy(e->data, id, idn);
    memcpy(e->data + idn, name, namen);
    memcpy(e->data + idn + namen, args, argsn);
    setkv_str(&e->p[2], "id", e->data, idn);
    setkv_str(&e->p[3], "name", e->data + idn, namen);
    setkv_str(&e->p[4], "args", e->data + idn + namen, argsn);
    e->root.n = 5;
    q_push(r, e);
    return 0;
}

int llm_emit_usage(llm_req *r, int64_t input, int64_t output)
{
    pev *e;
    if (r->terminal)
        return -1;
    if (materialise_text(r) != 0 || (e = pev_new(r, "usage", 0)) == NULL) {
        llm_fail(r, "unavailable", "out of memory queueing usage");
        return -1;
    }
    setkv_int(&e->p[2], "input", input);
    setkv_int(&e->p[3], "output", output);
    e->root.n = 4;
    q_push(r, e);
    return 0;
}

/* ---- request lifetime ---- */

typedef struct lres {
    canlin_module_result r;
    canlin_value v;
    canlin_value_pair p[1];
    char msg[CANLIN_LLM_MSG_MAX];
} lres;

static void free_payload(void *p, size_t bytes)
{
    (void)bytes;
    free(p);
}

static void q_free(llm_req *r)
{
    pev *e = r->qhead, *n;
    for (; e; e = n) {
        n = e->next;
        free(e);
    }
    r->qhead = r->qtail = NULL;
}

/* Unlink and free; posted events are detached (they stay valid until released). */
static void req_free(llm_req *r)
{
    llm_req **pp;
    pev *e, *n;
    for (pp = &r->llm->reqs; *pp; pp = &(*pp)->next)
        if (*pp == r) {
            *pp = r->next;
            break;
        }
    close_stream(r);
    if (r->dst)
        r->d->end(r->dst);
    for (e = r->posted; e; e = n) {
        n = e->next;
        e->owner = NULL;
        e->prev = e->next = NULL;
    }
    q_free(r);
    free(r->ptext);
    llm_jw_free(&r->body);
    free(r);
}

/* Post pending events in order; complete once terminal and drained. Returns 1 if r was
 * freed. */
static int req_pump(llm_req *r)
{
    canlin_llm *llm = r->llm;
    lres *res;
    int rc;

    if (r->ptext_len && materialise_text(r) != 0)
        llm_fail(r, "unavailable", "out of memory queueing text");
    while (r->qhead) {
        pev *e = r->qhead;
        rc = canlin_ev_post(llm->ev, r->gen, CANLIN_MODULE_SOURCE_EVENT, 0, 0, 0, e, e->bytes,
                            pev_release);
        if (rc == CANLIN_EV_EFULL)
            return 0; /* stream backpressure: retried by the next pump */
        r->qhead = e->next;
        if (r->qhead == NULL)
            r->qtail = NULL;
        e->next = NULL;
        if (rc != CANLIN_EV_OK) {
            free(e); /* generation gone: its stop cancels this request */
            continue;
        }
        e->owner = r;
        e->next = r->posted;
        if (r->posted)
            r->posted->prev = e;
        r->posted = e;
    }
    if (!r->terminal)
        return 0;
    res = calloc(1, sizeof *res);
    if (res == NULL)
        return 0; /* retried by the next pump */
    if (r->ok) {
        res->v.type = CANLIN_VALUE_TABLE;
        res->v.pairs = res->p;
        res->v.n = 1;
        setkv_str(&res->p[0], "stop", r->stop, strlen(r->stop));
        res->r.value = &res->v;
    } else {
        memcpy(res->msg, r->msg, sizeof res->msg);
        res->r.code = r->code;
        res->r.message = res->msg;
    }
    canlin_ev_op_complete(llm->ev, r->h, r->ok ? CANLIN_EV_SUCCESS : CANLIN_EV_FAILURE, res,
                          sizeof *res, free_payload);
    if (!r->ok)
        llog(llm, "llm: request failed: %s: %s", r->code, r->msg);
    req_free(r);
    return 1;
}

/* Producer cancel hook (canlin.cancel, generation stop): nothing more is delivered. */
static void req_cancel(void *ctx, canlin_ev_handle h)
{
    llm_req *r = ctx;
    pev *e;
    close_stream(r);
    for (e = r->posted; e; e = e->next)
        e->e.name = ""; /* retracted: dropped without entering Lua (module.h) */
    canlin_ev_op_complete(r->llm->ev, h, CANLIN_EV_FAILURE, NULL, 0, NULL); /* retires */
    req_free(r);
}

static void req_abort_profile(canlin_llm *llm, profile *p, const char *why)
{
    llm_req *r, *n;
    for (r = llm->reqs; r; r = n) {
        n = r->next;
        if (r->prof == p) {
            llm_fail(r, "unavailable", why);
            req_pump(r);
        }
    }
}

void canlin_llm_pump(canlin_llm *llm)
{
    llm_req *r, *n;
    canlin_ev_time now;
    if (llm == NULL)
        return;
    now = canlin_ev_now(llm->ev);
    for (r = llm->reqs; r; r = n) {
        n = r->next;
        if (!r->terminal && now - r->last_byte >= CANLIN_LLM_IDLE_TIMEOUT_MS)
            llm_fail(r, "unavailable", "idle timeout: no data from the provider");
        req_pump(r);
    }
}

unsigned canlin_llm_inflight(const canlin_llm *llm)
{
    unsigned n = 0;
    const llm_req *r;
    for (r = llm ? llm->reqs : NULL; r; r = r->next)
        n++;
    return n;
}

void canlin_llm_free(canlin_llm *llm)
{
    unsigned i;
    if (llm == NULL)
        return;
    while (llm->reqs) {
        llm_req *r = llm->reqs;
        llm_fail(r, "unavailable", "LLM service shut down");
        if (!req_pump(r)) {
            q_free(r); /* queue full: drop the rest, the terminal still completes */
            canlin_ev_op_complete(llm->ev, r->h, CANLIN_EV_FAILURE, NULL, 0, NULL);
            req_free(r);
        }
    }
    for (i = 0; i < CANLIN_LLM_PROFILES_MAX; i++)
        key_drop(&llm->prof[i]);
    zeroise(llm, sizeof *llm);
    free(llm);
}

/* ---- transport callbacks ---- */

static void cb_status(void *ud, int status)
{
    llm_req *r = ud;
    const char *code = "unavailable";
    char msg[64];
    r->status = status;
    r->last_byte = canlin_ev_now(r->llm->ev);
    if (status >= 200 && status < 300)
        return;
    if (status == 401 || status == 403)
        code = "denied";
    else if (status == 404 || status == 400 || status == 422)
        code = "unsupported";
    else if (status == 413)
        code = "full";
    /* Provider error bodies are not forwarded: they may echo request or key material. */
    snprintf(msg, sizeof msg, "provider answered HTTP %d", status);
    llm_fail(r, code, msg);
    req_pump(r);
}

static int ieq_prefix(const char *s, const char *pre)
{
    for (; *pre; s++, pre++) {
        char c = *s;
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        if (c != *pre)
            return 0;
    }
    return 1;
}

static void cb_header(void *ud, const char *name, const char *value)
{
    llm_req *r = ud;
    if (r->terminal)
        return;
    if (ieq_prefix(name, "content-type") && name[12] == 0 &&
        !ieq_prefix(value, "text/event-stream")) {
        llm_fail(r, "unavailable", "provider did not answer with text/event-stream");
        req_pump(r);
    }
}

static void cb_body(void *ud, const void *buf, size_t len)
{
    llm_req *r = ud;
    if (r->terminal)
        return;
    r->last_byte = canlin_ev_now(r->llm->ev);
    if (r->dst == NULL) {
        r->dst = r->d->begin(r);
        if (r->dst == NULL) {
            llm_fail(r, "unavailable", "out of memory for the stream parser");
            req_pump(r);
            return;
        }
    }
    r->d->feed(r->dst, buf, len);
    req_pump(r);
}

static void cb_end(void *ud)
{
    llm_req *r = ud;
    if (!r->terminal) {
        if (r->dst)
            r->d->eof(r->dst);
        if (!r->terminal)
            llm_fail(r, "unavailable", "stream ended without a terminal marker");
    }
    req_pump(r);
}

static void cb_error(void *ud, const char *msg)
{
    llm_req *r = ud;
    char m[CANLIN_LLM_MSG_MAX];
    snprintf(m, sizeof m, "transport: %s", msg ? msg : "error");
    llm_fail(r, "unavailable", m);
    req_pump(r);
}

static const canlin_llm_stream_cb stream_cb = {cb_status, cb_header, cb_body, cb_end, cb_error};

/* ---- Lua surface (spec §4) ---- */

static canlin_llm *llm_of(lua_State *L)
{
    return lua_touserdata(L, lua_upvalueindex(1));
}

static canlin_module *mod_of(lua_State *L)
{
    return lua_touserdata(L, lua_upvalueindex(2));
}

static int lfail(lua_State *L, const char *code, const char *msg)
{
    lua_pushnil(L);
    lua_pushstring(L, code);
    lua_pushstring(L, msg);
    return 3;
}

/* canlin.llm.profiles(): ids, titles, models and tool capability only. */
static int l_profiles(lua_State *L)
{
    canlin_llm *llm = llm_of(L);
    unsigned i;
    lua_Integer n = 0;
    lua_createtable(L, (int)CANLIN_LLM_PROFILES_MAX, 0);
    for (i = 0; i < CANLIN_LLM_PROFILES_MAX; i++) {
        const profile *p = &llm->prof[i];
        if (!p->used)
            continue;
        lua_createtable(L, 0, 4);
        lua_pushstring(L, p->id);
        lua_setfield(L, -2, "id");
        lua_pushstring(L, p->title);
        lua_setfield(L, -2, "title");
        lua_pushstring(L, p->model);
        lua_setfield(L, -2, "model");
        lua_pushboolean(L, p->tools);
        lua_setfield(L, -2, "tools");
        lua_rawseti(L, -2, ++n);
    }
    return 1;
}

/* Raw field lookup; leaves the value on the stack. */
static int rawfield(lua_State *L, int t, const char *k)
{
    lua_pushstring(L, k);
    return lua_rawget(L, t < 0 ? t - 1 : t);
}

/* A string field; missing -> 0 (nil), wrong type raises. */
static int str_field(lua_State *L, int t, const char *k, llm_str *out)
{
    int ty = rawfield(L, t, k);
    if (ty == LUA_TNIL) {
        lua_pop(L, 1);
        return 0;
    }
    if (ty != LUA_TSTRING)
        luaL_error(L, "llm.request: field '%s' must be a string", k);
    out->s = lua_tolstring(L, -1, &out->n); /* anchored by the table on the stack */
    lua_pop(L, 1);
    return 1;
}

static int l_request(lua_State *L)
{
    canlin_llm *llm = llm_of(L);
    canlin_module *m = mod_of(L);
    const char *pid = luaL_checkstring(L, 1);
    canlin_ev_gen gen = canlin_module_gen(m);
    profile *p;
    const llm_dialect *d;
    llm_msg *msgs;
    llm_call *calls;
    const llm_tool *tl[sizeof tools / sizeof tools[0]];
    size_t nmsgs, i, ntools = 0, cost = 0, nh = 0;
    lua_Integer maxtok = 0;
    llm_build b;
    llm_req *r, *q;
    unsigned inflight = 0;
    canlin_ev_producer prod;
    canlin_llm_http_request hr;
    const char *tmsg = NULL, *omsg = NULL;
    int ty, rc;

    luaL_checktype(L, 2, LUA_TTABLE);
    lua_settop(L, 2);
    p = prof_find(llm, pid);
    if (p == NULL)
        return lfail(L, "invalid", "unknown profile");
    d = dialect_of(p->dialect);
    if (d == NULL)
        return lfail(L, "unsupported", "the profile's dialect is not available");
    if (p->keyneed == CANLIN_LLM_KEY_REQUIRED && p->key == NULL)
        return lfail(L, "denied", "the profile has no credential");
    if (strncmp(p->origin, "https://", 8) != 0 && !llm->cfg.allow_insecure_dev &&
        !(p->local && origin_loopback(p->origin)))
        return lfail(L, "denied", "plain http endpoint not permitted");
    for (q = llm->reqs; q; q = q->next)
        if (q->gen == gen)
            inflight++;
    if (inflight >= CANLIN_LLM_INFLIGHT_MAX)
        return lfail(L, "full", "CANLIN_LLM_INFLIGHT_MAX requests in flight");

    /* messages: stack 3 = messages, 4 = msgs, 5 = calls (userdata: collected by the GC
     * even when a type error raises, so nothing leaks) */
    if (rawfield(L, 2, "messages") != LUA_TTABLE)
        return luaL_error(L, "llm.request: 'messages' must be a table");
    nmsgs = (size_t)lua_rawlen(L, 3);
    if (nmsgs == 0 || nmsgs > CANLIN_LLM_MESSAGES_MAX)
        return lfail(L, "invalid", "message count out of range");
    msgs = lua_newuserdatauv(L, nmsgs * sizeof *msgs, 0);
    calls = lua_newuserdatauv(L, nmsgs * CANLIN_LLM_TOOL_CALLS_MAX * sizeof *calls, 0);
    memset(msgs, 0, nmsgs * sizeof *msgs);
    for (i = 0; i < nmsgs; i++) {
        llm_msg *mm = &msgs[i];
        llm_str role = {NULL, 0};
        lua_settop(L, 5);
        if (lua_rawgeti(L, 3, (lua_Integer)i + 1) != LUA_TTABLE) /* 6 */
            return luaL_error(L, "llm.request: messages[%d] must be a table", (int)i + 1);
        if (!str_field(L, 6, "role", &role))
            return luaL_error(L, "llm.request: messages[%d].role missing", (int)i + 1);
        mm->has_text = str_field(L, 6, "text", &mm->text);
        cost += mm->text.n + role.n;
        if (role.n == 6 && memcmp(role.s, "system", 6) == 0)
            mm->role = LLM_ROLE_SYSTEM;
        else if (role.n == 4 && memcmp(role.s, "user", 4) == 0)
            mm->role = LLM_ROLE_USER;
        else if (role.n == 4 && memcmp(role.s, "tool", 4) == 0)
            mm->role = LLM_ROLE_TOOL;
        else if (role.n == 9 && memcmp(role.s, "assistant", 9) == 0)
            mm->role = LLM_ROLE_ASSISTANT;
        else
            return lfail(L, "invalid", "unknown message role");
        if (mm->role == LLM_ROLE_TOOL && !str_field(L, 6, "tool_call_id", &mm->tool_call_id))
            return luaL_error(L, "llm.request: messages[%d].tool_call_id missing", (int)i + 1);
        cost += mm->tool_call_id.n;
        if (mm->role != LLM_ROLE_ASSISTANT && !mm->has_text)
            return luaL_error(L, "llm.request: messages[%d].text missing", (int)i + 1);
        ty = rawfield(L, 6, "tool_calls"); /* 7 */
        if (ty != LUA_TNIL) {
            size_t k, nc;
            if (ty != LUA_TTABLE)
                return luaL_error(L, "llm.request: tool_calls must be a table");
            if (mm->role != LLM_ROLE_ASSISTANT)
                return lfail(L, "invalid", "tool_calls only on assistant messages");
            nc = (size_t)lua_rawlen(L, 7);
            if (nc > CANLIN_LLM_TOOL_CALLS_MAX)
                return lfail(L, "invalid", "too many tool_calls in a message");
            mm->calls = &calls[i * CANLIN_LLM_TOOL_CALLS_MAX];
            mm->ncalls = nc;
            for (k = 0; k < nc; k++) {
                llm_call *c = &calls[i * CANLIN_LLM_TOOL_CALLS_MAX + k];
                lua_settop(L, 7);
                if (lua_rawgeti(L, 7, (lua_Integer)k + 1) != LUA_TTABLE) /* 8 */
                    return luaL_error(L, "llm.request: tool_calls[%d] must be a table",
                                      (int)k + 1);
                if (!str_field(L, 8, "id", &c->id) || !str_field(L, 8, "name", &c->name) ||
                    !str_field(L, 8, "args", &c->args))
                    return luaL_error(L, "llm.request: tool call needs id, name and args");
                cost += c->id.n + c->name.n + c->args.n;
            }
        }
        if (mm->role == LLM_ROLE_ASSISTANT && !mm->has_text && mm->ncalls == 0)
            return luaL_error(L, "llm.request: messages[%d] needs text or tool_calls",
                              (int)i + 1);
    }
    lua_settop(L, 5);

    /* tools: names of core-provided tools only */
    ty = rawfield(L, 2, "tools"); /* 6 */
    if (ty != LUA_TNIL) {
        size_t k, nt;
        if (ty != LUA_TTABLE)
            return luaL_error(L, "llm.request: 'tools' must be a table");
        nt = (size_t)lua_rawlen(L, 6);
        for (k = 0; k < nt && !tmsg; k++) {
            llm_str s;
            const llm_tool *t;
            size_t j;
            lua_settop(L, 6);
            if (lua_rawgeti(L, 6, (lua_Integer)k + 1) != LUA_TSTRING)
                return luaL_error(L, "llm.request: tools[%d] must be a string", (int)k + 1);
            s.s = lua_tolstring(L, 7, &s.n);
            t = llm_tool_find(s.s, s.n);
            if (t == NULL) {
                tmsg = "unknown tool (only core-provided tools can be named)";
                break;
            }
            for (j = 0; j < ntools; j++)
                if (tl[j] == t)
                    tmsg = "tool named twice";
            if (!tmsg)
                tl[ntools++] = t;
        }
    }
    lua_settop(L, 5);
    if (tmsg)
        return lfail(L, "invalid", tmsg);
    if (ntools && !p->tools)
        return lfail(L, "unsupported", "the profile is not configured with tool support");

    ty = rawfield(L, 2, "max_tokens"); /* 6 */
    if (ty != LUA_TNIL) {
        if (!lua_isinteger(L, 6))
            return luaL_error(L, "llm.request: 'max_tokens' must be an integer");
        maxtok = lua_tointeger(L, 6);
        if (maxtok < 1 || (lua_Unsigned)maxtok > CANLIN_LLM_MAX_TOKENS_MAX)
            return lfail(L, "invalid", "max_tokens out of range");
    }
    lua_settop(L, 5);

    /* Serialising is proportional work: charge it before doing it (may fault). */
    canlin_sandbox_charge(L, cost / CANLIN_SB_COST_BYTES_PER_UNIT +
                                 nmsgs * CANLIN_SB_COST_ELEM);

    r = calloc(1, sizeof *r);
    if (r == NULL)
        return lfail(L, "unavailable", "out of memory");
    r->llm = llm;
    r->prof = p;
    r->gen = gen;
    r->d = d;
    memset(&b, 0, sizeof b);
    b.model = p->model;
    b.msgs = msgs;
    b.nmsgs = nmsgs;
    b.tools = tl;
    b.ntools = ntools;
    b.max_tokens = (unsigned long)maxtok;
    llm_jw_init(&r->body, CANLIN_LLM_REQUEST_MAX);
    d->build(&r->body, &b);
    if (r->body.err) {
        rc = r->body.err;
        llm_jw_free(&r->body);
        free(r);
        if (rc == JW_EFULL)
            return lfail(L, "invalid", "request body exceeds CANLIN_LLM_REQUEST_MAX");
        if (rc == JW_EUTF8)
            return lfail(L, "invalid", "message text is not valid UTF-8");
        return lfail(L, "unavailable", "out of memory");
    }
    snprintf(r->url, sizeof r->url, "%s%s", p->base, d->path);
    r->hdrs[nh].name = "Content-Type";
    r->hdrs[nh++].value = "application/json";
    r->hdrs[nh].name = "Accept";
    r->hdrs[nh++].value = "text/event-stream";
    for (i = 0; d->headers && d->headers[i].name && nh < sizeof r->hdrs / sizeof r->hdrs[0]; i++)
        r->hdrs[nh++] = d->headers[i];
    memset(&hr, 0, sizeof hr);
    hr.method = "POST";
    hr.url = r->url;
    hr.headers = r->hdrs;
    hr.nheaders = nh;
    hr.body = r->body.buf;
    hr.body_len = r->body.len;
    if (p->key && p->auth != CANLIN_LLM_AUTH_NONE) {
        /* Bound at set time; profile_update drops a key whose binding changed. */
        r->auth.origin = p->key_origin;
        r->auth.header = p->auth == CANLIN_LLM_AUTH_BEARER ? "Authorization" : "x-api-key";
        r->auth.prefix = p->auth == CANLIN_LLM_AUTH_BEARER ? "Bearer " : "";
        r->auth.secret = p->key;
        r->auth.secret_len = p->keylen;
        hr.auth = &r->auth;
    }

    prod.cancel = req_cancel;
    prod.ctx = r;
    rc = canlin_ev_op_begin(llm->ev, gen, sizeof(lres), &prod, &r->h);
    if (rc != CANLIN_EV_OK) {
        llm_jw_free(&r->body);
        free(r);
        return lfail(L, rc == CANLIN_EV_EFULL ? "full" : "stale",
                     rc == CANLIN_EV_EFULL ? "operation limit reached" : "generation is not live");
    }
    r->last_byte = canlin_ev_now(llm->ev);
    r->next = llm->reqs;
    llm->reqs = r;
    r->stream = llm->cfg.transport.open(llm->cfg.transport.ctx, &hr, &stream_cb, r, &omsg);
    if (r->stream == NULL) {
        /* Nothing was sent: undo the admission without any delivery. */
        canlin_ev_cancel(llm->ev, gen, r->h); /* runs req_cancel: retire and free */
        return lfail(L, "unavailable", omsg ? omsg : "transport could not open the request");
    }
    lua_pushinteger(L, (lua_Integer)r->h);
    return 1;
}

int canlin_llm_install(lua_State *L, canlin_module *m, void *ud)
{
    lua_createtable(L, 0, 2);
    lua_pushlightuserdata(L, ud);
    lua_pushlightuserdata(L, m);
    lua_pushcclosure(L, l_profiles, 2);
    lua_setfield(L, -2, "profiles");
    lua_pushlightuserdata(L, ud);
    lua_pushlightuserdata(L, m);
    lua_pushcclosure(L, l_request, 2);
    lua_setfield(L, -2, "request");
    lua_setfield(L, -2, "llm");
    return 0;
}
