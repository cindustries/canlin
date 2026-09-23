/* Flow UI contract tests (UI spec §2, §3, §8, §10): real sandbox, real scheduler with a
 * fake clock, the built-in theme and font, offscreen. Geometry is asserted through the
 * layout record and cross-checked against Nuklear's command list; one end-to-end pixel
 * check. Exits 0 on success. */
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
    canlin_ui_output *out2; /* optional second output/module (h_add_second) */
    canlin_module *m2;
    char log[LOGMAX];
    size_t loglen;
    unsigned draws, draws2;
    unsigned nin; /* input deliveries to m, in delivery order */
    struct {
        uint32_t type;
        int32_t a, b;
    } tr[64];
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
    if (h->m && canlin_module_gen(h->m) == d->gen) {
        if (d->kind == CANLIN_EV_D_DRAW)
            h->draws++;
        if (d->kind == CANLIN_EV_D_EVENT && d->source == CANLIN_MODULE_SOURCE_INPUT) {
            if (h->nin < sizeof h->tr / sizeof h->tr[0]) {
                h->tr[h->nin].type = d->type & 0xffu;
                h->tr[h->nin].a = d->a;
                h->tr[h->nin].b = d->b;
            }
            h->nin++;
        }
        canlin_module_deliver(h->m, d);
    } else if (h->m2 && canlin_module_gen(h->m2) == d->gen) {
        if (d->kind == CANLIN_EV_D_DRAW)
            h->draws2++;
        canlin_module_deliver(h->m2, d);
    }
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

static int has(const harness *h, const char *needle)
{
    return strstr(h->log, needle) != NULL;
}

/* theme: NULL = built-in. view: inner x/y at 0. */
static void h_init(harness *h, const canlin_theme *theme, float dpi, const char *src, int w,
                   int vh)
{
    canlin_ev_clock c;
    canlin_ev_callbacks cb;
    canlin_theme t;
    canlin_module_source s;
    canlin_sandbox_config cfg;
    int err = 1;

    memset(h, 0, sizeof *h);
    c = canlin_ev_fake_clock_bind(&h->clock);
    cb.deliver = on_deliver;
    h->ev = canlin_ev_new(&c, &cb, h);
    if (theme)
        t = *theme;
    else
        canlin_theme_builtin(&t);
    h->ui = canlin_ui_new(&t);
    {
        canlin_ui_output_desc d;
        d.id = "test-output";
        d.w = w;
        d.h = vh;
        d.dpi = dpi;
        d.scale = 100;
        h->out = canlin_ui_output_new(&d, NULL);
    }
    if (!h->ev || !h->ui || !h->out) {
        fprintf(stderr, "harness init failed\n");
        exit(1);
    }
    memset(&s, 0, sizeof s);
    memset(&cfg, 0, sizeof cfg);
    s.name = "uitest";
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
}

static canlin_module *load(harness *h, const char *name, const char *src)
{
    canlin_module_source s;
    canlin_sandbox_config cfg;
    int err = 1;
    canlin_module *m;
    memset(&s, 0, sizeof s);
    memset(&cfg, 0, sizeof cfg);
    s.name = name;
    s.version = "1";
    s.main = src;
    s.main_len = strlen(src);
    cfg.log = log_fn;
    cfg.ud = h;
    m = canlin_module_load(h->ev, &s, &cfg, &err);
    if (!m) {
        fprintf(stderr, "module load failed (%d): %s\n", err, h->log);
        exit(1);
    }
    return m;
}

/* A second module running src on its own output (same ui, same scheduler). */
static void h_add_second(harness *h, const char *id, float dpi, int scale, const char *src,
                         int w, int vh)
{
    canlin_ui_output_desc d;
    d.id = id;
    d.w = w;
    d.h = vh;
    d.dpi = dpi;
    d.scale = scale;
    h->out2 = canlin_ui_output_new(&d, NULL);
    if (!h->out2) {
        fprintf(stderr, "second output failed\n");
        exit(1);
    }
    h->m2 = load(h, "uitest2", src);
    CHECK(canlin_module_set_ui(h->m2, h->ui, h->out2, 0, 0, w, vh) == CANLIN_MODULE_OK);
}

static void h_done(harness *h)
{
    canlin_module_free(h->m2);
    canlin_module_free(h->m);
    CHECK(canlin_ev_free(h->ev) == CANLIN_EV_OK);
    CHECK(canlin_ui_output_free(h->out) == CANLIN_UI_OK);
    CHECK(canlin_ui_output_free(h->out2) == CANLIN_UI_OK);
    canlin_ui_free(h->ui);
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

static const canlin_ui_rect *rec(harness *h, size_t *n)
{
    return canlin_ui_view_record(canlin_module_view(h->m), n, NULL);
}

/* nth (from 0) record entry of kind/part, or NULL. */
static const canlin_ui_rect *find(harness *h, int kind, int part, int nth)
{
    size_t n, i;
    const canlin_ui_rect *r = rec(h, &n);
    for (i = 0; i < n; i++)
        if (r[i].kind == kind && r[i].part == part && nth-- == 0)
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

static void click(harness *h, const canlin_ui_rect *r)
{
    int x = r->x + r->w / 2, y = r->y + r->h / 2;
    input(h, CANLIN_UI_IN_MOTION, x, y, 0);
    input(h, CANLIN_UI_IN_BUTTON, x, y, 1);
    input(h, CANLIN_UI_IN_BUTTON, x, y, 0);
}

static void type_text(harness *h, const char *s)
{
    canlin_ui_input in;
    memset(&in, 0, sizeof in);
    in.type = CANLIN_UI_IN_TEXT;
    for (; *s; s++) {
        in.cp = (unsigned char)*s;
        CHECK(canlin_module_post_input(h->m, &in) == CANLIN_MODULE_OK);
    }
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
}

/* A filled rect command exactly at r exists in the view's command list (the record and
 * what Nuklear draws agree). */
static int nk_has_fill(harness *h, const canlin_ui_rect *r)
{
    const struct nk_command *cmd;
    struct nk_context *ctx = canlin_ui_view_nk(canlin_module_view(h->m));
    nk_foreach(cmd, ctx)
    {
        if (cmd->type == NK_COMMAND_RECT_FILLED) {
            const struct nk_command_rect_filled *f = (const void *)cmd;
            if (f->x == r->x && f->y == r->y && f->w == r->w && f->h == r->h)
                return 1;
        }
    }
    return 0;
}

static int iabs(int x)
{
    return x < 0 ? -x : x;
}

/* ---- geometry ---- */

static const char columns_src[] =
    "local M = {}\n"
    "function M.draw(ui)\n"
    "  ui:columns(function() ui:button('Left') end, function() ui:button('Right') end)\n"
    "  ui:button('Below')\n"
    "end\n"
    "return M\n";

/* Built-in theme at DPI 1: em 16, unit 4, view padding 8. Inner width 40 em = 640 px. */
static void test_columns(void)
{
    harness h;
    const canlin_ui_rect *l, *r, *b;

    h_init(&h, NULL, 1.0f, columns_src, 640 + 16, 600);
    h_drain(&h);
    CHECK(h.draws == 1);
    l = find(&h, CANLIN_UI_W_BUTTON, CANLIN_UI_P_BOX, 0);
    r = find(&h, CANLIN_UI_W_BUTTON, CANLIN_UI_P_BOX, 1);
    b = find(&h, CANLIN_UI_W_BUTTON, CANLIN_UI_P_BOX, 2);
    CHECK(l && r && b);
    if (l && r && b) {
        CHECK(l->y == r->y);                    /* side by side */
        CHECK(r->x > l->x + l->w);              /* with a gap */
        CHECK(iabs(l->w - r->w) <= 1);          /* equal */
        CHECK(l->x == 8 && r->x + r->w == 648); /* inside the padding */
        CHECK(b->y > l->y + l->h && b->w == 640);
        CHECK(nk_has_fill(&h, l) && nk_has_fill(&h, r) && nk_has_fill(&h, b));
    }
    h_done(&h);

    /* 638 px = 39.875 em < 40 em: stacked, full width. */
    h_init(&h, NULL, 1.0f, columns_src, 638 + 16, 600);
    h_drain(&h);
    l = find(&h, CANLIN_UI_W_BUTTON, CANLIN_UI_P_BOX, 0);
    r = find(&h, CANLIN_UI_W_BUTTON, CANLIN_UI_P_BOX, 1);
    CHECK(l && r);
    if (l && r) {
        CHECK(r->y > l->y + l->h);
        CHECK(l->x == r->x && l->w == 638 && r->w == 638);
        CHECK(nk_has_fill(&h, l) && nk_has_fill(&h, r));
    }
    h_done(&h);
}

static const char all_src[] =
    "local M = {}\n"
    "local name, notes, on, idx = '', '', false, 1\n"
    "function M.draw(ui)\n"
    "  ui:heading('Title')\n"
    "  ui:text('Some text')\n"
    "  ui:button('A')\n"
    "  on = ui:toggle('T', on)\n"
    "  name = ui:field('Name', name)\n"
    "  notes = ui:area('Notes', notes, {rows = 2})\n"
    "  idx = ui:choice('C', {'x', 'y'}, idx)\n"
    "  ui:meter('M', 0.5)\n"
    "  ui:separator()\n"
    "  ui:section('S', function() ui:button('In') end)\n"
    "  ui:bubble('user', 'hi')\n"
    "end\n"
    "return M\n";

static int interactive(const canlin_ui_rect *r)
{
    switch (r->kind) {
    case CANLIN_UI_W_FIELD:
    case CANLIN_UI_W_AREA:
    case CANLIN_UI_W_CHOICE:
        return r->part == CANLIN_UI_P_BOX || r->part == CANLIN_UI_P_OPTION;
    case CANLIN_UI_W_BUTTON:
    case CANLIN_UI_W_TOGGLE:
    case CANLIN_UI_W_SECTION:
        return 1;
    default:
        return 0;
    }
}

/* 2.5 em minimum after rounding, whole-pixel rects that stay ordered, at DPI 1.0/1.5/2.0
 * and with a fractional spacing so the float layout does not land on whole pixels. */
static void test_min_height_rounding(void)
{
    static const float dpis[] = {1.0f, 1.5f, 2.0f};
    canlin_theme t;
    size_t k, i, n;
    for (k = 0; k < 3; k++) {
        harness h;
        const canlin_ui_rect *r;
        float em;
        int min, prev_bottom = -1, nint = 0;
        canlin_theme_builtin(&t);
        t.font_size = 13;
        t.spacing = 1.3f;
        h_init(&h, &t, dpis[k], all_src, 400, 2000);
        h_drain(&h);
        CHECK(!has(&h, "draw:"));
        em = canlin_ui_view_em(canlin_module_view(h.m));
        CHECK(em == 13.0f * dpis[k]);
        min = (int)(2.5f * em + 0.999f);
        r = rec(&h, &n);
        CHECK(n > 10);
        for (i = 0; i < n; i++) {
            if (interactive(&r[i])) {
                nint++;
                CHECK(r[i].h >= min);
                /* Rounding never adds more than one pixel to the minimum (an area is
                 * taller by its rows). */
                if (r[i].kind != CANLIN_UI_W_AREA)
                    CHECK(r[i].h <= min + 1);
            }
            /* Flow order: top-level widgets never overlap the previous one. */
            if (r[i].kind != CANLIN_UI_W_SECTION && r[i].kind != CANLIN_UI_W_COLUMNS &&
                (r[i].part == CANLIN_UI_P_BOX || r[i].part == CANLIN_UI_P_LABEL)) {
                CHECK(r[i].y >= prev_bottom);
                prev_bottom = r[i].y + r[i].h;
            }
        }
        CHECK(nint == 7); /* button, toggle, field, area, choice, section, In */
        h_done(&h);
    }
}

static char wrap_src[4096];
static const char wrap_text[] =
    "alpha beta gamma delta epsilon zeta eta theta iota kappa lambda "
    "Supercalifragilisticexpialidocious_and_then_some_more_letters_to_overflow";

static void test_wrap_ellipsis(void)
{
    harness h;
    size_t n, i, lines = 0;
    const canlin_ui_rect *r, *b;
    const struct nk_user_font *f;
    char joined[512];
    size_t jl = 0;

    snprintf(wrap_src, sizeof wrap_src,
             "local M = {}\n"
             "function M.draw(ui)\n"
             "  ui:text('%s')\n"
             "  ui:button('A very long button label that cannot fit into the view')\n"
             "  ui:button('Ok')\n"
             "end\n"
             "return M\n",
             wrap_text);
    /* inner width 160 px = 10 em */
    h_init(&h, NULL, 1.0f, wrap_src, 176, 1000);
    h_drain(&h);
    CHECK(!has(&h, "draw:"));
    f = &canlin_ui_view_font(canlin_module_view(h.m))->font->handle;
    r = rec(&h, &n);
    for (i = 0; i < n; i++) {
        const char *s;
        size_t len;
        if (r[i].kind != CANLIN_UI_W_TEXT || r[i].part != CANLIN_UI_P_LINE)
            continue;
        s = wrap_text + r[i].off;
        len = r[i].len;
        lines++;
        CHECK(len > 0);
        CHECK(f->width(f->userdata, f->height, s, (int)len) <= 160.0f);
        CHECK(s[0] != ' ' && s[len - 1] != ' ');
        /* A line ends at a word boundary unless the word itself is wider than the
         * column (the last word here). */
        if (r[i].off + len < sizeof wrap_text - 1 && wrap_text[r[i].off + len] != ' ')
            CHECK(r[i].off >= (size_t)(strstr(wrap_text, "Super") - wrap_text));
        if (jl + len + 1 < sizeof joined) {
            memcpy(joined + jl, s, len);
            jl += len;
            joined[jl++] = ' ';
        }
    }
    CHECK(lines >= 6);
    /* Every word appears, in order: the text is split, never dropped. */
    joined[jl] = 0;
    CHECK(strncmp(joined, "alpha beta gamma", 16) == 0);
    CHECK(strstr(joined, "lambda Supercal") != NULL);

    b = find(&h, CANLIN_UI_W_BUTTON, CANLIN_UI_P_BOX, 0);
    CHECK(b && b->ellipsized && b->len > 0 && b->len < 20);
    b = find(&h, CANLIN_UI_W_BUTTON, CANLIN_UI_P_BOX, 1);
    CHECK(b && !b->ellipsized && b->len == 2);
    h_done(&h);
}

/* ---- controlled widgets ---- */

static const char button_src[] =
    "local M = {}\n"
    "function M.draw(ui)\n"
    "  if ui:button('Go') then print('clicked') end\n"
    "end\n"
    "return M\n";

static void test_button_once(void)
{
    harness h;
    const canlin_ui_rect *b;
    h_init(&h, NULL, 1.0f, button_src, 300, 300);
    h_drain(&h);
    b = find(&h, CANLIN_UI_W_BUTTON, CANLIN_UI_P_BOX, 0);
    CHECK(b != NULL);
    if (!b) {
        h_done(&h);
        return;
    }
    click(&h, b);
    h_turn(&h); /* press frame */
    CHECK(count(&h, "clicked") == 1);
    h_drain(&h); /* release frame (queued edge) and idle */
    CHECK(count(&h, "clicked") == 1);
    CHECK(h.draws == 3);
    CHECK(canlin_ui_view_pending(canlin_module_view(h.m)) == 0);
    /* A click outside the button does nothing. */
    input(&h, CANLIN_UI_IN_BUTTON, 1, 290, 1);
    input(&h, CANLIN_UI_IN_BUTTON, 1, 290, 0);
    h_drain(&h);
    CHECK(count(&h, "clicked") == 1);
    h_done(&h);
}

static const char controls_src[] =
    "local M = {}\n"
    "local name, on, idx, open = 'x', false, 1, nil\n"
    "function M.draw(ui)\n"
    "  local n = ui:field('Name', name)\n"
    "  if n ~= name then name = n print('name=' .. name) end\n"
    "  local o = ui:toggle('On', on)\n"
    "  if o ~= on then on = o print('on=' .. tostring(on)) end\n"
    "  local i = ui:choice('Pick', {'red', 'green', 'blue'}, idx)\n"
    "  if i ~= idx then idx = i print('idx=' .. idx) end\n"
    "  ui:section('Sec', function() ui:text('inside') end, {open = false})\n"
    "end\n"
    "return M\n";

static void test_controls(void)
{
    harness h;
    const canlin_ui_rect *r;
    h_init(&h, NULL, 1.0f, controls_src, 400, 800);
    h_drain(&h);
    CHECK(!has(&h, "draw:"));

    /* field: focus by click, type, backspace */
    r = find(&h, CANLIN_UI_W_FIELD, CANLIN_UI_P_BOX, 0);
    CHECK(r != NULL);
    if (r) {
        click(&h, r);
        h_drain(&h);
        type_text(&h, "yz");
        h_drain(&h);
        CHECK(has(&h, "name=xyz"));
        key(&h, CANLIN_UI_KEY_BACKSPACE);
        h_drain(&h);
        CHECK(has(&h, "name=xy\n"));
    }

    /* toggle */
    r = find(&h, CANLIN_UI_W_TOGGLE, CANLIN_UI_P_BOX, 0);
    CHECK(r != NULL);
    if (r) {
        click(&h, r);
        h_drain(&h);
        CHECK(count(&h, "on=true") == 1);
        click(&h, r);
        h_drain(&h);
        CHECK(count(&h, "on=false") == 1);
    }

    /* choice: expand, then pick the third option */
    CHECK(find(&h, CANLIN_UI_W_CHOICE, CANLIN_UI_P_OPTION, 0) == NULL);
    r = find(&h, CANLIN_UI_W_CHOICE, CANLIN_UI_P_BOX, 0);
    CHECK(r != NULL);
    if (r) {
        click(&h, r);
        h_drain(&h);
        r = find(&h, CANLIN_UI_W_CHOICE, CANLIN_UI_P_OPTION, 2);
        CHECK(r != NULL);
        CHECK(!has(&h, "idx="));
        if (r) {
            click(&h, r);
            h_drain(&h);
            CHECK(count(&h, "idx=3") == 1);
            CHECK(find(&h, CANLIN_UI_W_CHOICE, CANLIN_UI_P_OPTION, 0) == NULL);
        }
    }

    /* section: starts closed (opts.open = false), opens on click */
    CHECK(find(&h, CANLIN_UI_W_TEXT, CANLIN_UI_P_LINE, 0) == NULL);
    r = find(&h, CANLIN_UI_W_SECTION, CANLIN_UI_P_BOX, 0);
    CHECK(r != NULL);
    if (r) {
        click(&h, r);
        h_drain(&h);
        r = find(&h, CANLIN_UI_W_TEXT, CANLIN_UI_P_LINE, 0);
        CHECK(r != NULL);
    }
    CHECK(!has(&h, "draw:"));
    h_done(&h);
}

/* Field focus follows identity, not draw position: a widget inserted above the focused
 * field does not move typed text into another field. */
static const char focus_src[] =
    "local M = {}\n"
    "local a, b, extra = '', '', false\n"
    "function M.event() extra = true canlin.invalidate() end\n"
    "function M.draw(ui)\n"
    "  if extra then ui:field('Extra', '') end\n"
    "  a = ui:field('A', a)\n"
    "  b = ui:field('B', b)\n"
    "  print('a=' .. a .. ' b=' .. b)\n"
    "end\n"
    "return M\n";

static const canlin_module_event ev_go = {"go", NULL};

static void test_focus_identity(void)
{
    harness h;
    const canlin_ui_rect *r;
    h_init(&h, NULL, 1.0f, focus_src, 400, 800);
    h_drain(&h);
    r = find(&h, CANLIN_UI_W_FIELD, CANLIN_UI_P_BOX, 1); /* B */
    CHECK(r != NULL);
    if (r) {
        click(&h, r);
        h_drain(&h);
        CHECK(canlin_ev_post(h.ev, canlin_module_gen(h.m), CANLIN_MODULE_SOURCE_EVENT, 0, 0, 0,
                             (void *)&ev_go, 0, NULL) == CANLIN_EV_OK);
        h_drain(&h);
        type_text(&h, "q");
        h_drain(&h);
        CHECK(has(&h, "a= b=q"));
        CHECK(!has(&h, "a=q"));
    }
    h_done(&h);
}

/* ---- errors ---- */

static void expect_draw_error(const char *body, const char *needle)
{
    harness h;
    char src[2048];
    snprintf(src, sizeof src,
             "local M = {}\n"
             "function M.draw(ui)\n%s\nend\n"
             "return M\n",
             body);
    h_init(&h, NULL, 1.0f, src, 400, 400);
    h_drain(&h);
    if (!has(&h, needle))
        fprintf(stderr, "expected '%s' for:\n%s\nlog:\n%s\n", needle, body, h.log);
    CHECK(has(&h, needle));
    CHECK(canlin_module_get_state(h.m) == CANLIN_MODULE_RUNNING); /* one error, no fault */
    h_done(&h);
}

static void test_errors(void)
{
    /* identity */
    expect_draw_error("ui:button('a', {id='x'}) ui:toggle('b', true, {id='x'})",
                      "two stateful widgets");
    expect_draw_error("ui:field('n', '') ui:field('n', '') print('same label ok')",
                      "same label ok");
    /* limits (§8) */
    expect_draw_error("for i = 1, 513 do ui:separator() end", "more than 512 widgets");
    expect_draw_error("ui:button(string.rep('a', 257))", "label exceeds 256 bytes");
    expect_draw_error("ui:text(string.rep('a', 16 * 1024 + 1))", "text exceeds");
    expect_draw_error("ui:text(string.rep('a', 16 * 1024)) print('max ok')", "max ok");
    expect_draw_error("local o = {} for i = 1, 65 do o[i] = 'x' end ui:choice('c', o, 1)",
                      "1-64 options");
    expect_draw_error("ui:area('a', '', {rows = 21})", "opts.rows must be 2-20");
    expect_draw_error("ui:field('f', 'abc', {max = 2})", "exceeds opts.max");
    expect_draw_error("local function n(d) if d == 0 then ui:text('x') return end "
                      "ui:section('s' .. d, function() n(d - 1) end) end n(5)",
                      "nesting deeper than 4");
    expect_draw_error("local function n(d) if d == 0 then ui:text('x') return end "
                      "ui:section('s' .. d, function() n(d - 1) end) end n(4) print('d4 ok')",
                      "d4 ok");
    /* UTF-8 */
    expect_draw_error("ui:text('ok \\xff')", "not valid UTF-8");
    expect_draw_error("ui:button('\\xed\\xa0\\x80')", "not valid UTF-8"); /* surrogate */
    expect_draw_error("ui:text('\\xc0\\xaf')", "not valid UTF-8");        /* overlong */
    expect_draw_error("ui:text('gr\\xc3\\xbc\\xc3\\x9fe') print('utf8 ok')", "utf8 ok");
    /* arguments */
    expect_draw_error("ui:heading('h', 4)", "level must be 1-3");
    expect_draw_error("ui:text('t', {style = 'loud'})", "invalid option");
    expect_draw_error("ui:choice('c', {'a'}, 2)", "index out of range");
    expect_draw_error("ui.button('x')", "ui:name(...)");
    expect_draw_error("ui.foo = 1", "read-only");
    expect_draw_error("print(getmetatable(ui))", "false");
    /* not yet */
    expect_draw_error("ui:graph({nodes = 5})", "nodes must be an array"); /* canlin#19 */
    expect_draw_error("ui:icon('x')", "unsupported");
}

static const char stale_src[] =
    "local M = {}\n"
    "local saved\n"
    "function M.event() print(pcall(saved.text, saved, 'late')) canlin.invalidate() end\n"
    "function M.draw(ui)\n"
    "  if saved and saved ~= ui then print('stale', pcall(saved.text, saved, 'old')) end\n"
    "  saved = ui\n"
    "  ui:text('now')\n"
    "end\n"
    "return M\n";

static void test_after_draw(void)
{
    harness h;
    h_init(&h, NULL, 1.0f, stale_src, 400, 400);
    h_drain(&h);
    CHECK(canlin_ev_post(h.ev, canlin_module_gen(h.m), CANLIN_MODULE_SOURCE_EVENT, 0, 0, 0,
                         (void *)&ev_go, 0, NULL) == CANLIN_EV_OK);
    h_drain(&h);
    CHECK(count(&h, "outside the draw") == 2); /* in event, and from the next draw */
    CHECK(has(&h, "false\tui: object used outside"));
    CHECK(has(&h, "stale\tfalse"));
    h_done(&h);
}

/* An error caught by pcall inside ui:columns restores the column layout. */
static const char pcall_src[] =
    "local M = {}\n"
    "function M.draw(ui)\n"
    "  print(pcall(ui.columns, ui, function() ui:button('in') error('boom') end,\n"
    "              function() end))\n"
    "  ui:button('after')\n"
    "end\n"
    "return M\n";

static void test_columns_pcall(void)
{
    harness h;
    const canlin_ui_rect *a;
    h_init(&h, NULL, 1.0f, pcall_src, 656, 400);
    h_drain(&h);
    CHECK(has(&h, "false\t"));
    CHECK(has(&h, "boom"));
    a = find(&h, CANLIN_UI_W_BUTTON, CANLIN_UI_P_BOX, 1);
    CHECK(a && a->x == 8 && a->w == 640);
    h_done(&h);
}

/* Headless: no view attached, draw still runs, ui methods raise. */
static void test_headless(void)
{
    canlin_ev_fake_clock clock;
    canlin_ev_clock c = canlin_ev_fake_clock_bind(&clock);
    canlin_ev_callbacks cb;
    harness h;
    canlin_module_source s;
    canlin_sandbox_config cfg;
    int err;
    static const char src[] =
        "local M = {}\n"
        "function M.draw(ui) print(type(ui), pcall(ui.text, ui, 'x')) end\n"
        "return M\n";
    memset(&h, 0, sizeof h);
    cb.deliver = on_deliver;
    h.ev = canlin_ev_new(&c, &cb, &h);
    memset(&s, 0, sizeof s);
    memset(&cfg, 0, sizeof cfg);
    s.name = "headless";
    s.main = src;
    s.main_len = strlen(src);
    cfg.log = log_fn;
    cfg.ud = &h;
    h.m = canlin_module_load(h.ev, &s, &cfg, &err);
    CHECK(h.m != NULL);
    h_drain(&h);
    CHECK(has(&h, "table\tfalse\tui: no view"));
    CHECK(canlin_module_view(h.m) == NULL);
    canlin_module_free(h.m);
    canlin_ev_free(h.ev);
}

/* Input overflow drops the queue and still produces a draw; nothing stays pressed. */
static void test_input_overflow(void)
{
    harness h;
    unsigned i;
    canlin_ui_input in;
    h_init(&h, NULL, 1.0f, button_src, 300, 300);
    h_drain(&h);
    memset(&in, 0, sizeof in);
    in.type = CANLIN_UI_IN_MOTION;
    for (i = 0; i < CANLIN_UI_INPUT_MAX; i++)
        CHECK(canlin_ui_view_input(canlin_module_view(h.m), &in) == CANLIN_UI_OK);
    CHECK(canlin_ui_view_input(canlin_module_view(h.m), &in) == CANLIN_UI_EFULL);
    CHECK(canlin_ui_view_pending(canlin_module_view(h.m)) == 0);
    in.type = CANLIN_UI_IN_TEXT;
    in.cp = 0xd800;
    CHECK(canlin_module_post_input(h.m, &in) == CANLIN_MODULE_EINVAL);
    h_done(&h);
}

/* ---- input through the scheduler (canlin#27, event runtime §4–§5, core API §5.2) ---- */

static unsigned queued(harness *h)
{
    canlin_ev_stats st;
    int slot = canlin_ev_module_slot(h->ev, canlin_module_gen(h->m));
    CHECK(slot >= 0);
    CHECK(canlin_ev_get_stats(h->ev, slot, &st) == CANLIN_EV_OK);
    return st.queued;
}

static unsigned pending_control(harness *h)
{
    canlin_ev_stats st;
    int slot = canlin_ev_module_slot(h->ev, canlin_module_gen(h->m));
    CHECK(canlin_ev_get_stats(h->ev, slot, &st) == CANLIN_EV_OK);
    return st.control;
}

static int nk_left_down(harness *h)
{
    struct nk_context *ctx = canlin_ui_view_nk(canlin_module_view(h->m));
    return ctx->input.mouse.buttons[NK_BUTTON_LEFT].down;
}

/* motion A -> press -> motion B stays three items in that order; motions coalesce only
 * with an adjacent motion, to the latest position. */
static void test_input_order(void)
{
    harness h;
    int i;
    h_init(&h, NULL, 1.0f, button_src, 300, 300);
    h_drain(&h);
    h.nin = 0;
    input(&h, CANLIN_UI_IN_MOTION, 10, 11, 0);
    input(&h, CANLIN_UI_IN_BUTTON, 12, 13, 1);
    input(&h, CANLIN_UI_IN_MOTION, 14, 15, 0);
    CHECK(queued(&h) == 3);
    h_drain(&h);
    CHECK(h.nin == 3);
    CHECK(h.tr[0].type == CANLIN_UI_IN_MOTION && h.tr[0].a == 10 && h.tr[0].b == 11);
    CHECK(h.tr[1].type == CANLIN_UI_IN_BUTTON);
    CHECK(h.tr[2].type == CANLIN_UI_IN_MOTION && h.tr[2].a == 14 && h.tr[2].b == 15);

    /* 100 motions between two turns: one item, the last position. */
    h.nin = 0;
    for (i = 0; i < 100; i++)
        input(&h, CANLIN_UI_IN_MOTION, i, 2 * i, 0);
    CHECK(queued(&h) == 1);
    h_drain(&h);
    CHECK(h.nin == 1 && h.tr[0].a == 99 && h.tr[0].b == 198);

    /* Scrolls coalesce too, and keep their summed distance. */
    {
        canlin_ui_input in;
        memset(&in, 0, sizeof in);
        in.type = CANLIN_UI_IN_SCROLL;
        in.dy = 3;
        for (i = 0; i < 10; i++)
            CHECK(canlin_module_post_input(h.m, &in) == CANLIN_MODULE_OK);
        CHECK(queued(&h) == 1);
    }
    h_done(&h);
}

/* Saturated scheduler queue: the refused release raises INPUT_OVERFLOW; the input queued
 * before the gap is dropped, the held button is released and nothing fires. Input after
 * the gap works normally. */
static void test_input_overflow_latch(void)
{
    harness h;
    const canlin_ui_rect *b;
    canlin_ui_input in;
    unsigned n = 0;
    int x, y, rc;

    h_init(&h, NULL, 1.0f,
           "local M = {}\n"
           "function M.draw(ui)\n"
           "  if ui:button('Go') then print('clicked') end\n"
           "  local v = ui:field('F', '')\n"
           "  if v ~= '' then print('typed') end\n"
           "end\n"
           "return M\n",
           300, 300);
    h_drain(&h);
    b = find(&h, CANLIN_UI_W_BUTTON, CANLIN_UI_P_BOX, 0);
    CHECK(b != NULL);
    if (!b) {
        h_done(&h);
        return;
    }
    /* Press outside every widget and hold it. */
    x = 290;
    y = 290;
    input(&h, CANLIN_UI_IN_BUTTON, x, y, 1);
    h_drain(&h);
    CHECK(nk_left_down(&h) == 1);

    /* Queued before the gap: release, then a full click on the button. None of it may
     * take effect once an edge after it was lost. */
    input(&h, CANLIN_UI_IN_BUTTON, x, y, 0);
    click(&h, b);
    /* Flood with text edges until the scheduler refuses one. */
    memset(&in, 0, sizeof in);
    in.type = CANLIN_UI_IN_TEXT;
    in.cp = 'a';
    while ((rc = canlin_module_post_input(h.m, &in)) == CANLIN_MODULE_OK && n < 1000)
        n++;
    CHECK(rc == CANLIN_MODULE_EFULL);
    CHECK(n == CANLIN_EV_MOD_MAX_ITEMS - 4); /* release, motion, press, release */
    CHECK(pending_control(&h) & CANLIN_EV_CTL_INPUT_OVERFLOW);
    /* The release is refused too (queue still full): the button stays held until the
     * latch is consumed. */
    memset(&in, 0, sizeof in);
    in.type = CANLIN_UI_IN_BUTTON;
    in.x = x;
    in.y = y;
    CHECK(canlin_module_post_input(h.m, &in) == CANLIN_MODULE_EFULL);
    h_drain(&h);
    CHECK(pending_control(&h) == 0);
    CHECK(queued(&h) == 0);
    CHECK(nk_left_down(&h) == 0); /* released by the reset */
    CHECK(!has(&h, "typed"));     /* stale edges from before the gap were dropped */
    CHECK(!has(&h, "clicked"));
    CHECK(canlin_ui_view_pending(canlin_module_view(h.m)) == 0);

    /* After the gap input is delivered normally. */
    click(&h, b);
    h_drain(&h);
    CHECK(count(&h, "clicked") == 1);
    h_done(&h);
}

/* Input for a stopped or replaced generation never reaches the next one. */
static void test_input_stale_generation(void)
{
    harness h;
    const canlin_ui_rect *b;
    canlin_module_source s;
    canlin_ui_input in;
    h_init(&h, NULL, 1.0f, button_src, 300, 300);
    h_drain(&h);
    b = find(&h, CANLIN_UI_W_BUTTON, CANLIN_UI_P_BOX, 0);
    CHECK(b != NULL);
    if (!b) {
        h_done(&h);
        return;
    }
    click(&h, b); /* queued for the old generation */
    CHECK(queued(&h) == 3);
    memset(&s, 0, sizeof s);
    s.name = "uitest";
    s.version = "2";
    s.main = button_src;
    s.main_len = strlen(button_src);
    CHECK(canlin_module_reload(h.m, &s) == CANLIN_MODULE_OK);
    CHECK(queued(&h) == 0); /* retired with the old generation */
    h.nin = 0;
    h_drain(&h);
    CHECK(h.nin == 0);
    CHECK(!has(&h, "clicked"));
    CHECK(nk_left_down(&h) == 0);

    canlin_module_stop(h.m);
    memset(&in, 0, sizeof in);
    in.type = CANLIN_UI_IN_BUTTON;
    in.down = 1;
    CHECK(canlin_module_post_input(h.m, &in) == CANLIN_MODULE_ESTALE);
    in.type = CANLIN_UI_IN_MOTION;
    CHECK(canlin_module_post_input(h.m, &in) == CANLIN_MODULE_ESTALE);
    h_drain(&h);
    CHECK(h.nin == 0);
    h_done(&h);
}

/* Lua is never entered for input: every Lua entry is a draw delivery. */
static void test_input_no_lua(void)
{
    harness h;
    canlin_ui_input in;
    int i;
    h_init(&h, NULL, 1.0f,
           "local M = {}\n"
           "local n = 0\n"
           "function M.event(name) n = n + 1 print('EVENT ' .. name) end\n"
           "function M.draw(ui) n = n + 1 print('enter') ui:button('Go') ui:field('F', '') end\n"
           "return M\n",
           300, 300);
    h_drain(&h);
    h.draws = 0;
    h.nin = 0;
    h.loglen = 0;
    h.log[0] = 0;
    for (i = 0; i < 8; i++) {
        memset(&in, 0, sizeof in);
        in.type = CANLIN_UI_IN_MOTION;
        in.x = i;
        CHECK(canlin_module_post_input(h.m, &in) == CANLIN_MODULE_OK);
        in.type = CANLIN_UI_IN_BUTTON;
        in.down = i & 1;
        CHECK(canlin_module_post_input(h.m, &in) == CANLIN_MODULE_OK);
        in.type = CANLIN_UI_IN_KEY;
        in.key = CANLIN_UI_KEY_SHIFT;
        CHECK(canlin_module_post_input(h.m, &in) == CANLIN_MODULE_OK);
        in.type = CANLIN_UI_IN_TEXT;
        in.cp = 'x';
        CHECK(canlin_module_post_input(h.m, &in) == CANLIN_MODULE_OK);
        in.type = CANLIN_UI_IN_SCROLL;
        in.dy = 1;
        CHECK(canlin_module_post_input(h.m, &in) == CANLIN_MODULE_OK);
    }
    h_drain(&h);
    CHECK(h.nin == 40);
    CHECK(h.draws > 0);
    CHECK(count(&h, "enter") == h.draws);
    CHECK(!has(&h, "EVENT"));
    h_done(&h);
}

/* The core-owned scroll offset moves every rect and is clamped to the content. */
static void test_scroll(void)
{
    harness h;
    const canlin_ui_rect *b;
    int y0, ch, i;
    canlin_ui_input in;
    h_init(&h, NULL, 1.0f,
           "local M = {}\n"
           "function M.draw(ui) for i = 1, 20 do ui:button('b' .. i) end end\n"
           "return M\n",
           300, 200);
    h_drain(&h);
    b = find(&h, CANLIN_UI_W_BUTTON, CANLIN_UI_P_BOX, 0);
    CHECK(b && b->y == 8);
    y0 = b ? b->y : 0;
    ch = canlin_ui_view_content_height(canlin_module_view(h.m));
    CHECK(ch > 200);
    memset(&in, 0, sizeof in);
    in.type = CANLIN_UI_IN_SCROLL;
    in.dy = 20; /* five scrolls coalesce into one item that keeps their sum */
    for (i = 0; i < 5; i++)
        CHECK(canlin_module_post_input(h.m, &in) == CANLIN_MODULE_OK);
    CHECK(queued(&h) == 1);
    h_drain(&h);
    b = find(&h, CANLIN_UI_W_BUTTON, CANLIN_UI_P_BOX, 0);
    CHECK(b && b->y == y0 - 100);
    in.dy = 100000;
    CHECK(canlin_module_post_input(h.m, &in) == CANLIN_MODULE_OK);
    h_drain(&h);
    CHECK(canlin_ui_view_scroll(canlin_module_view(h.m)) == ch - 200);
    h_done(&h);
}

/* Widget work is charged to the draw budget (CANLIN_SB_BUDGET_DRAW): 512 maximal texts
 * are within every per-draw limit but not within the budget, and fault the scope; the
 * same draw with short texts fits. */
static void test_draw_charge(void)
{
    harness h;
    h_init(&h, NULL, 1.0f,
           "local M = {}\n"
           "local s = string.rep('a', 16 * 1024)\n"
           "function M.draw(ui) for i = 1, 512 do ui:text(s) end end\n"
           "return M\n",
           400, 200);
    h_drain(&h);
    CHECK(canlin_module_get_state(h.m) == CANLIN_MODULE_FAULTED);
    CHECK(has(&h, "budget"));
    h_done(&h);

    h_init(&h, NULL, 1.0f,
           "local M = {}\n"
           "function M.draw(ui) for i = 1, 512 do ui:text('short') end print('fits') end\n"
           "return M\n",
           400, 200);
    h_drain(&h);
    CHECK(canlin_module_get_state(h.m) == CANLIN_MODULE_RUNNING);
    CHECK(has(&h, "fits"));
    h_done(&h);
}

/* ---- end to end: module -> ev turn -> render ---- */

static void test_render_e2e(void)
{
    harness h;
    const canlin_ui_rect *b;
    canlin_render r;
    const canlin_render_font *f;
    enum { W = 320, H = 200 };
    static unsigned char px[W * H * 3];
    canlin_theme t;
    const unsigned char *p;

    h_init(&h, NULL, 1.0f, button_src, W, H);
    h_drain(&h);
    b = find(&h, CANLIN_UI_W_BUTTON, CANLIN_UI_P_BOX, 0);
    CHECK(b != NULL);
    CHECK(canlin_render_init(&r, px, W, H, W * 3, NULL, NULL) == 0);
    f = canlin_ui_view_font(canlin_module_view(h.m));
    CHECK(f != NULL && f->alpha != NULL);
    canlin_render_begin(&r);
    canlin_render_clear(&r, nk_rgb(255, 0, 255));
    canlin_ui_view_render(canlin_module_view(h.m), &r);
    canlin_render_end(&r);
    canlin_theme_builtin(&t);
    if (b) {
        /* inside the button, left of its centred label: surface colour */
        p = px + ((size_t)(b->y + b->h / 2) * W + (size_t)(b->x + 6)) * 3;
        CHECK(p[0] == t.color[CANLIN_THEME_SURFACE].r && p[1] == t.color[CANLIN_THEME_SURFACE].g &&
              p[2] == t.color[CANLIN_THEME_SURFACE].b);
        /* label pixels exist near the centre (text colour differs from surface) */
        {
            int x, y, text = 0;
            for (y = b->y + 4; y < b->y + b->h - 4; y++)
                for (x = b->x + b->w / 2 - 12; x < b->x + b->w / 2 + 12; x++) {
                    const unsigned char *q = px + ((size_t)y * W + (size_t)x) * 3;
                    if (q[0] != t.color[CANLIN_THEME_SURFACE].r ||
                        q[1] != t.color[CANLIN_THEME_SURFACE].g)
                        text++;
                }
            CHECK(text > 10);
        }
        /* below the button: view background, window covered the clear colour */
        p = px + ((size_t)(b->y + b->h + 20) * W + 20) * 3;
        CHECK(p[0] == t.color[CANLIN_THEME_BACKGROUND].r &&
              p[1] == t.color[CANLIN_THEME_BACKGROUND].g &&
              p[2] == t.color[CANLIN_THEME_BACKGROUND].b);
    }
    h_done(&h);
}

/* ---- canlin#36 polish: pixel checks at 100/150/200 % ---- */

static const int polish_scales[] = {100, 150, 200};

enum { PW = 1000, PH = 1000 }; /* largest view: 5 x 200 */
static unsigned char polish_px[PW * PH * 3];

/* Initialises h at scale s with a (5 s) x (5 s) px view, draws and renders it over a
 * magenta clear. */
static void polish_render(harness *h, const char *src, int s)
{
    canlin_render r;
    int w = 5 * s;
    h_init(h, NULL, 1.0f, src, w, w);
    CHECK(canlin_ui_output_set_scale(h->out, s) == CANLIN_UI_OK);
    h_drain(h);
    CHECK(!has(h, "draw:"));
    CHECK(canlin_ui_view_em(canlin_module_view(h->m)) == 16.0f * (float)s / 100.0f);
    CHECK(canlin_render_init(&r, polish_px, w, w, (size_t)w * 3, NULL, NULL) == 0);
    canlin_render_begin(&r);
    canlin_render_clear(&r, nk_rgb(255, 0, 255));
    canlin_ui_view_render(canlin_module_view(h->m), &r);
    canlin_render_end(&r);
}

static int pix(int w, int x, int y, canlin_theme_rgb c)
{
    const unsigned char *p = polish_px + ((size_t)y * (size_t)w + (size_t)x) * 3;
    return p[0] == c.r && p[1] == c.g && p[2] == c.b;
}

static const char bg_src[] =
    "local M = {}\n"
    "function M.draw(ui)\n"
    "  ui:button('A')\n"
    "  ui:text('t')\n"
    "end\n"
    "return M\n";

/* The view background covers the whole view rect with square corners: no pixel of the
 * clear colour survives, the four corners carry the background role. Before canlin#36
 * the window used the theme radius and left the corners unpainted. */
static void test_view_background_square(void)
{
    size_t k;
    canlin_theme t;
    canlin_theme_builtin(&t);
    CHECK(t.radius > 0.0f); /* otherwise the test proves nothing */
    for (k = 0; k < 3; k++) {
        int s = polish_scales[k], w = 5 * s, x, y, magenta = 0;
        harness h;
        canlin_theme_rgb bg = t.color[CANLIN_THEME_BACKGROUND], mg = {255, 0, 255};
        polish_render(&h, bg_src, s);
        for (y = 0; y < w; y++)
            for (x = 0; x < w; x++)
                magenta += pix(w, x, y, mg);
        CHECK(magenta == 0);
        CHECK(pix(w, 0, 0, bg) && pix(w, w - 1, 0, bg) && pix(w, 0, w - 1, bg) &&
              pix(w, w - 1, w - 1, bg));
        h_done(&h);
    }
}

static const char toggle_src[] =
    "local M = {}\n"
    "local on = false\n"
    "function M.draw(ui)\n"
    "  local o = ui:toggle('Toggle', on)\n"
    "  if o ~= on then on = o print('on=' .. tostring(on)) end\n"
    "end\n"
    "return M\n";

/* Toggle: a framed box of 1.25 em, a gap of 2 spacing units to the label (both scale
 * with em), and a hit target of at least 2.5 em covering box and label: a click on the
 * label toggles. */
static void test_toggle_geometry(void)
{
    size_t k;
    canlin_theme t;
    int gaps[3];
    canlin_theme_builtin(&t);
    for (k = 0; k < 3; k++) {
        int s = polish_scales[k], w = 5 * s;
        harness h;
        const canlin_ui_rect *r;
        float em = 16.0f * (float)s / 100.0f, unit = t.spacing * 0.25f * em;
        canlin_theme_rgb bg = t.color[CANLIN_THEME_BACKGROUND];
        gaps[k] = 0;
        polish_render(&h, toggle_src, s);
        r = find(&h, CANLIN_UI_W_TOGGLE, CANLIN_UI_P_BOX, 0);
        CHECK(r != NULL);
        if (r) {
            int bs = (int)(1.25f * em + 0.5f), bx = r->x, by = r->y + (r->h - bs) / 2;
            int my = by + bs / 2, x, y, lx0 = w, lx1 = 0;
            CHECK(r->h >= (int)(2.5f * em + 0.999f));
            /* frame (muted) on both sides, surface inside, background right after */
            CHECK(pix(w, bx, my, t.color[CANLIN_THEME_MUTED]));
            CHECK(pix(w, bx + bs - 1, my, t.color[CANLIN_THEME_MUTED]));
            CHECK(pix(w, bx + bs / 2, my, t.color[CANLIN_THEME_SURFACE]));
            CHECK(pix(w, bx + bs, my, bg));
            /* label extent: every non-background pixel right of the box within the row */
            for (y = r->y; y < r->y + r->h; y++)
                for (x = bx + bs; x < w; x++)
                    if (!pix(w, x, y, bg)) {
                        lx0 = x < lx0 ? x : lx0;
                        lx1 = x > lx1 ? x : lx1;
                    }
            CHECK(lx0 < lx1);
            gaps[k] = lx0 - (bx + bs);
            CHECK(gaps[k] >= (int)(2.0f * unit) && gaps[k] <= (int)(2.0f * unit + 0.25f * em));
            /* the hit target covers box and label */
            CHECK(r->x <= bx && r->x + r->w > lx1 && r->y <= by && r->y + r->h >= by + bs);
            /* a click on the label (not the box) toggles; the cursor shows in accent */
            input(&h, CANLIN_UI_IN_MOTION, (lx0 + lx1) / 2, r->y + r->h / 2, 0);
            input(&h, CANLIN_UI_IN_BUTTON, (lx0 + lx1) / 2, r->y + r->h / 2, 1);
            input(&h, CANLIN_UI_IN_BUTTON, (lx0 + lx1) / 2, r->y + r->h / 2, 0);
            h_drain(&h);
            CHECK(count(&h, "on=true") == 1);
            {
                canlin_render rr;
                CHECK(canlin_render_init(&rr, polish_px, w, w, (size_t)w * 3, NULL, NULL) == 0);
                canlin_render_begin(&rr);
                canlin_ui_view_render(canlin_module_view(h.m), &rr);
                canlin_render_end(&rr);
                CHECK(pix(w, bx + bs / 2, my, t.color[CANLIN_THEME_ACCENT]));
                CHECK(pix(w, bx, my, t.color[CANLIN_THEME_MUTED]));
            }
        }
        h_done(&h);
    }
    /* the gap grows with em (x1.5, x2 within the glyph's own bearing) */
    CHECK(gaps[1] > gaps[0] && gaps[2] > gaps[1]);
    CHECK(gaps[2] >= 2 * gaps[0] - 2);
}

static const char section_src[] =
    "local M = {}\n"
    "function M.draw(ui)\n"
    "  ui:section('Open', function() ui:text('in') end)\n"
    "  ui:section('Shut', function() ui:text('no') end, {open = false})\n"
    "  ui:choice('Pick', {'aa', 'bb'}, 1)\n"
    "end\n"
    "return M\n";

/* Bounding box of pixels of colour c inside [x0, x1) x [y0, y1); 0 if none. */
static int bbox(int w, canlin_theme_rgb c, int x0, int y0, int x1, int y1, int *bx0, int *by0,
                int *bx1, int *by1)
{
    int x, y, n = 0;
    *bx0 = *by0 = 1 << 30;
    *bx1 = *by1 = -1;
    for (y = y0; y < y1; y++)
        for (x = x0; x < x1; x++)
            if (pix(w, x, y, c)) {
                n++;
                *bx0 = x < *bx0 ? x : *bx0;
                *by0 = y < *by0 ? y : *by0;
                *bx1 = x > *bx1 ? x : *bx1;
                *by1 = y > *by1 ? y : *by1;
            }
    return n;
}

/* Section header vs choice: the header is a heading-style row (no box fill, separator
 * along the bottom, arrow in its first em: down when open, right when closed, title
 * left-aligned after it); the choice keeps its surface box with the arrow on the right. */
static void test_section_header_look(void)
{
    size_t k;
    canlin_theme t;
    canlin_theme_builtin(&t);
    for (k = 0; k < 3; k++) {
        int s = polish_scales[k], w = 5 * s, i;
        harness h;
        const canlin_ui_rect *sec[2], *ch;
        float em = 16.0f * (float)s / 100.0f;
        canlin_theme_rgb tx = t.color[CANLIN_THEME_TEXT], bg = t.color[CANLIN_THEME_BACKGROUND];
        polish_render(&h, section_src, s);
        sec[0] = find(&h, CANLIN_UI_W_SECTION, CANLIN_UI_P_BOX, 0);
        sec[1] = find(&h, CANLIN_UI_W_SECTION, CANLIN_UI_P_BOX, 1);
        ch = find(&h, CANLIN_UI_W_CHOICE, CANLIN_UI_P_BOX, 0);
        CHECK(sec[0] && sec[1] && ch);
        if (!(sec[0] && sec[1] && ch)) {
            h_done(&h);
            continue;
        }
        for (i = 0; i < 2; i++) {
            const canlin_ui_rect *r = sec[i];
            int ax0, ay0, ax1, ay1, tx0, ty0, tx1, ty1, e = (int)em, mid = r->y + r->h / 2;
            CHECK(r->h >= (int)(2.5f * em + 0.999f));
            /* no box fill: background at both ends of the row's centre line */
            CHECK(pix(w, r->x + r->w - 3, mid, bg));
            CHECK(pix(w, r->x + e + 2, r->y + 2, bg));
            /* separator along the bottom edge */
            CHECK(pix(w, r->x + r->w / 2, r->y + r->h - 1, t.color[CANLIN_THEME_MUTED]));
            /* arrow inside the first em */
            CHECK(bbox(w, tx, r->x, r->y, r->x + e, r->y + r->h, &ax0, &ay0, &ax1, &ay1) > 4);
            CHECK(ax0 >= r->x && ax1 < r->x + e);
            if (i == 0) { /* open: pointing down, the top row is the widest */
                int top = bbox(w, tx, ax0, ay0, ax1 + 1, ay0 + 1, &tx0, &ty0, &tx1, &ty1);
                int bot = bbox(w, tx, ax0, ay1, ax1 + 1, ay1 + 1, &tx0, &ty0, &tx1, &ty1);
                CHECK(top > bot && top >= (ax1 - ax0) - 1);
            } else { /* closed: pointing right, the left column is the tallest */
                int lft = bbox(w, tx, ax0, ay0, ax0 + 1, ay1 + 1, &tx0, &ty0, &tx1, &ty1);
                int rgt = bbox(w, tx, ax1, ay0, ax1 + 1, ay1 + 1, &tx0, &ty0, &tx1, &ty1);
                CHECK(lft > rgt && lft >= (ay1 - ay0) - 1);
            }
            /* title left-aligned: starts within the second em, not centred */
            CHECK(bbox(w, tx, r->x + e, r->y, r->x + r->w, r->y + r->h, &tx0, &ty0, &tx1, &ty1) >
                  0);
            CHECK(tx0 >= r->x + e && tx0 < r->x + 2 * e);
        }
        /* choice unchanged: surface box, its arrow in the right part, not the left em */
        {
            int x0, y0, x1, y1;
            CHECK(pix(w, ch->x + ch->w / 2, ch->y + 3, t.color[CANLIN_THEME_SURFACE]));
            CHECK(bbox(w, tx, ch->x + ch->w - 3 * (int)em, ch->y, ch->x + ch->w, ch->y + ch->h,
                       &x0, &y0, &x1, &y1) > 4);
            CHECK(bbox(w, tx, ch->x, ch->y, ch->x + (int)em / 2, ch->y + ch->h, &x0, &y0, &x1,
                       &y1) == 0);
        }
        h_done(&h);
    }
}

/* ---- outputs and UI scale (spec §2.1) ---- */

static const char scale_src[] =
    "local M = {}\n"
    "local name, on = 'x', false\n"
    "function M.draw(ui)\n"
    "  ui:heading('T')\n"
    "  ui:button('A')\n"
    "  on = ui:toggle('T', on)\n"
    "  name = ui:field('Name', name)\n"
    "  ui:separator()\n"
    "  ui:columns(function() ui:button('L') end, function() ui:button('R') end)\n"
    "  ui:section('S', function() ui:button('In') end)\n"
    "  ui:meter('M', 0.5)\n"
    "end\n"
    "return M\n";

/* Same draw at 100 % and 200 % (view twice as wide): every rectangle doubles, within
 * rounding (each edge is rounded once, so at most 2 px per coordinate). */
static void test_scale_doubles(void)
{
    harness a, b;
    size_t na, nb, i;
    const canlin_ui_rect *ra, *rb;
    h_init(&a, NULL, 1.0f, scale_src, 700, 2000);
    h_drain(&a);
    h_init(&b, NULL, 1.0f, scale_src, 1400, 4000);
    CHECK(canlin_ui_output_set_scale(b.out, 200) == CANLIN_UI_OK);
    h_drain(&b);
    CHECK(!has(&a, "draw:") && !has(&b, "draw:"));
    CHECK(canlin_ui_view_em(canlin_module_view(a.m)) == 16.0f);
    CHECK(canlin_ui_view_em(canlin_module_view(b.m)) == 32.0f);
    CHECK(canlin_ui_output_em(b.ui, b.out) == 32.0f);
    ra = rec(&a, &na);
    rb = rec(&b, &nb);
    CHECK(na == nb && na > 10);
    for (i = 0; i < na && i < nb; i++) {
        CHECK(ra[i].kind == rb[i].kind && ra[i].part == rb[i].part);
        CHECK(iabs(rb[i].x - 2 * ra[i].x) <= 2 && iabs(rb[i].y - 2 * ra[i].y) <= 2);
        CHECK(iabs(rb[i].w - 2 * ra[i].w) <= 2 && iabs(rb[i].h - 2 * ra[i].h) <= 2);
    }
    CHECK(iabs(canlin_ui_view_content_height(canlin_module_view(b.m)) -
               2 * canlin_ui_view_content_height(canlin_module_view(a.m))) <= 2);
    h_done(&a);
    h_done(&b);
}

/* 40 em at 100 % is 20 em at 200 %: columns stack after the scale change, the view is
 * marked dirty and redrawn without any module action. */
static void test_scale_columns_dirty(void)
{
    harness h;
    const canlin_ui_rect *l, *r;
    canlin_ui_view *v;
    unsigned draws;
    h_init(&h, NULL, 1.0f, columns_src, 640 + 16, 600);
    h_drain(&h);
    v = canlin_module_view(h.m);
    CHECK(!canlin_ui_view_dirty(v));
    l = find(&h, CANLIN_UI_W_BUTTON, CANLIN_UI_P_BOX, 0);
    r = find(&h, CANLIN_UI_W_BUTTON, CANLIN_UI_P_BOX, 1);
    CHECK(l && r && l->y == r->y);
    draws = h.draws;
    CHECK(canlin_ui_output_set_scale(h.out, 200) == CANLIN_UI_OK);
    CHECK(canlin_ui_view_dirty(v));
    h_drain(&h);
    CHECK(h.draws == draws + 1);
    CHECK(!canlin_ui_view_dirty(v));
    l = find(&h, CANLIN_UI_W_BUTTON, CANLIN_UI_P_BOX, 0);
    r = find(&h, CANLIN_UI_W_BUTTON, CANLIN_UI_P_BOX, 1);
    CHECK(l && r);
    if (l && r) {
        CHECK(r->y > l->y + l->h);        /* stacked */
        CHECK(l->x == 16 && l->w == 624); /* padding 2 units = 16 px at em 32 */
        CHECK(l->h >= 80);                /* 2.5 em at em 32 */
    }
    /* The same value again changes nothing and requests no draw. */
    draws = h.draws;
    CHECK(canlin_ui_output_set_scale(h.out, 200) == CANLIN_UI_OK);
    CHECK(!canlin_ui_view_dirty(v));
    h_drain(&h);
    CHECK(h.draws == draws);
    /* A DPI change invalidates too. */
    CHECK(canlin_ui_output_set_dpi(h.out, 0.5f) == CANLIN_UI_OK);
    CHECK(canlin_ui_view_dirty(v));
    h_drain(&h);
    CHECK(h.draws == draws + 1);
    CHECK(canlin_ui_view_em(v) == 16.0f); /* 16 x 0.5 x 2 */
    h_done(&h);
}

/* 2.5 em minimum at 75 %, including a fractional em (13 px x 1.5 x 0.75 = 14.625). */
static void test_scale_min_touch(void)
{
    static const float dpis[] = {1.0f, 1.5f};
    size_t k, i, n;
    for (k = 0; k < 2; k++) {
        harness h;
        canlin_theme t;
        const canlin_ui_rect *r;
        float em;
        int min, nint = 0;
        canlin_theme_builtin(&t);
        if (k == 1)
            t.font_size = 13;
        h_init(&h, &t, dpis[k], all_src, 400, 2000);
        CHECK(canlin_ui_output_set_scale(h.out, 75) == CANLIN_UI_OK);
        h_drain(&h);
        CHECK(!has(&h, "draw:"));
        em = canlin_ui_view_em(canlin_module_view(h.m));
        CHECK(em == (float)t.font_size * dpis[k] * 0.75f);
        min = (int)(2.5f * em + 0.999f);
        r = rec(&h, &n);
        for (i = 0; i < n; i++)
            if (interactive(&r[i])) {
                nint++;
                CHECK(r[i].h >= min);
            }
        CHECK(nint == 7);
        h_done(&h);
    }
}

static void test_scale_invalid(void)
{
    static const int bad[] = {0, 50, 74, 76, 99, 101, 110, 175, 199, 201, 250, -100, 7500};
    harness h;
    canlin_ui_output_desc d;
    canlin_ui_view *v;
    size_t i;
    int err = 0;
    volatile float zero = 0.0f;
    h_init(&h, NULL, 1.0f, columns_src, 400, 400);
    h_drain(&h);
    v = canlin_module_view(h.m);
    for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        CHECK(!canlin_ui_scale_valid(bad[i]));
        CHECK(canlin_ui_output_set_scale(h.out, bad[i]) == CANLIN_UI_EINVAL);
    }
    CHECK(canlin_ui_output_set_dpi(h.out, 0.0f) == CANLIN_UI_EINVAL);
    CHECK(canlin_ui_output_set_dpi(h.out, -1.0f) == CANLIN_UI_EINVAL);
    CHECK(canlin_ui_output_set_dpi(h.out, 0.25f) == CANLIN_UI_EINVAL);
    CHECK(canlin_ui_output_set_dpi(h.out, 8.0f) == CANLIN_UI_EINVAL);
    CHECK(canlin_ui_output_set_dpi(h.out, zero / zero) == CANLIN_UI_EINVAL);
    CHECK(canlin_ui_output_set_size(h.out, 0, 10) == CANLIN_UI_EINVAL);
    CHECK(canlin_ui_output_set_scale(NULL, 100) == CANLIN_UI_EINVAL);
    CHECK(canlin_ui_output_scale(h.out) == 100 && canlin_ui_output_dpi(h.out) == 1.0f);
    CHECK(!canlin_ui_view_dirty(v)); /* rejected values invalidate nothing */
    CHECK(canlin_ui_scale_valid(75) && canlin_ui_scale_valid(100) && canlin_ui_scale_valid(125) &&
          canlin_ui_scale_valid(150) && canlin_ui_scale_valid(200));

    d.id = "o";
    d.w = d.h = 100;
    d.dpi = 1.0f;
    d.scale = 110;
    CHECK(canlin_ui_output_new(&d, &err) == NULL && err == CANLIN_UI_EINVAL);
    d.scale = 100;
    d.id = "";
    CHECK(canlin_ui_output_new(&d, &err) == NULL && err == CANLIN_UI_EINVAL);
    d.id = NULL;
    CHECK(canlin_ui_output_new(&d, &err) == NULL && err == CANLIN_UI_EINVAL);
    d.id = "o";
    d.dpi = zero / zero;
    CHECK(canlin_ui_output_new(&d, &err) == NULL && err == CANLIN_UI_EINVAL);
    /* An output with a bound view cannot be freed. */
    CHECK(canlin_ui_output_free(h.out) == CANLIN_UI_EBUSY);
    CHECK(strcmp(canlin_ui_output_id(h.out), "test-output") == 0);
    h_done(&h);
}

static const char keep_src[] =
    "local M = {}\n"
    "local b = ''\n"
    "function M.draw(ui)\n"
    "  ui:section('S', function() ui:button('In') end)\n"
    "  b = ui:field('B', b)\n"
    "  print('b=' .. b)\n"
    "end\n"
    "return M\n";

/* Widget state survives a scale change: a closed section stays closed, the focused field
 * keeps focus and its text, typing continues where it was. */
static void test_scale_keeps_state(void)
{
    harness h;
    const canlin_ui_rect *r;
    h_init(&h, NULL, 1.0f, keep_src, 400, 800);
    h_drain(&h);
    CHECK(find(&h, CANLIN_UI_W_BUTTON, CANLIN_UI_P_BOX, 0) != NULL); /* In: open */
    r = find(&h, CANLIN_UI_W_SECTION, CANLIN_UI_P_BOX, 0);
    CHECK(r != NULL);
    if (r)
        click(&h, r);
    h_drain(&h);
    CHECK(find(&h, CANLIN_UI_W_BUTTON, CANLIN_UI_P_BOX, 0) == NULL); /* closed */
    r = find(&h, CANLIN_UI_W_FIELD, CANLIN_UI_P_BOX, 0);
    CHECK(r != NULL);
    if (r)
        click(&h, r);
    h_drain(&h);
    type_text(&h, "ab");
    h_drain(&h);
    CHECK(has(&h, "b=ab"));

    CHECK(canlin_ui_output_set_scale(h.out, 150) == CANLIN_UI_OK);
    h_drain(&h);
    CHECK(canlin_ui_view_em(canlin_module_view(h.m)) == 24.0f);
    CHECK(find(&h, CANLIN_UI_W_BUTTON, CANLIN_UI_P_BOX, 0) == NULL); /* still closed */
    r = find(&h, CANLIN_UI_W_FIELD, CANLIN_UI_P_BOX, 0);
    CHECK(r && r->h >= 60);
    type_text(&h, "c");
    h_drain(&h);
    CHECK(has(&h, "b=abc")); /* focus and text kept */
    h_done(&h);
}

/* nth button box in the record of view v. */
static const canlin_ui_rect *find_button(canlin_ui_view *v, int nth)
{
    size_t n, i;
    const canlin_ui_rect *r = canlin_ui_view_record(v, &n, NULL);
    for (i = 0; i < n; i++)
        if (r[i].kind == CANLIN_UI_W_BUTTON && r[i].part == CANLIN_UI_P_BOX && nth-- == 0)
            return &r[i];
    return NULL;
}

/* Two outputs with different DPI lay out the same draw independently; a change on one
 * invalidates only the views on it. */
static void test_two_outputs(void)
{
    harness h;
    const canlin_ui_rect *l1, *r1, *l2, *r2;
    canlin_ui_view *v1, *v2;
    unsigned d1, d2;
    h_init(&h, NULL, 1.0f, columns_src, 640 + 16, 600);
    h_add_second(&h, "second", 2.0f, 100, columns_src, 640 + 16, 600);
    h_drain(&h);
    CHECK(h.draws == 1 && h.draws2 == 1);
    v1 = canlin_module_view(h.m);
    v2 = canlin_module_view(h.m2);
    CHECK(canlin_ui_view_em(v1) == 16.0f && canlin_ui_view_em(v2) == 32.0f);
    l1 = find_button(v1, 0);
    r1 = find_button(v1, 1);
    l2 = find_button(v2, 0);
    r2 = find_button(v2, 1);
    CHECK(l1 && r1 && l2 && r2);
    if (l1 && r1 && l2 && r2) {
        CHECK(l1->y == r1->y && l1->h == 40);        /* side by side at DPI 1 */
        CHECK(r2->y > l2->y + l2->h && l2->h == 80); /* stacked at DPI 2 */
    }
    /* Each view renders with its own atlas. */
    CHECK(canlin_ui_view_font(v1) != canlin_ui_view_font(v2));
    d1 = h.draws;
    d2 = h.draws2;
    CHECK(canlin_ui_output_set_scale(h.out2, 75) == CANLIN_UI_OK);
    CHECK(!canlin_ui_view_dirty(v1) && canlin_ui_view_dirty(v2));
    h_drain(&h);
    CHECK(h.draws == d1 && h.draws2 == d2 + 1);
    CHECK(canlin_ui_view_em(v1) == 16.0f && canlin_ui_view_em(v2) == 24.0f);
    h_done(&h);
}

/* Font cache: one entry per em, at most CANLIN_UI_FONT_SIZES_MAX, never releasing an
 * entry a live view uses, LRU among the unused ones. Bare views, no Lua. */
static void test_font_cache(void)
{
    static const int scales[5] = {75, 100, 125, 150, 200};
    canlin_theme t;
    canlin_ui *ui;
    canlin_ui_output *o[5];
    canlin_ui_view *v[5];
    const canlin_render_font *f0;
    canlin_ui_output_desc d;
    unsigned held, bakes;
    int i, err;

    CHECK(CANLIN_UI_FONT_SIZES_MAX == 4);
    canlin_theme_builtin(&t);
    ui = canlin_ui_new(&t);
    CHECK(ui != NULL);
    canlin_ui_font_stats(ui, &held, &bakes);
    CHECK(held == 0 && bakes == 0); /* nothing baked before a view needs it */
    for (i = 0; i < 5; i++) {
        char id[8];
        snprintf(id, sizeof id, "o%d", i);
        d.id = id;
        d.w = d.h = 400;
        d.dpi = 1.0f;
        d.scale = scales[i];
        o[i] = canlin_ui_output_new(&d, &err);
        CHECK(o[i] != NULL && err == CANLIN_UI_OK);
    }
    for (i = 0; i < 4; i++) {
        v[i] = canlin_ui_view_new(ui, o[i], &err);
        CHECK(v[i] != NULL);
        canlin_ui_view_set_rect(v[i], 0, 0, 400, 400);
        CHECK(canlin_ui_view_frame_begin(v[i]) == CANLIN_UI_OK);
        canlin_ui_view_frame_end(v[i]);
    }
    /* A second view on an em already cached shares the entry. */
    v[4] = canlin_ui_view_new(ui, o[1], &err);
    CHECK(v[4] != NULL);
    canlin_ui_font_stats(ui, &held, &bakes);
    CHECK(held == 4 && bakes == 4);
    CHECK(canlin_ui_view_font(v[4]) == canlin_ui_view_font(v[1]));
    canlin_ui_view_free(v[4]);
    /* All four pinned: a fifth em fails explicitly, nothing is released. */
    CHECK(canlin_ui_view_new(ui, o[4], &err) == NULL && err == CANLIN_UI_EFONT);
    f0 = canlin_ui_view_font(v[0]);
    CHECK(canlin_ui_output_set_scale(o[0], 200) == CANLIN_UI_OK);
    CHECK(canlin_ui_view_frame_begin(v[0]) == CANLIN_UI_EFONT);
    CHECK(canlin_ui_view_frame_begin(v[0]) == CANLIN_UI_EFONT); /* no frame left open */
    CHECK(canlin_ui_view_font(v[0]) == f0 && canlin_ui_view_em(v[0]) == 12.0f);
    CHECK(canlin_ui_view_dirty(v[0]));
    canlin_ui_font_stats(ui, &held, &bakes);
    CHECK(held == 4 && bakes == 4);

    /* Unpin 100 %: the next frame of v[0] evicts it for 200 % and unpins 75 %. */
    canlin_ui_view_free(v[1]);
    CHECK(canlin_ui_view_frame_begin(v[0]) == CANLIN_UI_OK);
    canlin_ui_view_frame_end(v[0]);
    CHECK(canlin_ui_view_em(v[0]) == 32.0f && !canlin_ui_view_dirty(v[0]));
    canlin_ui_font_stats(ui, &held, &bakes);
    CHECK(held == 4 && bakes == 5);

    /* Unpinned now: 75 % (used longest ago) and, after freeing, 125 % and 150 %. 125 %
     * is touched again, so 75 % is the LRU victim, then 150 %. */
    canlin_ui_view_free(v[2]);
    canlin_ui_view_free(v[3]);
    v[2] = canlin_ui_view_new(ui, o[2], &err); /* hit: 125 % */
    CHECK(v[2] != NULL);
    canlin_ui_view_free(v[2]);
    canlin_ui_font_stats(ui, &held, &bakes);
    CHECK(held == 4 && bakes == 5);
    v[1] = canlin_ui_view_new(ui, o[1], &err); /* 100 %: bakes, evicts 75 % */
    CHECK(v[1] != NULL);
    v[3] = canlin_ui_view_new(ui, o[0], &err); /* 200 %: hit (pinned by v[0]) */
    CHECK(v[3] != NULL);
    canlin_ui_font_stats(ui, &held, &bakes);
    CHECK(held == 4 && bakes == 6);
    v[2] = canlin_ui_view_new(ui, o[3], &err); /* 150 %: still cached */
    CHECK(v[2] != NULL);
    v[4] = canlin_ui_view_new(ui, o[2], &err); /* 125 %: still cached */
    CHECK(v[4] != NULL);
    canlin_ui_font_stats(ui, &held, &bakes);
    CHECK(held == 4 && bakes == 6);
    /* 75 % was the one evicted: it needs a bake, and every slot is pinned now. */
    CHECK(canlin_ui_output_set_scale(o[4], 75) == CANLIN_UI_OK);
    CHECK(canlin_ui_view_new(ui, o[4], &err) == NULL && err == CANLIN_UI_EFONT);

    for (i = 0; i < 5; i++)
        canlin_ui_view_free(v[i]);
    for (i = 0; i < 5; i++)
        CHECK(canlin_ui_output_free(o[i]) == CANLIN_UI_OK);
    canlin_ui_free(ui);
}

/* A module whose output needs a fifth pinned size: its draw is skipped with a log line,
 * not run headless and not counted as a module error; it draws once a slot frees. */
static void test_font_cache_module(void)
{
    static const int scales[3] = {75, 125, 150};
    harness h;
    canlin_ui_output *o[3];
    canlin_ui_view *v[3];
    canlin_ui_output_desc d;
    unsigned draws;
    int i, err;
    h_init(&h, NULL, 1.0f, button_src, 300, 300);
    h_drain(&h);
    for (i = 0; i < 3; i++) {
        d.id = "pin";
        d.w = d.h = 300;
        d.dpi = 1.0f;
        d.scale = scales[i];
        o[i] = canlin_ui_output_new(&d, &err);
        v[i] = canlin_ui_view_new(h.ui, o[i], &err);
        CHECK(v[i] != NULL);
    }
    draws = h.draws;
    CHECK(canlin_ui_output_set_scale(h.out, 200) == CANLIN_UI_OK);
    h_drain(&h);
    CHECK(has(&h, "draw: skipped: every font size slot is pinned"));
    CHECK(h.draws == draws + 1); /* delivered, but Lua was not called */
    CHECK(canlin_module_get_state(h.m) == CANLIN_MODULE_RUNNING);
    CHECK(canlin_module_errors(h.m) == 0);
    CHECK(canlin_ui_view_em(canlin_module_view(h.m)) == 16.0f); /* previous frame kept */
    CHECK(find(&h, CANLIN_UI_W_BUTTON, CANLIN_UI_P_BOX, 0) != NULL);
    canlin_ui_view_free(v[0]);
    CHECK(canlin_module_set_ui(h.m, h.ui, h.out, 0, 0, 300, 300) == CANLIN_MODULE_OK);
    h_drain(&h);
    CHECK(canlin_ui_view_em(canlin_module_view(h.m)) == 32.0f);
    for (i = 1; i < 3; i++)
        canlin_ui_view_free(v[i]);
    for (i = 0; i < 3; i++)
        CHECK(canlin_ui_output_free(o[i]) == CANLIN_UI_OK);
    h_done(&h);
}

/* Lua has no path to scale, DPI or the output: the whole canlin.* surface is an
 * allowlist here (a new entry must be reviewed against §2.1), and the ui object carries
 * no metric accessors and cannot be written. */
static void test_lua_no_scale(void)
{
    harness h;
    h_init(&h, NULL, 1.0f,
           "local M = {}\n"
           "local function walk(t, p)\n"
           "  for k, v in pairs(t) do\n"
           "    if type(v) == 'table' then walk(v, p .. k .. '.')\n"
           "    else print('api ' .. p .. k) end\n"
           "  end\n"
           "end\n"
           "function M.draw(ui)\n"
           "  walk(canlin, 'canlin.')\n"
           "  for _, k in ipairs({'scale', 'set_scale', 'dpi', 'set_dpi', 'output',\n"
           "                      'set_output', 'em', 'metrics', 'set_metrics'}) do\n"
           "    print('ui.' .. k .. '=' .. tostring(ui[k]))\n"
           "  end\n"
           "  print(pcall(function() ui.scale = 2 end))\n"
           "end\n"
           "return M\n",
           400, 400);
    h_drain(&h);
    CHECK(count(&h, "api canlin.") == 4);
    CHECK(has(&h, "api canlin.timer.after\n") && has(&h, "api canlin.cancel\n") &&
          has(&h, "api canlin.invalidate\n") && has(&h, "api canlin.sys.now\n"));
    CHECK(count(&h, "=nil\n") == 9);
    CHECK(has(&h, "ui is read-only"));
    CHECK(canlin_ui_view_em(canlin_module_view(h.m)) == 16.0f);
    CHECK(canlin_ui_output_scale(h.out) == 100);
    h_done(&h);
}

int main(void)
{
    test_columns();
    test_min_height_rounding();
    test_wrap_ellipsis();
    test_button_once();
    test_controls();
    test_focus_identity();
    test_errors();
    test_after_draw();
    test_columns_pcall();
    test_headless();
    test_input_overflow();
    test_input_order();
    test_input_overflow_latch();
    test_input_stale_generation();
    test_input_no_lua();
    test_scroll();
    test_draw_charge();
    test_render_e2e();
    test_view_background_square();
    test_toggle_geometry();
    test_section_header_look();
    test_scale_doubles();
    test_scale_columns_dirty();
    test_scale_min_touch();
    test_scale_invalid();
    test_scale_keeps_state();
    test_two_outputs();
    test_font_cache();
    test_font_cache_module();
    test_lua_no_scale();
    if (failures) {
        fprintf(stderr, "ui: %d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("ui: %d checks passed\n", checks);
    return 0;
}
