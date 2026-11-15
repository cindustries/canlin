/* Assistant-view binding contract tests (module roles spec §3; UI spec §7; core API
 * §3.1; ADR 0001): the privileged Lua surface a module with the assistant-view
 * capability uses.
 *
 * The module's own Lua code runs in the real sandbox and the real scheduler: it draws
 * the model into the model's own view and picks up the submission. The model's widgets
 * live in that view and in no other, and a queued click in it is dropped when the model
 * is replaced. A module without the capability never sees canlin.view (it is nil); a
 * revoked grant answers denied, never a pretend success. Exits 0 on success. */
#include "canlin/assistant_binding.h"
#include "canlin/json.h"
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
    canlin_ui_view *mv; /* the model's own view (ADR 0001) */
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
    int err = 0;
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
    d.scale = CANLIN_UI_SCALE_STEP_100;
    h->out = canlin_ui_output_new(&d, NULL);
    h->av = canlin_assistant_view_new();
    /* The model has a view of its own: the loader owns it and hands it to the
     * assistant view (ADR 0001). */
    h->mv = canlin_ui_view_new(h->ui, h->out, &err);
    h->bind.view = h->av;
    h->bind.granted = 1;
    if (!h->ev || !h->ui || !h->out || !h->av || !h->mv) {
        fprintf(stderr, "harness init failed\n");
        exit(1);
    }
    canlin_ui_view_set_rect(h->mv, 0, 0, w, hh);
    canlin_assistant_view_attach(h->av, h->mv);
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
    canlin_ui_view_free(h->mv);
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

/* A click in the model's own view: the shell routes the pointer to the view under the
 * point, and that is the model's view. The app asks for the redraw the same way it
 * does in the running app: the module's generation is invalidated. */
static void post_model(harness *h, canlin_ui_input_type type, int x, int y, int down)
{
    canlin_ui_input in;
    memset(&in, 0, sizeof in);
    in.type = type;
    in.x = x;
    in.y = y;
    in.down = down;
    CHECK(canlin_ui_view_input(h->mv, &in) == CANLIN_UI_OK);
    canlin_ev_invalidate(h->ev, canlin_module_gen(h->m));
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

/* The n-th (from 0) record entry of kind/part of the model's own view. */
static const canlin_ui_rect *mfind(harness *h, int kind, int part, int nth)
{
    size_t n, i;
    const canlin_ui_rect *r = canlin_ui_view_record(h->mv, &n, NULL);
    for (i = 0; i < n; i++)
        if (r[i].kind == kind && r[i].part == part && nth-- == 0)
            return &r[i];
    return NULL;
}

/* ---- the module under test ---- */

static const char mod_src[] =
    "local M = {}\n"
    "local history = {}\n"   /* the conversation: what goes to the provider */
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
    "    print('sub', type(sub), sub)\n"
    "    -- what the assistant module does with it: the submission goes into the\n"
    "    -- conversation as it is, so the provider gets the values the JSON carries\n"
    "    history[#history + 1] = {role = 'user', text = sub}\n"
    "    print('msg', history[#history].text)\n"
    "  elseif scode then\n"
    "    print('subfail', scode, smsg)\n"
    "  else\n"
    "    print('subnone')\n"
    "  end\n"
    "end\n"
    "return M\n";

/* The module that draws the model view but never takes the submission: the C side
 * takes it and compares the bytes with the ones the other module received. */
static const char mod_quiet[] =
    "local M = {}\n"
    "function M.draw(ui)\n"
    "  canlin.view.draw()\n"
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

/* The model view in the assistant view (the C side; the module only sees the result).
 * The call id names the call: a second call with another id replaces the model. */
static void set_form_id(harness *h, const char *id)
{
    canlin_av_result r;
    canlin_av_call c;
    c.gen = 1;
    c.turn = 1;
    c.id = id;
    c.id_len = strlen(id);
    canlin_assistant_view_set_scope(h->av, 1, 1);
    CHECK(canlin_assistant_view_set_view(h->av, &c, FORM, strlen(FORM), &r) == CANLIN_AV_OK);
    if (r.status != CANLIN_AV_OK)
        fprintf(stderr, "set_view: %s %s\n", r.err.path, r.err.message);
}

static void set_form(harness *h)
{
    set_form_id(h, "c1");
    CHECK(canlin_assistant_view_revision(h->av) == 1);
}

/* Presses the submit button of the form: the core builds the submission and the
 * module takes it in its draw. The click goes into the model's own view - that is
 * where the model's widgets are - and the module's draw frames that view and so
 * applies the click. */
static void press_submit(harness *h)
{
    const canlin_ui_rect *b;
    int x, y;
    b = mfind(h, CANLIN_UI_W_BUTTON, CANLIN_UI_P_BOX, 0);
    CHECK(b != NULL);
    if (b == NULL)
        return;
    x = b->x + b->w / 2;
    y = b->y + b->h / 2;
    canlin_ev_fake_clock_advance(&h->clock, 1000);
    post_model(h, CANLIN_UI_IN_MOTION, x, y, 0);
    post_model(h, CANLIN_UI_IN_BUTTON, x, y, 1);
    h_turns(h, 4);
    post_model(h, CANLIN_UI_IN_BUTTON, x, y, 0);
    h_turns(h, 4);
}

/* ---- the values of a JSON document ---- */

/* The text of the log line that starts with (prefix), copied into buf. The JSON of a
 * submission holds no newline (the core escapes the control bytes), so the line ends
 * where the JSON ends. */
static int line_copy(const char *log, const char *prefix, char *buf, size_t cap)
{
    const char *p = strstr(log, prefix);
    size_t n = 0;
    if (p == NULL)
        return 0;
    p += strlen(prefix);
    while (p[n] != 0 && p[n] != '\n' && n + 1 < cap) {
        buf[n] = p[n];
        n++;
    }
    buf[n] = 0;
    return 1;
}

/* The pairs of a JSON document: "key=value" for every scalar, the value decoded (the
 * house reader resolves the escapes). The pairs of the values a module hands back are
 * the pairs of the submission: the values are what the JSON carries. */
typedef struct pairs {
    char buf[8192];
    size_t len;
    char key[64];
} pairs;

static int pair_token(void *ud, const canlin_json_token *t)
{
    pairs *p = ud;
    char v[256];
    size_t n = 0;
    switch (t->type) {
    case CANLIN_JSON_KEY:
        if (t->len < sizeof p->key) {
            memcpy(p->key, t->ptr, t->len);
            p->key[t->len] = 0;
        }
        return 0;
    case CANLIN_JSON_STRING:
    case CANLIN_JSON_NUMBER:
        n = t->len < sizeof v - 1 ? t->len : sizeof v - 1;
        memcpy(v, t->ptr, n);
        v[n] = 0;
        break;
    case CANLIN_JSON_TRUE:
        strcpy(v, "true");
        break;
    case CANLIN_JSON_FALSE:
        strcpy(v, "false");
        break;
    case CANLIN_JSON_NULL:
        strcpy(v, "null");
        break;
    default:
        return 0; /* a container: it holds the values */
    }
    if (p->len + strlen(p->key) + strlen(v) + 2 < sizeof p->buf)
        p->len += (size_t)snprintf(p->buf + p->len, sizeof p->buf - p->len, "%s=%s\n",
                                   p->key, v);
    return 0;
}

/* The pairs of a JSON document; 0 when the bytes are not the JSON they claim to be. */
static int json_pairs(const char *json, pairs *p)
{
    canlin_json js;
    canlin_json_limits lim;
    char tok[256];
    memset(p, 0, sizeof *p);
    lim.max_depth = 16;
    lim.max_string = 200;
    lim.max_number = CANLIN_AV_MAX_NUMBER;
    lim.max_total = CANLIN_AV_SUBMIT_MAX;
    if (canlin_json_init(&js, &lim, tok, sizeof tok, pair_token, p) != CANLIN_JSON_OK)
        return 0;
    if (canlin_json_feed(&js, json, strlen(json)) != CANLIN_JSON_OK)
        return 0;
    return canlin_json_finish(&js) == CANLIN_JSON_OK;
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

/* The capability granted: the model is drawn into the model's own view and the
 * submission is picked up. */
static void test_binding(void)
{
    harness h;
    const canlin_ui_rect *b;
    h_init(&h, 600, 800);
    CHECK(h_load(&h, mod_src, 1) == CANLIN_MODULE_OK);
    set_form(&h);
    h_turns(&h, 2);
    CHECK(has(h.log.buf, "init\ttable"));
    CHECK(has(h.log.buf, "fns\tfunction\tfunction"));
    /* the draw answers the revision it drew */
    CHECK(has(h.log.buf, "draw\t1"));
    /* the model view is in the model's own view */
    CHECK(mfind(&h, CANLIN_UI_W_FIELD, CANLIN_UI_P_BOX, 0) != NULL);
    CHECK(mfind(&h, CANLIN_UI_W_TOGGLE, CANLIN_UI_P_BOX, 0) != NULL);
    CHECK(mfind(&h, CANLIN_UI_W_CHOICE, CANLIN_UI_P_BOX, 0) != NULL);
    CHECK(mfind(&h, CANLIN_UI_W_GRAPH, CANLIN_UI_P_BOX, 0) != NULL);
    b = mfind(&h, CANLIN_UI_W_BUTTON, CANLIN_UI_P_BOX, 0);
    CHECK(b != NULL);
    if (b == NULL) {
        h_done(&h);
        return;
    }
    /* nothing is submitted before the user presses the button */
    CHECK(!has(h.log.buf, "sub\t"));
    CHECK(has(h.log.buf, "subnone"));
    press_submit(&h);
    /* the submission: the bytes of that view, nothing else */
    CHECK(has(h.log.buf, "sub\tstring\t{\"revision\":1,\"action\":\"ok\""));
    CHECK(count(h.log.buf, "sub\t") == 1);
    /* it waits for one consumer only */
    CHECK(count(h.log.buf, "subnone") >= 1);
    h_done(&h);
}

/* The model has a view of its own (ADR 0001): the model's widgets live in that view
 * and in no other, the frame of that view arises in the module's draw, and a queued
 * click in that view is dropped when the model is replaced - the click was meant for
 * the old model and must not act on the new one's widget. */
static void test_model_view(void)
{
    harness h;
    canlin_ui_input in;
    uint32_t frames;

    h_init(&h, 600, 800);
    CHECK(h_load(&h, mod_src, 1) == CANLIN_MODULE_OK);
    set_form(&h);
    h_turns(&h, 2);

    /* the model's widgets are in the model's own view */
    CHECK(mfind(&h, CANLIN_UI_W_FIELD, CANLIN_UI_P_BOX, 0) != NULL);
    /* and in the module's own view there is none of them */
    CHECK(find(&h, CANLIN_UI_W_FIELD, CANLIN_UI_P_BOX, 0) == NULL);
    CHECK(find(&h, CANLIN_UI_W_BUTTON, CANLIN_UI_P_BOX, 0) == NULL);

    /* the frame of the model's view arises in the module's draw */
    CHECK(canlin_ui_view_frames(h.mv) > 0);
    frames = canlin_ui_view_frames(h.mv);
    canlin_ev_invalidate(h.ev, canlin_module_gen(h.m));
    h_turns(&h, 2);
    CHECK(canlin_ui_view_frames(h.mv) > frames);

    /* a click waits in the model's own view: that is where the model's widgets are */
    memset(&in, 0, sizeof in);
    in.type = CANLIN_UI_IN_MOTION;
    in.x = 10;
    in.y = 10;
    CHECK(canlin_ui_view_input(h.mv, &in) == CANLIN_UI_OK);
    CHECK(canlin_ui_view_pending(h.mv) == 1);
    /* the module's own view keeps its own input */
    CHECK(canlin_ui_view_input(canlin_module_view(h.m), &in) == CANLIN_UI_OK);
    CHECK(canlin_ui_view_pending(canlin_module_view(h.m)) == 1);

    /* the model is replaced */
    set_form_id(&h, "c2");
    CHECK(canlin_assistant_view_revision(h.av) == 2);
    /* the reset of the queued input hit the view the click lands in */
    CHECK(canlin_ui_view_pending(h.mv) == 0);
    /* the module's own input is the module's own business */
    CHECK(canlin_ui_view_pending(canlin_module_view(h.m)) == 1);

    h_done(&h);
}

/* The submission the module receives: the core's own JSON bytes (UI spec §7), so the
 * values the module hands back to the provider are the values the JSON carries. The
 * module used to serialise them in Lua again, which dropped what the JSON held. */
static void test_submission_json(void)
{
    harness h;
    harness hc;
    char sub[4096], msg[4096];
    canlin_av_submission s;
    pairs pm, ps;

    /* the module takes the submission */
    h_init(&h, 600, 800);
    CHECK(h_load(&h, mod_src, 1) == CANLIN_MODULE_OK);
    set_form(&h);
    h_turns(&h, 2);
    press_submit(&h);
    /* the module receives the bytes, not a table of the module's own making */
    CHECK(line_copy(h.log.buf, "sub\tstring\t", sub, sizeof sub));
    CHECK(strstr(sub, "{\"revision\":1,\"action\":\"ok\",\"values\":{") == sub);
    /* what the module hands back to the provider: the submission's own bytes */
    CHECK(line_copy(h.log.buf, "msg\t", msg, sizeof msg));
    CHECK_STR(msg, sub);
    /* the bytes are the contract's JSON: the house reader reads them */
    CHECK(json_pairs(msg, &pm));
    /* the values of the view, the graph's nodes included */
    CHECK(has(pm.buf, "revision=1\n"));
    CHECK(has(pm.buf, "action=ok\n"));
    CHECK(has(pm.buf, "item=Milk\n"));
    CHECK(has(pm.buf, "urgent=false\n"));
    CHECK(has(pm.buf, "shop=2\n"));
    CHECK(has(pm.buf, "id=a\n"));
    CHECK(has(pm.buf, "title=A\n"));
    CHECK(has(pm.buf, "x=20\n"));
    CHECK(has(pm.buf, "id=out\n"));
    CHECK(has(pm.buf, "id=in\n"));
    CHECK(has(pm.buf, "from=a\n"));
    CHECK(has(pm.buf, "to=b\n"));
    h_done(&h);

    /* the same form, but no module takes the submission: the core's own bytes */
    h_init(&hc, 600, 800);
    CHECK(h_load(&hc, mod_quiet, 1) == CANLIN_MODULE_OK);
    set_form(&hc);
    h_turns(&hc, 2);
    press_submit(&hc);
    CHECK(canlin_assistant_view_pending(hc.av));
    CHECK(canlin_assistant_view_take(hc.av, &s) == 1);
    /* the module received the submission's own bytes */
    CHECK_STR(sub, s.json);
    /* and the values it hands back are the values the submission carried */
    CHECK(json_pairs(s.json, &ps));
    CHECK_STR(pm.buf, ps.buf);
    canlin_av_submission_free(&s);
    h_done(&hc);
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
    test_model_view();
    test_submission_json();
    test_revoked();
    if (failures) {
        fprintf(stderr, "%d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("assistant binding: %d checks passed\n", checks);
    return 0;
}
