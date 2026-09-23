/* On-screen keyboard contract tests (UI spec §3, canlin#45): real sandbox and scheduler
 * with a fake clock, the built-in theme and font, offscreen. Keys are found through the
 * layout record (CANLIN_UI_W_KEYBOARD / CANLIN_UI_P_KEY) and tapped with ordinary pointer
 * input through canlin_module_post_input, like a touch backend would. Exits 0 on success. */
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

static void clear_log(harness *h)
{
    h->loglen = 0;
    h->log[0] = 0;
}

static void h_drain(harness *h)
{
    int i;
    canlin_ev_turn_result r;
    for (i = 0; i < 64; i++)
        CHECK(canlin_ev_turn(h->ev, &r) == CANLIN_EV_OK);
}

static void h_init(harness *h, unsigned input, float dpi, const char *src, int w, int vh)
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
    d.id = "kbd-test";
    d.w = w;
    d.h = vh;
    d.dpi = dpi;
    d.scale = 100;
    h->out = canlin_ui_output_new(&d, NULL);
    if (!h->ev || !h->ui || !h->out) {
        fprintf(stderr, "harness init failed\n");
        exit(1);
    }
    CHECK(canlin_ui_output_input(h->out) == 0);
    CHECK(canlin_ui_output_set_input(h->out, input) == CANLIN_UI_OK);
    memset(&s, 0, sizeof s);
    memset(&cfg, 0, sizeof cfg);
    s.name = "kbdtest";
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

static canlin_ui_view *view(harness *h)
{
    return canlin_module_view(h->m);
}

static const canlin_ui_rect *find(harness *h, int kind, int part, int nth)
{
    size_t n, i;
    const canlin_ui_rect *r = canlin_ui_view_record(view(h), &n, NULL);
    for (i = 0; i < n; i++)
        if (r[i].kind == kind && r[i].part == part && nth-- == 0)
            return &r[i];
    return NULL;
}

static const canlin_ui_rect *panel(harness *h)
{
    return find(h, CANLIN_UI_W_KEYBOARD, CANLIN_UI_P_BOX, 0);
}

/* Key of function fn; for CHAR the one that types cp now. */
static const canlin_ui_rect *kkey(harness *h, int fn, uint32_t cp)
{
    size_t n, i;
    const canlin_ui_rect *r = canlin_ui_view_record(view(h), &n, NULL);
    for (i = 0; i < n; i++)
        if (r[i].kind == CANLIN_UI_W_KEYBOARD && r[i].part == CANLIN_UI_P_KEY &&
            r[i].len == (uint32_t)fn && (fn != CANLIN_UI_KBD_CHAR || r[i].off == cp))
            return &r[i];
    return NULL;
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

static void tap_at(harness *h, int x, int y)
{
    post(h, CANLIN_UI_IN_MOTION, x, y, 0);
    post(h, CANLIN_UI_IN_BUTTON, x, y, 1);
    h_drain(h);
    post(h, CANLIN_UI_IN_BUTTON, x, y, 0);
    h_drain(h);
}

static void tap(harness *h, const canlin_ui_rect *r)
{
    CHECK(r != NULL);
    if (r)
        tap_at(h, r->x + r->w / 2, r->y + r->h / 2);
}

static void tap_fn(harness *h, int fn)
{
    tap(h, kkey(h, fn, 0));
}

static void tap_char(harness *h, uint32_t cp)
{
    const canlin_ui_rect *r = kkey(h, CANLIN_UI_KBD_CHAR, cp);
    if (r == NULL)
        fprintf(stderr, "no key for U+%04X\n", (unsigned)cp);
    tap(h, r);
}

/* The latest "tag=value" line of the log equals want. */
static int last_is(const harness *h, const char *tag, const char *want)
{
    const char *p = h->log, *hit = NULL;
    size_t tl = strlen(tag), wl = strlen(want);
    while ((p = strstr(p, tag)) != NULL) {
        if ((p == h->log || p[-1] == '\n') && p[tl] == '=')
            hit = p + tl + 1;
        p += tl;
    }
    return hit && strncmp(hit, want, wl) == 0 && hit[wl] == '\n';
}

/* ---- the test module ---- */

/* `fill` text rows push the field down; name and notes are printed when they change
 * (newlines in notes as '|'). M.event would print EV: nothing may reach it. */
static const char module_fmt[] =
    "local M = {}\n"
    "local name, notes = '', ''\n"
    "function M.event(n) print('EV ' .. tostring(n)) end\n"
    "function M.draw(ui)\n"
    "  for i = 1, %d do ui:text('filler ' .. i) end\n"
    "  local n = ui:field('Name', name)\n"
    "  local a = ui:area('Notes', notes, { rows = 3 })\n"
    "  if ui:button('Go') then print('clicked') end\n"
    "  if n ~= name then name = n; print('name=' .. name) end\n"
    "  if a ~= notes then notes = a; print('notes=' .. (notes:gsub('\\n', '|'))) end\n"
    "end\n"
    "return M\n";

static char srcbuf[2048];

static const char *src(int fill)
{
    snprintf(srcbuf, sizeof srcbuf, module_fmt, fill);
    return srcbuf;
}

static const canlin_ui_rect *field_box(harness *h)
{
    return find(h, CANLIN_UI_W_FIELD, CANLIN_UI_P_BOX, 0);
}

/* 720x1280 at DPI 1.5 (em 24): keys 2.5 em = 60 px square. */
static void phone(harness *h, unsigned input)
{
    h_init(h, input, 1.5f, src(0), 720, 1280);
    clear_log(h);
}

/* ---- tests ---- */

/* The panel exists only on touch-without-keyboard outputs, and only while a field or
 * area is focused; no redraw loop is left behind. */
static void test_visibility(void)
{
    static const unsigned no_panel[] = {0, CANLIN_UI_INPUT_POINTER,
                                        CANLIN_UI_INPUT_TOUCH | CANLIN_UI_INPUT_KEYBOARD,
                                        CANLIN_UI_INPUT_POINTER | CANLIN_UI_INPUT_KEYBOARD};
    harness h;
    size_t i;
    for (i = 0; i < sizeof no_panel / sizeof no_panel[0]; i++) {
        phone(&h, no_panel[i]);
        tap(&h, field_box(&h));
        CHECK(panel(&h) == NULL);
        h_done(&h);
    }

    phone(&h, CANLIN_UI_INPUT_TOUCH);
    CHECK(panel(&h) == NULL); /* nothing focused yet */
    tap(&h, field_box(&h));
    CHECK(panel(&h) != NULL);
    CHECK(canlin_ui_view_pending(view(&h)) == 0);
    if (panel(&h)) {
        const canlin_ui_rect *p = panel(&h);
        CHECK(p->x == 0 && p->w == 720 && p->y + p->h == 1280);
        CHECK(p->h == 4 * 60 + 2 * 12); /* four 2.5 em rows plus the view padding */
    }
    CHECK(kkey(&h, CANLIN_UI_KBD_CHAR, 'q')->w == 60 && kkey(&h, CANLIN_UI_KBD_CHAR, 'q')->h == 60);
    /* The area takes it too; a button does not, and pressing it drops the field focus. */
    tap(&h, find(&h, CANLIN_UI_W_AREA, CANLIN_UI_P_BOX, 0));
    CHECK(panel(&h) != NULL && kkey(&h, CANLIN_UI_KBD_ENTER, 0)->off == 1);
    tap(&h, find(&h, CANLIN_UI_W_BUTTON, CANLIN_UI_P_BOX, 0));
    CHECK(has(&h, "clicked") && panel(&h) == NULL);
    CHECK(canlin_ui_view_pending(view(&h)) == 0);
    /* The output changes to one with a keyboard: the panel goes with the next frame. */
    tap(&h, field_box(&h));
    CHECK(panel(&h) != NULL);
    CHECK(canlin_ui_output_set_input(h.out, CANLIN_UI_INPUT_TOUCH | CANLIN_UI_INPUT_KEYBOARD) ==
          CANLIN_UI_OK);
    h_drain(&h);
    CHECK(panel(&h) == NULL);
    CHECK(canlin_ui_output_set_input(h.out, 8) == CANLIN_UI_EINVAL);
    CHECK(!has(&h, "EV "));
    h_done(&h);
}

/* Typing through the panel reaches the field as ordinary text; Lua sees only the value. */
static void test_typing(void)
{
    harness h;
    phone(&h, CANLIN_UI_INPUT_TOUCH);
    tap(&h, field_box(&h));
    tap_fn(&h, CANLIN_UI_KBD_SHIFT);
    tap_char(&h, 'G');
    tap_char(&h, 'r');
    tap_char(&h, 0xfc);
    tap_char(&h, 0xdf);
    tap_char(&h, 'e');
    tap_fn(&h, CANLIN_UI_KBD_SPACE);
    tap_fn(&h, CANLIN_UI_KBD_LAYER);
    CHECK(kkey(&h, CANLIN_UI_KBD_LAYER, 0)->off == 1);
    tap_char(&h, '1');
    tap_char(&h, '2');
    tap_char(&h, 0x20ac);
    CHECK(last_is(&h, "name", "Gr\xC3\xBC\xC3\x9F" "e 12\xE2\x82\xAC"));
    CHECK(!has(&h, "EV "));
    /* Backspace deletes one character (a code point, not a byte). */
    tap_fn(&h, CANLIN_UI_KBD_BACKSPACE);
    CHECK(last_is(&h, "name", "Gr\xC3\xBC\xC3\x9F" "e 12"));
    tap_fn(&h, CANLIN_UI_KBD_LAYER);
    CHECK(kkey(&h, CANLIN_UI_KBD_LAYER, 0)->off == 0);
    CHECK(canlin_ui_view_pending(view(&h)) == 0);
    h_done(&h);
}

/* Shift is one-shot; a quick second tap locks capitals until the next tap. */
static void test_shift(void)
{
    harness h;
    phone(&h, CANLIN_UI_INPUT_TOUCH);
    tap(&h, field_box(&h));
    CHECK(kkey(&h, CANLIN_UI_KBD_SHIFT, 0)->off == 0 && kkey(&h, CANLIN_UI_KBD_CHAR, 'a'));
    tap_fn(&h, CANLIN_UI_KBD_SHIFT);
    CHECK(kkey(&h, CANLIN_UI_KBD_SHIFT, 0)->off == 1 && kkey(&h, CANLIN_UI_KBD_CHAR, 'A'));
    CHECK(kkey(&h, CANLIN_UI_KBD_CHAR, 0xc4) && kkey(&h, CANLIN_UI_KBD_CHAR, 0xdf)); /* Ä, ß */
    tap_char(&h, 'A');
    CHECK(kkey(&h, CANLIN_UI_KBD_SHIFT, 0)->off == 0);
    tap_char(&h, 'a');
    CHECK(last_is(&h, "name", "Aa"));
    /* A slow second tap turns shift off again. */
    tap_fn(&h, CANLIN_UI_KBD_SHIFT);
    canlin_ev_fake_clock_advance(&h.clock, CANLIN_UI_DOUBLE_TAP_MS + 1);
    tap_fn(&h, CANLIN_UI_KBD_SHIFT);
    CHECK(kkey(&h, CANLIN_UI_KBD_SHIFT, 0)->off == 0);
    /* Double tap: caps lock. */
    tap_fn(&h, CANLIN_UI_KBD_SHIFT);
    tap_fn(&h, CANLIN_UI_KBD_SHIFT);
    CHECK(kkey(&h, CANLIN_UI_KBD_SHIFT, 0)->off == 2);
    tap_char(&h, 'B');
    tap_char(&h, 0xd6);
    CHECK(kkey(&h, CANLIN_UI_KBD_SHIFT, 0)->off == 2);
    tap_fn(&h, CANLIN_UI_KBD_SHIFT);
    CHECK(kkey(&h, CANLIN_UI_KBD_SHIFT, 0)->off == 0);
    tap_char(&h, 'c');
    CHECK(last_is(&h, "name", "AaB\xC3\x96" "c"));
    h_done(&h);
}

/* Enter in a field moves the focus on (the area: the panel stays); in an area it inserts
 * a newline. The accent marks a pressed key while it is held. */
static void test_enter(void)
{
    harness h;
    const canlin_ui_rect *k;
    phone(&h, CANLIN_UI_INPUT_TOUCH);
    tap(&h, field_box(&h));
    tap_char(&h, 'x');
    CHECK(kkey(&h, CANLIN_UI_KBD_ENTER, 0)->off == 0);
    tap_fn(&h, CANLIN_UI_KBD_ENTER);
    CHECK(panel(&h) != NULL && kkey(&h, CANLIN_UI_KBD_ENTER, 0)->off == 1); /* area focused */
    CHECK(last_is(&h, "name", "x"));
    tap_char(&h, 'a');
    tap_fn(&h, CANLIN_UI_KBD_ENTER);
    tap_char(&h, 'b');
    CHECK(last_is(&h, "notes", "a|b") && last_is(&h, "name", "x"));
    /* Held key: accent fill under it until release. */
    k = kkey(&h, CANLIN_UI_KBD_CHAR, 'k');
    CHECK(k != NULL);
    if (k) {
        int x = k->x + k->w / 2, y = k->y + k->h / 2;
        const struct nk_command *cmd;
        canlin_theme_rgb ac = canlin_ui_theme(h.ui)->color[CANLIN_THEME_ACCENT];
        int accent = 0;
        post(&h, CANLIN_UI_IN_BUTTON, x, y, 1);
        h_drain(&h);
        nk_foreach(cmd, canlin_ui_view_nk(view(&h)))
        {
            if (cmd->type == NK_COMMAND_RECT_FILLED) {
                const struct nk_command_rect_filled *f = (const void *)cmd;
                if (f->color.r == ac.r && f->color.g == ac.g && f->color.b == ac.b &&
                    f->x > k->x && f->x < k->x + k->w && f->y > k->y && f->y < k->y + k->h)
                    accent = 1;
            }
        }
        CHECK(accent);
        CHECK(last_is(&h, "notes", "a|b")); /* acts at release */
        post(&h, CANLIN_UI_IN_BUTTON, x, y, 0);
        h_drain(&h);
        CHECK(last_is(&h, "notes", "a|bk"));
    }
    /* A press that slides off its key before release types nothing. */
    k = kkey(&h, CANLIN_UI_KBD_CHAR, 'l');
    if (k) {
        post(&h, CANLIN_UI_IN_BUTTON, k->x + 2, k->y + 2, 1);
        post(&h, CANLIN_UI_IN_BUTTON, k->x - 20, k->y + 2, 0);
        h_drain(&h);
        CHECK(last_is(&h, "notes", "a|bk"));
    }
    h_done(&h);
}

/* Hide removes the panel but keeps the focus and value; a tap on the field shows it. */
static void test_hide(void)
{
    harness h;
    canlin_ui_input in;
    phone(&h, CANLIN_UI_INPUT_TOUCH);
    tap(&h, field_box(&h));
    tap_char(&h, 'q');
    tap_fn(&h, CANLIN_UI_KBD_HIDE);
    CHECK(panel(&h) == NULL);
    CHECK(canlin_ui_view_pending(view(&h)) == 0);
    memset(&in, 0, sizeof in);
    in.type = CANLIN_UI_IN_TEXT;
    in.cp = 'w'; /* still the field's: its focus survived */
    CHECK(canlin_module_post_input(h.m, &in) == CANLIN_MODULE_OK);
    h_drain(&h);
    CHECK(last_is(&h, "name", "qw"));
    tap(&h, field_box(&h));
    CHECK(panel(&h) != NULL);
    tap_char(&h, 'e');
    CHECK(last_is(&h, "name", "qwe"));
    h_done(&h);
}

/* Keys tile each row's width without gaps or overlap, rows are equally wide and stacked
 * without overlap; narrower views shrink keys evenly, square down to 1.5 em. */
static void check_tiling(harness *h, int w, float em)
{
    size_t n, i;
    const canlin_ui_rect *r = canlin_ui_view_record(view(h), &n, NULL);
    const canlin_ui_rect *prev = NULL;
    int row_x0 = -1, row_x1 = -1, rows = 0, keys = 0;
    for (i = 0; i < n; i++) {
        if (r[i].kind != CANLIN_UI_W_KEYBOARD || r[i].part != CANLIN_UI_P_KEY)
            continue;
        keys++;
        CHECK(r[i].x >= 0 && r[i].x + r[i].w <= w && r[i].w > 0);
        CHECK(r[i].w <= (int)(5.5f * 2.5f * em) + 1); /* widest key: space, 5.5 columns */
        CHECK(r[i].h >= (int)(1.5f * em) && r[i].h <= (int)(2.5f * em + 0.5f));
        if (prev && prev->y == r[i].y) {
            CHECK(r[i].x == prev->x + prev->w); /* abut */
        } else {
            if (prev) {
                CHECK(prev->x + prev->w == row_x1 || row_x1 < 0);
                row_x1 = prev->x + prev->w;
                CHECK(r[i].y == prev->y + prev->h); /* rows stack */
            }
            if (row_x0 >= 0)
                CHECK(r[i].x == row_x0);
            row_x0 = r[i].x;
            rows++;
        }
        prev = &r[i];
    }
    CHECK(prev && prev->x + prev->w == row_x1);
    CHECK(keys == 36 && rows == 4);
}

static void test_narrow(void)
{
    harness h;
    const canlin_ui_rect *q;
    /* 400 px at DPI 1 (em 16): inner 384 / 11 = 34.9 px keys, square. */
    h_init(&h, CANLIN_UI_INPUT_TOUCH, 1.0f, src(0), 400, 700);
    tap(&h, field_box(&h));
    CHECK(panel(&h) != NULL);
    q = kkey(&h, CANLIN_UI_KBD_CHAR, 'q');
    CHECK(q && q->w >= 34 && q->w <= 35 && q->h >= 34 && q->h <= 35);
    check_tiling(&h, 400, 16.0f);
    tap_char(&h, 'o');
    CHECK(last_is(&h, "name", "o"));
    h_done(&h);
    /* 200 px: 184 / 11 = 16.7 px wide, below 1.5 em: height stays 24. */
    h_init(&h, CANLIN_UI_INPUT_TOUCH, 1.0f, src(0), 200, 700);
    tap(&h, field_box(&h));
    CHECK(panel(&h) != NULL);
    q = kkey(&h, CANLIN_UI_KBD_CHAR, 'q');
    CHECK(q && q->w >= 16 && q->w <= 17 && q->h == 24);
    check_tiling(&h, 200, 16.0f);
    tap_char(&h, 0xe4);
    CHECK(last_is(&h, "name", "\xC3\xA4"));
    h_done(&h);
    /* Too low for the panel plus one touch row of content: no panel. */
    h_init(&h, CANLIN_UI_INPUT_TOUCH, 1.0f, src(0), 400, 190);
    tap(&h, field_box(&h));
    CHECK(panel(&h) == NULL);
    CHECK(canlin_ui_view_pending(view(&h)) == 0);
    h_done(&h);
}

/* A field in the part the panel will cover is scrolled above it when the panel appears,
 * and back into view when typed into after the user scrolled away. */
static void test_field_visible(void)
{
    harness h;
    const canlin_ui_rect *f, *p;
    canlin_ui_input in;
    h_init(&h, CANLIN_UI_INPUT_TOUCH, 1.5f, src(24), 720, 1280);
    f = field_box(&h);
    CHECK(f && f->y > 1280 - 264 && f->y + f->h <= 1280); /* visible now, under the panel later */
    tap(&h, f);
    p = panel(&h);
    f = field_box(&h);
    CHECK(p && f && f->y >= 0 && f->y + f->h <= p->y);
    CHECK(canlin_ui_view_scroll(view(&h)) > 0);
    /* Scroll the field out of view, then type: it comes back. */
    memset(&in, 0, sizeof in);
    in.type = CANLIN_UI_IN_SCROLL;
    in.dy = -100000;
    CHECK(canlin_module_post_input(h.m, &in) == CANLIN_MODULE_OK);
    h_drain(&h);
    CHECK(canlin_ui_view_scroll(view(&h)) == 0);
    tap_char(&h, 'z');
    CHECK(last_is(&h, "name", "z"));
    p = panel(&h);
    f = field_box(&h);
    CHECK(p && f && f->y >= 0 && f->y + f->h <= p->y);
    h_done(&h);
}

int main(void)
{
    test_visibility();
    test_typing();
    test_shift();
    test_enter();
    test_hide();
    test_narrow();
    test_field_visible();
    if (failures) {
        fprintf(stderr, "kbd: %d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("kbd: %d checks passed\n", checks);
    return 0;
}
