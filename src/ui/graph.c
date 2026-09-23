/* CanLin graph editor widget, ui:graph (UI spec §5). See src/ui/graph.h for the seam.
 *
 * Controlled widget: the module passes g = {nodes, edges, selected} on every draw; this
 * file never writes into it. It validates g (raw table reads, so no metamethod runs and
 * every borrowed string stays anchored in g), keeps pan/drag state per graph identity,
 * draws with Nuklear command-buffer primitives (no Nuklear widgets, so a Lua error can
 * never leave a Nuklear widget half-open) and returns the user's actions as edits.
 *
 * Geometry, graph space in em (x right, y down), node origin = its top-left corner:
 *   width  CANLIN_UI_GRAPH_NODE_W_EM
 *   title  CANLIN_UI_GRAPH_TITLE_EM
 *   rows   max(#inputs, #outputs) x CANLIN_UI_MIN_TOUCH_EM; input i on the left edge and
 *          output i on the right edge of row i, so every port gets a 2.5 em square touch
 *          target centred on its dot without overlapping its neighbours
 *   body   #body x CANLIN_UI_GRAPH_BODY_EM, then 0.5 em padding
 * Screen px = canvas origin + (graph em + pan em) x view em. No zoom (spec §5.3).
 *
 * Interaction (one primary pointer; time from canlin_ui_input.t, see ui.h):
 *   press on a port      drag to a port of the other direction -> connect on release;
 *                        from an input that has an edge: the edge's output end is taken
 *                        along -> disconnect (+ connect if dropped on another input)
 *   press on a node      drag -> ONE move edit on release (the drag is drawn as a preview;
 *                        per-frame edits would make every module apply dozens of moves and
 *                        fight rounding; release matches "local until committed")
 *   press elsewhere      drag pans (core state); press on an edge then drag also pans
 *   tap (moved < slop)   node/port -> select, second tap on it within DOUBLE_TAP_MS or a
 *                        press held >= LONG_PRESS_MS -> open; edge -> select edge; empty
 *                        -> select none, or add at that point when held >= LONG_PRESS_MS
 *   keys (graph focused by a press inside it): Del -> delete / disconnect the selection,
 *                        Tab / Shift-Tab -> select next / previous node
 * Long-press is recognised at release: the view redraws on input only, so a held press
 * with no further input produces no frame to act in.
 */
#include "graph.h"

#include "canlin/sandbox.h"
#include "canlin/sandbox_limits.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

enum { G_NONE = 0, G_PAN, G_NODE, G_PORT, G_EDGE };
enum { H_NONE = 0, H_PORT, H_NODE, H_EDGE };
enum { E_MOVE = 1, E_CONNECT, E_DISCONNECT, E_SELECT, E_DELETE, E_ADD, E_OPEN };

typedef struct gport {
    const char *id, *label, *type; /* label/type may be NULL */
    size_t idl, labl, tyl;
    uint64_t h;
    int out;
} gport;

typedef struct gbody {
    int meter;
    const char *s;
    size_t l;
    float val;
} gbody;

typedef struct gnode {
    const char *id, *title;
    size_t idl, til;
    uint64_t h;
    float x, y;
    unsigned p0, nin, nout, nbody; /* ports p0 .. p0+nin-1 inputs, then outputs */
    gbody body[CANLIN_UI_GRAPH_MAX_BODY];
} gnode;

typedef struct gedge {
    unsigned fn, fp, tn, tp; /* node indices and absolute port indices */
} gedge;

#define HASH_SLOTS 512u /* >= 2 x CANLIN_UI_GRAPH_MAX_NODES, power of two */

struct canlin_ui_graph {
    unsigned nn, np, ne;
    gnode *n;
    gport *p;
    gedge *e;
    int sel_node, sel_edge; /* -1 = none */
    unsigned short hash[HASH_SLOTS]; /* node index + 1 */
};

typedef struct gedit {
    int kind;
    int n1, p1, n2, p2; /* node / absolute port indices */
    int edge;           /* from 0 */
    int has_n1, has_edge;
    float x, y;
} gedit;

#define EDITS_MAX 4

/* ---- helpers ---- */

static uint64_t hash_str(const char *s, size_t n)
{
    uint64_t h = 14695981039346656037ull;
    size_t i;
    for (i = 0; i < n; i++) {
        h ^= (unsigned char)s[i];
        h *= 1099511628211ull;
    }
    return h ? h : 1;
}

static int streq(const char *a, size_t al, const char *b, size_t bl)
{
    return al == bl && memcmp(a, b, al) == 0;
}

/* Pushes t[key] (raw) of the table at index t; returns its type. */
static int rawfield(lua_State *L, int t, const char *key)
{
    t = lua_absindex(L, t);
    lua_pushstring(L, key);
    return lua_rawget(L, t);
}

/* ---- validation (spec §5.1, §8) ---- */

/* String at the top of the stack (not popped). NULL if nil and optional. */
static const char *gstr(lua_State *L, int optional, const char *path, size_t *len)
{
    const char *s;
    int t = lua_type(L, -1);
    *len = 0;
    if (t == LUA_TNIL && optional)
        return NULL;
    if (t != LUA_TSTRING)
        luaL_error(L, "ui:graph: %s must be a string", path);
    s = lua_tolstring(L, -1, len);
    if (*len > CANLIN_UI_MAX_LABEL)
        luaL_error(L, "ui:graph: %s exceeds %d bytes", path, (int)CANLIN_UI_MAX_LABEL);
    if (*len == 0 && !optional)
        luaL_error(L, "ui:graph: %s must not be empty", path);
    canlin_sandbox_charge(L, *len / CANLIN_SB_COST_BYTES_PER_UNIT + 1u);
    if (!canlin_ui_utf8_valid(s, *len))
        luaL_error(L, "ui:graph: %s is not valid UTF-8", path);
    return s;
}

/* Array length of the value at the top: nil = 0, table = rawlen, else error. */
static size_t arr_len(lua_State *L, const char *path)
{
    int t = lua_type(L, -1);
    if (t == LUA_TNIL)
        return 0;
    if (t != LUA_TTABLE)
        luaL_error(L, "ui:graph: %s must be an array", path);
    return (size_t)lua_rawlen(L, -1);
}

static float coord(lua_State *L, const char *path)
{
    lua_Number x;
    if (lua_type(L, -1) != LUA_TNUMBER)
        luaL_error(L, "ui:graph: %s must be a number", path);
    x = lua_tonumber(L, -1);
    if (!(x >= -CANLIN_UI_GRAPH_MAX_COORD && x <= CANLIN_UI_GRAPH_MAX_COORD))
        luaL_error(L, "ui:graph: %s outside +-%d em", path, (int)CANLIN_UI_GRAPH_MAX_COORD);
    return (float)x;
}

static int find_node(const canlin_ui_graph *g, const char *id, size_t len)
{
    uint64_t h = hash_str(id, len);
    unsigned i = (unsigned)(h ^ (h >> 32)) & (HASH_SLOTS - 1);
    while (g->hash[i]) {
        const gnode *n = &g->n[g->hash[i] - 1];
        if (n->h == h && streq(n->id, n->idl, id, len))
            return g->hash[i] - 1;
        i = (i + 1) & (HASH_SLOTS - 1);
    }
    return -1;
}

static int node_by_hash(const canlin_ui_graph *g, uint64_t h)
{
    unsigned i = (unsigned)(h ^ (h >> 32)) & (HASH_SLOTS - 1);
    while (g->hash[i]) {
        if (g->n[g->hash[i] - 1].h == h)
            return g->hash[i] - 1;
        i = (i + 1) & (HASH_SLOTS - 1);
    }
    return -1;
}

static int port_by_hash(const canlin_ui_graph *g, int ni, uint64_t h, int out)
{
    const gnode *n = &g->n[ni];
    unsigned k;
    for (k = 0; k < n->nin + n->nout; k++)
        if (g->p[n->p0 + k].h == h && g->p[n->p0 + k].out == out)
            return (int)(n->p0 + k);
    return -1;
}

/* Edge endpoint {node, port} at the top of the stack; port must have direction out. */
static void endpoint(lua_State *L, const canlin_ui_graph *g, const char *path, int out,
                     unsigned *ni, unsigned *pi)
{
    int t = lua_gettop(L), n;
    size_t nl, pl, k;
    const char *ns, *ps;
    const gnode *nd;
    if (lua_type(L, t) != LUA_TTABLE)
        luaL_error(L, "ui:graph: %s must be {node, port}", path);
    lua_rawgeti(L, t, 1);
    ns = gstr(L, 0, path, &nl);
    lua_rawgeti(L, t, 2);
    ps = gstr(L, 0, path, &pl);
    n = find_node(g, ns, nl);
    if (n < 0)
        luaL_error(L, "ui:graph: %s names an unknown node", path);
    nd = &g->n[n];
    for (k = 0; k < nd->nin + nd->nout; k++) {
        const gport *p = &g->p[nd->p0 + k];
        if (streq(p->id, p->idl, ps, pl)) {
            if (p->out != out)
                luaL_error(L, "ui:graph: %s must name an %s port", path,
                           out ? "output" : "input");
            *ni = (unsigned)n;
            *pi = (unsigned)(nd->p0 + k);
            lua_pop(L, 2);
            return;
        }
    }
    luaL_error(L, "ui:graph: %s names an unknown port", path);
}

canlin_ui_graph *canlin_ui_graph_parse(lua_State *L, int gidx, int oidx, float *height_em,
                                       int *fit)
{
    char path[96];
    size_t nn, ne, np = 0, i, k;
    int nodes, edges, top;
    canlin_ui_graph *g;
    size_t sz;

    gidx = lua_absindex(L, gidx);
    oidx = lua_absindex(L, oidx);
    luaL_checktype(L, gidx, LUA_TTABLE);
    luaL_checkstack(L, 16, "ui:graph");

    /* opts: height (em), fit */
    *height_em = CANLIN_UI_GRAPH_HEIGHT_EM;
    *fit = 0;
    if (!lua_isnoneornil(L, oidx)) {
        luaL_checktype(L, oidx, LUA_TTABLE);
        if (rawfield(L, oidx, "height") != LUA_TNIL) {
            lua_Number h;
            if (lua_type(L, -1) != LUA_TNUMBER)
                luaL_error(L, "ui:graph: opts.height must be a number");
            h = lua_tonumber(L, -1);
            if (!(h >= CANLIN_UI_GRAPH_HEIGHT_MIN_EM && h <= CANLIN_UI_GRAPH_HEIGHT_MAX_EM))
                luaL_error(L, "ui:graph: opts.height must be %d-%d em",
                           (int)CANLIN_UI_GRAPH_HEIGHT_MIN_EM,
                           (int)CANLIN_UI_GRAPH_HEIGHT_MAX_EM);
            *height_em = (float)h;
        }
        lua_pop(L, 1);
        if (rawfield(L, oidx, "fit") != LUA_TNIL) {
            if (lua_type(L, -1) != LUA_TBOOLEAN)
                luaL_error(L, "ui:graph: opts.fit must be a boolean");
            *fit = lua_toboolean(L, -1);
        }
        lua_pop(L, 1);
    }

    /* pass 1: counts and shapes */
    rawfield(L, gidx, "nodes");
    nodes = lua_gettop(L);
    nn = arr_len(L, "nodes");
    if (nn > CANLIN_UI_GRAPH_MAX_NODES)
        luaL_error(L, "ui:graph: more than %d nodes", (int)CANLIN_UI_GRAPH_MAX_NODES);
    rawfield(L, gidx, "edges");
    edges = lua_gettop(L);
    ne = arr_len(L, "edges");
    if (ne > CANLIN_UI_GRAPH_MAX_EDGES)
        luaL_error(L, "ui:graph: more than %d edges", (int)CANLIN_UI_GRAPH_MAX_EDGES);
    canlin_sandbox_charge(L, (nn + ne + 1) * CANLIN_SB_COST_ELEM);
    for (i = 0; i < nn; i++) {
        size_t a, b;
        snprintf(path, sizeof path, "nodes[%u]", (unsigned)(i + 1));
        if (lua_rawgeti(L, nodes, (lua_Integer)i + 1) != LUA_TTABLE)
            luaL_error(L, "ui:graph: %s must be a table", path);
        snprintf(path, sizeof path, "nodes[%u].inputs", (unsigned)(i + 1));
        rawfield(L, -1, "inputs");
        a = arr_len(L, path);
        snprintf(path, sizeof path, "nodes[%u].outputs", (unsigned)(i + 1));
        rawfield(L, -2, "outputs");
        b = arr_len(L, path);
        if (a + b > CANLIN_UI_GRAPH_MAX_PORTS)
            luaL_error(L, "ui:graph: nodes[%d] has more than %d ports", (int)(i + 1),
                       (int)CANLIN_UI_GRAPH_MAX_PORTS);
        np += a + b;
        lua_pop(L, 3);
    }
    canlin_sandbox_charge(L, np * CANLIN_SB_COST_ELEM);

    sz = sizeof *g + nn * sizeof(gnode) + np * sizeof(gport) + ne * sizeof(gedge);
    g = lua_newuserdatauv(L, sz, 0); /* stays on the stack: the caller's scratch */
    memset(g, 0, sz);
    g->nn = (unsigned)nn;
    g->np = (unsigned)np;
    g->ne = (unsigned)ne;
    g->n = (gnode *)(void *)(g + 1);
    g->p = (gport *)(void *)(g->n + nn);
    g->e = (gedge *)(void *)(g->p + np);
    g->sel_node = g->sel_edge = -1;
    top = lua_gettop(L);

    /* pass 2: nodes and ports */
    np = 0;
    for (i = 0; i < nn; i++) {
        gnode *n = &g->n[i];
        int nt, dir;
        unsigned slot;
        lua_rawgeti(L, nodes, (lua_Integer)i + 1);
        nt = lua_gettop(L);
        snprintf(path, sizeof path, "nodes[%u].id", (unsigned)(i + 1));
        rawfield(L, nt, "id");
        n->id = gstr(L, 0, path, &n->idl);
        n->h = hash_str(n->id, n->idl);
        if (find_node(g, n->id, n->idl) >= 0)
            luaL_error(L, "ui:graph: %s is not unique", path);
        slot = (unsigned)(n->h ^ (n->h >> 32)) & (HASH_SLOTS - 1);
        while (g->hash[slot])
            slot = (slot + 1) & (HASH_SLOTS - 1);
        g->hash[slot] = (unsigned short)(i + 1);
        snprintf(path, sizeof path, "nodes[%u].title", (unsigned)(i + 1));
        rawfield(L, nt, "title");
        n->title = gstr(L, 1, path, &n->til);
        snprintf(path, sizeof path, "nodes[%u].x", (unsigned)(i + 1));
        rawfield(L, nt, "x");
        n->x = coord(L, path);
        snprintf(path, sizeof path, "nodes[%u].y", (unsigned)(i + 1));
        rawfield(L, nt, "y");
        n->y = coord(L, path);
        lua_settop(L, nt);
        n->p0 = (unsigned)np;
        for (dir = 0; dir < 2; dir++) {
            const char *key = dir ? "outputs" : "inputs";
            int at;
            size_t cnt;
            rawfield(L, nt, key);
            at = lua_gettop(L);
            cnt = lua_istable(L, at) ? (size_t)lua_rawlen(L, at) : 0;
            for (k = 0; k < cnt; k++) {
                gport *p = &g->p[np];
                size_t j;
                int pt;
                snprintf(path, sizeof path, "nodes[%u].%s[%u]", (unsigned)(i + 1), key,
                         (unsigned)(k + 1));
                if (lua_rawgeti(L, at, (lua_Integer)k + 1) != LUA_TTABLE)
                    luaL_error(L, "ui:graph: %s must be a table", path);
                pt = lua_gettop(L);
                rawfield(L, pt, "id");
                p->id = gstr(L, 0, path, &p->idl);
                rawfield(L, pt, "label");
                p->label = gstr(L, 1, path, &p->labl);
                rawfield(L, pt, "type");
                p->type = gstr(L, 1, path, &p->tyl);
                p->h = hash_str(p->id, p->idl);
                p->out = dir;
                for (j = n->p0; j < np; j++)
                    if (streq(g->p[j].id, g->p[j].idl, p->id, p->idl))
                        luaL_error(L, "ui:graph: %s.id is not unique in its node", path);
                np++;
                lua_settop(L, at);
            }
            if (dir)
                n->nout = (unsigned)cnt;
            else
                n->nin = (unsigned)cnt;
            lua_settop(L, nt);
        }
        snprintf(path, sizeof path, "nodes[%u].body", (unsigned)(i + 1));
        rawfield(L, nt, "body");
        k = arr_len(L, path);
        if (k > CANLIN_UI_GRAPH_MAX_BODY)
            luaL_error(L, "ui:graph: %s has more than %d rows", path,
                       (int)CANLIN_UI_GRAPH_MAX_BODY);
        n->nbody = (unsigned)k;
        canlin_sandbox_charge(L, k * CANLIN_SB_COST_ELEM);
        for (k = 0; k < n->nbody; k++) {
            gbody *b = &g->n[i].body[k];
            int bt, bl = lua_gettop(L);
            size_t kl;
            const char *kind;
            snprintf(path, sizeof path, "nodes[%u].body[%u]", (unsigned)(i + 1),
                     (unsigned)(k + 1));
            if (lua_rawgeti(L, bl, (lua_Integer)k + 1) != LUA_TTABLE)
                luaL_error(L, "ui:graph: %s must be a table", path);
            bt = lua_gettop(L);
            rawfield(L, bt, "kind");
            kind = gstr(L, 0, path, &kl);
            if (streq(kind, kl, "text", 4)) {
                rawfield(L, bt, "text");
                b->s = gstr(L, 0, path, &b->l);
            } else if (streq(kind, kl, "meter", 5)) {
                lua_Number x;
                rawfield(L, bt, "value");
                if (lua_type(L, -1) != LUA_TNUMBER)
                    luaL_error(L, "ui:graph: %s.value must be a number", path);
                x = lua_tonumber(L, -1);
                b->meter = 1;
                b->val = !(x > 0) ? 0.0f : x > 1 ? 1.0f : (float)x; /* NaN -> 0 */
            } else {
                luaL_error(L, "ui:graph: %s.kind must be \"text\" or \"meter\"", path);
            }
            lua_settop(L, bl);
        }
        lua_settop(L, top);
    }

    /* edges */
    for (i = 0; i < ne; i++) {
        gedge *e = &g->e[i];
        int et;
        snprintf(path, sizeof path, "edges[%u]", (unsigned)(i + 1));
        if (lua_rawgeti(L, edges, (lua_Integer)i + 1) != LUA_TTABLE)
            luaL_error(L, "ui:graph: %s must be a table", path);
        et = lua_gettop(L);
        snprintf(path, sizeof path, "edges[%u].from", (unsigned)(i + 1));
        rawfield(L, et, "from");
        endpoint(L, g, path, 1, &e->fn, &e->fp);
        lua_settop(L, et);
        snprintf(path, sizeof path, "edges[%u].to", (unsigned)(i + 1));
        rawfield(L, et, "to");
        endpoint(L, g, path, 0, &e->tn, &e->tp);
        lua_settop(L, top);
    }

    /* selected: nil, a node id or an edge index */
    switch (rawfield(L, gidx, "selected")) {
    case LUA_TNIL:
        break;
    case LUA_TSTRING: {
        size_t l;
        const char *s = gstr(L, 0, "selected", &l);
        g->sel_node = find_node(g, s, l);
        if (g->sel_node < 0)
            luaL_error(L, "ui:graph: selected names an unknown node");
    } break;
    case LUA_TNUMBER: {
        lua_Integer ix;
        if (!lua_isinteger(L, -1))
            luaL_error(L, "ui:graph: selected edge index must be an integer");
        ix = lua_tointeger(L, -1);
        if (ix < 1 || (size_t)ix > ne)
            luaL_error(L, "ui:graph: selected edge index out of range");
        g->sel_edge = (int)ix - 1;
    } break;
    default:
        luaL_error(L, "ui:graph: selected must be a node id, an edge index or nil");
    }
    lua_settop(L, top);
    /* leave only the userdata above the caller's arguments */
    lua_replace(L, nodes);
    lua_settop(L, nodes);
    return g;
}

/* ---- per-identity state ---- */

canlin_ui_graph_state *canlin_ui_graph_state_get(canlin_ui_graph_state *states, uint64_t id,
                                                 uint32_t frame)
{
    unsigned i, used = 0;
    canlin_ui_graph_state *found = NULL, *fr = NULL;
    for (i = 0; i < CANLIN_UI_GRAPH_STATES; i++) {
        canlin_ui_graph_state *s = &states[i];
        if (s->id == 0) {
            if (fr == NULL)
                fr = s;
            continue;
        }
        if (s->frame == frame)
            used++;
        if (s->id == id)
            found = s;
    }
    if (used >= CANLIN_UI_GRAPH_MAX_PER_DRAW)
        return NULL;
    if (found == NULL) {
        if (fr == NULL)
            return NULL; /* cannot happen: at most MAX_PER_DRAW survive a frame */
        memset(fr, 0, sizeof *fr);
        fr->id = id;
        fr->dedge = -1;
        found = fr;
    }
    found->frame = frame;
    return found;
}

void canlin_ui_graph_state_evict(canlin_ui_graph_state *states, uint32_t frame)
{
    unsigned i;
    for (i = 0; i < CANLIN_UI_GRAPH_STATES; i++)
        if (states[i].id && states[i].frame != frame)
            memset(&states[i], 0, sizeof states[i]);
}

void canlin_ui_graph_state_cancel(canlin_ui_graph_state *states)
{
    unsigned i;
    for (i = 0; i < CANLIN_UI_GRAPH_STATES; i++) {
        canlin_ui_graph_state *st = &states[i];
        if (st->moved && (st->drag == G_PAN || st->drag == G_EDGE)) {
            st->panx = st->ax; /* a cancelled pan snaps back like a node preview */
            st->pany = st->ay;
        }
        st->drag = G_NONE;
        st->moved = 0;
        st->has_tap = 0; /* a tap before the gap never pairs with one after it */
        st->dedge = -1;
    }
}

/* ---- geometry ---- */

typedef struct geo {
    const canlin_ui_graph *g;
    canlin_ui_graph_env *e;
    canlin_ui_graph_state *st;
    float em, ox, oy;       /* screen px of graph (0,0) incl. pan */
    struct nk_rect clip;    /* visible part of the canvas */
    float mx, my;           /* pointer */
    int preview;            /* node drag preview active */
    int pnode;
    float pdx, pdy;         /* preview offset, em */
} geo;

static unsigned node_rows(const gnode *n)
{
    return n->nin > n->nout ? n->nin : n->nout;
}

static float node_h(const gnode *n)
{
    return CANLIN_UI_GRAPH_TITLE_EM + (float)node_rows(n) * CANLIN_UI_MIN_TOUCH_EM +
           (float)n->nbody * CANLIN_UI_GRAPH_BODY_EM + 0.5f;
}

/* Node origin in em, including the drag preview. */
static void node_pos(const geo *c, int i, float *x, float *y)
{
    *x = c->g->n[i].x;
    *y = c->g->n[i].y;
    if (c->preview && i == c->pnode) {
        *x += c->pdx;
        *y += c->pdy;
    }
}

static struct nk_rect node_rect(const geo *c, int i)
{
    float x, y;
    long x0, y0, x1, y1;
    node_pos(c, i, &x, &y);
    x0 = lroundf(c->ox + x * c->em);
    y0 = lroundf(c->oy + y * c->em);
    x1 = lroundf(c->ox + (x + CANLIN_UI_GRAPH_NODE_W_EM) * c->em);
    y1 = lroundf(c->oy + (y + node_h(&c->g->n[i])) * c->em);
    return nk_rect((float)x0, (float)y0, (float)(x1 - x0), (float)(y1 - y0));
}

/* Port dot centre in screen px; p is an absolute port index of node i. */
static void port_centre(const geo *c, int i, unsigned p, float *px, float *py)
{
    const gnode *n = &c->g->n[i];
    unsigned k = p - n->p0, row = c->g->p[p].out ? k - n->nin : k;
    float x, y;
    node_pos(c, i, &x, &y);
    if (c->g->p[p].out)
        x += CANLIN_UI_GRAPH_NODE_W_EM;
    y += CANLIN_UI_GRAPH_TITLE_EM + ((float)row + 0.5f) * CANLIN_UI_MIN_TOUCH_EM;
    *px = c->ox + x * c->em;
    *py = c->oy + y * c->em;
}

/* Touch target: a whole-pixel square of at least 2.5 em around the port centre. */
static struct nk_rect port_target(const geo *c, int i, unsigned p)
{
    float cx, cy, s = ceilf(CANLIN_UI_MIN_TOUCH_EM * c->em - 1e-3f);
    port_centre(c, i, p, &cx, &cy);
    return nk_rect((float)lroundf(cx - s / 2.0f), (float)lroundf(cy - s / 2.0f), s, s);
}

static int in_rect(struct nk_rect r, float x, float y)
{
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

static int overlaps(struct nk_rect a, struct nk_rect b)
{
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

static struct nk_rect intersect(struct nk_rect a, struct nk_rect b)
{
    float x0 = a.x > b.x ? a.x : b.x, y0 = a.y > b.y ? a.y : b.y;
    float x1 = a.x + a.w < b.x + b.w ? a.x + a.w : b.x + b.w;
    float y1 = a.y + a.h < b.y + b.h ? a.y + a.h : b.y + b.h;
    return nk_rect(x0, y0, x1 > x0 ? x1 - x0 : 0, y1 > y0 ? y1 - y0 : 0);
}

/* Cubic Bezier control points of an edge from (ax, ay) to (bx, by). */
static void edge_ctrl(float em, float ax, float ay, float bx, float by, float *q)
{
    float d = fabsf(bx - ax) * 0.5f;
    if (d < 2.0f * em)
        d = 2.0f * em;
    q[0] = ax;
    q[1] = ay;
    q[2] = ax + d;
    q[3] = ay;
    q[4] = bx - d;
    q[5] = by;
    q[6] = bx;
    q[7] = by;
}

static void bez(const float *q, float t, float *x, float *y)
{
    float u = 1.0f - t, w1 = u * u * u, w2 = 3 * u * u * t, w3 = 3 * u * t * t, w4 = t * t * t;
    *x = w1 * q[0] + w2 * q[2] + w3 * q[4] + w4 * q[6];
    *y = w1 * q[1] + w2 * q[3] + w3 * q[5] + w4 * q[7];
}

static struct nk_rect ctrl_bounds(const float *q, float pad)
{
    float x0 = q[0], x1 = q[0], y0 = q[1], y1 = q[1];
    int k;
    for (k = 1; k < 4; k++) {
        x0 = q[2 * k] < x0 ? q[2 * k] : x0;
        x1 = q[2 * k] > x1 ? q[2 * k] : x1;
        y0 = q[2 * k + 1] < y0 ? q[2 * k + 1] : y0;
        y1 = q[2 * k + 1] > y1 ? q[2 * k + 1] : y1;
    }
    return nk_rect(x0 - pad, y0 - pad, x1 - x0 + 2 * pad, y1 - y0 + 2 * pad);
}

static float seg_dist2(float px, float py, float ax, float ay, float bx, float by)
{
    float dx = bx - ax, dy = by - ay, l = dx * dx + dy * dy, t = 0;
    if (l > 0) {
        t = ((px - ax) * dx + (py - ay) * dy) / l;
        t = t < 0 ? 0 : t > 1 ? 1 : t;
    }
    dx = ax + t * dx - px;
    dy = ay + t * dy - py;
    return dx * dx + dy * dy;
}

static void edge_ends(const geo *c, unsigned ei, float *q)
{
    const gedge *e = &c->g->e[ei];
    float ax, ay, bx, by;
    port_centre(c, (int)e->fn, e->fp, &ax, &ay);
    port_centre(c, (int)e->tn, e->tp, &bx, &by);
    edge_ctrl(c->em, ax, ay, bx, by, q);
}

/* What is under (x, y): ports first (topmost node first), then nodes, then edges. */
static int hit(const geo *c, float x, float y, int *ni, unsigned *pi)
{
    const canlin_ui_graph *g = c->g;
    int i;
    unsigned k;
    for (i = (int)g->nn - 1; i >= 0; i--) {
        const gnode *n = &g->n[i];
        for (k = 0; k < n->nin + n->nout; k++)
            if (in_rect(port_target(c, i, n->p0 + k), x, y)) {
                *ni = i;
                *pi = n->p0 + k;
                return H_PORT;
            }
    }
    for (i = (int)g->nn - 1; i >= 0; i--)
        if (in_rect(node_rect(c, i), x, y)) {
            *ni = i;
            return H_NODE;
        }
    {
        float r = CANLIN_UI_GRAPH_EDGE_HIT_EM * c->em, best = r * r;
        int found = -1;
        for (k = 0; k < g->ne; k++) {
            float q[8], lx, ly;
            unsigned s;
            edge_ends(c, k, q);
            if (!in_rect(ctrl_bounds(q, r), x, y))
                continue;
            lx = q[0];
            ly = q[1];
            for (s = 1; s <= CANLIN_UI_GRAPH_CURVE_SEGMENTS; s++) {
                float nx, ny, d;
                bez(q, (float)s / (float)CANLIN_UI_GRAPH_CURVE_SEGMENTS, &nx, &ny);
                d = seg_dist2(x, y, lx, ly, nx, ny);
                if (d <= best) {
                    best = d;
                    found = (int)k;
                }
                lx = nx;
                ly = ny;
            }
        }
        if (found >= 0) {
            *ni = found;
            return H_EDGE;
        }
    }
    return H_NONE;
}

/* ---- connection rules (spec §5.2) ---- */

static int connect_ok(const canlin_ui_graph *g, int an, unsigned ap, int bn, unsigned bp)
{
    const gport *a = &g->p[ap], *b = &g->p[bp];
    unsigned k;
    int on, in;
    unsigned op, ip;
    if (a->out == b->out || an == bn)
        return 0;
    if (a->type && b->type && !streq(a->type, a->tyl, b->type, b->tyl))
        return 0;
    on = a->out ? an : bn;
    op = a->out ? ap : bp;
    in = a->out ? bn : an;
    ip = a->out ? bp : ap;
    for (k = 0; k < g->ne; k++)
        if ((int)g->e[k].fn == on && g->e[k].fp == op && (int)g->e[k].tn == in &&
            g->e[k].tp == ip)
            return 0;
    return 1;
}

static int type_clash(const gport *a, const gport *b)
{
    return a->out != b->out && a->type && b->type && !streq(a->type, a->tyl, b->type, b->tyl);
}

/* Drag preview (spec §5.2): port p of node n shows as incompatible when a drop there
 * would be rejected locally for a reason visible before the drop: a type clash, the
 * dragging node's own ports, or an input that already carries the identical edge. The
 * edge lifted by this drag (lifted >= 0) does not count: dropping it back is a no-op. */
static int preview_bad(const canlin_ui_graph *g, int sn, unsigned sp, int lifted, int n,
                       unsigned p)
{
    const gport *a = &g->p[sp], *b = &g->p[p];
    unsigned k;
    if (type_clash(a, b) || n == sn)
        return 1;
    if (a->out == b->out)
        return 0;
    for (k = 0; k < g->ne; k++) {
        const gedge *x = &g->e[k];
        if ((int)k == lifted)
            continue;
        if (a->out ? ((int)x->fn == sn && x->fp == sp && (int)x->tn == n && x->tp == p)
                   : ((int)x->fn == n && x->fp == p && (int)x->tn == sn && x->tp == sp))
            return 1;
    }
    return 0;
}

/* ---- drawing ---- */

static struct nk_color role(const canlin_ui_graph_env *e, int r)
{
    canlin_theme_rgb c = e->theme->color[r];
    return nk_rgb(c.r, c.g, c.b);
}

/* Clips segment (x0,y0)-(x1,y1) to r (Liang-Barsky); 0 if nothing is left. */
static int clip_seg(struct nk_rect r, float *x0, float *y0, float *x1, float *y1)
{
    float t0 = 0, t1 = 1, dx = *x1 - *x0, dy = *y1 - *y0;
    float p[4], q[4];
    int k;
    p[0] = -dx; q[0] = *x0 - r.x;
    p[1] = dx;  q[1] = r.x + r.w - *x0;
    p[2] = -dy; q[2] = *y0 - r.y;
    p[3] = dy;  q[3] = r.y + r.h - *y0;
    for (k = 0; k < 4; k++) {
        if (p[k] == 0) {
            if (q[k] < 0)
                return 0;
        } else {
            float t = q[k] / p[k];
            if (p[k] < 0) {
                if (t > t1)
                    return 0;
                if (t > t0)
                    t0 = t;
            } else {
                if (t < t0)
                    return 0;
                if (t < t1)
                    t1 = t;
            }
        }
    }
    *x1 = *x0 + t1 * dx;
    *y1 = *y0 + t1 * dy;
    *x0 = *x0 + t0 * dx;
    *y0 = *y0 + t0 * dy;
    return 1;
}

/* Nuklear stores command coordinates as short: far curves are drawn as clipped lines. */
#define SHORT_SAFE 16000.0f

static void draw_curve(struct nk_command_buffer *out, struct nk_rect clip, const float *q,
                       float th, struct nk_color col)
{
    struct nk_rect bb = ctrl_bounds(q, th);
    int k, safe = 1;
    if (!overlaps(bb, clip))
        return; /* culled */
    for (k = 0; k < 8; k++)
        if (q[k] < -SHORT_SAFE || q[k] > SHORT_SAFE)
            safe = 0;
    if (safe) {
        nk_stroke_curve(out, q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7], th, col);
    } else {
        float lx = q[0], ly = q[1];
        unsigned s;
        struct nk_rect wide = nk_rect(clip.x - th, clip.y - th, clip.w + 2 * th, clip.h + 2 * th);
        for (s = 1; s <= CANLIN_UI_GRAPH_CURVE_SEGMENTS; s++) {
            float nx, ny, x0 = lx, y0 = ly, x1, y1;
            bez(q, (float)s / (float)CANLIN_UI_GRAPH_CURVE_SEGMENTS, &nx, &ny);
            x1 = nx;
            y1 = ny;
            if (clip_seg(wide, &x0, &y0, &x1, &y1))
                nk_stroke_line(out, x0, y0, x1, y1, th, col);
            lx = nx;
            ly = ny;
        }
    }
}

static size_t cp_len(const char *s, size_t left)
{
    unsigned c = (unsigned char)s[0];
    size_t n = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
    return n > left ? left : n;
}

static float text_w(const struct nk_user_font *f, const char *s, size_t len)
{
    return len ? f->width(f->userdata, f->height, s, (int)len) : 0.0f;
}

/* U+2026 in UTF-8: 3 bytes, like the "..." it replaced (the built-in font has it, canlin#22). */
#define ELLIPSIS "\xE2\x80\xA6"

/* Single-line label ending in U+2026 when too wide (spec §2); returns the width drawn. */
static float draw_label(struct nk_command_buffer *out, const struct nk_user_font *f,
                        const char *s, size_t len, float x, float cy, float avail, int right,
                        struct nk_color col)
{
    char buf[CANLIN_UI_MAX_LABEL + 4];
    float w;
    if (len == 0 || avail <= 0)
        return 0;
    w = text_w(f, s, len);
    if (w > avail) {
        float dots = text_w(f, ELLIPSIS, 3), acc = 0;
        size_t i = 0;
        while (i < len) {
            size_t n = cp_len(s + i, len - i);
            float cw = text_w(f, s + i, n);
            if (acc + cw + dots > avail)
                break;
            acc += cw;
            i += n;
        }
        memcpy(buf, s, i);
        memcpy(buf + i, ELLIPSIS, 3);
        s = buf;
        len = i + 3;
        w = acc + dots;
    }
    if (right)
        x -= w;
    nk_draw_text(out, nk_rect(x, cy - f->height / 2.0f, w + 1.0f, f->height), s, (int)len, f,
                 nk_rgba(0, 0, 0, 0), col);
    return w;
}

/* src_n/src_p: the port drag in progress (src_p < 0: none); lifted: its lifted edge. */
static void draw_node(geo *c, int i, int src_n, int src_p, int lifted)
{
    canlin_ui_graph_env *e = c->e;
    const canlin_ui_graph *g = c->g;
    const gnode *n = &g->n[i];
    struct nk_command_buffer *out = nk_window_get_canvas(e->nk);
    const struct nk_user_font *f = e->font;
    struct nk_rect r = node_rect(c, i);
    float em = c->em, rad = e->theme->radius * em, th = em / 8.0f < 1.0f ? 1.0f : floorf(em / 8.0f);
    float dotr = 0.3f * em, y;
    unsigned k;
    int sel = g->sel_node == i;

    if (!overlaps(r, c->clip))
        return; /* culled: no command, no record */
    canlin_ui_graph_record(e->view, CANLIN_UI_P_NODE, r, (uint32_t)i, 0);
    nk_fill_rect(out, r, rad, role(e, CANLIN_THEME_SURFACE));
    nk_stroke_rect(out, r, rad, sel ? 2.0f * th : th,
                   role(e, sel ? CANLIN_THEME_ACCENT : CANLIN_THEME_MUTED));
    draw_label(out, f, n->title ? n->title : n->id, n->title ? n->til : n->idl,
               r.x + 0.5f * em, r.y + CANLIN_UI_GRAPH_TITLE_EM * em / 2.0f, r.w - em, 0,
               role(e, CANLIN_THEME_TEXT));
    y = r.y + CANLIN_UI_GRAPH_TITLE_EM * em;
    /* A filled band, not a line: the renderer's line stamps a th square on both inclusive
     * end points and overshot the right border (spec §6.2: strokes stay inside r). */
    nk_fill_rect(out, nk_rect(r.x, floorf(y), r.w, th), 0, role(e, CANLIN_THEME_MUTED));
    for (k = 0; k < n->nin + n->nout; k++) {
        const gport *p = &g->p[n->p0 + k];
        float cx, cy;
        struct nk_rect t = port_target(c, i, n->p0 + k);
        int bad = src_p >= 0 && preview_bad(g, src_n, (unsigned)src_p, lifted, i, n->p0 + k);
        port_centre(c, i, n->p0 + k, &cx, &cy);
        nk_fill_circle(out, nk_rect(cx - dotr, cy - dotr, 2 * dotr, 2 * dotr),
                       role(e, bad ? CANLIN_THEME_DANGER : CANLIN_THEME_ACCENT));
        if (p->label)
            draw_label(out, f, p->label, p->labl, p->out ? cx - 0.75f * em : cx + 0.75f * em,
                       cy, r.w / 2.0f - em, p->out, role(e, CANLIN_THEME_TEXT));
        if (overlaps(t, c->clip))
            canlin_ui_graph_record(e->view, CANLIN_UI_P_PORT, t, (uint32_t)i, k);
    }
    y = r.y + (CANLIN_UI_GRAPH_TITLE_EM + (float)node_rows(n) * CANLIN_UI_MIN_TOUCH_EM) * em;
    for (k = 0; k < n->nbody; k++) {
        const gbody *b = &n->body[k];
        float cy = y + ((float)k + 0.5f) * CANLIN_UI_GRAPH_BODY_EM * em;
        if (b->meter) {
            struct nk_rect bar = nk_rect(r.x + 0.5f * em, cy - 0.35f * em, r.w - em, 0.7f * em);
            nk_fill_rect(out, bar, 0, role(e, CANLIN_THEME_BACKGROUND));
            bar.w = floorf(bar.w * b->val + 0.5f);
            if (bar.w > 0)
                nk_fill_rect(out, bar, 0, role(e, CANLIN_THEME_ACCENT));
        } else {
            draw_label(out, f, b->s, b->l, r.x + 0.5f * em, cy, r.w - em, 0,
                       role(e, CANLIN_THEME_MUTED));
        }
    }
}

/* ---- edits ---- */

static float q4(float v)
{
    v = floorf(v * 4.0f + 0.5f) / 4.0f;
    return v < -CANLIN_UI_GRAPH_MAX_COORD ? -CANLIN_UI_GRAPH_MAX_COORD
         : v > CANLIN_UI_GRAPH_MAX_COORD ? CANLIN_UI_GRAPH_MAX_COORD : v;
}

static float clamp_pan(float v)
{
    return v < -CANLIN_UI_GRAPH_MAX_COORD ? -CANLIN_UI_GRAPH_MAX_COORD
         : v > CANLIN_UI_GRAPH_MAX_COORD ? CANLIN_UI_GRAPH_MAX_COORD : v;
}

static gedit *add_edit(gedit *ed, int *ned, int kind)
{
    gedit *x = &ed[*ned];
    memset(x, 0, sizeof *x);
    x->kind = kind;
    (*ned)++;
    return x;
}

static void select_node(const canlin_ui_graph *g, gedit *ed, int *ned, int ni)
{
    if (g->sel_node != ni) {
        gedit *x = add_edit(ed, ned, E_SELECT);
        x->n1 = ni;
        x->has_n1 = 1;
    }
}

/* The source port of a port drag, resolved against this draw's graph; -1 if gone. */
static int drag_source(const canlin_ui_graph *g, const canlin_ui_graph_state *st, int *sn,
                       int *valid_edge)
{
    int n = node_by_hash(g, st->dnode), p;
    *valid_edge = 0;
    if (n < 0)
        return -1;
    p = port_by_hash(g, n, st->dport, st->dport_out);
    if (p < 0)
        return -1;
    if (st->dedge >= 0) {
        const gedge *e;
        if ((unsigned)st->dedge >= g->ne)
            return -1;
        e = &g->e[st->dedge];
        if ((int)e->tn != n || (int)e->tp != p)
            return -1; /* graph changed under the drag */
        *valid_edge = 1;
        *sn = (int)e->fn;
        return (int)e->fp;
    }
    *sn = n;
    return p;
}

static void release(geo *c, gedit *ed, int *ned)
{
    const canlin_ui_graph *g = c->g;
    canlin_ui_graph_env *e = c->e;
    canlin_ui_graph_state *st = c->st;
    int held = e->btn_t - st->press_t >= CANLIN_UI_LONG_PRESS_MS;
    int ni;
    unsigned pi;

    if (!st->moved) {
        switch (st->drag) {
        case G_PAN:
            if (held) {
                gedit *x = add_edit(ed, ned, E_ADD);
                x->x = q4((st->px0 - c->ox) / c->em);
                x->y = q4((st->py0 - c->oy) / c->em);
            } else if (g->sel_node >= 0 || g->sel_edge >= 0) {
                add_edit(ed, ned, E_SELECT);
            }
            break;
        case G_EDGE:
            if (st->dedge >= 0 && (unsigned)st->dedge < g->ne && g->sel_edge != st->dedge) {
                gedit *x = add_edit(ed, ned, E_SELECT);
                x->edge = st->dedge;
                x->has_edge = 1;
            }
            break;
        case G_NODE:
        case G_PORT:
            ni = node_by_hash(g, st->dnode);
            if (ni < 0)
                break;
            if (held || (st->has_tap && st->tap_node == st->dnode &&
                         e->btn_t - st->tap_t <= CANLIN_UI_DOUBLE_TAP_MS)) {
                gedit *x = add_edit(ed, ned, E_OPEN);
                x->n1 = ni;
                x->has_n1 = 1;
                st->has_tap = 0;
            } else {
                select_node(g, ed, ned, ni);
                st->has_tap = 1;
                st->tap_node = st->dnode;
                st->tap_t = e->btn_t;
            }
            break;
        }
        return;
    }
    st->has_tap = 0;
    if (st->drag == G_NODE) {
        ni = node_by_hash(g, st->dnode);
        if (ni >= 0) {
            float x = q4(g->n[ni].x + c->pdx), y = q4(g->n[ni].y + c->pdy);
            if (x != g->n[ni].x || y != g->n[ni].y) {
                gedit *m = add_edit(ed, ned, E_MOVE);
                m->n1 = ni;
                m->has_n1 = 1;
                m->x = x;
                m->y = y;
            }
        }
    } else if (st->drag == G_PORT) {
        int sn = -1, sp, took, tgt;
        sp = drag_source(g, st, &sn, &took);
        if (sp < 0)
            return;
        tgt = hit(c, c->mx, c->my, &ni, &pi) == H_PORT;
        if (took) {
            int orig_n = node_by_hash(g, st->dnode);
            if (tgt && ni == orig_n && (int)pi == port_by_hash(g, orig_n, st->dport, 0))
                return; /* dropped back where it was */
            {
                gedit *x = add_edit(ed, ned, E_DISCONNECT);
                x->edge = st->dedge;
                x->has_edge = 1;
            }
        }
        if (tgt && connect_ok(g, sn, (unsigned)sp, ni, pi)) {
            int so = g->p[sp].out;
            gedit *x = add_edit(ed, ned, E_CONNECT);
            x->n1 = so ? sn : ni;
            x->p1 = so ? sp : (int)pi;
            x->n2 = so ? ni : sn;
            x->p2 = so ? (int)pi : sp;
        }
    }
}

static void push_str_field(lua_State *L, const char *k, const char *s, size_t l)
{
    lua_pushlstring(L, s, l);
    lua_setfield(L, -2, k);
}

static void push_end(lua_State *L, const canlin_ui_graph *g, const char *k, int n, int p)
{
    lua_createtable(L, 2, 0);
    lua_pushlstring(L, g->n[n].id, g->n[n].idl);
    lua_rawseti(L, -2, 1);
    lua_pushlstring(L, g->p[p].id, g->p[p].idl);
    lua_rawseti(L, -2, 2);
    lua_setfield(L, -2, k);
}

static void push_edits(lua_State *L, const canlin_ui_graph *g, const gedit *ed, int ned)
{
    static const char *const names[] = {"", "move", "connect", "disconnect", "select",
                                        "delete", "add", "open"};
    int i;
    lua_createtable(L, ned, 0);
    for (i = 0; i < ned; i++) {
        const gedit *x = &ed[i];
        lua_createtable(L, 0, 4);
        lua_pushstring(L, names[x->kind]);
        lua_setfield(L, -2, "kind");
        if (x->has_n1)
            push_str_field(L, "node", g->n[x->n1].id, g->n[x->n1].idl);
        if (x->has_edge) {
            lua_pushinteger(L, (lua_Integer)x->edge + 1);
            lua_setfield(L, -2, "edge");
        }
        if (x->kind == E_MOVE || x->kind == E_ADD) {
            lua_pushnumber(L, (lua_Number)x->x);
            lua_setfield(L, -2, "x");
            lua_pushnumber(L, (lua_Number)x->y);
            lua_setfield(L, -2, "y");
        }
        if (x->kind == E_CONNECT) {
            push_end(L, g, "from", x->n1, x->p1);
            push_end(L, g, "to", x->n2, x->p2);
        }
        lua_rawseti(L, -2, i + 1);
    }
}

/* ---- the frame ---- */

void canlin_ui_graph_run(lua_State *L, canlin_ui_graph *g, int fit, canlin_ui_graph_env *e)
{
    canlin_ui_graph_state *st = e->st;
    const struct nk_input *in = &e->nk->input;
    const struct nk_mouse_button *b = &in->mouse.buttons[NK_BUTTON_LEFT];
    struct nk_command_buffer *out;
    struct nk_rect old;
    gedit ed[EDITS_MAX];
    int ned = 0, press, rel, ni, h, src_n = -1, src_p = -1, took = 0;
    unsigned pi, k;
    geo c;
    float em = e->em, th = em / 8.0f < 1.0f ? 1.0f : floorf(em / 8.0f);

    /* Rendering work, charged before any Nuklear call (spec §8: within the draw budget). */
    canlin_sandbox_charge(L, (g->nn + g->ne + g->np + 1) * CANLIN_SB_COST_ELEM);

    memset(&c, 0, sizeof c);
    c.g = g;
    c.e = e;
    c.st = st;
    c.em = em;
    c.pnode = -1;
    out = nk_window_get_canvas(e->nk);
    c.clip = intersect(intersect(e->box, e->vis), out->clip);

    if (fit && !st->fit_prev) {
        if (g->nn == 0) {
            st->panx = st->pany = 0;
        } else {
            float x0 = g->n[0].x, x1 = x0, y0 = g->n[0].y, y1 = y0;
            for (k = 0; k < g->nn; k++) {
                float r = g->n[k].x + CANLIN_UI_GRAPH_NODE_W_EM, bt = g->n[k].y + node_h(&g->n[k]);
                x0 = g->n[k].x < x0 ? g->n[k].x : x0;
                y0 = g->n[k].y < y0 ? g->n[k].y : y0;
                x1 = r > x1 ? r : x1;
                y1 = bt > y1 ? bt : y1;
            }
            st->panx = clamp_pan(e->box.w / em / 2.0f - (x0 + x1) / 2.0f);
            st->pany = clamp_pan(e->box.h / em / 2.0f - (y0 + y1) / 2.0f);
        }
    }
    st->fit_prev = (unsigned char)(fit != 0);

    c.mx = in->mouse.pos.x;
    c.my = in->mouse.pos.y;
    press = b->clicked && b->down;
    rel = b->clicked && !b->down;

    /* Drag continuation: movement, pan, node preview. */
    if (st->drag && !press) {
        float dx = c.mx - st->px0, dy = c.my - st->py0;
        float slop = CANLIN_UI_GRAPH_SLOP_EM * em;
        if (dx * dx + dy * dy > slop * slop)
            st->moved = 1;
        if (st->moved && (st->drag == G_PAN || st->drag == G_EDGE)) {
            st->panx = clamp_pan(st->ax + dx / em);
            st->pany = clamp_pan(st->ay + dy / em);
        }
        if (st->moved && st->drag == G_NODE) {
            c.pnode = node_by_hash(g, st->dnode);
            c.preview = c.pnode >= 0;
            c.pdx = dx / em;
            c.pdy = dy / em;
        }
    }
    c.ox = e->box.x + st->panx * em;
    c.oy = e->box.y + st->pany * em;

    if (press) {
        float px = b->clicked_pos.x, py = b->clicked_pos.y;
        st->drag = G_NONE;
        if (in_rect(c.clip, px, py)) {
            *e->focus = e->id;
            st->px0 = px;
            st->py0 = py;
            st->press_t = e->btn_t;
            st->moved = 0;
            st->dedge = -1;
            h = hit(&c, px, py, &ni, &pi);
            if (h == H_PORT) {
                st->drag = G_PORT;
                st->dnode = g->n[ni].h;
                st->dport = g->p[pi].h;
                st->dport_out = g->p[pi].out;
                if (!g->p[pi].out)
                    for (k = g->ne; k-- > 0;)
                        if ((int)g->e[k].tn == ni && g->e[k].tp == pi) {
                            st->dedge = (int)k; /* topmost edge into this input */
                            break;
                        }
            } else if (h == H_NODE) {
                st->drag = G_NODE;
                st->dnode = g->n[ni].h;
            } else {
                st->drag = h == H_EDGE ? G_EDGE : G_PAN;
                st->dedge = h == H_EDGE ? ni : -1;
                st->ax = st->panx;
                st->ay = st->pany;
            }
        } else if (*e->focus == e->id) {
            *e->focus = 0; /* a press elsewhere in the view takes the keyboard away */
        }
    } else if (rel && st->drag) {
        release(&c, ed, &ned);
        st->drag = G_NONE;
        c.preview = 0;
    } else if (st->drag && !b->down) {
        st->drag = G_NONE; /* release not seen (view reset); drop the gesture */
        c.preview = 0;
    }

    /* Keyboard (spec §5.3), only while this graph has the view's focus. */
    if (!press && *e->focus == e->id) {
        if (nk_input_is_key_pressed(in, NK_KEY_DEL)) {
            if (g->sel_node >= 0) {
                gedit *x = add_edit(ed, &ned, E_DELETE);
                x->n1 = g->sel_node;
                x->has_n1 = 1;
            } else if (g->sel_edge >= 0) {
                gedit *x = add_edit(ed, &ned, E_DISCONNECT);
                x->edge = g->sel_edge;
                x->has_edge = 1;
            }
        } else if (nk_input_is_key_pressed(in, NK_KEY_TAB) && g->nn > 0) {
            int back = nk_input_is_key_down(in, NK_KEY_SHIFT), n = (int)g->nn, s = g->sel_node;
            s = s < 0 ? (back ? n - 1 : 0) : (s + (back ? n - 1 : 1)) % n;
            select_node(g, ed, &ned, s);
        }
    }

    /* The port drag in progress, for the preview and the incompatible-port colour. */
    if (st->drag == G_PORT && st->moved) {
        src_p = drag_source(g, st, &src_n, &took);
        if (src_p < 0)
            src_n = -1;
    }

    if (c.clip.w > 0 && c.clip.h > 0) {
        old = out->clip;
        nk_push_scissor(out, c.clip);
        nk_stroke_rect(out, e->box, 0, th, role(e, CANLIN_THEME_MUTED));
        {
            /* Background grid, every 4 em, moving with the pan. */
            float step = 4.0f * em, x = c.ox - step * floorf((c.ox - c.clip.x) / step);
            float y = c.oy - step * floorf((c.oy - c.clip.y) / step);
            struct nk_color m = role(e, CANLIN_THEME_MUTED);
            m.a = 96;
            for (; x < c.clip.x + c.clip.w; x += step)
                nk_stroke_line(out, x, c.clip.y, x, c.clip.y + c.clip.h, 1.0f, m);
            for (; y < c.clip.y + c.clip.h; y += step)
                nk_stroke_line(out, c.clip.x, y, c.clip.x + c.clip.w, y, 1.0f, m);
        }
        for (k = 0; k < g->ne; k++) {
            float q[8];
            if (took && (int)k == st->dedge)
                continue; /* lifted off its input by the drag */
            edge_ends(&c, k, q);
            draw_curve(out, c.clip, q, g->sel_edge == (int)k ? 2.0f * th : th,
                       role(e, g->sel_edge == (int)k ? CANLIN_THEME_ACCENT : CANLIN_THEME_TEXT));
        }
        for (k = 0; k < g->nn; k++)
            draw_node(&c, (int)k, src_n, src_p, took ? st->dedge : -1);
        if (src_p >= 0) {
            float q[8], sx, sy;
            port_centre(&c, src_n, (unsigned)src_p, &sx, &sy);
            if (g->p[src_p].out)
                edge_ctrl(em, sx, sy, c.mx, c.my, q);
            else
                edge_ctrl(em, c.mx, c.my, sx, sy, q);
            draw_curve(out, c.clip, q, th, role(e, CANLIN_THEME_ACCENT));
        }
        nk_push_scissor(out, old);
    }

    push_edits(L, g, ed, ned); /* first Lua allocation after drawing */
}
