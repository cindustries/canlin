/* Assistant-view binding contract tests (module roles spec §3; UI spec §7; core API
 * §3.1): the privileged Lua surface a module with the assistant-view capability uses.
 *
 * The module's own Lua code runs in the real sandbox and the real scheduler: it draws
 * the model view into its own view and picks up the submission. A module without the
 * capability never sees canlin.view (it is nil); a revoked grant answers denied, never
 * a pretend success. Exits 0 on success. */
#include "canlin/assistant_binding.h"
#include "canlin/module.h"
#include "canlin/theme.h"
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

#define CHECK_STR(a, b)                                                            \
    do {                                                                           \
        checks++;                                                                  \
        if (strcmp((a), (b)) != 0) {                                               \
            fprintf(stderr, "%s:%d: got  %s\n%*swant %s\n", __FILE__, __LINE__, (a), \
                    (int)strlen(__FILE__) + 6, "", (b));                           \
            failures++;                                                            \
        }                                                                          \
    } while (0)

/* ---- log helpers ---- */

static int has(const char *log, const char *s)
{
    return strstr(log, s) != NULL;
}

static int count(const char *log, const char *s)
{
    int n = 0;
    size_t l = strlen(s);
    const char *p = log;
    while ((p = strstr(p, s)) != NULL) {
        n++;
        p += l;
    }
    return n;
}

/* ---- harness ---- */

#define LOGMAX 8192

typedef struct logbuf {
    char buf[LOGMAX];
    size_t len;
} logbuf;

typedef struct harness {
    canlin_ev_fake_clock clock;
    canlin_ev *ev;
    canlin_module *m;
    logbuf log;
    canlin_ui *ui;
    canlin_ui_output *out;
    canlin_assistant_view *av;
    canlin_assistant_binding bind;
    int w, h;
} harness;

static void log_fn(void *ud, const char *msg, size_t len)
{
    logbuf *l = ud;
    if (len + 1 > sizeof l->buf - l->len - 1)
        return;
    memcpy(l->buf + l->len, msg, len);
    l->len += len;
    l->buf[l->len++] = '\n';
    l->buf[l->len] = 0;
}

static void on_deliver(void *ud, canlin_ev *ev, const canlin_ev_delivery *d)
{
    harness *h = ud;
    (void)ev;
    if (h->m && canlin_module_gen(h->m) == d->gen)
        canlin_module_deliver(h->m, d);
}

static void h_init(harness *h, int w, int hh)
{
    canlin_ev_clock c;
    canlin_ev_callbacks cb;
    canlin_theme t;
    canlin_ui_output_desc d;
    memset(h, 0, sizeof *h);
    h->w = w;
    h->h = hh;
    c = canlin_ev_fake_clock_bind(&h->clock);
    memset(&cb, 0, sizeof cb);
    cb.deliver = on_deliver;
    h->ev = canlin_ev_new(&c, &cb, h);
    canlin_theme_builtin(&t);
    h->ui = canlin_ui_new(&t);
    d.id = "bind-test";
    d.w = w;
    d.h = hh;
    d.dpi = 1.0f;
    d.scale = 100;
    h->out = canlin_ui_output_new(&d, NULL);
    h->av = canlin_assistant_view_new();
    h->bind.view = h->av;
    h->bind.granted = 1;
    if (!h->ev || !h->ui || !h->out || !h->av) {
        fprintf(stderr, "harness init failed\n");
        exit(1);
    }
}

static void h_done(harness *h)
{
    canlin_ev_stats st;
    if (h->m)
        canlin_module_free(h->m);
    canlin_ev_get_stats(h->ev, -1, &st);
    CHECK(st.items == 0 && st.bytes == 0 && st.ops == 0 && st.timers == 0);
    CHECK(canlin_ev_free(h->ev) == CANLIN_EV_OK);
    canlin_assistant_view_free(h->av);
    CHECK(canlin_ui_output_free(h->out) == CANLIN_UI_OK);
    canlin_ui_free(h->ui);
}

/* Load the module with (cap) the assistant-view capability granted. */
static int h_load(harness *h, const char *src, int cap)
{
    canlin_module_source s;
    canlin_sandbox_config cfg;
    int err = 12345;
    memset(&s, 0, sizeof s);
    memset(&cfg, 0, sizeof cfg);
    s.name = "assistant";
    s.version = "1";
    s.main = src;
    s.main_len = strlen(src);
    cfg.log = log_fn;
    cfg.ud = &h->log;
    if (cap) {
        s.assistant = canlin_assistant_binding_install;
        s.assistant_ud = &h->bind;
    }
    h->m = canlin_module_load(h->ev, &s, &cfg, &err);
    CHECK((h->m != NULL) == (err == CANLIN_MODULE_OK));
    if (h->m)
        CHECK(canlin_module_set_ui(h->m, h->ui, h->out, 0, 0, h->w, h->h) == CANLIN_MODULE_OK);
    return err;
}

static void h_turns(harness *h, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        canlin_ev_turn_result tr;
        CHECK(canlin_ev_turn(h->ev, &tr) == CANLIN_EV_OK);
    }
}

static void post(harness *h, canlin_ui_input_type type, int x, int y, int down)
{
    canlin_ui_input in;
    memset(&in, 0, sizeof in);
    in.type = type;
    in.x = x;
    in.y = y;
    in.down = down;
    CHECK(canlin_module_post_input(h->m, &in) == CANLIN_MODULE_OK);
}

/* The n-th (from 0) record entry of kind/part of the module's own view. */
static const canlin_ui_rect *find(harness *h, int kind, int part, int nth)
{
    size_t n, i;
    const canlin_ui_rect *r = canlin_ui_view_record(canlin_module_view(h->m), &n, NULL);
    for (i = 0; i < n; i++)
        if (r[i].kind == kind && r[i].part == part && nth-- == 0)
            return &r[i];
    return NULL;
}

/* ---- the module under test ---- */

static const char mod_src[] =
    "local M = {}\n"
    "function M.init(ctx)\n"
    "  print('init', type(canlin.view))\n"
    "  if canlin.view then print('fns', type(canlin.view.draw), type(canlin.view.submission)) end\n"
    "end\n"
    "function M.draw(ui)\n"
    "  if not canlin.view then print('noview') return end\n"
    "  local rev, code, msg = canlin.view.draw()\n"
    "  print('draw', rev, code, msg)\n"
    "  local sub, scode, smsg = canlin.view.submission()\n"
    "  if sub then\n"
    "    print('sub', sub.revision, sub.action)\n"
    "    print('vals', sub.values.item, sub.values.urgent, sub.values.shop)\n"
    "    local p = sub.values.plan\n"
    "    if p then\n"
    "      local n = p.nodes\n"
    "      print('graph', #n, n[1].id, n[2].id, n[2].x, p.edges[1].from[1], p.edges[1].from[2],\n"
    "            p.edges[1].to[1])\n"
    "    end\n"
    "  elseif scode then\n"
    "    print('subfail', scode, smsg)\n"
    "  else\n"
    "    print('subnone')\n"
    "  end\n"
    "end\n"
    "return M\n";

static const char FORM[] =
    "{\"title\":\"Form\",\"widgets\":["
    "{\"type\":\"field\",\"id\":\"item\",\"label\":\"Item\",\"value\":\"Milk\"},"
    "{\"type\":\"toggle\",\"id\":\"urgent\",\"label\":\"Urgent\"},"
    "{\"type\":\"choice\",\"id\":\"shop\",\"label\":\"Shop\",\"options\":[\"Market\",\"Drugstore\"],"
    "\"index\":2},"
    "{\"type\":\"graph\",\"id\":\"plan\",\"nodes\":["
    "{\"id\":\"a\",\"title\":\"A\",\"x\":1,\"y\":1,\"outputs\":[{\"id\":\"out\",\"type\":\"num\"}]},"
    "{\"id\":\"b\",\"x\":20,\"y\":1,\"inputs\":[{\"id\":\"in\",\"type\":\"num\"}]}],"
    "\"edges\":[{\"from\":[\"a\",\"out\"],\"to\":[\"b\",\"in\"]}]},"
    "{\"type\":\"button\",\"id\":\"ok\",\"label\":\"Add\",\"action\":\"submit\"}]}";

/* The model view in the assistant view (the C side; the module only sees the result). */
static void set_form(harness *h)
{
    canlin_av_result r;
    canlin_av_call c;
    c.gen = 1;
    c.turn = 1;
    c.id = "c1";
    c.id_len = 2;
    canlin_assistant_view_set_scope(h->av, 1, 1);
    CHECK(canlin_assistant_view_set_view(h->av, &c, FORM, strlen(FORM), &r) == CANLIN_AV_OK);
    if (r.status != CANLIN_AV_OK)
        fprintf(stderr, "set_view: %s %s\n", r.err.path, r.err.message);
    CHECK(r.revision == 1);
}

/* The capability not granted: the API is absent, the module sees nil. */
static void test_absent(void)
{
    harness h;
    h_init(&h, 600, 800);
    CHECK(h_load(&h, mod_src, 0) == CANLIN_MODULE_OK);
    h_turns(&h, 2);
    CHECK(has(h.log.buf, "init\tnil"));
    CHECK(has(h.log.buf, "noview"));
    CHECK(!has(h.log.buf, "init\ttable"));
    h_done(&h);
}

/* The capability granted: the model view is drawn into the module's own view and the
 * submission is picked up. */
static void test_binding(void)
{
    harness h;
    const canlin_ui_rect *b;
    int x, y;
    h_init(&h, 600, 800);
    CHECK(h_load(&h, mod_src, 1) == CANLIN_MODULE_OK);
    set_form(&h);
    h_turns(&h, 2);
    CHECK(has(h.log.buf, "init\ttable"));
    CHECK(has(h.log.buf, "fns\tfunction\tfunction"));
    /* the draw answers the revision it drew */
    CHECK(has(h.log.buf, "draw\t1"));
    /* the model view is in the module's own view */
    CHECK(find(&h, CANLIN_UI_W_FIELD, CANLIN_UI_P_BOX, 0) != NULL);
    CHECK(find(&h, CANLIN_UI_W_TOGGLE, CANLIN_UI_P_BOX, 0) != NULL);
    CHECK(find(&h, CANLIN_UI_W_CHOICE, CANLIN_UI_P_BOX, 0) != NULL);
    CHECK(find(&h, CANLIN_UI_W_GRAPH, CANLIN_UI_P_BOX, 0) != NULL);
    b = find(&h, CANLIN_UI_W_BUTTON, CANLIN_UI_P_BOX, 0);
    CHECK(b != NULL);
    if (b == NULL) {
        h_done(&h);
        return;
    }
    /* nothing is submitted before the user presses the button */
    CHECK(!has(h.log.buf, "sub\t"));
    CHECK(has(h.log.buf, "subnone"));
    x = b->x + b->w / 2;
    y = b->y + b->h / 2;
    canlin_ev_fake_clock_advance(&h.clock, 1000);
    post(&h, CANLIN_UI_IN_MOTION, x, y, 0);
    post(&h, CANLIN_UI_IN_BUTTON, x, y, 1);
    h_turns(&h, 4);
    post(&h, CANLIN_UI_IN_BUTTON, x, y, 0);
    h_turns(&h, 4);
    /* the submission: the declared values of that view, nothing else */
    CHECK(has(h.log.buf, "sub\t1\tok"));
    CHECK(has(h.log.buf, "vals\tMilk\tfalse\t2"));
    CHECK(has(h.log.buf, "graph\t2\ta\tb\t20\ta\tout\tb"));
    CHECK(count(h.log.buf, "sub\t1\tok") == 1);
    /* it waits for one consumer only */
    CHECK(count(h.log.buf, "subnone") >= 1);
    h_done(&h);
}

/* The grant revoked: every answer is denied, never a pretend success. */
static void test_revoked(void)
{
    harness h;
    h_init(&h, 600, 800);
    CHECK(h_load(&h, mod_src, 1) == CANLIN_MODULE_OK);
    set_form(&h);
    h_turns(&h, 2);
    CHECK(has(h.log.buf, "draw\t1"));
    h.bind.granted = 0;
    post(&h, CANLIN_UI_IN_MOTION, 1, 1, 0); /* any input requests a draw */
    h_turns(&h, 3);
    CHECK(has(h.log.buf, "draw\tnil\tdenied"));
    CHECK(has(h.log.buf, "subfail\tdenied"));
    CHECK(!has(h.log.buf, "draw\tnil\tok"));
    h_done(&h);
}

int main(void)
{
    test_absent();
    test_binding();
    test_revoked();
    if (failures) {
        fprintf(stderr, "%d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("assistant binding: %d checks passed\n", checks);
    return 0;
}
