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
    canlin_module *m;
    char log[LOGMAX];
    size_t loglen;
    unsigned draws;
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
        canlin_module_deliver(h->m, d);
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
    h->ui = canlin_ui_new(&t, dpi);
    if (!h->ev || !h->ui) {
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
    CHECK(canlin_module_set_ui(h->m, h->ui, 0, 0, w, vh) == CANLIN_MODULE_OK);
}

static void h_done(harness *h)
{
    canlin_module_free(h->m);
    CHECK(canlin_ev_free(h->ev) == CANLIN_EV_OK);
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
    CHECK(canlin_module_input(h->m, &in) == CANLIN_MODULE_OK);
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
        CHECK(canlin_module_input(h->m, &in) == CANLIN_MODULE_OK);
    }
}

static void key(harness *h, int k)
{
    canlin_ui_input in;
    memset(&in, 0, sizeof in);
    in.type = CANLIN_UI_IN_KEY;
    in.key = k;
    in.down = 1;
    CHECK(canlin_module_input(h->m, &in) == CANLIN_MODULE_OK);
    in.down = 0;
    CHECK(canlin_module_input(h->m, &in) == CANLIN_MODULE_OK);
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
        em = canlin_ui_em(h.ui);
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
    f = &canlin_ui_font(h.ui)->font->handle;
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
    expect_draw_error("ui:graph({nodes = {}, edges = {}})", "not implemented");
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
    CHECK(canlin_module_input(h.m, &in) == CANLIN_MODULE_EINVAL);
    h_done(&h);
}

/* The core-owned scroll offset moves every rect and is clamped to the content. */
static void test_scroll(void)
{
    harness h;
    const canlin_ui_rect *b;
    int y0, ch;
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
    in.dy = 100;
    CHECK(canlin_module_input(h.m, &in) == CANLIN_MODULE_OK);
    h_drain(&h);
    b = find(&h, CANLIN_UI_W_BUTTON, CANLIN_UI_P_BOX, 0);
    CHECK(b && b->y == y0 - 100);
    in.dy = 100000;
    CHECK(canlin_module_input(h.m, &in) == CANLIN_MODULE_OK);
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
    f = canlin_ui_font(h.ui);
    canlin_render_set_atlas(&r, f->alpha, f->w, f->h);
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
    test_scroll();
    test_draw_charge();
    test_render_e2e();
    if (failures) {
        fprintf(stderr, "ui: %d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("ui: %d checks passed\n", checks);
    return 0;
}
