/* Graph editor contract tests (UI spec §5, §8, §10): real sandbox and scheduler with a
 * fake clock, the built-in theme and font, offscreen. Input is injected through
 * canlin_module_post_input, which stamps each edge with the scheduler clock at post
 * time; long-press and double-tap timing therefore follow canlin_ev_fake_clock_advance.
 * Positions come from the layout record (CANLIN_UI_P_NODE / CANLIN_UI_P_PORT). Exits 0 on success. */
#include "canlin/module.h"
#include "canlin/ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;

#define CHECK(c)                                                                   \
    do {                                                                           \
        checks++;                                                                  \
        if (!(c)) {                                                                \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c);  \
            failures++;                                                            \
        }                                                                          \
    } while (0)

/* ---- harness ---- */

#define LOGMAX 65536

typedef struct harness {
    canlin_ev_fake_clock clock;
    canlin_ev *ev;
    canlin_ui *ui;
    canlin_ui_output *out;
    canlin_module *m;
    char log[LOGMAX];
    size_t loglen;
} harness;

static void log_fn(void *ud, const char *msg, size_t len)
{
    harness *h = ud;
    if (len + 2 > sizeof h->log - h->loglen)
        return;
    memcpy(h->log + h->loglen, msg, len);
    h->loglen += len;
    h->log[h->loglen++] = '\n';
    h->log[h->loglen] = 0;
}

static void on_deliver(void *ud, canlin_ev *ev, const canlin_ev_delivery *d)
{
    harness *h = ud;
    (void)ev;
    if (h->m && canlin_module_gen(h->m) == d->gen)
        canlin_module_deliver(h->m, d);
}

static int has(const harness *h, const char *needle)
{
    return strstr(h->log, needle) != NULL;
}

static unsigned count(const harness *h, const char *needle)
{
    unsigned n = 0;
    const char *p = h->log;
    size_t k = strlen(needle);
    while ((p = strstr(p, needle)) != NULL) {
        n++;
        p += k;
    }
    return n;
}

static void clear_log(harness *h)
{
    h->loglen = 0;
    h->log[0] = 0;
}

static void h_turn(harness *h)
{
    canlin_ev_turn_result r;
    CHECK(canlin_ev_turn(h->ev, &r) == CANLIN_EV_OK);
}

static void h_drain(harness *h)
{
    int i;
    for (i = 0; i < 64; i++)
        h_turn(h);
}

static void h_init(harness *h, float dpi, const char *src, int w, int vh)
{
    canlin_ev_clock c;
    canlin_ev_callbacks cb;
    canlin_theme t;
    canlin_module_source s;
    canlin_sandbox_config cfg;
    canlin_ui_output_desc d;
    int err = 1;

    memset(h, 0, sizeof *h);
    c = canlin_ev_fake_clock_bind(&h->clock);
    cb.deliver = on_deliver;
    h->ev = canlin_ev_new(&c, &cb, h);
    canlin_theme_builtin(&t);
    h->ui = canlin_ui_new(&t);
    d.id = "graph-test";
    d.w = w;
    d.h = vh;
    d.dpi = dpi;
    d.scale = 100;
    h->out = canlin_ui_output_new(&d, NULL);
    if (!h->ev || !h->ui || !h->out) {
        fprintf(stderr, "harness init failed\n");
        exit(1);
    }
    memset(&s, 0, sizeof s);
    memset(&cfg, 0, sizeof cfg);
    s.name = "graphtest";
    s.version = "1";
    s.main = src;
    s.main_len = strlen(src);
    cfg.log = log_fn;
    cfg.ud = h;
    h->m = canlin_module_load(h->ev, &s, &cfg, &err);
    if (!h->m) {
        fprintf(stderr, "module load failed (%d): %s\n", err, h->log);
        exit(1);
    }
    CHECK(canlin_module_set_ui(h->m, h->ui, h->out, 0, 0, w, vh) == CANLIN_MODULE_OK);
    h_drain(h);
}

static void h_done(harness *h)
{
    canlin_module_free(h->m);
    CHECK(canlin_ev_free(h->ev) == CANLIN_EV_OK);
    CHECK(canlin_ui_output_free(h->out) == CANLIN_UI_OK);
    canlin_ui_free(h->ui);
}

static const canlin_ui_rect *rec(harness *h, size_t *n)
{
    return canlin_ui_view_record(canlin_module_view(h->m), n, NULL);
}

/* Graph record entry: part NODE/PORT of graph widget `widget`, node index, port index. */
static const canlin_ui_rect *grec(harness *h, int widget, int part, unsigned node,
                                  unsigned port)
{
    size_t n, i;
    const canlin_ui_rect *r = rec(h, &n);
    for (i = 0; i < n; i++)
        if (r[i].kind == CANLIN_UI_W_GRAPH && r[i].part == part && r[i].widget == widget &&
            r[i].off == node && (part != CANLIN_UI_P_PORT || r[i].len == port))
            return &r[i];
    return NULL;
}

static void input(harness *h, canlin_ui_input_type type, int x, int y, int down)
{
    canlin_ui_input in;
    memset(&in, 0, sizeof in);
    in.type = type;
    in.x = x;
    in.y = y;
    in.down = down;
    CHECK(canlin_module_post_input(h->m, &in) == CANLIN_MODULE_OK);
}

static void key(harness *h, int k)
{
    canlin_ui_input in;
    memset(&in, 0, sizeof in);
    in.type = CANLIN_UI_IN_KEY;
    in.key = k;
    in.down = 1;
    CHECK(canlin_module_post_input(h->m, &in) == CANLIN_MODULE_OK);
    in.down = 0;
    CHECK(canlin_module_post_input(h->m, &in) == CANLIN_MODULE_OK);
    h_drain(h);
}

/* A pause long enough that the next tap is neither a double tap nor a long press. */
static void gap(harness *h)
{
    canlin_ev_fake_clock_advance(&h->clock, 1000);
}

static void press(harness *h, int x, int y)
{
    input(h, CANLIN_UI_IN_MOTION, x, y, 0);
    input(h, CANLIN_UI_IN_BUTTON, x, y, 1);
    h_drain(h);
}

static void release(harness *h, int x, int y)
{
    input(h, CANLIN_UI_IN_BUTTON, x, y, 0);
    h_drain(h);
}

static void tap(harness *h, int x, int y)
{
    press(h, x, y);
    release(h, x, y);
}

static void hold(harness *h, int x, int y, unsigned ms)
{
    press(h, x, y);
    canlin_ev_fake_clock_advance(&h->clock, ms);
    release(h, x, y);
}

/* Press, move (one held frame), release. */
static void drag(harness *h, int x0, int y0, int x1, int y1)
{
    press(h, x0, y0);
    input(h, CANLIN_UI_IN_MOTION, x1, y1, 0);
    h_drain(h);
    release(h, x1, y1);
}

static int cx(const canlin_ui_rect *r)
{
    return r->x + r->w / 2;
}

static int cy(const canlin_ui_rect *r)
{
    return r->y + r->h / 2;
}

/* Title row of a node: 1 em below its top edge (em 16 at DPI 1). */
static void tap_title(harness *h, unsigned node)
{
    const canlin_ui_rect *r = grec(h, 0, CANLIN_UI_P_NODE, node, 0);
    CHECK(r != NULL);
    if (r)
        tap(h, cx(r), r->y + 16);
}

static void drag_port(harness *h, unsigned n0, unsigned p0, unsigned n1, unsigned p1)
{
    const canlin_ui_rect *a = grec(h, 0, CANLIN_UI_P_PORT, n0, p0);
    const canlin_ui_rect *b = grec(h, 0, CANLIN_UI_P_PORT, n1, p1);
    CHECK(a && b);
    if (a && b)
        drag(h, cx(a), cy(a), cx(b), cy(b));
}

static unsigned edits(const harness *h)
{
    return count(h, "E ");
}

static int nk_has_text(harness *h, const char *s)
{
    const struct nk_command *cmd;
    struct nk_context *ctx = canlin_ui_view_nk(canlin_module_view(h->m));
    nk_foreach(cmd, ctx)
    {
        if (cmd->type == NK_COMMAND_TEXT &&
            strcmp(((const struct nk_command_text *)cmd)->string, s) == 0)
            return 1;
    }
    return 0;
}

static unsigned nk_circles(harness *h, canlin_theme_rgb c)
{
    const struct nk_command *cmd;
    struct nk_context *ctx = canlin_ui_view_nk(canlin_module_view(h->m));
    unsigned n = 0;
    nk_foreach(cmd, ctx)
    {
        if (cmd->type == NK_COMMAND_CIRCLE_FILLED) {
            const struct nk_command_circle_filled *f = (const void *)cmd;
            if (f->color.r == c.r && f->color.g == c.g && f->color.b == c.b)
                n++;
        }
    }
    return n;
}

/* ---- the test module ---- */

/* Nodes: A (out o:t), B (in i:t, in j:u, out p:t; body), C (in k, untyped).
 * Record port indices: A.o 0; B.i 0, B.j 1, B.p 2; C.k 0. One edge A.o -> B.i. */
#define GRAPH_G                                                                        \
    "{ nodes = {\n"                                                                    \
    "  { id = 'A', title = 'Alpha', x = 2, y = 2, outputs = { { id = 'o', label = 'out', type = 't' } } },\n" \
    "  { id = 'B', title = 'Beta', x = 20, y = 2,\n"                                   \
    "    inputs = { { id = 'i', label = 'in', type = 't' }, { id = 'j', label = 'j', type = 'u' } },\n" \
    "    outputs = { { id = 'p', type = 't' } },\n"                                     \
    "    body = { { kind = 'text', text = 'ok' }, { kind = 'meter', value = 0.5 } } },\n" \
    "  { id = 'C', title = 'Gamma', x = 2, y = 14, inputs = { { id = 'k' } } } },\n"   \
    "  edges = { { from = { 'A', 'o' }, to = { 'B', 'i' } } }, selected = %s }"

static const char module_fmt[] =
    "local M = {}\n"
    "local g = " GRAPH_G "\n"
    "local apply = %s\n"
    "local opts = %s\n"
    "local function ser(v)\n"
    "  if type(v) ~= 'table' then return tostring(v) end\n"
    "  local ks = {}\n"
    "  for k in pairs(v) do ks[#ks + 1] = tostring(k) end\n"
    "  table.sort(ks)\n"
    "  local o = {}\n"
    "  for _, k in ipairs(ks) do\n"
    "    local val = v[k]\n"
    "    if val == nil then val = v[tonumber(k)] end\n"
    "    o[#o + 1] = k .. '=' .. ser(val)\n"
    "  end\n"
    "  return '{' .. table.concat(o, ',') .. '}'\n"
    "end\n"
    "local function fmt(e)\n"
    "  if e.kind == 'move' then return 'move ' .. e.node .. ' ' .. e.x .. ' ' .. e.y end\n"
    "  if e.kind == 'add' then return 'add ' .. e.x .. ' ' .. e.y end\n"
    "  if e.kind == 'connect' then\n"
    "    return 'connect ' .. e.from[1] .. '.' .. e.from[2] .. '>' .. e.to[1] .. '.' .. e.to[2]\n"
    "  end\n"
    "  if e.kind == 'disconnect' then return 'disconnect ' .. e.edge end\n"
    "  if e.kind == 'select' then return 'select ' .. tostring(e.node or e.edge) end\n"
    "  return e.kind .. ' ' .. e.node\n"
    "end\n"
    "local function index_of(id)\n"
    "  for i, n in ipairs(g.nodes) do if n.id == id then return i end end\n"
    "end\n"
    "local function applyedit(e)\n"
    "  if e.kind == 'move' then local n = g.nodes[index_of(e.node)]; n.x = e.x; n.y = e.y\n"
    "  elseif e.kind == 'connect' then g.edges[#g.edges + 1] = { from = e.from, to = e.to }\n"
    "  elseif e.kind == 'disconnect' then table.remove(g.edges, e.edge); g.selected = nil\n"
    "  elseif e.kind == 'select' then g.selected = e.node or e.edge\n"
    "  elseif e.kind == 'delete' then\n"
    "    table.remove(g.nodes, index_of(e.node))\n"
    "    for i = #g.edges, 1, -1 do\n"
    "      local ed = g.edges[i]\n"
    "      if ed.from[1] == e.node or ed.to[1] == e.node then table.remove(g.edges, i) end\n"
    "    end\n"
    "    g.selected = nil\n"
    "  end\n"
    "end\n"
    "function M.draw(ui)\n"
    "  local before = ser(g)\n"
    "  local edits = ui:graph(g, opts)\n"
    "  if ser(g) ~= before then print('MUTATED') end\n"
    "  for _, e in ipairs(edits) do\n"
    "    print('E ' .. fmt(e))\n"
    "    if apply then applyedit(e) end\n"
    "  end\n"
    "  if apply and #edits > 0 then\n"
    "    print('N nodes=' .. #g.nodes .. ' edges=' .. #g.edges)\n"
    "    canlin.invalidate()\n"
    "  end\n"
    "end\n"
    "return M\n";

static char srcbuf[16384];

static const char *module_src(const char *selected, int apply, const char *opts)
{
    snprintf(srcbuf, sizeof srcbuf, module_fmt, selected, apply ? "true" : "false", opts);
    return srcbuf;
}

/* View 800x600 at DPI 1: em 16, padding 8; the canvas starts at (8, 8), pan 0, so graph
 * (x, y) em is screen (8 + 16x, 8 + 16y). */
static void base(harness *h, const char *selected)
{
    h_init(h, 1.0f, module_src(selected, 0, "{ id = 'flow' }"), 800, 600);
    CHECK(!has(h, "draw:"));
    CHECK(!has(h, "MUTATED"));
    clear_log(h);
}

/* ---- every edit kind from injected input ---- */

static void test_edit_kinds(void)
{
    harness h;
    const canlin_ui_rect *a;
    int ax, ay;

    base(&h, "nil");
    a = grec(&h, 0, CANLIN_UI_P_NODE, 0, 0);
    CHECK(a && a->x == 40 && a->y == 40 && a->w == 192);

    tap_title(&h, 1);
    CHECK(has(&h, "E select B\n") && edits(&h) == 1);

    /* move: one edit on release, rounded to 0.25 em (+2.5 em, +1.25 em; +3 px lost) */
    gap(&h);
    clear_log(&h);
    ax = a ? cx(a) : 0;
    ay = a ? a->y + 16 : 0;
    press(&h, ax, ay);
    input(&h, CANLIN_UI_IN_MOTION, ax + 20, ay + 10, 0);
    h_drain(&h);
    CHECK(edits(&h) == 0); /* no edit while dragging */
    a = grec(&h, 0, CANLIN_UI_P_NODE, 0, 0);
    CHECK(a && a->x == 60 && a->y == 50); /* drawn as a preview */
    input(&h, CANLIN_UI_IN_MOTION, 139, 105, 0);
    h_drain(&h);
    release(&h, 139, 105); /* start (136, 56): +3 px, +49 px */
    CHECK(has(&h, "E move A 2.25 5.0\n") && edits(&h) == 1);
    a = grec(&h, 0, CANLIN_UI_P_NODE, 0, 0);
    CHECK(a && a->x == 40 && a->y == 40); /* not applied by the module: back in place */

    /* connect: output -> input */
    gap(&h);
    clear_log(&h);
    drag_port(&h, 0, 0, 2, 0);
    CHECK(has(&h, "E connect A.o>C.k\n") && edits(&h) == 1);
    /* connect started on the input side */
    gap(&h);
    clear_log(&h);
    drag_port(&h, 2, 0, 0, 0);
    CHECK(has(&h, "E connect A.o>C.k\n") && edits(&h) == 1);

    /* drag an edge off its input onto another input: disconnect + connect */
    gap(&h);
    clear_log(&h);
    drag_port(&h, 1, 0, 2, 0);
    CHECK(has(&h, "E disconnect 1\nE connect A.o>C.k\n") && edits(&h) == 2);
    /* ... onto empty canvas: disconnect only */
    gap(&h);
    clear_log(&h);
    a = grec(&h, 0, CANLIN_UI_P_PORT, 1, 0);
    if (a)
        drag(&h, cx(a), cy(a), 488, 328);
    CHECK(has(&h, "E disconnect 1\n") && edits(&h) == 1);
    /* ... back onto the same input: nothing */
    gap(&h);
    clear_log(&h);
    a = grec(&h, 0, CANLIN_UI_P_PORT, 1, 0);
    ax = a ? cx(a) : 0;
    ay = a ? cy(a) : 0;
    press(&h, ax, ay);
    input(&h, CANLIN_UI_IN_MOTION, ax + 40, ay, 0);
    h_drain(&h);
    release(&h, ax, ay);
    CHECK(edits(&h) == 0);

    /* tap on the edge's curve: select edge 1 (A.o (232,92) -> B.i (328,92), straight) */
    gap(&h);
    clear_log(&h);
    tap(&h, 280, 92);
    CHECK(has(&h, "E select 1\n") && edits(&h) == 1);

    /* long press on empty canvas: add at (30, 20) em */
    gap(&h);
    clear_log(&h);
    hold(&h, 8 + 30 * 16 + 3, 8 + 20 * 16 + 1, CANLIN_UI_LONG_PRESS_MS);
    CHECK(has(&h, "E add 30.25 20.0\n") && edits(&h) == 1);
    /* just under the long-press time: a tap on empty canvas, nothing selected: no edit */
    gap(&h);
    clear_log(&h);
    hold(&h, 488, 328, CANLIN_UI_LONG_PRESS_MS - 1);
    CHECK(edits(&h) == 0);

    /* long press on a node: open */
    gap(&h);
    clear_log(&h);
    a = grec(&h, 0, CANLIN_UI_P_NODE, 0, 0);
    if (a)
        hold(&h, cx(a), a->y + 16, CANLIN_UI_LONG_PRESS_MS);
    CHECK(has(&h, "E open A\n") && edits(&h) == 1);
    /* double tap: select, then open */
    gap(&h);
    clear_log(&h);
    tap_title(&h, 2);
    canlin_ev_fake_clock_advance(&h.clock, CANLIN_UI_DOUBLE_TAP_MS - 100);
    tap_title(&h, 2);
    CHECK(has(&h, "E select C\nE open C\n") && edits(&h) == 2);
    /* two taps further apart: two selects, no open */
    gap(&h);
    clear_log(&h);
    tap_title(&h, 2);
    canlin_ev_fake_clock_advance(&h.clock, CANLIN_UI_DOUBLE_TAP_MS + 1);
    tap_title(&h, 2);
    CHECK(count(&h, "E select C\n") == 2 && !has(&h, "open"));

    /* keyboard: the graph has focus from the taps; Tab selects the first node */
    gap(&h);
    clear_log(&h);
    key(&h, CANLIN_UI_KEY_TAB);
    CHECK(has(&h, "E select A\n") && edits(&h) == 1);
    CHECK(!has(&h, "MUTATED") && !has(&h, "draw:"));
    h_done(&h);

    /* delete a selected node / disconnect a selected edge with Del */
    base(&h, "'B'");
    tap_title(&h, 1); /* already selected: focus only, no edit */
    CHECK(edits(&h) == 0);
    key(&h, CANLIN_UI_KEY_DELETE);
    CHECK(has(&h, "E delete B\n") && edits(&h) == 1);
    /* a press outside the graph takes its keyboard focus away */
    clear_log(&h);
    tap(&h, 400, 590);
    key(&h, CANLIN_UI_KEY_DELETE);
    CHECK(edits(&h) == 0);
    h_done(&h);

    base(&h, "1");
    tap(&h, 488, 328); /* empty: clears the selection (edit), takes focus */
    CHECK(has(&h, "E select nil\n"));
    clear_log(&h);
    key(&h, CANLIN_UI_KEY_DELETE);
    CHECK(has(&h, "E disconnect 1\n") && edits(&h) == 1);
    CHECK(!has(&h, "MUTATED") && !has(&h, "draw:"));
    h_done(&h);
}

/* ---- local rejections: no edit ---- */

static void test_rejections(void)
{
    harness h;
    canlin_theme t;
    const canlin_ui_rect *a, *b;
    canlin_theme_builtin(&t);

    base(&h, "nil");
    CHECK(nk_circles(&h, t.color[CANLIN_THEME_DANGER]) == 0);

    drag_port(&h, 0, 0, 1, 2); /* output -> output */
    CHECK(edits(&h) == 0);
    gap(&h);
    drag_port(&h, 2, 0, 1, 1); /* input -> input */
    CHECK(edits(&h) == 0);
    gap(&h);
    drag_port(&h, 1, 2, 1, 0); /* B.p -> B.i: same node (types match) */
    CHECK(edits(&h) == 0);
    gap(&h);
    drag_port(&h, 0, 0, 1, 0); /* A.o -> B.i: exists already */
    CHECK(edits(&h) == 0);

    /* type mismatch A.o:t -> B.j:u, shown in danger during the drag */
    gap(&h);
    a = grec(&h, 0, CANLIN_UI_P_PORT, 0, 0);
    b = grec(&h, 0, CANLIN_UI_P_PORT, 1, 1);
    CHECK(a && b);
    if (a && b) {
        int bx = cx(b), by = cy(b);
        press(&h, cx(a), cy(a));
        input(&h, CANLIN_UI_IN_MOTION, bx, by, 0);
        h_drain(&h);
        /* Was "== 1, B.j only": canlin#36 also marks A.o (own node) and B.i (A.o -> B.i
         * exists); per-port colours are asserted in test_drag_marking. */
        CHECK(nk_circles(&h, t.color[CANLIN_THEME_DANGER]) == 3);
        release(&h, bx, by);
        CHECK(nk_circles(&h, t.color[CANLIN_THEME_DANGER]) == 0);
    }
    CHECK(edits(&h) == 0);
    /* untyped C.k accepts A.o (control: the drag itself works) */
    gap(&h);
    drag_port(&h, 0, 0, 2, 0);
    CHECK(edits(&h) == 1);
    CHECK(!has(&h, "MUTATED") && !has(&h, "draw:"));
    h_done(&h);
}

/* ---- pan per id, fit, culling ---- */

static const char two_src[] =
    "local M = {}\n"
    "local g1 = { nodes = { { id = 'n', title = 'One', x = 2, y = 1 } } }\n"
    "local g2 = { nodes = { { id = 'n', title = 'Two', x = 2, y = 1 } } }\n"
    "function M.draw(ui)\n"
    "  local a = ui:graph(g1, { id = 'g1', height = 10 })\n"
    "  local b = ui:graph(g2, { id = 'g2', height = 10 })\n"
    "  if #a + #b > 0 then print('EDIT') end\n"
    "end\n"
    "return M\n";

static void test_pan(void)
{
    harness h;
    const canlin_ui_rect *n1, *n2;
    int x1, y1, x2, y2;
    h_init(&h, 1.0f, two_src, 800, 600);
    n1 = grec(&h, 0, CANLIN_UI_P_NODE, 0, 0);
    n2 = grec(&h, 1, CANLIN_UI_P_NODE, 0, 0);
    CHECK(n1 && n2);
    if (!n1 || !n2) {
        h_done(&h);
        return;
    }
    x1 = n1->x;
    y1 = n1->y;
    x2 = n2->x;
    y2 = n2->y;
    CHECK(y2 > y1 + 10 * 16); /* second canvas below the first */
    /* drag empty canvas of g1 by (+32, +16) px = (2, 1) em */
    drag(&h, 600, 100, 632, 116);
    n1 = grec(&h, 0, CANLIN_UI_P_NODE, 0, 0);
    n2 = grec(&h, 1, CANLIN_UI_P_NODE, 0, 0);
    CHECK(n1 && n1->x == x1 + 32 && n1->y == y1 + 16);
    CHECK(n2 && n2->x == x2 && n2->y == y2); /* the other graph's pan is its own */
    CHECK(!has(&h, "EDIT"));                 /* panning is state, not an edit */
    /* survives further redraws */
    input(&h, CANLIN_UI_IN_MOTION, 10, 590, 0);
    h_drain(&h);
    input(&h, CANLIN_UI_IN_MOTION, 11, 590, 0);
    h_drain(&h);
    n1 = grec(&h, 0, CANLIN_UI_P_NODE, 0, 0);
    CHECK(n1 && n1->x == x1 + 32 && n1->y == y1 + 16);
    /* pan g2 the other way */
    drag(&h, 600, y2 + 100, 568, y2 + 100);
    n1 = grec(&h, 0, CANLIN_UI_P_NODE, 0, 0);
    n2 = grec(&h, 1, CANLIN_UI_P_NODE, 0, 0);
    CHECK(n1 && n1->x == x1 + 32);
    CHECK(n2 && n2->x == x2 - 32);
    CHECK(!has(&h, "draw:"));
    h_done(&h);
}

static const char fit_src[] =
    "local M = {}\n"
    "local g = { nodes = { { id = 'far', title = 'Far', x = 100, y = 100,\n"
    "                        inputs = { { id = 'i' } } } } }\n"
    "function M.draw(ui) ui:graph(g, { fit = true, height = 20 }) end\n"
    "return M\n";

static void test_fit(void)
{
    harness h;
    const canlin_ui_rect *n;
    /* canvas (8, 8) 784 x 320 px; node 12 x 5 em = 192 x 80 px */
    h_init(&h, 1.0f, fit_src, 800, 600);
    n = grec(&h, 0, CANLIN_UI_P_NODE, 0, 0);
    CHECK(n != NULL);
    if (n) {
        CHECK(n->w == 192 && n->h == 80);
        CHECK(n->x == 8 + (784 - 192) / 2 && n->y == 8 + (320 - 80) / 2);
    }
    /* fit is edge-triggered: a pan after it is kept although fit stays true */
    drag(&h, 20, 20, 52, 20);
    input(&h, CANLIN_UI_IN_MOTION, 30, 30, 0); /* one more draw without a drag */
    h_drain(&h);
    n = grec(&h, 0, CANLIN_UI_P_NODE, 0, 0);
    CHECK(n && n->x == 8 + (784 - 192) / 2 + 32);
    CHECK(!has(&h, "draw:"));
    h_done(&h);
}

static const char cull_src[] =
    "local M = {}\n"
    "local g = { nodes = {\n"
    "  { id = 'near', title = 'NEARBY', x = 2, y = 2, outputs = { { id = 'o' } } },\n"
    "  { id = 'far', title = 'FARAWAY', x = 1000, y = 2, inputs = { { id = 'i' } } } },\n"
    "  edges = { { from = { 'near', 'o' }, to = { 'far', 'i' } } } }\n"
    "function M.draw(ui) ui:graph(g) end\n"
    "return M\n";

static void test_cull(void)
{
    harness h;
    h_init(&h, 1.0f, cull_src, 800, 600);
    CHECK(!has(&h, "draw:"));
    CHECK(grec(&h, 0, CANLIN_UI_P_NODE, 0, 0) != NULL);
    CHECK(grec(&h, 0, CANLIN_UI_P_NODE, 1, 0) == NULL);
    CHECK(grec(&h, 0, CANLIN_UI_P_PORT, 1, 0) == NULL);
    CHECK(nk_has_text(&h, "NEARBY"));
    CHECK(!nk_has_text(&h, "FARAWAY"));
    h_done(&h);
}

/* ---- port hit area ---- */

static void test_port_hit(void)
{
    static const float dpis[] = {1.0f, 1.5f, 2.0f};
    size_t k, i, n;
    for (k = 0; k < 3; k++) {
        harness h;
        const canlin_ui_rect *r, *a, *b;
        int min, ports = 0;
        h_init(&h, dpis[k], module_src("nil", 0, "{ id = 'flow', height = 30 }"), 1600, 1200);
        min = (int)(2.5f * canlin_ui_view_em(canlin_module_view(h.m)) + 0.999f);
        r = rec(&h, &n);
        for (i = 0; i < n; i++)
            if (r[i].kind == CANLIN_UI_W_GRAPH && r[i].part == CANLIN_UI_P_PORT) {
                ports++;
                CHECK(r[i].w >= min && r[i].h >= min);
            }
        CHECK(ports == 5);
        /* the corner of the target, well outside the drawn dot, starts the drag */
        a = grec(&h, 0, CANLIN_UI_P_PORT, 0, 0);
        b = grec(&h, 0, CANLIN_UI_P_PORT, 2, 0);
        CHECK(a && b);
        if (a && b)
            drag(&h, a->x + 1, a->y + 1, b->x + b->w - 2, b->y + b->h - 2);
        CHECK(has(&h, "E connect A.o>C.k\n"));
        h_done(&h);
    }
}

/* ---- validation and limits ---- */

typedef struct bad_case {
    const char *setup; /* Lua statements building g (and opts) */
    const char *msg;
} bad_case;

static const bad_case bad[] = {
    {"g = { nodes = 5 }", "nodes must be an array"},
    {"g = { nodes = { { title = 'x', x = 0, y = 0 } } }", "nodes[1].id must be a string"},
    {"g = { nodes = { { id = 'a', x = 0, y = 0 }, { id = 'a', x = 1, y = 1 } } }",
     "nodes[2].id is not unique"},
    {"g = { nodes = { { id = 'a', x = 0, y = 0, inputs = { { id = 'p' } },"
     " outputs = { { id = 'p' } } } } }",
     "is not unique in its node"},
    {"g = { nodes = { { id = 'a', x = 0, y = 0, outputs = { { id = 'o' } } } },"
     " edges = { { from = { 'a', 'o' }, to = { 'zz', 'i' } } } }",
     "edges[1].to names an unknown node"},
    {"g = { nodes = { { id = 'a', x = 0, y = 0, outputs = { { id = 'o' } } },"
     " { id = 'b', x = 0, y = 0, inputs = { { id = 'i' } } } },"
     " edges = { { from = { 'a', 'o' }, to = { 'b', 'nope' } } } }",
     "edges[1].to names an unknown port"},
    {"g = { nodes = { { id = 'a', x = 0, y = 0, inputs = { { id = 'i' } } },"
     " { id = 'b', x = 0, y = 0, inputs = { { id = 'i' } } } },"
     " edges = { { from = { 'a', 'i' }, to = { 'b', 'i' } } } }",
     "edges[1].from must name an output port"},
    {"g = { nodes = { { id = 'a', title = string.rep('x', 257), x = 0, y = 0 } } }",
     "nodes[1].title exceeds 256 bytes"},
    {"g = { nodes = { { id = 'a', title = '\\xff', x = 0, y = 0 } } }",
     "nodes[1].title is not valid UTF-8"},
    {"g = { nodes = { { id = 'a', x = 'left', y = 0 } } }", "nodes[1].x must be a number"},
    {"g = { nodes = { { id = 'a', x = 1e9, y = 0 } } }", "nodes[1].x outside"},
    {"local p = {} for i = 1, 17 do p[i] = { id = 'p' .. i } end\n"
     "g = { nodes = { { id = 'a', x = 0, y = 0, inputs = p } } }",
     "nodes[1] has more than 16 ports"},
    {"local b = {} for i = 1, 5 do b[i] = { kind = 'text', text = 'x' } end\n"
     "g = { nodes = { { id = 'a', x = 0, y = 0, body = b } } }",
     "has more than 4 rows"},
    {"g = { nodes = { { id = 'a', x = 0, y = 0, body = { { kind = 'lua' } } } } }",
     "kind must be \"text\" or \"meter\""},
    {"g = { nodes = { { id = 'a', x = 0, y = 0 } }, selected = 'b' }",
     "selected names an unknown node"},
    {"g = { nodes = {}, selected = 3 }", "selected edge index out of range"},
    {"g = { nodes = {} } opts = { height = 1 }", "opts.height must be"},
    {"local n = {} for i = 1, 257 do n[i] = { id = 'n' .. i, x = 0, y = 0 } end\n"
     "g = { nodes = n }",
     "more than 256 nodes"},
    {"local e = {} for i = 1, 1025 do e[i] = {} end\n"
     "g = { nodes = {}, edges = e }",
     "more than 1024 edges"},
};

static void test_validation(void)
{
    size_t i;
    char src[4096];
    for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        harness h;
        snprintf(src, sizeof src,
                 "local M = {}\n"
                 "local g, opts\n"
                 "%s\n"
                 "function M.draw(ui) ui:graph(g, opts) print('drawn') end\n"
                 "return M\n",
                 bad[i].setup);
        h_init(&h, 1.0f, src, 800, 600);
        if (!has(&h, "draw:") || !has(&h, bad[i].msg) || has(&h, "drawn"))
            fprintf(stderr, "validation case %u: %s\n", (unsigned)i, h.log);
        CHECK(has(&h, "draw:"));
        CHECK(has(&h, bad[i].msg));
        CHECK(!has(&h, "drawn"));
        h_done(&h);
    }
    /* too many graphs in one draw */
    {
        harness h;
        h_init(&h, 1.0f,
               "local M = {}\n"
               "function M.draw(ui)\n"
               "  for i = 1, 9 do ui:graph({ nodes = {} }, { id = 'g' .. i, height = 5 }) end\n"
               "end\n"
               "return M\n",
               800, 600);
        CHECK(has(&h, "more than 8 graphs in one draw"));
        h_done(&h);
    }
}

static const char limits_src[] =
    "local M = {}\n"
    "local n, e = {}, {}\n"
    "for i = 1, 256 do\n"
    "  n[i] = { id = 'n' .. i, title = 'Node ' .. i, x = (i %% 16) * 14, y = (i // 16) * 12,\n"
    "    inputs = { { id = 'i1', type = 't' }, { id = 'i2' } },\n"
    "    outputs = { { id = 'o1', type = 't' }, { id = 'o2' } },\n"
    "    body = { { kind = 'text', text = 'status' }, { kind = 'meter', value = 0.3 } } }\n"
    "end\n"
    "for k = 1, 1024 do\n"
    "  local a, b = (k - 1) %% 256 + 1, (k * 7) %% 256 + 1\n"
    "  e[k] = { from = { 'n' .. a, 'o' .. (k %% 2 + 1) }, to = { 'n' .. b, 'i' .. (k %% 2 + 1) } }\n"
    "end\n"
    "local g = { nodes = n, edges = e, selected = 'n1' }\n"
    "function M.draw(ui)\n"
    "  local ed = ui:graph(g, { height = %d })\n"
    "  print('drawn ' .. #ed)\n"
    "end\n"
    "return M\n";

static void test_limits(void)
{
    harness h;
    char src[2048];
    size_t n, i;
    const canlin_ui_rect *r;
    unsigned nodes = 0;
    snprintf(src, sizeof src, limits_src, 60);
    h_init(&h, 1.0f, src, 1024, 1000);
    CHECK(has(&h, "drawn 0"));
    CHECK(!has(&h, "draw:") && !has(&h, "fault"));
    r = rec(&h, &n);
    for (i = 0; i < n; i++)
        if (r[i].kind == CANLIN_UI_W_GRAPH && r[i].part == CANLIN_UI_P_NODE)
            nodes++;
    CHECK(nodes > 10 && nodes < 256); /* visible ones only */
    /* input frames keep working at the limit */
    clear_log(&h);
    drag(&h, 216, 188, 116, 88); /* between nodes: pans */
    CHECK(count(&h, "drawn 0") == 3);
    CHECK(!has(&h, "draw:"));
    if (has(&h, "draw:"))
        fprintf(stderr, "%s\n", h.log);
    h_done(&h);
}

/* ---- end to end: a flow module that applies its own edits ---- */

static void test_e2e(void)
{
    harness h;
    const canlin_ui_rect *a;
    h_init(&h, 1.0f, module_src("nil", 1, "{ id = 'flow' }"), 800, 600);
    clear_log(&h);
    a = grec(&h, 0, CANLIN_UI_P_NODE, 0, 0);
    CHECK(a != NULL);
    if (a)
        drag(&h, cx(a), a->y + 16, cx(a) + 40, a->y + 16 + 20);
    CHECK(has(&h, "E move A 4.5 3.25\n"));
    a = grec(&h, 0, CANLIN_UI_P_NODE, 0, 0);
    CHECK(a && a->x == 8 + 72 && a->y == 8 + 52); /* the module applied it and redrew */

    gap(&h);
    clear_log(&h);
    drag_port(&h, 0, 0, 2, 0);
    CHECK(has(&h, "E connect A.o>C.k\n") && has(&h, "N nodes=3 edges=2"));
    /* the new edge now exists: the same drag is a duplicate and yields nothing */
    gap(&h);
    clear_log(&h);
    drag_port(&h, 0, 0, 2, 0);
    CHECK(!has(&h, "E "));

    gap(&h);
    clear_log(&h);
    tap_title(&h, 0);
    CHECK(has(&h, "E select A\n"));
    key(&h, CANLIN_UI_KEY_DELETE);
    CHECK(has(&h, "E delete A\n") && has(&h, "N nodes=2 edges=0"));
    CHECK(grec(&h, 0, CANLIN_UI_P_NODE, 2, 0) == NULL);
    CHECK(!has(&h, "MUTATED") && !has(&h, "draw:"));
    h_done(&h);
}

/* ---- rasterized: the command list the graph builds renders ---- */

static int px_is(const unsigned char *px, int w, int x, int y, canlin_theme_rgb c)
{
    const unsigned char *p = px + ((size_t)y * (size_t)w + (size_t)x) * 3;
    return p[0] == c.r && p[1] == c.g && p[2] == c.b;
}

static void test_render(void)
{
    enum { W = 800, H = 600 };
    static unsigned char px[W * H * 3];
    harness h;
    canlin_render r;
    const canlin_render_font *f;
    canlin_theme t;
    canlin_theme_builtin(&t);
    base(&h, "1"); /* edge 1 selected: drawn in accent */
    CHECK(canlin_render_init(&r, px, W, H, W * 3, NULL, NULL) == 0);
    f = canlin_ui_view_font(canlin_module_view(h.m));
    CHECK(f != NULL);
    if (f)
        canlin_render_set_atlas(&r, f->alpha, f->w, f->h);
    canlin_render_begin(&r);
    canlin_render_clear(&r, nk_rgb(255, 0, 255));
    canlin_ui_view_render(canlin_module_view(h.m), &r);
    canlin_render_end(&r);
    CHECK(px_is(px, W, 280, 92, t.color[CANLIN_THEME_ACCENT]));   /* the edge A.o -> B.i */
    CHECK(px_is(px, W, 60, 100, t.color[CANLIN_THEME_SURFACE]));  /* inside node A */
    CHECK(px_is(px, W, 232, 92, t.color[CANLIN_THEME_ACCENT]));   /* port dot A.o */
    CHECK(px_is(px, W, 400, 590, t.color[CANLIN_THEME_BACKGROUND])); /* below the canvas */
    h_done(&h);
}

/* canlin#36: the title separator of a node stays inside the node rect (spec §6.2), at
 * 100/150/200 %. Beside the node's right border no pixel carries the separator's solid
 * muted colour (the grid is muted at alpha 96, so never exactly muted). */
static void test_node_separator_inside(void)
{
    static const int scales[] = {100, 150, 200};
    enum { W = 1600, H = 1200 };
    static unsigned char px[W * H * 3];
    size_t k;
    canlin_theme t;
    canlin_theme_builtin(&t);
    for (k = 0; k < 3; k++) {
        int s = scales[k], w = 8 * s, vh = 6 * s;
        harness h;
        canlin_render r;
        const canlin_ui_rect *a;
        float em;
        h_init(&h, 1.0f, module_src("nil", 0, "{ id = 'flow' }"), w, vh);
        CHECK(canlin_ui_output_set_scale(h.out, s) == CANLIN_UI_OK);
        h_drain(&h);
        em = canlin_ui_view_em(canlin_module_view(h.m));
        CHECK(em == 16.0f * (float)s / 100.0f);
        CHECK(canlin_render_init(&r, px, w, vh, (size_t)w * 3, NULL, NULL) == 0);
        canlin_render_begin(&r);
        canlin_render_clear(&r, nk_rgb(255, 0, 255));
        canlin_ui_view_render(canlin_module_view(h.m), &r);
        canlin_render_end(&r);
        a = grec(&h, 0, CANLIN_UI_P_NODE, 0, 0); /* node A: no port on the title row */
        CHECK(a != NULL);
        if (a) {
            int sy = a->y + (int)(2.0f * em), th = em / 8.0f < 1.0f ? 1 : (int)(em / 8.0f);
            int x, y, out = 0;
            CHECK(px_is(px, w, a->x + a->w / 2, sy, t.color[CANLIN_THEME_MUTED]));
            for (y = sy - 2 * th; y <= sy + 2 * th; y++)
                for (x = a->x + a->w; x < a->x + a->w + 6; x++)
                    out += px_is(px, w, x, y, t.color[CANLIN_THEME_MUTED]);
            CHECK(out == 0);
        }
        h_done(&h);
    }
}

/* Filled circle of colour c whose centre lies in port target r. */
static int dot_is(harness *h, const canlin_ui_rect *r, canlin_theme_rgb c)
{
    const struct nk_command *cmd;
    struct nk_context *ctx = canlin_ui_view_nk(canlin_module_view(h->m));
    if (r == NULL)
        return 0;
    nk_foreach(cmd, ctx)
    {
        if (cmd->type == NK_COMMAND_CIRCLE_FILLED) {
            const struct nk_command_circle_filled *f = (const void *)cmd;
            int x = f->x + f->w / 2, y = f->y + f->h / 2;
            if (x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h)
                return f->color.r == c.r && f->color.g == c.g && f->color.b == c.b;
        }
    }
    return 0;
}

/* canlin#36: during a port drag the preview marks every locally rejected drop visible
 * beforehand (spec §5.2) in danger: the dragging node's own ports, an input that already
 * carries the identical edge, and a type clash. A lifted edge does not mark its own
 * input (dropping it back is a no-op). Visual only: the drops still produce no edit. */
static void test_drag_marking(void)
{
    harness h;
    canlin_theme t;
    canlin_theme_rgb dg, ok;
    const canlin_ui_rect *ao, *bi, *bj, *bp, *ck;
    canlin_theme_builtin(&t);
    dg = t.color[CANLIN_THEME_DANGER];
    ok = t.color[CANLIN_THEME_ACCENT];

    base(&h, "nil");
    ao = grec(&h, 0, CANLIN_UI_P_PORT, 0, 0);
    bi = grec(&h, 0, CANLIN_UI_P_PORT, 1, 0);
    bj = grec(&h, 0, CANLIN_UI_P_PORT, 1, 1);
    bp = grec(&h, 0, CANLIN_UI_P_PORT, 1, 2);
    ck = grec(&h, 0, CANLIN_UI_P_PORT, 2, 0);
    CHECK(ao && bi && bj && bp && ck);
    if (!(ao && bi && bj && bp && ck)) {
        h_done(&h);
        return;
    }

    /* from A.o (edge A.o -> B.i exists) */
    press(&h, cx(ao), cy(ao));
    input(&h, CANLIN_UI_IN_MOTION, 488, 328, 0);
    h_drain(&h);
    CHECK(dot_is(&h, ao, dg)); /* own node */
    CHECK(dot_is(&h, bi, dg)); /* identical edge exists */
    CHECK(dot_is(&h, bj, dg)); /* type t vs u */
    CHECK(dot_is(&h, bp, ok)); /* output: not marked (unchanged scope) */
    CHECK(dot_is(&h, ck, ok)); /* untyped input: accepted */
    release(&h, cx(bi), cy(bi));
    CHECK(edits(&h) == 0); /* the marked drop is still rejected without an edit */
    CHECK(nk_circles(&h, dg) == 0);
    gap(&h);

    /* from B.i: lifts A.o -> B.i, the source is A.o; B.i itself is not marked */
    press(&h, cx(bi), cy(bi));
    input(&h, CANLIN_UI_IN_MOTION, 488, 328, 0);
    h_drain(&h);
    CHECK(dot_is(&h, ao, dg));
    CHECK(dot_is(&h, bi, ok));
    CHECK(dot_is(&h, bj, dg));
    CHECK(dot_is(&h, ck, ok));
    release(&h, cx(bi), cy(bi)); /* dropped back: nothing */
    CHECK(edits(&h) == 0);
    gap(&h);

    /* from C.k (input, no edge): its own port is marked, A.o and B.p are not */
    press(&h, cx(ck), cy(ck));
    input(&h, CANLIN_UI_IN_MOTION, 488, 328, 0);
    h_drain(&h);
    CHECK(dot_is(&h, ck, dg));
    CHECK(dot_is(&h, ao, ok));
    CHECK(dot_is(&h, bp, ok));
    release(&h, 488, 328);
    CHECK(edits(&h) == 0);
    CHECK(!has(&h, "MUTATED") && !has(&h, "draw:"));
    h_done(&h);
}

/* ---- input through the scheduler (canlin#27) ---- */

/* Fills the scheduler queue with text edges, then posts a release that is refused:
 * CANLIN_EV_CTL_INPUT_OVERFLOW is raised. */
static void saturate_then_release(harness *h, int x, int y)
{
    canlin_ui_input in;
    unsigned n = 0;
    memset(&in, 0, sizeof in);
    in.type = CANLIN_UI_IN_TEXT;
    in.cp = 'q';
    while (canlin_module_post_input(h->m, &in) == CANLIN_MODULE_OK && n < 1000)
        n++;
    CHECK(n > 0 && n <= CANLIN_EV_MOD_MAX_ITEMS);
    memset(&in, 0, sizeof in);
    in.type = CANLIN_UI_IN_BUTTON;
    in.x = x;
    in.y = y;
    CHECK(canlin_module_post_input(h->m, &in) == CANLIN_MODULE_EFULL);
}

/* An overflow during a node drag or a pan cancels the gesture: no edit, the preview and
 * the pan return to where they started; the next gesture works. */
static void test_overflow_cancels_drag(void)
{
    harness h;
    const canlin_ui_rect *a;
    int ax, ay;

    base(&h, "nil");
    a = grec(&h, 0, CANLIN_UI_P_NODE, 0, 0);
    CHECK(a && a->x == 40 && a->y == 40);
    ax = a ? cx(a) : 0;
    ay = a ? a->y + 16 : 0;
    press(&h, ax, ay);
    input(&h, CANLIN_UI_IN_MOTION, ax + 40, ay + 20, 0);
    h_drain(&h);
    a = grec(&h, 0, CANLIN_UI_P_NODE, 0, 0);
    CHECK(a && a->x == 80 && a->y == 60); /* drag preview */
    saturate_then_release(&h, ax + 40, ay + 20);
    h_drain(&h);
    CHECK(edits(&h) == 0);
    CHECK(!has(&h, "MUTATED"));
    a = grec(&h, 0, CANLIN_UI_P_NODE, 0, 0);
    CHECK(a && a->x == 40 && a->y == 40); /* preview dropped */
    /* A late release edge after the gap does not revive the gesture. */
    release(&h, ax + 40, ay + 20);
    CHECK(edits(&h) == 0);

    /* Pan on empty canvas, cancelled: the pan snaps back. */
    gap(&h);
    press(&h, 488, 328);
    input(&h, CANLIN_UI_IN_MOTION, 520, 344, 0);
    h_drain(&h);
    a = grec(&h, 0, CANLIN_UI_P_NODE, 0, 0);
    CHECK(a && a->x == 72 && a->y == 56);
    saturate_then_release(&h, 520, 344);
    h_drain(&h);
    a = grec(&h, 0, CANLIN_UI_P_NODE, 0, 0);
    CHECK(a && a->x == 40 && a->y == 40);
    CHECK(edits(&h) == 0);

    /* Input after the gap works normally. */
    gap(&h);
    tap_title(&h, 1);
    CHECK(has(&h, "E select B\n") && edits(&h) == 1);
    h_done(&h);
}

/* Edges carry the time they were posted, not delivered: press and release posted
 * CANLIN_UI_LONG_PRESS_MS apart but delivered in one drain at one clock value are a long
 * press, and a quick tap delivered long after it happened stays a tap. */
static void test_post_time_stamps(void)
{
    harness h;
    const canlin_ui_rect *a;
    int ax, ay;

    base(&h, "nil");
    a = grec(&h, 0, CANLIN_UI_P_NODE, 0, 0);
    CHECK(a != NULL);
    ax = a ? cx(a) : 0;
    ay = a ? a->y + 16 : 0;
    input(&h, CANLIN_UI_IN_MOTION, ax, ay, 0);
    input(&h, CANLIN_UI_IN_BUTTON, ax, ay, 1);
    canlin_ev_fake_clock_advance(&h.clock, CANLIN_UI_LONG_PRESS_MS);
    input(&h, CANLIN_UI_IN_BUTTON, ax, ay, 0);
    h_drain(&h);
    CHECK(has(&h, "E open A\n") && edits(&h) == 1);

    gap(&h);
    clear_log(&h);
    input(&h, CANLIN_UI_IN_BUTTON, ax, ay, 1);
    input(&h, CANLIN_UI_IN_BUTTON, ax, ay, 0);
    canlin_ev_fake_clock_advance(&h.clock, 4 * CANLIN_UI_LONG_PRESS_MS);
    h_drain(&h);
    CHECK(has(&h, "E select A\n") && edits(&h) == 1);
    h_done(&h);
}

static void sec(harness *h, int button, int x, int y)
{
    canlin_ui_input in;
    memset(&in, 0, sizeof in);
    in.type = CANLIN_UI_IN_MOTION;
    in.x = x;
    in.y = y;
    CHECK(canlin_module_post_input(h->m, &in) == CANLIN_MODULE_OK);
    in.type = CANLIN_UI_IN_BUTTON;
    in.button = button;
    in.down = 1;
    CHECK(canlin_module_post_input(h->m, &in) == CANLIN_MODULE_OK);
    h_drain(h);
    in.down = 0;
    CHECK(canlin_module_post_input(h->m, &in) == CANLIN_MODULE_OK);
    h_drain(h);
}

/* canlin#32, spec §5.2: a secondary click on empty canvas is the context action `add` at
 * that point (one edit, at the press); on nodes, ports and edges it does nothing, and
 * middle does nothing. A secondary press during a primary gesture is ignored. */
static void test_secondary(void)
{
    harness h;
    const canlin_ui_rect *a, *p;
    base(&h, "nil");
    sec(&h, CANLIN_UI_BUTTON_SECONDARY, 8 + 30 * 16 + 3, 8 + 20 * 16 + 1);
    CHECK(has(&h, "E add 30.25 20.0\n") && edits(&h) == 1);
    clear_log(&h);
    a = grec(&h, 0, CANLIN_UI_P_NODE, 0, 0);
    p = grec(&h, 0, CANLIN_UI_P_PORT, 1, 1);
    CHECK(a && p);
    if (a && p) {
        sec(&h, CANLIN_UI_BUTTON_SECONDARY, cx(a), a->y + 16);
        sec(&h, CANLIN_UI_BUTTON_SECONDARY, cx(p), cy(p));
    }
    sec(&h, CANLIN_UI_BUTTON_MIDDLE, 8 + 30 * 16, 8 + 20 * 16);
    CHECK(edits(&h) == 0);
    /* Held primary pan: secondary press ignored, the pan's release still a plain tap. */
    press(&h, 488, 328);
    sec(&h, CANLIN_UI_BUTTON_SECONDARY, 488, 328);
    release(&h, 488, 328);
    CHECK(edits(&h) == 0);
    /* Primary input works normally afterwards. */
    gap(&h);
    tap_title(&h, 2);
    CHECK(has(&h, "E select C\n"));
    h_done(&h);
}

static unsigned timers(harness *h)
{
    canlin_ev_stats st;
    int slot = canlin_ev_module_slot(h->ev, canlin_module_gen(h->m));
    CHECK(canlin_ev_get_stats(h->ev, slot, &st) == CANLIN_EV_OK);
    return st.timers;
}

static const canlin_ui_rect *hold_mark(harness *h)
{
    size_t n, i;
    const canlin_ui_rect *r = rec(h, &n);
    for (i = 0; i < n; i++)
        if (r[i].kind == CANLIN_UI_W_GRAPH && r[i].part == CANLIN_UI_P_HOLD)
            return &r[i];
    return NULL;
}

/* canlin#33: a press held in place shows long-press feedback once CANLIN_UI_LONG_PRESS_MS
 * has elapsed, with no further input: a core timer wakes the view, and Lua never sees
 * it. Recognition stays at release (one open/add). Movement or release cancel it. */
static void test_long_press_feedback(void)
{
    static char src[sizeof srcbuf + 128];
    harness h;
    const canlin_ui_rect *a, *m;
    int ax, ay;
    char *end;

    snprintf(src, sizeof src, "%s", module_src("nil", 0, "{ id = 'flow' }"));
    end = strstr(src, "return M\n");
    CHECK(end != NULL);
    if (end)
        snprintf(end, sizeof src - (size_t)(end - src),
                 "function M.event(n, p) print('EV ' .. tostring(n)) end\nreturn M\n");
    h_init(&h, 1.0f, src, 800, 600);
    clear_log(&h);
    a = grec(&h, 0, CANLIN_UI_P_NODE, 0, 0);
    CHECK(a != NULL);
    ax = a ? cx(a) : 0;
    ay = a ? a->y + 16 : 0;

    /* Held on node A: nothing yet, one core timer armed. */
    press(&h, ax, ay);
    CHECK(hold_mark(&h) == NULL && timers(&h) == 1);
    canlin_ev_fake_clock_advance(&h.clock, CANLIN_UI_LONG_PRESS_MS - 1);
    h_drain(&h);
    CHECK(hold_mark(&h) == NULL);
    canlin_ev_fake_clock_advance(&h.clock, 1);
    h_drain(&h);
    m = hold_mark(&h);
    CHECK(m != NULL && m->off == 0);
    if (m && a)
        CHECK(m->x < a->x && m->y < a->y && m->x + m->w > a->x + a->w); /* ring around A */
    CHECK(timers(&h) == 0 && edits(&h) == 0 && !has(&h, "EV "));
    release(&h, ax, ay);
    CHECK(has(&h, "E open A\n") && edits(&h) == 1);
    CHECK(hold_mark(&h) == NULL && !has(&h, "EV "));

    /* Held on empty canvas: a marker at the press point, then add at release. */
    gap(&h);
    clear_log(&h);
    press(&h, 488, 328);
    canlin_ev_fake_clock_advance(&h.clock, CANLIN_UI_LONG_PRESS_MS);
    h_drain(&h);
    m = hold_mark(&h);
    CHECK(m != NULL && m->off == CANLIN_UI_HOLD_CANVAS);
    if (m)
        CHECK(m->x < 488 && m->x + m->w > 488 && m->y < 328 && m->y + m->h > 328);
    release(&h, 488, 328);
    CHECK(has(&h, "E add ") && edits(&h) == 1 && hold_mark(&h) == NULL);

    /* Movement past the slop cancels: timer gone, no feedback, a move instead. */
    gap(&h);
    clear_log(&h);
    press(&h, ax, ay);
    CHECK(timers(&h) == 1);
    input(&h, CANLIN_UI_IN_MOTION, ax + 40, ay, 0);
    h_drain(&h);
    CHECK(timers(&h) == 0);
    canlin_ev_fake_clock_advance(&h.clock, CANLIN_UI_LONG_PRESS_MS);
    h_drain(&h);
    CHECK(hold_mark(&h) == NULL);
    release(&h, ax + 40, ay);
    CHECK(has(&h, "E move A ") && edits(&h) == 1);

    /* Released before the deadline: timer cancelled, plain tap. */
    gap(&h);
    clear_log(&h);
    a = grec(&h, 0, CANLIN_UI_P_NODE, 1, 0);
    CHECK(a != NULL);
    if (a) {
        press(&h, cx(a), a->y + 16);
        canlin_ev_fake_clock_advance(&h.clock, 100);
        release(&h, cx(a), a->y + 16);
    }
    CHECK(timers(&h) == 0);
    canlin_ev_fake_clock_advance(&h.clock, CANLIN_UI_LONG_PRESS_MS);
    h_drain(&h);
    CHECK(hold_mark(&h) == NULL && has(&h, "E select B\n") && edits(&h) == 1);
    CHECK(!has(&h, "EV "));
    h_done(&h);
}

static void post_try(harness *h, const char *name)
{
    static canlin_module_event e;
    e.name = name;
    e.value = NULL;
    CHECK(canlin_ev_post(h->ev, canlin_module_gen(h->m), CANLIN_MODULE_SOURCE_EVENT, 0, 0, 0,
                         &e, 0, NULL) == CANLIN_EV_OK);
    h_drain(h);
}

/* canlin#46: the core-owned long-press timer is not the module's. Its handle is the next
 * serial after the module's own timer T (the only other timer of the generation), and
 * canlin.cancel refuses it as "stale" without touching it: the feedback still appears.
 * The module's own handle stays cancellable. */
static void test_cancel_refuses_core_timer(void)
{
    static char src[sizeof srcbuf + 512];
    harness h;
    const canlin_ui_rect *a;
    const char *t;
    unsigned long long th = 0;
    char *end;

    snprintf(src, sizeof src, "%s", module_src("nil", 0, "{ id = 'flow' }"));
    end = strstr(src, "return M\n");
    CHECK(end != NULL);
    if (end)
        snprintf(end, sizeof src - (size_t)(end - src),
                 "local T\n"
                 "function M.init() T = canlin.timer.after(1000000); print('T ' .. T) end\n"
                 "function M.event(n)\n"
                 "  local h = n == 'mine' and T or T + 1\n"
                 "  local r, c = canlin.cancel(h)\n"
                 "  print('C ' .. n .. ' ' .. tostring(r) .. ' ' .. tostring(c))\n"
                 "end\n"
                 "return M\n");
    h_init(&h, 1.0f, src, 800, 600);
    t = strstr(h.log, "T ");
    CHECK(t != NULL && sscanf(t, "T %llu", &th) == 1 && th != 0);
    CHECK(timers(&h) == 1);
    a = grec(&h, 0, CANLIN_UI_P_NODE, 0, 0);
    CHECK(a != NULL);
    if (a)
        press(&h, cx(a), a->y + 16);
    CHECK(timers(&h) == 2); /* T and the core's long-press timer, serial T + 1 */
    post_try(&h, "core");
    CHECK(has(&h, "C core nil stale\n"));
    CHECK(timers(&h) == 2);
    canlin_ev_fake_clock_advance(&h.clock, CANLIN_UI_LONG_PRESS_MS);
    h_drain(&h);
    CHECK(hold_mark(&h) != NULL && timers(&h) == 1); /* fired as a draw, not refused */
    CHECK(canlin_ev_cancel(h.ev, canlin_module_gen(h.m), (canlin_ev_handle)th + 1) ==
          CANLIN_EV_ALREADY); /* it was issued: T + 1 was the core timer */
    if (a)
        release(&h, cx(a), a->y + 16);
    CHECK(has(&h, "E open A\n"));
    post_try(&h, "mine");
    CHECK(has(&h, "C mine cancelled nil\n") && timers(&h) == 0);
    h_done(&h);
}

int main(void)
{
    test_edit_kinds();
    test_rejections();
    test_pan();
    test_fit();
    test_cull();
    test_port_hit();
    test_validation();
    test_limits();
    test_e2e();
    test_render();
    test_node_separator_inside();
    test_drag_marking();
    test_overflow_cancels_drag();
    test_post_time_stamps();
    test_secondary();
    test_long_press_feedback();
    test_cancel_refuses_core_timer();
    printf("test_graph: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
