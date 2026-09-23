/* CanLin assistant-owned view. See include/canlin/assistant_view.h for the contract.
 *
 * Validation runs in two steps so that nothing changes before the whole argument is
 * known to be valid: the bounded push tokenizer (src/net/json.c) builds a transient parse
 * tree (bounded by CANLIN_AV_MAX_VALUES), then the schema walk builds the immutable
 * model. Strings are copied once into the model's own arena; the tree is freed at
 * finish. Split input only changes where the tokenizer resumes, so the result is the
 * same for any split.
 *
 * Rendering reuses the flow widgets unchanged: the model is drawn by calling the methods
 * of the per-draw `ui` object (src/ui/ui.c), from C. Every stateful widget gets an
 * explicit identity "r<revision>:<id>", so widget state (focus, open sections, graph pan)
 * never crosses a replacement even when ids repeat.
 */
#include "canlin/assistant_view.h"
#include "canlin/json.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"

/* ---- arena ---- */

typedef struct ablock {
    struct ablock *next;
    size_t used, cap;
} ablock;

#define AHDR ((sizeof(ablock) + 15u) & ~(size_t)15u)
#define ABLOCK 16384u

typedef struct arena {
    ablock *head;
    size_t total, limit;
} arena;

static void *aalloc(arena *a, size_t n)
{
    ablock *b = a->head;
    void *p;
    n = (n + 15u) & ~(size_t)15u;
    if (n == 0)
        n = 16;
    if (b == NULL || b->cap - b->used < n) {
        size_t cap = n > ABLOCK ? n : ABLOCK;
        if (a->total + cap > a->limit)
            return NULL;
        b = malloc(AHDR + cap);
        if (b == NULL)
            return NULL;
        b->next = a->head;
        b->used = 0;
        b->cap = cap;
        a->head = b;
        a->total += cap;
    }
    p = (char *)b + AHDR + b->used;
    b->used += n;
    return p;
}

static void afree(arena *a)
{
    ablock *b = a->head;
    while (b) {
        ablock *n = b->next;
        free(b);
        b = n;
    }
    a->head = NULL;
    a->total = 0;
}

/* ---- errors ---- */

const char *canlin_av_code(int status)
{
    switch (status) {
    case CANLIN_AV_OK: return "ok";
    case CANLIN_AV_ESTALE: return "stale";
    case CANLIN_AV_EFULL: return "full";
    case CANLIN_AV_ENOMEM: return "unavailable";
    default: return "invalid";
    }
}

static void set_err(canlin_av_error *e, int status, const char *path, const char *msg)
{
    size_t n;
    e->status = status;
    n = strlen(path);
    if (n >= sizeof e->path)
        n = sizeof e->path - 1;
    memcpy(e->path, path, n);
    e->path[n] = 0;
    n = strlen(msg);
    if (n >= sizeof e->message)
        n = sizeof e->message - 1;
    memcpy(e->message, msg, n);
    e->message[n] = 0;
}

/* ---- parse tree ---- */

enum { J_OBJ = 1, J_ARR, J_STR, J_NUM, J_TRUE, J_FALSE, J_NULL };

typedef struct jv {
    unsigned char t;
    unsigned n;             /* children */
    const char *s;          /* STR (in the kept arena, NUL-terminated) / NUM text */
    size_t len;
    const char *k;          /* key when the parent is an object */
    size_t kl;
    struct jv *kid, *last, *next;
} jv;

#define JDEPTH (2u * CANLIN_UI_MAX_DEPTH + 10u) /* tokenizer backstop, see widget depth */

struct canlin_av_parser {
    canlin_json js;
    arena tree, keep;
    jv *root;
    jv *stack[JDEPTH + 1];
    unsigned sp;
    char *pk;               /* pending key (tree arena) */
    size_t pkl;
    int have_key;
    unsigned values;
    int status;
    canlin_av_error err;
    int spent;
    char tok[CANLIN_UI_MAX_TEXT];
};

/* Model plus its storage. */
typedef struct holder {
    canlin_av_model m;
    arena keep;
} holder;

static int key_printable(const char *k, size_t n)
{
    size_t i;
    if (n == 0 || n > CANLIN_AV_ID_MAX)
        return 0;
    for (i = 0; i < n; i++) {
        char c = k[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_'))
            return 0;
    }
    return 1;
}

static void path_key(char *buf, size_t cap, const char *k, size_t kl)
{
    size_t n = strlen(buf);
    if (key_printable(k, kl))
        snprintf(buf + n, cap - n, ".%.*s", (int)kl, k);
    else
        snprintf(buf + n, cap - n, ".?");
}

static void path_idx(char *buf, size_t cap, unsigned i)
{
    size_t n = strlen(buf);
    snprintf(buf + n, cap - n, "[%u]", i);
}

/* Path of the value being parsed now (tokenizer-level failures). */
static void stack_path(const canlin_av_parser *p, char *buf, size_t cap)
{
    unsigned i;
    snprintf(buf, cap, "$");
    for (i = 0; i < p->sp; i++) {
        const jv *c = p->stack[i];
        int top = i + 1 == p->sp;
        if (c->t == J_ARR) {
            if (top)
                path_idx(buf, cap, c->n);
            else
                path_idx(buf, cap, c->n - 1);
        } else if (top) {
            if (p->have_key)
                path_key(buf, cap, p->pk, p->pkl);
        } else {
            path_key(buf, cap, c->last->k, c->last->kl);
        }
    }
}

static int tree_fail(canlin_av_parser *p, const char *msg)
{
    char path[CANLIN_AV_PATH_MAX];
    stack_path(p, path, sizeof path);
    if (p->status == CANLIN_AV_OK)
        set_err(&p->err, CANLIN_AV_EINVALID, path, msg);
    p->status = CANLIN_AV_EINVALID;
    return 1;
}

static jv *tree_add(canlin_av_parser *p, int t)
{
    jv *v, *par = p->sp ? p->stack[p->sp - 1] : NULL;
    if (p->values >= CANLIN_AV_MAX_VALUES) {
        tree_fail(p, "too many JSON values");
        return NULL;
    }
    if (par == NULL && p->root) {
        tree_fail(p, "more than one value");
        return NULL;
    }
    v = aalloc(&p->tree, sizeof *v);
    if (v == NULL) {
        tree_fail(p, "argument too large");
        return NULL;
    }
    memset(v, 0, sizeof *v);
    v->t = (unsigned char)t;
    p->values++;
    if (par == NULL) {
        p->root = v;
    } else {
        if (par->t == J_OBJ) {
            v->k = p->pk;
            v->kl = p->pkl;
            p->have_key = 0;
        }
        if (par->last)
            par->last->next = v;
        else
            par->kid = v;
        par->last = v;
        par->n++;
    }
    return v;
}

static int on_token(void *ud, const canlin_json_token *t)
{
    canlin_av_parser *p = ud;
    jv *v;
    switch (t->type) {
    case CANLIN_JSON_KEY: {
        jv *o = p->stack[p->sp - 1], *c;
        if (o->n >= CANLIN_AV_MAX_KEYS)
            return tree_fail(p, "too many keys in one object");
        for (c = o->kid; c; c = c->next)
            if (c->kl == t->len && memcmp(c->k, t->ptr, t->len) == 0) {
                p->pk = (char *)c->k;
                p->pkl = c->kl;
                p->have_key = 1;
                return tree_fail(p, "duplicate key");
            }
        p->pk = aalloc(&p->tree, t->len + 1);
        if (p->pk == NULL)
            return tree_fail(p, "argument too large");
        memcpy(p->pk, t->ptr, t->len);
        p->pk[t->len] = 0;
        p->pkl = t->len;
        p->have_key = 1;
        return 0;
    }
    case CANLIN_JSON_OBJ_BEGIN:
    case CANLIN_JSON_ARR_BEGIN:
        v = tree_add(p, t->type == CANLIN_JSON_OBJ_BEGIN ? J_OBJ : J_ARR);
        if (v == NULL)
            return 1;
        if (p->sp >= JDEPTH)
            return tree_fail(p, "nesting too deep");
        p->stack[p->sp++] = v;
        return 0;
    case CANLIN_JSON_OBJ_END:
    case CANLIN_JSON_ARR_END:
        p->sp--;
        return 0;
    case CANLIN_JSON_STRING:
    case CANLIN_JSON_NUMBER: {
        char *s;
        v = tree_add(p, t->type == CANLIN_JSON_STRING ? J_STR : J_NUM);
        if (v == NULL)
            return 1;
        s = aalloc(t->type == CANLIN_JSON_STRING ? &p->keep : &p->tree, t->len + 1);
        if (s == NULL)
            return tree_fail(p, "argument too large");
        memcpy(s, t->ptr, t->len);
        s[t->len] = 0;
        v->s = s;
        v->len = t->len;
        return 0;
    }
    case CANLIN_JSON_TRUE:
        return tree_add(p, J_TRUE) == NULL;
    case CANLIN_JSON_FALSE:
        return tree_add(p, J_FALSE) == NULL;
    default:
        return tree_add(p, J_NULL) == NULL;
    }
}

canlin_av_parser *canlin_av_parser_new(void)
{
    canlin_av_parser *p = calloc(1, sizeof *p);
    canlin_json_limits lim;
    if (p == NULL)
        return NULL;
    lim.max_depth = JDEPTH;
    lim.max_string = CANLIN_UI_MAX_TEXT;
    lim.max_number = CANLIN_AV_MAX_NUMBER;
    lim.max_total = CANLIN_UI_TOOL_MAX_BYTES;
    /* Backstops: the tree is bounded by the value count, the kept strings by the input. */
    p->tree.limit = (size_t)CANLIN_AV_MAX_VALUES * (sizeof(jv) + 16u) + 2u * ABLOCK +
                    2u * CANLIN_UI_TOOL_MAX_BYTES;
    p->keep.limit = 4u * CANLIN_UI_TOOL_MAX_BYTES + 64u * ABLOCK;
    if (canlin_json_init(&p->js, &lim, p->tok, sizeof p->tok, on_token, p) != CANLIN_JSON_OK) {
        free(p);
        return NULL;
    }
    return p;
}

void canlin_av_parser_free(canlin_av_parser *p)
{
    if (p == NULL)
        return;
    afree(&p->tree);
    afree(&p->keep);
    free(p);
}

static void json_fail(canlin_av_parser *p, int rc)
{
    char path[CANLIN_AV_PATH_MAX], msg[CANLIN_AV_MSG_MAX];
    if (p->status != CANLIN_AV_OK || rc == CANLIN_JSON_E_ABORTED)
        return; /* the callback recorded it */
    stack_path(p, path, sizeof path);
    if (rc == CANLIN_JSON_E_TOTAL)
        snprintf(msg, sizeof msg, "argument exceeds %u bytes", (unsigned)CANLIN_UI_TOOL_MAX_BYTES);
    else
        snprintf(msg, sizeof msg, "JSON: %s", canlin_json_strerror(rc));
    set_err(&p->err, CANLIN_AV_EINVALID, path, msg);
    p->status = CANLIN_AV_EINVALID;
}

int canlin_av_parser_feed(canlin_av_parser *p, const void *data, size_t len)
{
    int rc;
    if (p == NULL || (data == NULL && len))
        return CANLIN_AV_EARG;
    if (p->status != CANLIN_AV_OK || p->spent)
        return p->status != CANLIN_AV_OK ? p->status : CANLIN_AV_EARG;
    rc = canlin_json_feed(&p->js, data, len);
    if (rc != CANLIN_JSON_OK)
        json_fail(p, rc);
    return p->status;
}

/* ---- schema walk ---- */

typedef struct vctx {
    arena *keep;
    canlin_av_error *err;
    char path[CANLIN_AV_PATH_MAX];
    unsigned total, graphs;
    canlin_av_str ids[CANLIN_UI_TOOL_MAX_WIDGETS];
    unsigned nids;
} vctx;

static int fail(vctx *c, const char *msg)
{
    set_err(c->err, CANLIN_AV_EINVALID, c->path, msg);
    return -1;
}

/* Path helpers: push returns the old length to restore. */
static size_t pk(vctx *c, const char *k)
{
    size_t n = strlen(c->path);
    path_key(c->path, sizeof c->path, k, strlen(k));
    return n;
}

static size_t pi(vctx *c, unsigned i)
{
    size_t n = strlen(c->path);
    path_idx(c->path, sizeof c->path, i);
    return n;
}

static void pop(vctx *c, size_t n)
{
    c->path[n] = 0;
}

static const jv *get(const jv *o, const char *k)
{
    const jv *v;
    size_t n = strlen(k);
    for (v = o->kid; v; v = v->next)
        if (v->kl == n && memcmp(v->k, k, n) == 0)
            return v;
    return NULL;
}

/* Every key of o must be in allowed (NULL-terminated). */
static int only_keys(vctx *c, const jv *o, const char *const *allowed)
{
    const jv *v;
    for (v = o->kid; v; v = v->next) {
        const char *const *a;
        int ok = 0;
        for (a = allowed; *a; a++)
            if (strlen(*a) == v->kl && memcmp(*a, v->k, v->kl) == 0)
                ok = 1;
        if (!ok) {
            path_key(c->path, sizeof c->path, v->k, v->kl);
            return fail(c, "unknown key");
        }
    }
    return 0;
}

/* No C0, DEL or C1 control character (UI spec §7); multi-line content (text/bubble text,
 * area value) may hold '\n' and '\t'. s is valid UTF-8 (tokenizer). */
static int has_control(const char *s, size_t n, int multiline)
{
    size_t i;
    for (i = 0; i < n; i++) {
        unsigned char b = (unsigned char)s[i];
        if (multiline && (b == '\n' || b == '\t'))
            continue;
        if (b < 0x20 || b == 0x7f)
            return 1;
        if (b == 0xc2 && i + 1 < n && (unsigned char)s[i + 1] >= 0x80 &&
            (unsigned char)s[i + 1] <= 0x9f)
            return 1;
    }
    return 0;
}

static const char EMPTY[1] = "";

/* String member k of o. Missing: error if required, else empty. multiline admits '\n' and
 * '\t' (UI spec §7). */
static int str_ml(vctx *c, const jv *o, const char *k, int required, size_t max, int multiline,
                  canlin_av_str *out)
{
    const jv *v = get(o, k);
    size_t n;
    char msg[CANLIN_AV_MSG_MAX];
    out->s = EMPTY;
    out->len = 0;
    if (v == NULL) {
        if (!required)
            return 0;
        n = pk(c, k);
        fail(c, "required");
        return -1;
    }
    n = pk(c, k);
    if (v->t != J_STR)
        return fail(c, "must be a string");
    if (v->len > max) {
        snprintf(msg, sizeof msg, "exceeds %u bytes", (unsigned)max);
        return fail(c, msg);
    }
    if (has_control(v->s, v->len, multiline))
        return fail(c, "contains a control character");
    pop(c, n);
    out->s = v->s;
    out->len = v->len;
    return 0;
}

/* Single-line string: no control character at all. */
static int str_of(vctx *c, const jv *o, const char *k, int required, size_t max,
                  canlin_av_str *out)
{
    return str_ml(c, o, k, required, max, 0, out);
}

static int id_ok(const char *s, size_t n)
{
    size_t i;
    if (n < 1 || n > CANLIN_AV_ID_MAX)
        return 0;
    for (i = 0; i < n; i++) {
        char ch = s[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-'))
            return 0;
    }
    return 1;
}

static int id_of(vctx *c, const jv *o, const char *k, int required, canlin_av_str *out)
{
    size_t n;
    if (str_of(c, o, k, required, CANLIN_UI_MAX_LABEL, out) < 0)
        return -1;
    if (out->len == 0 && !get(o, k))
        return 0;
    if (!id_ok(out->s, out->len)) {
        n = pk(c, k);
        (void)n;
        return fail(c, "must match [a-z0-9_-]{1,32}");
    }
    return 0;
}

/* Parses validated JSON number text (RFC 8259 grammar, tokenizer-checked). */
static double num_val(const char *s, size_t n, int *integer)
{
    double m = 0;
    long e = 0, ex = 0;
    int neg = 0, eneg = 0;
    size_t i = 0;
    *integer = 1;
    if (i < n && s[i] == '-') {
        neg = 1;
        i++;
    }
    while (i < n && s[i] >= '0' && s[i] <= '9')
        m = m * 10.0 + (s[i++] - '0');
    if (i < n && s[i] == '.') {
        *integer = 0;
        i++;
        while (i < n && s[i] >= '0' && s[i] <= '9') {
            m = m * 10.0 + (s[i++] - '0');
            e--;
        }
    }
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        *integer = 0;
        i++;
        if (i < n && (s[i] == '+' || s[i] == '-'))
            eneg = s[i++] == '-';
        while (i < n && s[i] >= '0' && s[i] <= '9') {
            if (ex < 10000)
                ex = ex * 10 + (s[i] - '0');
            i++;
        }
    }
    e += eneg ? -ex : ex;
    if (m != 0 && e != 0)
        m *= pow(10.0, (double)e);
    return neg ? -m : m;
}

/* Number member; integer: only integer text (no fraction/exponent). */
static int num_of(vctx *c, const jv *o, const char *k, int required, int integer, double lo,
                  double hi, double *out)
{
    const jv *v = get(o, k);
    size_t n;
    int isint;
    double x;
    char msg[CANLIN_AV_MSG_MAX];
    if (v == NULL) {
        if (!required)
            return 1; /* absent: caller keeps its default */
        n = pk(c, k);
        (void)n;
        return fail(c, "required");
    }
    n = pk(c, k);
    if (v->t != J_NUM)
        return fail(c, integer ? "must be an integer" : "must be a number");
    x = num_val(v->s, v->len, &isint);
    if (integer && !isint)
        return fail(c, "must be an integer");
    if (!(x >= lo && x <= hi)) {
        if (integer)
            snprintf(msg, sizeof msg, "must be %ld-%ld", (long)lo, (long)hi);
        else
            snprintf(msg, sizeof msg, "must be within %g..%g", lo, hi);
        return fail(c, msg);
    }
    pop(c, n);
    *out = x;
    return 0;
}

static int bool_of(vctx *c, const jv *o, const char *k, int *out)
{
    const jv *v = get(o, k);
    size_t n;
    if (v == NULL)
        return 0;
    n = pk(c, k);
    if (v->t != J_TRUE && v->t != J_FALSE)
        return fail(c, "must be a boolean");
    pop(c, n);
    *out = v->t == J_TRUE;
    return 0;
}

static int enum_of(vctx *c, const jv *o, const char *k, int required,
                   const char *const *names, int *out)
{
    canlin_av_str s;
    size_t n;
    int i;
    if (str_of(c, o, k, required, CANLIN_UI_MAX_LABEL, &s) < 0)
        return -1;
    if (!get(o, k))
        return 0;
    for (i = 0; names[i]; i++)
        if (strlen(names[i]) == s.len && memcmp(names[i], s.s, s.len) == 0) {
            *out = i;
            return 0;
        }
    n = pk(c, k);
    (void)n;
    return fail(c, "value not allowed");
}

static const jv *arr_of(vctx *c, const jv *o, const char *k, int required)
{
    const jv *v = get(o, k);
    size_t n;
    if (v == NULL) {
        if (required) {
            n = pk(c, k);
            (void)n;
            fail(c, "required");
        }
        return NULL;
    }
    if (v->t != J_ARR) {
        n = pk(c, k);
        (void)n;
        fail(c, "must be an array");
        return NULL;
    }
    return v;
}

static const char *const TYPES[] = {"heading", "text", "bubble", "field", "area", "toggle",
                                    "choice", "meter", "separator", "section", "columns",
                                    "graph", "button", "icon", NULL};
/* Built-in assistant icon names (UI spec §7). Empty until an icon set exists (the image
 * decoder is canlin#23), so every icon is rejected. */
static const char *const ICONS[] = {NULL};

static int widgets_of(vctx *c, const jv *arr, unsigned depth, const canlin_av_widget **out,
                      unsigned *nout);

static int port_list(vctx *c, const jv *a, canlin_av_port *ports, unsigned base)
{
    static const char *const keys[] = {"id", "label", "type", NULL};
    const jv *e;
    unsigned i = 0, j;
    for (e = a->kid; e; e = e->next, i++) {
        canlin_av_port *p = &ports[base + i];
        size_t n = pi(c, i);
        if (e->t != J_OBJ)
            return fail(c, "must be an object");
        if (only_keys(c, e, keys) < 0 || id_of(c, e, "id", 1, &p->id) < 0 ||
            str_of(c, e, "label", 0, CANLIN_UI_MAX_LABEL, &p->label) < 0 ||
            str_of(c, e, "type", 0, CANLIN_UI_MAX_LABEL, &p->type) < 0)
            return -1;
        for (j = 0; j < base + i; j++)
            if (ports[j].id.len == p->id.len && memcmp(ports[j].id.s, p->id.s, p->id.len) == 0) {
                pk(c, "id");
                return fail(c, "not unique in its node");
            }
        pop(c, n);
    }
    return 0;
}

static int find_node(const canlin_av_node *nodes, unsigned nn, const char *s, size_t l)
{
    unsigned i;
    for (i = 0; i < nn; i++)
        if (nodes[i].id.len == l && memcmp(nodes[i].id.s, s, l) == 0)
            return (int)i;
    return -1;
}

/* [node, port] naming a port of direction out; returns node/port index. */
static int endpoint(vctx *c, const jv *e, const char *k, const canlin_av_node *nodes,
                    unsigned nn, int out, unsigned *ni, unsigned *pj)
{
    const jv *v = get(e, k), *a, *b;
    size_t n;
    int node;
    unsigned q;
    n = pk(c, k);
    if (v == NULL)
        return fail(c, "required");
    if (v->t != J_ARR || v->n != 2 || v->kid->t != J_STR || v->kid->next->t != J_STR)
        return fail(c, "must be [node, port]");
    a = v->kid;
    b = a->next;
    node = find_node(nodes, nn, a->s, a->len);
    if (node < 0) {
        pi(c, 0);
        return fail(c, "names an unknown node");
    }
    for (q = 0; q < nodes[node].nin + nodes[node].nout; q++) {
        const canlin_av_port *p = &nodes[node].ports[q];
        if (p->id.len == b->len && memcmp(p->id.s, b->s, b->len) == 0) {
            if ((q >= nodes[node].nin) != out) {
                pi(c, 1);
                return fail(c, out ? "must name an output port" : "must name an input port");
            }
            *ni = (unsigned)node;
            *pj = q;
            pop(c, n);
            return 0;
        }
    }
    pi(c, 1);
    return fail(c, "names an unknown port");
}

static int graph_of(vctx *c, const jv *w, canlin_av_widget *out)
{
    static const char *const nkeys[] = {"id", "title", "x", "y", "inputs", "outputs", "body",
                                        NULL};
    static const char *const ekeys[] = {"from", "to", NULL};
    static const char *const bkeys[] = {"kind", "text", "value", NULL};
    const jv *nodes, *edges, *e;
    canlin_av_node *nv;
    canlin_av_edge *ev;
    unsigned i;
    double h = CANLIN_UI_GRAPH_HEIGHT_EM;
    size_t n;

    if (c->graphs >= CANLIN_UI_GRAPH_MAX_PER_DRAW)
        return fail(c, "too many graphs in one view");
    c->graphs++;
    if (num_of(c, w, "height", 0, 0, CANLIN_UI_GRAPH_HEIGHT_MIN_EM,
               CANLIN_UI_GRAPH_HEIGHT_MAX_EM, &h) < 0)
        return -1;
    out->height = (float)h;
    nodes = arr_of(c, w, "nodes", 1);
    if (nodes == NULL)
        return -1;
    edges = arr_of(c, w, "edges", 1);
    if (edges == NULL)
        return -1;
    if (nodes->n > CANLIN_UI_GRAPH_MAX_NODES) {
        pk(c, "nodes");
        return fail(c, "too many nodes");
    }
    if (edges->n > CANLIN_UI_GRAPH_MAX_EDGES) {
        pk(c, "edges");
        return fail(c, "too many edges");
    }
    nv = aalloc(c->keep, (nodes->n ? nodes->n : 1) * sizeof *nv);
    ev = aalloc(c->keep, (edges->n ? edges->n : 1) * sizeof *ev);
    if (nv == NULL || ev == NULL)
        return fail(c, "view too large");
    memset(nv, 0, (nodes->n ? nodes->n : 1) * sizeof *nv);

    n = pk(c, "nodes");
    for (e = nodes->kid, i = 0; e; e = e->next, i++) {
        canlin_av_node *nd = &nv[i];
        const jv *in, *outs, *body;
        canlin_av_port *ports;
        double x = 0, y = 0;
        size_t m = pi(c, i);
        if (e->t != J_OBJ)
            return fail(c, "must be an object");
        if (only_keys(c, e, nkeys) < 0 || id_of(c, e, "id", 1, &nd->id) < 0)
            return -1;
        if (find_node(nv, i, nd->id.s, nd->id.len) >= 0) {
            pk(c, "id");
            return fail(c, "not unique in the graph");
        }
        if (str_of(c, e, "title", 0, CANLIN_UI_MAX_LABEL, &nd->title) < 0 ||
            num_of(c, e, "x", 1, 0, -CANLIN_UI_GRAPH_MAX_COORD, CANLIN_UI_GRAPH_MAX_COORD, &x) < 0 ||
            num_of(c, e, "y", 1, 0, -CANLIN_UI_GRAPH_MAX_COORD, CANLIN_UI_GRAPH_MAX_COORD, &y) < 0)
            return -1;
        nd->x = (float)x;
        nd->y = (float)y;
        in = get(e, "inputs") ? arr_of(c, e, "inputs", 1) : NULL;
        if (get(e, "inputs") && in == NULL)
            return -1;
        outs = get(e, "outputs") ? arr_of(c, e, "outputs", 1) : NULL;
        if (get(e, "outputs") && outs == NULL)
            return -1;
        nd->nin = in ? in->n : 0;
        nd->nout = outs ? outs->n : 0;
        if (nd->nin + nd->nout > CANLIN_UI_GRAPH_MAX_PORTS)
            return fail(c, "too many ports");
        ports = aalloc(c->keep, (nd->nin + nd->nout + 1) * sizeof *ports);
        if (ports == NULL)
            return fail(c, "view too large");
        if (in) {
            size_t q = pk(c, "inputs");
            if (port_list(c, in, ports, 0) < 0)
                return -1;
            pop(c, q);
        }
        if (outs) {
            size_t q = pk(c, "outputs");
            if (port_list(c, outs, ports, nd->nin) < 0)
                return -1;
            pop(c, q);
        }
        nd->ports = ports;
        body = get(e, "body") ? arr_of(c, e, "body", 1) : NULL;
        if (get(e, "body") && body == NULL)
            return -1;
        if (body) {
            const jv *b;
            unsigned k = 0;
            size_t q = pk(c, "body");
            if (body->n > CANLIN_UI_GRAPH_MAX_BODY)
                return fail(c, "too many body rows");
            for (b = body->kid; b; b = b->next, k++) {
                static const char *const kinds[] = {"text", "meter", NULL};
                canlin_av_body *row = &nd->body[k];
                size_t r = pi(c, k);
                int kind = 0;
                double val = 0;
                row->text.s = EMPTY;
                row->text.len = 0;
                if (b->t != J_OBJ)
                    return fail(c, "must be an object");
                if (only_keys(c, b, bkeys) < 0 || enum_of(c, b, "kind", 1, kinds, &kind) < 0)
                    return -1;
                row->meter = kind == 1;
                if (kind == 0) {
                    if (str_of(c, b, "text", 1, CANLIN_UI_MAX_LABEL, &row->text) < 0)
                        return -1;
                    if (row->text.len == 0) {
                        pk(c, "text");
                        return fail(c, "must not be empty");
                    }
                    if (get(b, "value")) {
                        pk(c, "value");
                        return fail(c, "not allowed for a text row");
                    }
                } else {
                    if (num_of(c, b, "value", 1, 0, 0.0, 1.0, &val) < 0)
                        return -1;
                    if (get(b, "text")) {
                        pk(c, "text");
                        return fail(c, "not allowed for a meter row");
                    }
                    row->value = (float)val;
                }
                pop(c, r);
            }
            nd->nbody = k;
            pop(c, q);
        }
        pop(c, m);
    }
    pop(c, n);

    n = pk(c, "edges");
    for (e = edges->kid, i = 0; e; e = e->next, i++) {
        canlin_av_edge *ed = &ev[i];
        unsigned j;
        size_t m = pi(c, i);
        const canlin_av_port *a, *b;
        if (e->t != J_OBJ)
            return fail(c, "must be an object");
        if (only_keys(c, e, ekeys) < 0 ||
            endpoint(c, e, "from", nv, nodes->n, 1, &ed->fn, &ed->fp) < 0 ||
            endpoint(c, e, "to", nv, nodes->n, 0, &ed->tn, &ed->tp) < 0)
            return -1;
        if (ed->fn == ed->tn)
            return fail(c, "connects a node to itself");
        a = &nv[ed->fn].ports[ed->fp];
        b = &nv[ed->tn].ports[ed->tp];
        if (a->type.len && b->type.len &&
            (a->type.len != b->type.len || memcmp(a->type.s, b->type.s, a->type.len) != 0))
            return fail(c, "connects ports of different types");
        for (j = 0; j < i; j++)
            if (ev[j].fn == ed->fn && ev[j].fp == ed->fp && ev[j].tn == ed->tn &&
                ev[j].tp == ed->tp)
                return fail(c, "duplicate edge");
        pop(c, m);
    }
    pop(c, n);
    out->nodes = nv;
    out->nnodes = nodes->n;
    out->edges = ev;
    out->nedges = edges->n;
    return 0;
}

static int widget_of(vctx *c, const jv *w, unsigned depth, canlin_av_widget *out)
{
    static const char *const K_HEADING[] = {"type", "id", "text", "level", NULL};
    static const char *const K_TEXT[] = {"type", "id", "text", "style", NULL};
    static const char *const K_BUBBLE[] = {"type", "id", "role", "text", NULL};
    static const char *const K_FIELD[] = {"type", "id", "label", "value", "placeholder", "max",
                                          NULL};
    static const char *const K_AREA[] = {"type", "id", "label", "value", "rows", NULL};
    static const char *const K_TOGGLE[] = {"type", "id", "label", "value", NULL};
    static const char *const K_CHOICE[] = {"type", "id", "label", "options", "index", NULL};
    static const char *const K_METER[] = {"type", "id", "label", "value", NULL};
    static const char *const K_SEP[] = {"type", "id", NULL};
    static const char *const K_SECTION[] = {"type", "id", "title", "widgets", "open", NULL};
    static const char *const K_COLUMNS[] = {"type", "id", "left", "right", NULL};
    static const char *const K_GRAPH[] = {"type", "id", "nodes", "edges", "height", NULL};
    static const char *const K_BUTTON[] = {"type", "id", "label", "action", NULL};
    static const char *const K_ICON[] = {"type", "id", "name", NULL};
    static const char *const *const KEYS[] = {K_HEADING, K_TEXT, K_BUBBLE, K_FIELD, K_AREA,
                                              K_TOGGLE, K_CHOICE, K_METER, K_SEP, K_SECTION,
                                              K_COLUMNS, K_GRAPH, K_BUTTON, K_ICON};
    static const char *const STYLES[] = {"normal", "muted", "danger", "success", NULL};
    /* "system" is reserved for trusted content (UI spec §7) */
    static const char *const ROLES[] = {"user", "assistant", NULL};
    static const char *const ACTIONS[] = {"submit", NULL};
    int type = 0, needs_id;
    unsigned i;
    double d;

    if (w->t != J_OBJ)
        return fail(c, "must be an object");
    if (c->total >= CANLIN_UI_TOOL_MAX_WIDGETS) {
        char msg[CANLIN_AV_MSG_MAX];
        snprintf(msg, sizeof msg, "more than %u widgets", (unsigned)CANLIN_UI_TOOL_MAX_WIDGETS);
        return fail(c, msg);
    }
    memset(out, 0, sizeof *out);
    out->slot = c->total++;
    if (enum_of(c, w, "type", 1, TYPES, &type) < 0)
        return -1;
    out->type = (canlin_av_type)(type + 1);
    if (only_keys(c, w, KEYS[type]) < 0)
        return -1;
    needs_id = out->type == CANLIN_AV_FIELD || out->type == CANLIN_AV_AREA ||
               out->type == CANLIN_AV_TOGGLE || out->type == CANLIN_AV_CHOICE ||
               out->type == CANLIN_AV_GRAPH || out->type == CANLIN_AV_BUTTON;
    if (id_of(c, w, "id", needs_id, &out->id) < 0)
        return -1;
    if (out->id.len) {
        for (i = 0; i < c->nids; i++)
            if (c->ids[i].len == out->id.len && memcmp(c->ids[i].s, out->id.s, out->id.len) == 0) {
                pk(c, "id");
                return fail(c, "not unique in the view");
            }
        c->ids[c->nids++] = out->id;
    }
    out->level = 1;
    out->rows = 4;
    out->open = 1;
    out->max = CANLIN_UI_MAX_TEXT;
    out->index = 1;
    out->text.s = out->label.s = out->title.s = out->value.s = out->placeholder.s =
        out->name.s = EMPTY;

    switch (out->type) {
    case CANLIN_AV_HEADING:
        if (str_of(c, w, "text", 1, CANLIN_UI_MAX_TEXT, &out->text) < 0)
            return -1;
        d = 1;
        if (num_of(c, w, "level", 0, 1, 1, 3, &d) < 0)
            return -1;
        out->level = (int)d;
        return 0;
    case CANLIN_AV_TEXT:
        if (str_ml(c, w, "text", 1, CANLIN_UI_MAX_TEXT, 1, &out->text) < 0)
            return -1;
        return enum_of(c, w, "style", 0, STYLES, &out->style);
    case CANLIN_AV_BUBBLE:
        if (enum_of(c, w, "role", 1, ROLES, &out->role) < 0)
            return -1;
        return str_ml(c, w, "text", 1, CANLIN_UI_MAX_TEXT, 1, &out->text);
    case CANLIN_AV_FIELD:
        if (str_of(c, w, "label", 1, CANLIN_UI_MAX_LABEL, &out->label) < 0 ||
            str_of(c, w, "placeholder", 0, CANLIN_UI_MAX_LABEL, &out->placeholder) < 0)
            return -1;
        d = CANLIN_UI_MAX_TEXT;
        if (num_of(c, w, "max", 0, 1, 0, CANLIN_UI_MAX_TEXT, &d) < 0)
            return -1;
        out->max = (size_t)d;
        return str_of(c, w, "value", 0, out->max, &out->value);
    case CANLIN_AV_AREA:
        if (str_of(c, w, "label", 1, CANLIN_UI_MAX_LABEL, &out->label) < 0 ||
            str_ml(c, w, "value", 0, CANLIN_UI_MAX_TEXT, 1, &out->value) < 0)
            return -1;
        d = 4;
        if (num_of(c, w, "rows", 0, 1, 2, 20, &d) < 0)
            return -1;
        out->rows = (int)d;
        return 0;
    case CANLIN_AV_TOGGLE:
        if (str_of(c, w, "label", 1, CANLIN_UI_MAX_LABEL, &out->label) < 0)
            return -1;
        return bool_of(c, w, "value", &out->on);
    case CANLIN_AV_CHOICE: {
        const jv *o, *e;
        canlin_av_str *opts;
        size_t n;
        if (str_of(c, w, "label", 1, CANLIN_UI_MAX_LABEL, &out->label) < 0)
            return -1;
        o = arr_of(c, w, "options", 1);
        if (o == NULL)
            return -1;
        n = pk(c, "options");
        if (o->n < 1 || o->n > CANLIN_UI_MAX_OPTIONS)
            return fail(c, "must hold 1-64 options");
        opts = aalloc(c->keep, o->n * sizeof *opts);
        if (opts == NULL)
            return fail(c, "view too large");
        for (e = o->kid, i = 0; e; e = e->next, i++) {
            size_t m = pi(c, i);
            if (e->t != J_STR)
                return fail(c, "must be a string");
            if (e->len > CANLIN_UI_MAX_LABEL)
                return fail(c, "exceeds 256 bytes");
            if (has_control(e->s, e->len, 0))
                return fail(c, "contains a control character");
            opts[i].s = e->s;
            opts[i].len = e->len;
            pop(c, m);
        }
        pop(c, n);
        out->options = opts;
        out->noptions = o->n;
        d = 1;
        if (num_of(c, w, "index", 0, 1, 1, (double)o->n, &d) < 0)
            return -1;
        out->index = (unsigned)d;
        return 0;
    }
    case CANLIN_AV_METER:
        if (str_of(c, w, "label", 1, CANLIN_UI_MAX_LABEL, &out->label) < 0 ||
            num_of(c, w, "value", 1, 0, 0.0, 1.0, &d) < 0)
            return -1;
        out->meter = (float)d;
        return 0;
    case CANLIN_AV_SEPARATOR:
        return 0;
    case CANLIN_AV_SECTION: {
        const jv *a;
        size_t n;
        if (depth >= CANLIN_UI_MAX_DEPTH)
            return fail(c, "nesting deeper than 4");
        if (str_of(c, w, "title", 1, CANLIN_UI_MAX_LABEL, &out->title) < 0 ||
            bool_of(c, w, "open", &out->open) < 0)
            return -1;
        a = arr_of(c, w, "widgets", 1);
        if (a == NULL)
            return -1;
        n = pk(c, "widgets");
        if (widgets_of(c, a, depth + 1, &out->child, &out->nchild) < 0)
            return -1;
        pop(c, n);
        return 0;
    }
    case CANLIN_AV_COLUMNS: {
        const jv *l, *r;
        size_t n;
        if (depth >= CANLIN_UI_MAX_DEPTH)
            return fail(c, "nesting deeper than 4");
        l = arr_of(c, w, "left", 1);
        if (l == NULL)
            return -1;
        r = arr_of(c, w, "right", 1);
        if (r == NULL)
            return -1;
        n = pk(c, "left");
        if (widgets_of(c, l, depth + 1, &out->child, &out->nchild) < 0)
            return -1;
        pop(c, n);
        n = pk(c, "right");
        if (widgets_of(c, r, depth + 1, &out->child2, &out->nchild2) < 0)
            return -1;
        pop(c, n);
        return 0;
    }
    case CANLIN_AV_GRAPH:
        return graph_of(c, w, out);
    case CANLIN_AV_BUTTON: {
        int action = 0;
        if (str_of(c, w, "label", 1, CANLIN_UI_MAX_LABEL, &out->label) < 0)
            return -1;
        return enum_of(c, w, "action", 1, ACTIONS, &action);
    }
    case CANLIN_AV_ICON: {
        int icon = 0;
        if (ICONS[0] == NULL) {
            canlin_av_str s;
            if (str_of(c, w, "name", 1, CANLIN_UI_MAX_LABEL, &s) < 0)
                return -1;
            pk(c, "name");
            return fail(c, "not a built-in assistant icon");
        }
        return enum_of(c, w, "name", 1, ICONS, &icon);
    }
    }
    return fail(c, "unknown type");
}

static int widgets_of(vctx *c, const jv *arr, unsigned depth, const canlin_av_widget **out,
                      unsigned *nout)
{
    canlin_av_widget *ws;
    const jv *e;
    unsigned i;
    ws = aalloc(c->keep, (arr->n ? arr->n : 1) * sizeof *ws);
    if (ws == NULL)
        return fail(c, "view too large");
    for (e = arr->kid, i = 0; e; e = e->next, i++) {
        size_t n = pi(c, i);
        if (widget_of(c, e, depth, &ws[i]) < 0)
            return -1;
        pop(c, n);
    }
    *out = ws;
    *nout = arr->n;
    return 0;
}

int canlin_av_parser_finish(canlin_av_parser *p, canlin_av_model **out, canlin_av_error *err)
{
    static const char *const TOP[] = {"title", "widgets", NULL};
    holder *h;
    vctx *c;
    const jv *w;
    int rc;
    if (out)
        *out = NULL;
    if (p == NULL || out == NULL || err == NULL)
        return CANLIN_AV_EARG;
    memset(err, 0, sizeof *err);
    if (p->spent)
        return CANLIN_AV_EARG;
    p->spent = 1;
    if (p->status == CANLIN_AV_OK) {
        rc = canlin_json_finish(&p->js);
        if (rc != CANLIN_JSON_OK)
            json_fail(p, rc);
    }
    if (p->status != CANLIN_AV_OK) {
        *err = p->err;
        return p->status;
    }
    c = calloc(1, sizeof *c);
    h = calloc(1, sizeof *h);
    if (c == NULL || h == NULL) {
        free(c);
        free(h);
        set_err(err, CANLIN_AV_ENOMEM, "$", "out of memory");
        return CANLIN_AV_ENOMEM;
    }
    c->keep = &p->keep;
    c->err = err;
    strcpy(c->path, "$");
    rc = -1;
    if (p->root->t != J_OBJ)
        fail(c, "must be an object");
    else if (only_keys(c, p->root, TOP) == 0 &&
             str_of(c, p->root, "title", 0, CANLIN_UI_MAX_LABEL, &h->m.title) == 0 &&
             (w = arr_of(c, p->root, "widgets", 1)) != NULL) {
        size_t n = pk(c, "widgets");
        if (widgets_of(c, w, 0, &h->m.widgets, &h->m.nwidgets) == 0) {
            pop(c, n);
            h->m.total = c->total;
            rc = 0;
        }
    }
    if (h->m.title.s == NULL)
        h->m.title.s = EMPTY;
    free(c);
    if (rc < 0) {
        free(h);
        return err->status ? err->status : CANLIN_AV_EINVALID;
    }
    h->keep = p->keep; /* the model takes the strings and structures */
    p->keep.head = NULL;
    p->keep.total = 0;
    afree(&p->tree);
    *out = &h->m;
    return CANLIN_AV_OK;
}

void canlin_av_model_free(canlin_av_model *m)
{
    holder *h = (holder *)(void *)m;
    if (m == NULL)
        return;
    afree(&h->keep);
    free(h);
}

/* ---- bounded JSON writer (submissions and tool results) ---- */

typedef struct wbuf {
    char *b;
    size_t cap, len;
    int over;
} wbuf;

static void w_raw(wbuf *w, const char *s, size_t n)
{
    if (w->over || w->cap - w->len < n) {
        w->over = 1;
        return;
    }
    memcpy(w->b + w->len, s, n);
    w->len += n;
}

static void w_lit(wbuf *w, const char *s)
{
    w_raw(w, s, strlen(s));
}

/* JSON string; control bytes (a user can type them) as \u00XX. */
static void w_str(wbuf *w, const char *s, size_t n)
{
    static const char hex[] = "0123456789abcdef";
    size_t i, run = 0;
    w_raw(w, "\"", 1);
    for (i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch >= 0x20 && ch != '"' && ch != '\\')
            continue;
        w_raw(w, s + run, i - run);
        run = i + 1;
        if (ch == '"') {
            w_raw(w, "\\\"", 2);
        } else if (ch == '\\') {
            w_raw(w, "\\\\", 2);
        } else {
            char e[6];
            e[0] = '\\';
            e[1] = 'u';
            e[2] = '0';
            e[3] = '0';
            e[4] = hex[ch >> 4];
            e[5] = hex[ch & 15];
            w_raw(w, e, 6);
        }
    }
    w_raw(w, s + run, n - run);
    w_raw(w, "\"", 1);
}

static void w_uint(wbuf *w, unsigned long v)
{
    char t[24];
    int n = snprintf(t, sizeof t, "%lu", v);
    w_raw(w, t, (size_t)n);
}

/* Fixed point with at most two decimals, locale-independent. */
static void w_num(wbuf *w, float x)
{
    long v = lroundf(x * 100.0f), a = v < 0 ? -v : v;
    const char *sg = v < 0 ? "-" : "";
    char t[40];
    int n;
    if (a % 100 == 0)
        n = snprintf(t, sizeof t, "%s%ld", sg, a / 100);
    else if (a % 10 == 0)
        n = snprintf(t, sizeof t, "%s%ld.%ld", sg, a / 100, (a % 100) / 10);
    else
        n = snprintf(t, sizeof t, "%s%ld.%02ld", sg, a / 100, a % 100);
    w_raw(w, t, (size_t)n);
}

size_t canlin_av_result_json(const canlin_av_result *res, char *buf, size_t cap)
{
    wbuf w;
    if (res == NULL || buf == NULL || cap == 0)
        return 0;
    w.b = buf;
    w.cap = cap - 1;
    w.len = 0;
    w.over = 0;
    if (res->status == CANLIN_AV_OK) {
        w_lit(&w, "{\"revision\":");
        w_uint(&w, res->revision);
        w_lit(&w, "}");
    } else {
        const char *code = canlin_av_code(res->status);
        w_lit(&w, "{\"error\":{\"code\":");
        w_str(&w, code, strlen(code));
        w_lit(&w, ",\"path\":");
        w_str(&w, res->err.path, strlen(res->err.path));
        w_lit(&w, ",\"message\":");
        w_str(&w, res->err.message, strlen(res->err.message));
        w_lit(&w, "}}");
    }
    if (w.over)
        return 0;
    buf[w.len] = 0;
    return w.len;
}

/* ---- the view: local state per revision ---- */

typedef struct lnode {
    float x, y;
    unsigned char alive;
} lnode;

typedef struct ledge {
    uint16_t fn, fp, tn, tp;       /* node index, port index within the node */
} ledge;

typedef struct lgraph {
    lnode *nodes;                  /* one per model node */
    ledge *edges;                  /* capacity CANLIN_UI_GRAPH_MAX_EDGES */
    unsigned ne;
    int sel_node, sel_edge;        /* -1 = none */
} lgraph;

typedef struct wstate {
    char *text;                    /* field/area: current text, NUL-terminated */
    size_t len;
    int on;                        /* toggle */
    unsigned index;                /* choice, 1-based */
    lgraph *g;                     /* graph */
} wstate;

typedef struct call_rec {
    char id[CANLIN_AV_CALL_ID_MAX];
    size_t len;
    uint64_t hash;                 /* of the argument bytes */
    canlin_av_result res;
} call_rec;

struct canlin_assistant_view {
    canlin_av_model *model;
    wstate *st;                    /* model->total entries, by widget slot */
    uint32_t rev;
    uint64_t vgen, vturn;          /* scope of the call that set the current revision */
    uint64_t gen, turn;            /* current scope */
    call_rec calls[CANLIN_AV_CALLS_MAX];
    unsigned ncalls;
    canlin_ui_view *view;
    lua_State *L;
    size_t lmem;
    char errmsg[CANLIN_AV_MSG_MAX];
    const canlin_av_widget *submit; /* submit button pressed in the running draw */
    int pending;
    canlin_av_submission sub;
    unsigned refused;
};

static void state_free(wstate *st, unsigned n)
{
    unsigned i;
    if (st == NULL)
        return;
    for (i = 0; i < n; i++) {
        free(st[i].text);
        if (st[i].g) {
            free(st[i].g->nodes);
            free(st[i].g->edges);
            free(st[i].g);
        }
    }
    free(st);
}

static int state_init_list(wstate *st, const canlin_av_widget *w, unsigned n);

static int state_init_one(wstate *st, const canlin_av_widget *w)
{
    wstate *s = &st[w->slot];
    unsigned i;
    switch (w->type) {
    case CANLIN_AV_FIELD:
    case CANLIN_AV_AREA:
        s->text = malloc(w->value.len + 1);
        if (s->text == NULL)
            return -1;
        memcpy(s->text, w->value.s, w->value.len);
        s->text[w->value.len] = 0;
        s->len = w->value.len;
        return 0;
    case CANLIN_AV_TOGGLE:
        s->on = w->on;
        return 0;
    case CANLIN_AV_CHOICE:
        s->index = w->index;
        return 0;
    case CANLIN_AV_GRAPH:
        s->g = calloc(1, sizeof *s->g);
        if (s->g == NULL)
            return -1;
        s->g->nodes = calloc(w->nnodes ? w->nnodes : 1, sizeof *s->g->nodes);
        s->g->edges = calloc(CANLIN_UI_GRAPH_MAX_EDGES, sizeof *s->g->edges);
        if (s->g->nodes == NULL || s->g->edges == NULL)
            return -1;
        for (i = 0; i < w->nnodes; i++) {
            s->g->nodes[i].x = w->nodes[i].x;
            s->g->nodes[i].y = w->nodes[i].y;
            s->g->nodes[i].alive = 1;
        }
        for (i = 0; i < w->nedges; i++) {
            s->g->edges[i].fn = (uint16_t)w->edges[i].fn;
            s->g->edges[i].fp = (uint16_t)w->edges[i].fp;
            s->g->edges[i].tn = (uint16_t)w->edges[i].tn;
            s->g->edges[i].tp = (uint16_t)w->edges[i].tp;
        }
        s->g->ne = w->nedges;
        s->g->sel_node = s->g->sel_edge = -1;
        return 0;
    case CANLIN_AV_SECTION:
    case CANLIN_AV_COLUMNS:
        if (state_init_list(st, w->child, w->nchild) < 0)
            return -1;
        return state_init_list(st, w->child2, w->nchild2);
    default:
        return 0;
    }
}

static int state_init_list(wstate *st, const canlin_av_widget *w, unsigned n)
{
    unsigned i;
    for (i = 0; i < n; i++)
        if (state_init_one(st, &w[i]) < 0)
            return -1;
    return 0;
}

canlin_assistant_view *canlin_assistant_view_new(void)
{
    return calloc(1, sizeof(canlin_assistant_view));
}

void canlin_assistant_view_free(canlin_assistant_view *av)
{
    if (av == NULL)
        return;
    if (av->L)
        lua_close(av->L);
    if (av->model)
        state_free(av->st, av->model->total);
    canlin_av_model_free(av->model);
    canlin_av_submission_free(&av->sub);
    free(av);
}

void canlin_assistant_view_set_scope(canlin_assistant_view *av, uint64_t gen, uint64_t turn)
{
    if (av == NULL)
        return;
    if (av->gen != gen || av->turn != turn)
        av->ncalls = 0; /* results of the old scope are unreachable: its calls are stale */
    av->gen = gen;
    av->turn = turn;
}

static uint64_t fnv64(const void *p, size_t n)
{
    const unsigned char *s = p;
    uint64_t h = 14695981039346656037ull;
    size_t i;
    for (i = 0; i < n; i++) {
        h ^= s[i];
        h *= 1099511628211ull;
    }
    return h;
}

static int result_err(canlin_av_result *res, int status, const char *path, const char *msg)
{
    memset(res, 0, sizeof *res);
    res->status = status;
    set_err(&res->err, status, path, msg);
    return status;
}

/* Validates the complete argument into a model (one feed of the push parser). */
static int validate(const void *json, size_t len, canlin_av_model **m, canlin_av_error *err)
{
    canlin_av_parser *p;
    int rc;
    memset(err, 0, sizeof *err);
    if (len > CANLIN_UI_TOOL_MAX_BYTES) {
        char msg[CANLIN_AV_MSG_MAX];
        snprintf(msg, sizeof msg, "argument exceeds %u bytes", (unsigned)CANLIN_UI_TOOL_MAX_BYTES);
        set_err(err, CANLIN_AV_EINVALID, "$", msg);
        return CANLIN_AV_EINVALID;
    }
    p = canlin_av_parser_new();
    if (p == NULL) {
        set_err(err, CANLIN_AV_ENOMEM, "$", "out of memory");
        return CANLIN_AV_ENOMEM;
    }
    canlin_av_parser_feed(p, json ? json : "", len);
    rc = canlin_av_parser_finish(p, m, err);
    canlin_av_parser_free(p);
    return rc;
}

int canlin_assistant_view_set_view(canlin_assistant_view *av, const canlin_av_call *call,
                                   const void *json, size_t len, canlin_av_result *res)
{
    canlin_av_result r;
    canlin_av_model *m = NULL;
    wstate *st = NULL;
    call_rec *rec;
    uint64_t h;
    unsigned i;
    int rc;

    if (res == NULL)
        return CANLIN_AV_EARG;
    if (av == NULL || call == NULL || (json == NULL && len) || (call->id == NULL && call->id_len))
        return result_err(res, CANLIN_AV_EARG, "", "bad arguments");
    if (av->turn == 0 || call->gen != av->gen || call->turn != av->turn)
        return result_err(res, CANLIN_AV_ESTALE, "", "call outside the current turn");
    if (call->id_len < 1 || call->id_len > CANLIN_AV_CALL_ID_MAX)
        return result_err(res, CANLIN_AV_EINVALID, "", "tool-call id must be 1-128 bytes");
    h = fnv64(json ? json : "", len);
    for (i = 0; i < av->ncalls; i++) {
        rec = &av->calls[i];
        if (rec->len == call->id_len && memcmp(rec->id, call->id, call->id_len) == 0) {
            if (rec->hash != h)
                return result_err(res, CANLIN_AV_EINVALID, "",
                                  "tool-call id reused with other arguments");
            *res = rec->res;
            res->duplicate = 1;
            return res->status;
        }
    }
    if (av->ncalls >= CANLIN_AV_CALLS_MAX)
        return result_err(res, CANLIN_AV_EFULL, "", "too many tool calls in this turn");

    memset(&r, 0, sizeof r);
    rc = validate(json, len, &m, &r.err);
    if (rc == CANLIN_AV_OK) {
        st = calloc(m->total ? m->total : 1, sizeof *st);
        if (st == NULL || state_init_list(st, m->widgets, m->nwidgets) < 0) {
            state_free(st, m->total);
            canlin_av_model_free(m);
            m = NULL;
            rc = CANLIN_AV_ENOMEM;
            set_err(&r.err, rc, "$", "out of memory");
        }
    }
    r.status = rc;
    if (rc == CANLIN_AV_OK) {
        /* Atomic replacement: nothing above touched the current view. */
        if (av->model)
            state_free(av->st, av->model->total);
        canlin_av_model_free(av->model);
        av->model = m;
        av->st = st;
        av->rev++;
        av->vgen = call->gen;
        av->vturn = call->turn;
        if (av->view)
            canlin_ui_view_input_reset(av->view); /* queued input was for the old view */
        r.revision = av->rev;
        memset(&r.err, 0, sizeof r.err);
    }
    rec = &av->calls[av->ncalls++];
    memcpy(rec->id, call->id, call->id_len);
    rec->len = call->id_len;
    rec->hash = h;
    rec->res = r;
    *res = r;
    return rc;
}

const canlin_av_model *canlin_assistant_view_model(const canlin_assistant_view *av)
{
    return av ? av->model : NULL;
}

uint32_t canlin_assistant_view_revision(const canlin_assistant_view *av)
{
    return av ? av->rev : 0;
}

void canlin_assistant_view_attach(canlin_assistant_view *av, canlin_ui_view *v)
{
    if (av)
        av->view = v;
}

int canlin_assistant_view_input(canlin_assistant_view *av, uint32_t revision,
                                const canlin_ui_input *in)
{
    int rc;
    if (av == NULL || av->view == NULL || in == NULL)
        return CANLIN_AV_EARG;
    if (av->rev == 0 || revision != av->rev)
        return CANLIN_AV_ESTALE;
    rc = canlin_ui_view_input(av->view, in);
    return rc == CANLIN_UI_OK ? CANLIN_AV_OK : rc == CANLIN_UI_EFULL ? CANLIN_AV_EFULL
                                                                     : CANLIN_AV_EINVALID;
}

/* ---- submit ---- */

static void graph_value(wbuf *w, const canlin_av_widget *gw, const lgraph *g)
{
    unsigned i, k;
    int first = 1;
    w_lit(w, "{\"nodes\":[");
    for (i = 0; i < gw->nnodes; i++) {
        const canlin_av_node *nd = &gw->nodes[i];
        if (!g->nodes[i].alive)
            continue;
        if (!first)
            w_lit(w, ",");
        first = 0;
        w_lit(w, "{\"id\":");
        w_str(w, nd->id.s, nd->id.len);
        if (nd->title.len) {
            w_lit(w, ",\"title\":");
            w_str(w, nd->title.s, nd->title.len);
        }
        w_lit(w, ",\"x\":");
        w_num(w, g->nodes[i].x);
        w_lit(w, ",\"y\":");
        w_num(w, g->nodes[i].y);
        for (k = 0; k < 2; k++) {
            unsigned q, a = k ? nd->nin : 0, b = k ? nd->nin + nd->nout : nd->nin;
            w_lit(w, k ? ",\"outputs\":[" : ",\"inputs\":[");
            for (q = a; q < b; q++) {
                const canlin_av_port *p = &nd->ports[q];
                if (q > a)
                    w_lit(w, ",");
                w_lit(w, "{\"id\":");
                w_str(w, p->id.s, p->id.len);
                if (p->label.len) {
                    w_lit(w, ",\"label\":");
                    w_str(w, p->label.s, p->label.len);
                }
                if (p->type.len) {
                    w_lit(w, ",\"type\":");
                    w_str(w, p->type.s, p->type.len);
                }
                w_lit(w, "}");
            }
            w_lit(w, "]");
        }
        w_lit(w, "}");
    }
    w_lit(w, "],\"edges\":[");
    for (i = 0; i < g->ne; i++) {
        const ledge *e = &g->edges[i];
        const canlin_av_node *a = &gw->nodes[e->fn], *b = &gw->nodes[e->tn];
        if (i)
            w_lit(w, ",");
        w_lit(w, "{\"from\":[");
        w_str(w, a->id.s, a->id.len);
        w_lit(w, ",");
        w_str(w, a->ports[e->fp].id.s, a->ports[e->fp].id.len);
        w_lit(w, "],\"to\":[");
        w_str(w, b->id.s, b->id.len);
        w_lit(w, ",");
        w_str(w, b->ports[e->tp].id.s, b->ports[e->tp].id.len);
        w_lit(w, "]}");
    }
    w_lit(w, "]}");
}

/* Values of the input widgets only, in document order. */
static void values_of(const canlin_assistant_view *av, wbuf *w, const canlin_av_widget *ws,
                      unsigned n, int *first)
{
    unsigned i;
    for (i = 0; i < n; i++) {
        const canlin_av_widget *x = &ws[i];
        const wstate *s = &av->st[x->slot];
        if (x->type == CANLIN_AV_SECTION || x->type == CANLIN_AV_COLUMNS) {
            values_of(av, w, x->child, x->nchild, first);
            values_of(av, w, x->child2, x->nchild2, first);
            continue;
        }
        if (x->type != CANLIN_AV_FIELD && x->type != CANLIN_AV_AREA &&
            x->type != CANLIN_AV_TOGGLE && x->type != CANLIN_AV_CHOICE &&
            x->type != CANLIN_AV_GRAPH)
            continue;
        if (!*first)
            w_lit(w, ",");
        *first = 0;
        w_str(w, x->id.s, x->id.len);
        w_lit(w, ":");
        switch (x->type) {
        case CANLIN_AV_FIELD:
        case CANLIN_AV_AREA:
            w_str(w, s->text, s->len);
            break;
        case CANLIN_AV_TOGGLE:
            w_lit(w, s->on ? "true" : "false");
            break;
        case CANLIN_AV_CHOICE:
            w_uint(w, s->index);
            break;
        default:
            graph_value(w, x, s->g);
            break;
        }
    }
}

/* After a complete draw in which a submit button was pressed. Touches no Lua state. */
static void finish_submit(canlin_assistant_view *av)
{
    const canlin_av_widget *b = av->submit;
    wbuf w;
    int first = 1;
    av->submit = NULL;
    if (b == NULL)
        return;
    if (av->pending) {
        av->refused++; /* one submission per press; the earlier one was not taken yet */
        return;
    }
    w.b = malloc(CANLIN_AV_SUBMIT_MAX);
    if (w.b == NULL) {
        av->refused++;
        return;
    }
    w.cap = CANLIN_AV_SUBMIT_MAX - 1;
    w.len = 0;
    w.over = 0;
    w_lit(&w, "{\"revision\":");
    w_uint(&w, av->rev);
    w_lit(&w, ",\"action\":");
    w_str(&w, b->id.s, b->id.len);
    w_lit(&w, ",\"values\":{");
    values_of(av, &w, av->model->widgets, av->model->nwidgets, &first);
    w_lit(&w, "}}");
    if (w.over) {
        free(w.b);
        av->refused++;
        return;
    }
    {
        char *shrunk = realloc(w.b, w.len + 1);
        if (shrunk)
            w.b = shrunk;
    }
    w.b[w.len] = 0;
    memset(&av->sub, 0, sizeof av->sub);
    av->sub.revision = av->rev;
    av->sub.gen = av->vgen;
    av->sub.turn = av->vturn;
    memcpy(av->sub.action, b->id.s, b->id.len);
    av->sub.action[b->id.len] = 0;
    av->sub.json = w.b;
    av->sub.len = w.len;
    av->pending = 1;
}

int canlin_assistant_view_pending(const canlin_assistant_view *av)
{
    return av ? av->pending : 0;
}

int canlin_assistant_view_take(canlin_assistant_view *av, canlin_av_submission *out)
{
    if (av == NULL || out == NULL || !av->pending)
        return 0;
    *out = av->sub;
    memset(&av->sub, 0, sizeof av->sub);
    av->pending = 0;
    return 1;
}

void canlin_av_submission_free(canlin_av_submission *s)
{
    if (s == NULL)
        return;
    free(s->json);
    s->json = NULL;
    s->len = 0;
}

unsigned canlin_assistant_view_submit_refused(const canlin_assistant_view *av)
{
    return av ? av->refused : 0;
}

/* ---- rendering through the ui object ---- */

static const char *const STYLE_NAMES[] = {"normal", "muted", "danger", "success"};
static const char *const ROLE_NAMES[] = {"user", "assistant"};

static void draw_list(canlin_assistant_view *av, lua_State *L, int ui,
                      const canlin_av_widget *ws, unsigned n);

/* Pushes ui.name and ui (method call prologue). */
static void method(lua_State *L, int ui, const char *name)
{
    lua_getfield(L, ui, name);
    lua_pushvalue(L, ui);
}

/* opts table with the revision-scoped identity "r<rev>:<id>" (or "r<rev>:#<slot>"). */
static void opts_id(lua_State *L, const canlin_assistant_view *av, const canlin_av_widget *w)
{
    char id[64];
    if (w->id.len)
        snprintf(id, sizeof id, "r%lu:%s", (unsigned long)av->rev, w->id.s);
    else
        snprintf(id, sizeof id, "r%lu:#%u", (unsigned long)av->rev, w->slot);
    lua_createtable(L, 0, 4);
    lua_pushstring(L, id);
    lua_setfield(L, -2, "id");
}

/* Section/columns body: upvalues av, widgets, count, ui. */
static int list_fn(lua_State *L)
{
    canlin_assistant_view *av = lua_touserdata(L, lua_upvalueindex(1));
    const canlin_av_widget *ws = lua_touserdata(L, lua_upvalueindex(2));
    unsigned n = (unsigned)lua_tointeger(L, lua_upvalueindex(3));
    lua_pushvalue(L, lua_upvalueindex(4));
    draw_list(av, L, lua_gettop(L), ws, n);
    return 0;
}

static void push_list_fn(lua_State *L, canlin_assistant_view *av, int ui,
                         const canlin_av_widget *ws, unsigned n)
{
    lua_pushlightuserdata(L, av);
    lua_pushlightuserdata(L, (void *)ws);
    lua_pushinteger(L, (lua_Integer)n);
    lua_pushvalue(L, ui);
    lua_pushcclosure(L, list_fn, 4);
}

static void setstr(lua_State *L, const char *k, const canlin_av_str *s)
{
    lua_pushlstring(L, s->s, s->len);
    lua_setfield(L, -2, k);
}

/* The graph table of spec §5.1 from the local state (alive nodes, local edges). */
static void push_graph(lua_State *L, const canlin_av_widget *gw, const lgraph *g)
{
    unsigned i, k, q, row = 0;
    lua_createtable(L, 0, 3);
    lua_createtable(L, (int)gw->nnodes, 0);
    for (i = 0; i < gw->nnodes; i++) {
        const canlin_av_node *nd = &gw->nodes[i];
        if (!g->nodes[i].alive)
            continue;
        lua_createtable(L, 0, 7);
        setstr(L, "id", &nd->id);
        if (nd->title.len)
            setstr(L, "title", &nd->title);
        lua_pushnumber(L, g->nodes[i].x);
        lua_setfield(L, -2, "x");
        lua_pushnumber(L, g->nodes[i].y);
        lua_setfield(L, -2, "y");
        for (k = 0; k < 2; k++) {
            unsigned a = k ? nd->nin : 0, b = k ? nd->nin + nd->nout : nd->nin;
            lua_createtable(L, (int)(b - a), 0);
            for (q = a; q < b; q++) {
                const canlin_av_port *p = &nd->ports[q];
                lua_createtable(L, 0, 3);
                setstr(L, "id", &p->id);
                if (p->label.len)
                    setstr(L, "label", &p->label);
                if (p->type.len)
                    setstr(L, "type", &p->type);
                lua_rawseti(L, -2, (lua_Integer)(q - a + 1));
            }
            lua_setfield(L, -2, k ? "outputs" : "inputs");
        }
        if (nd->nbody) {
            lua_createtable(L, (int)nd->nbody, 0);
            for (k = 0; k < nd->nbody; k++) {
                lua_createtable(L, 0, 2);
                if (nd->body[k].meter) {
                    lua_pushliteral(L, "meter");
                    lua_setfield(L, -2, "kind");
                    lua_pushnumber(L, nd->body[k].value);
                    lua_setfield(L, -2, "value");
                } else {
                    lua_pushliteral(L, "text");
                    lua_setfield(L, -2, "kind");
                    setstr(L, "text", &nd->body[k].text);
                }
                lua_rawseti(L, -2, (lua_Integer)k + 1);
            }
            lua_setfield(L, -2, "body");
        }
        lua_rawseti(L, -2, (lua_Integer)++row);
    }
    lua_setfield(L, -2, "nodes");
    lua_createtable(L, (int)g->ne, 0);
    for (i = 0; i < g->ne; i++) {
        const ledge *e = &g->edges[i];
        const canlin_av_node *a = &gw->nodes[e->fn], *b = &gw->nodes[e->tn];
        lua_createtable(L, 0, 2);
        lua_createtable(L, 2, 0);
        lua_pushlstring(L, a->id.s, a->id.len);
        lua_rawseti(L, -2, 1);
        lua_pushlstring(L, a->ports[e->fp].id.s, a->ports[e->fp].id.len);
        lua_rawseti(L, -2, 2);
        lua_setfield(L, -2, "from");
        lua_createtable(L, 2, 0);
        lua_pushlstring(L, b->id.s, b->id.len);
        lua_rawseti(L, -2, 1);
        lua_pushlstring(L, b->ports[e->tp].id.s, b->ports[e->tp].id.len);
        lua_rawseti(L, -2, 2);
        lua_setfield(L, -2, "to");
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }
    lua_setfield(L, -2, "edges");
    if (g->sel_node >= 0) {
        setstr(L, "selected", &gw->nodes[g->sel_node].id);
    } else if (g->sel_edge >= 0) {
        lua_pushinteger(L, (lua_Integer)g->sel_edge + 1);
        lua_setfield(L, -2, "selected");
    }
}

static int node_of(const canlin_av_widget *gw, const lgraph *g, const char *s, size_t l)
{
    unsigned i;
    for (i = 0; i < gw->nnodes; i++)
        if (g->nodes[i].alive && gw->nodes[i].id.len == l && memcmp(gw->nodes[i].id.s, s, l) == 0)
            return (int)i;
    return -1;
}

/* String field k of the table at idx (raw), or NULL. */
static const char *rawstr(lua_State *L, int idx, const char *k, size_t *l)
{
    const char *s = NULL;
    lua_pushstring(L, k);
    if (lua_rawget(L, idx) == LUA_TSTRING)
        s = lua_tolstring(L, -1, l);
    lua_pop(L, 1); /* the edits table anchors the string */
    return s;
}

/* {node, port} at field k of the edit at idx; port of direction out. */
static int rawend(lua_State *L, int idx, const char *k, const canlin_av_widget *gw,
                  const lgraph *g, int out, unsigned *ni, unsigned *pj)
{
    const char *ns, *ps;
    size_t nl = 0, pl = 0;
    int node, t;
    unsigned q;
    lua_pushstring(L, k);
    if (lua_rawget(L, idx) != LUA_TTABLE) {
        lua_pop(L, 1);
        return -1;
    }
    t = lua_gettop(L);
    lua_rawgeti(L, t, 1);
    lua_rawgeti(L, t, 2);
    ns = lua_type(L, -2) == LUA_TSTRING ? lua_tolstring(L, -2, &nl) : NULL;
    ps = lua_type(L, -1) == LUA_TSTRING ? lua_tolstring(L, -1, &pl) : NULL;
    node = ns ? node_of(gw, g, ns, nl) : -1;
    if (node >= 0 && ps) {
        const canlin_av_node *nd = &gw->nodes[node];
        for (q = 0; q < nd->nin + nd->nout; q++)
            if ((q >= nd->nin) == out && nd->ports[q].id.len == pl &&
                memcmp(nd->ports[q].id.s, ps, pl) == 0) {
                *ni = (unsigned)node;
                *pj = q;
                lua_settop(L, t - 1);
                return 0;
            }
    }
    lua_settop(L, t - 1);
    return -1;
}

static float clampc(lua_Number x)
{
    if (!(x >= -CANLIN_UI_GRAPH_MAX_COORD))
        return -CANLIN_UI_GRAPH_MAX_COORD;
    if (x > CANLIN_UI_GRAPH_MAX_COORD)
        return CANLIN_UI_GRAPH_MAX_COORD;
    return (float)x;
}

/* The assistant owns this graph's data (UI spec §7): apply the edits returned by
 * ui:graph (table at idx). Edge indices refer to the edges pushed this draw. */
static void apply_edits(lua_State *L, int idx, const canlin_av_widget *gw, lgraph *g)
{
    unsigned char rm[CANLIN_UI_GRAPH_MAX_EDGES];
    int remap[CANLIN_UI_GRAPH_MAX_EDGES];
    ledge conn[8];
    unsigned nconn = 0, i, j, ne;
    int sel_set = 0, sel_node = -1, sel_edge = -1;
    lua_Integer n, k;
    if (!lua_istable(L, idx))
        return;
    memset(rm, 0, sizeof rm);
    n = (lua_Integer)lua_rawlen(L, idx);
    for (k = 1; k <= n; k++) {
        const char *kind, *s;
        size_t kl, sl;
        int e;
        if (lua_rawgeti(L, idx, k) != LUA_TTABLE) {
            lua_pop(L, 1);
            continue;
        }
        e = lua_gettop(L);
        kind = rawstr(L, e, "kind", &kl);
        s = rawstr(L, e, "node", &sl);
        if (kind == NULL) {
            lua_pop(L, 1);
            continue;
        }
        if (strcmp(kind, "move") == 0 && s) {
            int ni = node_of(gw, g, s, sl);
            if (ni >= 0) {
                lua_getfield(L, e, "x");
                lua_getfield(L, e, "y");
                if (lua_isnumber(L, -2) && lua_isnumber(L, -1)) {
                    g->nodes[ni].x = clampc(lua_tonumber(L, -2));
                    g->nodes[ni].y = clampc(lua_tonumber(L, -1));
                }
                lua_pop(L, 2);
            }
        } else if (strcmp(kind, "connect") == 0 && nconn < 8) {
            unsigned fn, fp, tn, tp;
            if (rawend(L, e, "from", gw, g, 1, &fn, &fp) == 0 &&
                rawend(L, e, "to", gw, g, 0, &tn, &tp) == 0) {
                conn[nconn].fn = (uint16_t)fn;
                conn[nconn].fp = (uint16_t)fp;
                conn[nconn].tn = (uint16_t)tn;
                conn[nconn].tp = (uint16_t)tp;
                nconn++;
            }
        } else if (strcmp(kind, "disconnect") == 0) {
            lua_getfield(L, e, "edge");
            if (lua_isinteger(L, -1)) {
                lua_Integer x = lua_tointeger(L, -1);
                if (x >= 1 && (lua_Unsigned)x <= g->ne)
                    rm[x - 1] = 1;
            }
            lua_pop(L, 1);
        } else if (strcmp(kind, "select") == 0) {
            sel_set = 1;
            sel_node = s ? node_of(gw, g, s, sl) : -1;
            sel_edge = -1;
            lua_getfield(L, e, "edge");
            if (lua_isinteger(L, -1)) {
                lua_Integer x = lua_tointeger(L, -1);
                if (x >= 1 && (lua_Unsigned)x <= g->ne)
                    sel_edge = (int)x - 1;
            }
            lua_pop(L, 1);
        } else if (strcmp(kind, "delete") == 0 && s) {
            int ni = node_of(gw, g, s, sl);
            if (ni >= 0) {
                g->nodes[ni].alive = 0;
                if (g->sel_node == ni)
                    g->sel_node = -1;
                if (sel_node == ni)
                    sel_node = -1;
            }
        } /* add, open: the model view has no node picker or detail view in v1 */
        lua_pop(L, 1);
    }
    /* Compact: drop disconnected edges and edges of deleted nodes, keep the order. */
    for (i = 0, ne = 0; i < g->ne; i++) {
        const ledge *x = &g->edges[i];
        if (rm[i] || !g->nodes[x->fn].alive || !g->nodes[x->tn].alive) {
            remap[i] = -1;
            continue;
        }
        remap[i] = (int)ne;
        g->edges[ne++] = *x;
    }
    if (g->sel_edge >= 0)
        g->sel_edge = (unsigned)g->sel_edge < g->ne ? remap[g->sel_edge] : -1;
    if (sel_edge >= 0)
        sel_edge = (unsigned)sel_edge < g->ne ? remap[sel_edge] : -1;
    g->ne = ne;
    for (i = 0; i < nconn; i++) {
        const ledge *c = &conn[i];
        int dup = 0;
        if (c->fn == c->tn || g->ne >= CANLIN_UI_GRAPH_MAX_EDGES)
            continue;
        for (j = 0; j < g->ne; j++)
            if (memcmp(&g->edges[j], c, sizeof *c) == 0)
                dup = 1;
        if (!dup)
            g->edges[g->ne++] = *c;
    }
    if (sel_set) {
        g->sel_node = sel_node;
        g->sel_edge = sel_node >= 0 ? -1 : sel_edge;
    }
}

static void draw_one(canlin_assistant_view *av, lua_State *L, int ui, const canlin_av_widget *w)
{
    wstate *s = &av->st[w->slot];
    switch (w->type) {
    case CANLIN_AV_HEADING:
        method(L, ui, "heading");
        lua_pushlstring(L, w->text.s, w->text.len);
        lua_pushinteger(L, w->level);
        lua_call(L, 3, 0);
        break;
    case CANLIN_AV_TEXT:
        method(L, ui, "text");
        lua_pushlstring(L, w->text.s, w->text.len);
        lua_createtable(L, 0, 1);
        lua_pushstring(L, STYLE_NAMES[w->style]);
        lua_setfield(L, -2, "style");
        lua_call(L, 3, 0);
        break;
    case CANLIN_AV_BUBBLE:
        method(L, ui, "bubble");
        lua_pushstring(L, ROLE_NAMES[w->role]);
        lua_pushlstring(L, w->text.s, w->text.len);
        lua_call(L, 3, 0);
        break;
    case CANLIN_AV_FIELD:
    case CANLIN_AV_AREA: {
        const char *nv;
        size_t nl;
        method(L, ui, w->type == CANLIN_AV_FIELD ? "field" : "area");
        lua_pushlstring(L, w->label.s, w->label.len);
        lua_pushlstring(L, s->text, s->len);
        opts_id(L, av, w);
        if (w->type == CANLIN_AV_FIELD) {
            if (w->placeholder.len)
                setstr(L, "placeholder", &w->placeholder);
            lua_pushinteger(L, (lua_Integer)w->max);
            lua_setfield(L, -2, "max");
        } else {
            lua_pushinteger(L, w->rows);
            lua_setfield(L, -2, "rows");
        }
        lua_call(L, 4, 1);
        if (lua_type(L, -1) == LUA_TSTRING) {
            nv = lua_tolstring(L, -1, &nl);
            if (nl <= w->max && (nl != s->len || memcmp(nv, s->text, nl) != 0)) {
                char *t = malloc(nl + 1);
                if (t) { /* no Lua runs between here and the pop: nv stays valid */
                    memcpy(t, nv, nl);
                    t[nl] = 0;
                    free(s->text);
                    s->text = t;
                    s->len = nl;
                }
            }
        }
        lua_pop(L, 1);
        break;
    }
    case CANLIN_AV_TOGGLE:
        method(L, ui, "toggle");
        lua_pushlstring(L, w->label.s, w->label.len);
        lua_pushboolean(L, s->on);
        opts_id(L, av, w);
        lua_call(L, 4, 1);
        s->on = lua_toboolean(L, -1);
        lua_pop(L, 1);
        break;
    case CANLIN_AV_CHOICE: {
        unsigned i;
        method(L, ui, "choice");
        lua_pushlstring(L, w->label.s, w->label.len);
        lua_createtable(L, (int)w->noptions, 0);
        for (i = 0; i < w->noptions; i++) {
            lua_pushlstring(L, w->options[i].s, w->options[i].len);
            lua_rawseti(L, -2, (lua_Integer)i + 1);
        }
        lua_pushinteger(L, (lua_Integer)s->index);
        opts_id(L, av, w);
        lua_call(L, 5, 1);
        if (lua_isinteger(L, -1)) {
            lua_Integer x = lua_tointeger(L, -1);
            if (x >= 1 && (lua_Unsigned)x <= w->noptions)
                s->index = (unsigned)x;
        }
        lua_pop(L, 1);
        break;
    }
    case CANLIN_AV_METER:
        method(L, ui, "meter");
        lua_pushlstring(L, w->label.s, w->label.len);
        lua_pushnumber(L, w->meter);
        lua_call(L, 3, 0);
        break;
    case CANLIN_AV_SEPARATOR:
        method(L, ui, "separator");
        lua_call(L, 1, 0);
        break;
    case CANLIN_AV_SECTION:
        method(L, ui, "section");
        lua_pushlstring(L, w->title.s, w->title.len);
        push_list_fn(L, av, ui, w->child, w->nchild);
        opts_id(L, av, w);
        lua_pushboolean(L, w->open);
        lua_setfield(L, -2, "open");
        lua_call(L, 4, 0);
        break;
    case CANLIN_AV_COLUMNS:
        method(L, ui, "columns");
        push_list_fn(L, av, ui, w->child, w->nchild);
        push_list_fn(L, av, ui, w->child2, w->nchild2);
        lua_call(L, 3, 0);
        break;
    case CANLIN_AV_GRAPH:
        method(L, ui, "graph");
        push_graph(L, w, s->g);
        opts_id(L, av, w);
        lua_pushnumber(L, w->height);
        lua_setfield(L, -2, "height");
        lua_call(L, 3, 1);
        apply_edits(L, lua_gettop(L), w, s->g);
        lua_pop(L, 1);
        break;
    case CANLIN_AV_BUTTON:
        method(L, ui, "button");
        lua_pushlstring(L, w->label.s, w->label.len);
        opts_id(L, av, w);
        lua_call(L, 3, 1);
        if (lua_toboolean(L, -1) && av->submit == NULL)
            av->submit = w; /* the only action in v1 is submit */
        lua_pop(L, 1);
        break;
    case CANLIN_AV_ICON:
        break; /* no built-in icon exists yet; the validator admits none */
    }
}

static void draw_list(canlin_assistant_view *av, lua_State *L, int ui,
                      const canlin_av_widget *ws, unsigned n)
{
    unsigned i;
    if (!lua_checkstack(L, 24)) {
        lua_pushliteral(L, "assistant view: Lua stack exhausted");
        lua_error(L);
    }
    for (i = 0; i < n; i++)
        draw_one(av, L, ui, &ws[i]);
}

void canlin_assistant_view_draw(canlin_assistant_view *av, lua_State *L, int ui_idx)
{
    if (av == NULL || L == NULL)
        return;
    ui_idx = lua_absindex(L, ui_idx);
    av->submit = NULL;
    if (av->model)
        draw_list(av, L, ui_idx, av->model->widgets, av->model->nwidgets);
    finish_submit(av);
}

/* ---- C-driven frames on a private Lua state ---- */

static void *lalloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
    canlin_assistant_view *av = ud;
    void *np;
    if (ptr == NULL)
        osize = 0;
    if (nsize == 0) {
        free(ptr);
        av->lmem -= osize;
        return NULL;
    }
    if (nsize > osize && av->lmem + (nsize - osize) > CANLIN_AV_LUA_MEM)
        return NULL;
    np = realloc(ptr, nsize);
    if (np == NULL)
        return NULL;
    av->lmem = av->lmem - osize + nsize;
    return np;
}

static int frame_entry(lua_State *L)
{
    canlin_assistant_view *av = lua_touserdata(L, 1);
    canlin_ui_push(L, av->view);
    av->submit = NULL;
    if (av->model)
        draw_list(av, L, lua_gettop(L), av->model->widgets, av->model->nwidgets);
    return 0;
}

int canlin_assistant_view_frame(canlin_assistant_view *av)
{
    int rc, st;
    if (av == NULL || av->view == NULL)
        return CANLIN_AV_EARG;
    if (av->L == NULL) {
        av->L = lua_newstate(lalloc, av);
        if (av->L == NULL) {
            snprintf(av->errmsg, sizeof av->errmsg, "no Lua state");
            return CANLIN_AV_ENOMEM;
        }
    }
    rc = canlin_ui_view_frame_begin(av->view);
    if (rc != CANLIN_UI_OK) {
        snprintf(av->errmsg, sizeof av->errmsg, "frame could not start (%d)", rc);
        return rc == CANLIN_UI_EBUSY ? CANLIN_AV_EARG : CANLIN_AV_ENOMEM;
    }
    lua_settop(av->L, 0);
    lua_pushcfunction(av->L, frame_entry);
    lua_pushlightuserdata(av->L, av);
    st = lua_pcall(av->L, 1, 0, 0);
    canlin_ui_view_frame_end(av->view);
    if (st != LUA_OK) {
        const char *m = lua_tostring(av->L, -1);
        snprintf(av->errmsg, sizeof av->errmsg, "%s", m ? m : "error");
        lua_settop(av->L, 0);
        av->submit = NULL;
        return st == LUA_ERRMEM ? CANLIN_AV_ENOMEM : CANLIN_AV_EINVALID;
    }
    lua_settop(av->L, 0);
    finish_submit(av);
    return CANLIN_AV_OK;
}

const char *canlin_assistant_view_errmsg(const canlin_assistant_view *av)
{
    return av ? av->errmsg : "";
}
