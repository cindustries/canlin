/* Shell contract tests (shell spec §2-§7): real sandbox, real scheduler with a fake
 * clock, a fake wall clock for the bar, the built-in theme and font, offscreen. Built-in
 * theme: em = 16 px at DPI 1.0 and scale 100 %, 20 px at 125 %. Exits 0 on success. */
#include "canlin/shell.h"

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

#define NMOD 3
#define LOGMAX 8192
#define MAXW 1600
#define MAXH 1000

static const char *SRC =
    "local M = {}\n"
    "function M.init() end\n"
    "function M.draw(ui)\n"
    "  if ui:button('Go') then print('clicked') end\n"
    "  ui:text('hello')\n"
    "end\n"
    "return M\n";

typedef struct harness {
    canlin_ev_fake_clock clock;
    canlin_ev *ev;
    canlin_ui *ui;
    canlin_ui_output *out;
    canlin_shell *sh;
    canlin_module *m[NMOD];
    canlin_module_source src[NMOD];
    char log[NMOD][LOGMAX];
    size_t loglen[NMOD];
    unsigned draws[NMOD];
    unsigned keys[NMOD], buttons[NMOD], motions[NMOD];
    int last_mx[NMOD], last_my[NMOD];
    int64_t wall;
    canlin_render r;
    unsigned char px[MAXW * MAXH * 3];
    unsigned char dirty[MAXH];
} harness;

static harness H;

static const char *NAMES[NMOD] = {"alpha", "beta", "gamma"};

static void log_fn(void *ud, const char *msg, size_t len)
{
    int i = (int)(size_t)ud;
    if (len + 2 > LOGMAX - H.loglen[i])
        return;
    memcpy(H.log[i] + H.loglen[i], msg, len);
    H.loglen[i] += len;
    H.log[i][H.loglen[i]++] = '\n';
    H.log[i][H.loglen[i]] = 0;
}

static unsigned count(int i, const char *needle)
{
    unsigned n = 0;
    const char *p = H.log[i];
    while ((p = strstr(p, needle)) != NULL) {
        n++;
        p += strlen(needle);
    }
    return n;
}

static void on_deliver(void *ud, canlin_ev *ev, const canlin_ev_delivery *d)
{
    int i;
    (void)ud;
    (void)ev;
    for (i = 0; i < NMOD; i++) {
        if (H.m[i] && canlin_module_gen(H.m[i]) == d->gen) {
            if (d->kind == CANLIN_EV_D_DRAW)
                H.draws[i]++;
            if (d->kind == CANLIN_EV_D_EVENT && d->source == CANLIN_MODULE_SOURCE_INPUT) {
                unsigned t = d->type & 0xffu;
                if (t == CANLIN_UI_IN_KEY)
                    H.keys[i]++;
                if (t == CANLIN_UI_IN_BUTTON)
                    H.buttons[i]++;
                if (t == CANLIN_UI_IN_MOTION) {
                    H.motions[i]++;
                    H.last_mx[i] = d->a;
                    H.last_my[i] = d->b;
                }
            }
            canlin_module_deliver(H.m[i], d);
            return;
        }
    }
}

static int64_t wall_fn(void *ud)
{
    (void)ud;
    return H.wall;
}

static void drain(void)
{
    int k;
    canlin_ev_turn_result r;
    for (k = 0; k < 32; k++)
        CHECK(canlin_ev_turn(H.ev, &r) == CANLIN_EV_OK);
}

/* w x h px at the given scale; nmod modules added to the shell in order. */
static void h_init(int w, int h, int scale, int nmod, unsigned input)
{
    canlin_ev_clock c;
    canlin_ev_callbacks cb;
    canlin_theme t;
    canlin_ui_output_desc d;
    canlin_shell_config sc;
    int i, err;

    memset(&H, 0, sizeof H);
    H.wall = 10 * 3600 + 5; /* 10:00:05 */
    c = canlin_ev_fake_clock_bind(&H.clock);
    cb.deliver = on_deliver;
    H.ev = canlin_ev_new(&c, &cb, NULL);
    canlin_theme_builtin(&t);
    H.ui = canlin_ui_new(&t);
    d.id = "test-output";
    d.w = w;
    d.h = h;
    d.dpi = 1.0f;
    d.scale = scale;
    H.out = canlin_ui_output_new(&d, NULL);
    memset(&sc, 0, sizeof sc);
    sc.ui = H.ui;
    sc.clock = wall_fn;
    H.sh = canlin_shell_new(&sc, &err);
    if (!H.ev || !H.ui || !H.out || !H.sh) {
        fprintf(stderr, "harness init failed\n");
        exit(1);
    }
    CHECK(canlin_shell_add_output(H.sh, H.out, input) == CANLIN_SHELL_OK);
    for (i = 0; i < nmod; i++) {
        canlin_sandbox_config cfg;
        memset(&H.src[i], 0, sizeof H.src[i]);
        memset(&cfg, 0, sizeof cfg);
        H.src[i].name = NAMES[i];
        H.src[i].version = "1";
        H.src[i].main = SRC;
        H.src[i].main_len = strlen(SRC);
        cfg.log = log_fn;
        cfg.ud = (void *)(size_t)i;
        H.m[i] = canlin_module_load(H.ev, &H.src[i], &cfg, &err);
        if (!H.m[i]) {
            fprintf(stderr, "module load failed (%d)\n", err);
            exit(1);
        }
        CHECK(canlin_shell_add_module(H.sh, H.m[i], &H.src[i]) == CANLIN_SHELL_OK);
    }
    CHECK(canlin_render_init(&H.r, H.px, w, h, (size_t)w * 3, H.dirty, NULL) == 0);
    canlin_shell_update(H.sh);
    drain();
}

static void h_done(void)
{
    int i;
    canlin_shell_free(H.sh);
    for (i = 0; i < NMOD; i++)
        canlin_module_free(H.m[i]);
    CHECK(canlin_ev_free(H.ev) == CANLIN_EV_OK);
    CHECK(canlin_ui_output_free(H.out) == CANLIN_UI_OK);
    canlin_ui_free(H.ui);
}

/* update + render when dirty; returns the number of rows written and the lo..hi range. */
static int frame(int *lo, int *hi)
{
    int n, y;
    canlin_shell_update(H.sh);
    canlin_render_begin(&H.r);
    if (canlin_shell_dirty(H.sh, H.out))
        canlin_shell_render(H.sh, H.out, &H.r);
    n = canlin_render_end(&H.r);
    if (lo)
        *lo = -1;
    if (hi)
        *hi = -1;
    for (y = 0; y < H.r.h; y++)
        if (H.dirty[y]) {
            if (lo && *lo < 0)
                *lo = y;
            if (hi)
                *hi = y;
        }
    return n;
}

static void settle(void)
{
    int k;
    for (k = 0; k < 4; k++) {
        canlin_shell_update(H.sh);
        drain();
        frame(NULL, NULL);
    }
}

static canlin_ui_input button(int x, int y, int down)
{
    canlin_ui_input in;
    memset(&in, 0, sizeof in);
    in.type = CANLIN_UI_IN_BUTTON;
    in.x = x;
    in.y = y;
    in.down = down;
    return in;
}

static canlin_ui_input motion(int x, int y)
{
    canlin_ui_input in;
    memset(&in, 0, sizeof in);
    in.type = CANLIN_UI_IN_MOTION;
    in.x = x;
    in.y = y;
    return in;
}

static canlin_ui_input key(int k, int down)
{
    canlin_ui_input in;
    memset(&in, 0, sizeof in);
    in.type = CANLIN_UI_IN_KEY;
    in.key = k;
    in.down = down;
    return in;
}

static int tap(int x, int y)
{
    canlin_ui_input a = button(x, y, 1), b = button(x, y, 0);
    int r = canlin_shell_input(H.sh, H.out, &a);
    canlin_shell_input(H.sh, H.out, &b);
    return r;
}

static canlin_shell_rect part(canlin_shell_part p, int idx)
{
    canlin_shell_rect r;
    memset(&r, 0, sizeof r);
    CHECK(canlin_shell_part_rect(H.sh, H.out, p, idx, &r) == 1);
    return r;
}

static int has_part(canlin_shell_part p, int idx)
{
    canlin_shell_rect r;
    return canlin_shell_part_rect(H.sh, H.out, p, idx, &r);
}

static int overlap(canlin_shell_rect a, canlin_shell_rect b)
{
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

static int contains(canlin_shell_rect o, canlin_shell_rect i)
{
    return i.x >= o.x && i.y >= o.y && i.x + i.w <= o.x + o.w && i.y + i.h <= o.y + o.h;
}

/* The Go button of module i's view, from its layout record. */
static canlin_ui_rect go_button(int i)
{
    size_t n, k;
    canlin_ui_rect none;
    const canlin_ui_rect *rec = canlin_ui_view_record(canlin_module_view(H.m[i]), &n, NULL);
    memset(&none, 0, sizeof none);
    for (k = 0; k < n; k++)
        if (rec[k].kind == CANLIN_UI_W_BUTTON && rec[k].part == CANLIN_UI_P_BOX)
            return rec[k];
    CHECK(!"no button");
    return none;
}

/* Every visible pane in tiled mode keeps the 30 em minimum in the split direction. */
static void check_min_panes(float em, int row)
{
    int i, n = canlin_shell_panes(H.sh, H.out);
    int minpx = (int)(30.0f * em + 0.999f);
    if (canlin_shell_mode_of(H.sh, H.out) != CANLIN_SHELL_TILED)
        return;
    for (i = 0; i < n; i++) {
        canlin_shell_rect p = part(CANLIN_SHELL_P_PANE, i);
        CHECK((row ? p.w : p.h) >= minpx);
    }
}

/* ---- §2 mode choice ---- */

static void test_mode_thresholds(void)
{
    /* scale 125 %: em = 20 px, so 59.9/60/79.9/80 em are 1198/1200/1598/1600 px. */
    static const struct {
        int w;
        int avail, auto_tiled;
    } c[] = {{1198, 0, 0}, {1200, 1, 0}, {1598, 1, 0}, {1600, 1, 1}};
    size_t i;
    h_init(1198, 900, 125, 2, CANLIN_SHELL_IN_POINTER);
    for (i = 0; i < sizeof c / sizeof c[0]; i++) {
        CHECK(canlin_ui_output_set_size(H.out, c[i].w, 900) == CANLIN_UI_OK);
        CHECK(canlin_shell_set_mode(H.sh, H.out, CANLIN_SHELL_AUTO) == CANLIN_SHELL_OK);
        CHECK(canlin_shell_tiled_available(H.sh, H.out) == c[i].avail);
        CHECK(canlin_shell_mode_of(H.sh, H.out) ==
              (c[i].auto_tiled ? CANLIN_SHELL_TILED : CANLIN_SHELL_FULL));
        CHECK(canlin_shell_set_mode(H.sh, H.out, CANLIN_SHELL_TILED) == CANLIN_SHELL_OK);
        CHECK(canlin_shell_mode_of(H.sh, H.out) ==
              (c[i].avail ? CANLIN_SHELL_TILED : CANLIN_SHELL_FULL));
        CHECK(canlin_shell_set_mode(H.sh, H.out, CANLIN_SHELL_FULL) == CANLIN_SHELL_OK);
        CHECK(canlin_shell_mode_of(H.sh, H.out) == CANLIN_SHELL_FULL);
        CHECK(canlin_shell_panes(H.sh, H.out) == 1);
    }
    /* Scale changes: 1600 px is 80 em at 125 %, 66.7 em at 150 %, 50 em at 200 %. */
    CHECK(canlin_shell_set_mode(H.sh, H.out, CANLIN_SHELL_AUTO) == CANLIN_SHELL_OK);
    CHECK(canlin_shell_mode_of(H.sh, H.out) == CANLIN_SHELL_TILED);
    CHECK(canlin_ui_output_set_scale(H.out, 150) == CANLIN_UI_OK);
    CHECK(canlin_shell_mode_of(H.sh, H.out) == CANLIN_SHELL_FULL);
    CHECK(canlin_shell_tiled_available(H.sh, H.out) == 1);
    CHECK(canlin_shell_set_mode(H.sh, H.out, CANLIN_SHELL_TILED) == CANLIN_SHELL_OK);
    CHECK(canlin_shell_mode_of(H.sh, H.out) == CANLIN_SHELL_TILED);
    check_min_panes(24.0f, 1);
    CHECK(canlin_ui_output_set_scale(H.out, 200) == CANLIN_UI_OK);
    CHECK(canlin_shell_mode_of(H.sh, H.out) == CANLIN_SHELL_FULL);
    CHECK(canlin_shell_tiled_available(H.sh, H.out) == 0);
    CHECK(canlin_ui_output_set_scale(H.out, 100) == CANLIN_UI_OK);
    CHECK(canlin_shell_mode_of(H.sh, H.out) == CANLIN_SHELL_TILED);
    h_done();
}

/* ---- §2 fallback and restore ---- */

static void test_fallback_restore(void)
{
    canlin_shell_rect bar, v;
    canlin_ui_output *fo;
    int pane;
    h_init(1600, 900, 100, 3, CANLIN_SHELL_IN_POINTER); /* 100 em: tiled by default */
    CHECK(canlin_shell_mode_of(H.sh, H.out) == CANLIN_SHELL_TILED);
    CHECK(canlin_shell_panes(H.sh, H.out) == 2);
    CHECK(canlin_shell_pane_module(H.sh, H.out, 0) == H.m[0]);
    CHECK(canlin_shell_pane_module(H.sh, H.out, 1) == H.m[1]);
    check_min_panes(16.0f, 1);
    /* Focus the second pane, then shrink below 80 em: full-screen shows the focused view. */
    v = part(CANLIN_SHELL_P_VIEW, 1);
    CHECK(tap(v.x + v.w - 4, v.y + v.h - 4) == CANLIN_SHELL_R_VIEW);
    CHECK(canlin_shell_focused(H.sh, &fo, &pane) == H.m[1] && fo == H.out && pane == 1);
    CHECK(canlin_ui_output_set_size(H.out, 1000, 900) == CANLIN_UI_OK);
    CHECK(canlin_shell_update(H.sh) == 1);
    CHECK(canlin_shell_mode_of(H.sh, H.out) == CANLIN_SHELL_FULL);
    CHECK(canlin_shell_panes(H.sh, H.out) == 1);
    CHECK(canlin_shell_pane_module(H.sh, H.out, 0) == H.m[1]);
    bar = part(CANLIN_SHELL_P_BAR, 0);
    v = part(CANLIN_SHELL_P_VIEW, 0);
    CHECK(v.x == 0 && v.y == bar.h && v.w == 1000 && v.h == 900 - bar.h);
    CHECK(!has_part(CANLIN_SHELL_P_DIVIDER, 0));
    /* Grows back: the same tiling returns, focus unchanged. */
    CHECK(canlin_ui_output_set_size(H.out, 1600, 900) == CANLIN_UI_OK);
    canlin_shell_update(H.sh);
    CHECK(canlin_shell_mode_of(H.sh, H.out) == CANLIN_SHELL_TILED);
    CHECK(canlin_shell_pane_module(H.sh, H.out, 0) == H.m[0]);
    CHECK(canlin_shell_pane_module(H.sh, H.out, 1) == H.m[1]);
    CHECK(canlin_shell_focused(H.sh, NULL, &pane) == H.m[1] && pane == 1);
    /* Three panes need 90 em: at 87.5 em no pane is squeezed, it falls back instead. */
    CHECK(canlin_shell_set_tiling(H.sh, H.out, CANLIN_SHELL_ROW, 3) == CANLIN_SHELL_OK);
    CHECK(canlin_shell_panes(H.sh, H.out) == 3);
    CHECK(canlin_shell_pane_module(H.sh, H.out, 2) == H.m[2]);
    check_min_panes(16.0f, 1);
    CHECK(canlin_ui_output_set_size(H.out, 1400, 900) == CANLIN_UI_OK);
    CHECK(canlin_shell_mode_of(H.sh, H.out) == CANLIN_SHELL_FULL);
    CHECK(canlin_ui_output_set_size(H.out, 1440, 900) == CANLIN_UI_OK);
    CHECK(canlin_shell_mode_of(H.sh, H.out) == CANLIN_SHELL_TILED);
    check_min_panes(16.0f, 1);
    /* Column tiling uses the content height. */
    CHECK(canlin_shell_set_tiling(H.sh, H.out, CANLIN_SHELL_COLUMN, 2) == CANLIN_SHELL_OK);
    /* Column tiling uses the content height: 900 - 40 px < 2 x 30 em. */
    CHECK(canlin_shell_mode_of(H.sh, H.out) == CANLIN_SHELL_FULL);
    CHECK(canlin_ui_output_set_size(H.out, 1440, 1000) == CANLIN_UI_OK);
    CHECK(canlin_shell_mode_of(H.sh, H.out) == CANLIN_SHELL_TILED);
    check_min_panes(16.0f, 0);
    h_done();
}

/* ---- §2 dividers ---- */

static void test_divider_drag(void)
{
    canlin_shell_rect d, p0, p1, p2;
    canlin_ui_input in;
    unsigned draws0, draws1, draws2, b0, b1;
    int minpx = 480;
    h_init(1600, 900, 100, 3, CANLIN_SHELL_IN_POINTER);
    CHECK(canlin_shell_set_tiling(H.sh, H.out, CANLIN_SHELL_ROW, 3) == CANLIN_SHELL_OK);
    settle();
    p2 = part(CANLIN_SHELL_P_VIEW, 2);
    d = part(CANLIN_SHELL_P_DIVIDER, 0);
    CHECK(d.w == 40); /* 2.5 em hit area */
    draws0 = H.draws[0];
    draws1 = H.draws[1];
    draws2 = H.draws[2];
    b0 = H.buttons[0];
    b1 = H.buttons[1];
    in = button(d.x + d.w / 2, d.y + 100, 1);
    CHECK(canlin_shell_input(H.sh, H.out, &in) == CANLIN_SHELL_R_CHROME);
    in = motion(0, d.y + 100);
    CHECK(canlin_shell_input(H.sh, H.out, &in) == CANLIN_SHELL_R_CHROME);
    p0 = part(CANLIN_SHELL_P_PANE, 0);
    p1 = part(CANLIN_SHELL_P_PANE, 1);
    CHECK(p0.w == minpx && p1.w >= minpx);
    in = motion(1599, d.y + 100);
    CHECK(canlin_shell_input(H.sh, H.out, &in) == CANLIN_SHELL_R_CHROME);
    p0 = part(CANLIN_SHELL_P_PANE, 0);
    p1 = part(CANLIN_SHELL_P_PANE, 1);
    CHECK(p1.w == minpx && p0.w >= minpx);
    CHECK(p0.x + p0.w == p1.x);
    in = motion(560, d.y + 100);
    CHECK(canlin_shell_input(H.sh, H.out, &in) == CANLIN_SHELL_R_CHROME);
    in = button(560, d.y + 100, 0);
    CHECK(canlin_shell_input(H.sh, H.out, &in) == CANLIN_SHELL_R_CHROME);
    p0 = part(CANLIN_SHELL_P_PANE, 0);
    CHECK(p0.w == 560);
    check_min_panes(16.0f, 1);
    settle();
    /* Only the two adjacent views were re-laid out; the drag never reached a view. */
    CHECK(H.draws[2] == draws2);
    CHECK(H.draws[0] > draws0 && H.draws[1] > draws1);
    {
        canlin_shell_rect v2 = part(CANLIN_SHELL_P_VIEW, 2);
        CHECK(v2.x == p2.x && v2.w == p2.w);
    }
    CHECK(H.buttons[0] == b0 && H.buttons[1] == b1);
    CHECK(canlin_shell_mode_of(H.sh, H.out) == CANLIN_SHELL_TILED);
    /* After shrink and regrow the dragged split is remembered (clamped to minimums). */
    CHECK(canlin_ui_output_set_size(H.out, 1000, 900) == CANLIN_UI_OK);
    canlin_shell_update(H.sh);
    CHECK(canlin_ui_output_set_size(H.out, 1600, 900) == CANLIN_UI_OK);
    canlin_shell_update(H.sh);
    p0 = part(CANLIN_SHELL_P_PANE, 0);
    CHECK(p0.w == 560);
    h_done();
}

/* ---- §4 focus and tap-through ---- */

static void test_focus_tap_through(void)
{
    canlin_ui_rect b;
    canlin_ui_input in;
    canlin_shell_rect v1;
    int pane;
    h_init(1600, 900, 100, 2, CANLIN_SHELL_IN_POINTER | CANLIN_SHELL_IN_KEYBOARD);
    settle();
    CHECK(canlin_shell_focused(H.sh, NULL, &pane) == H.m[0] && pane == 0);
    b = go_button(1);
    v1 = part(CANLIN_SHELL_P_VIEW, 1);
    CHECK(b.x >= v1.x && b.x + b.w <= v1.x + v1.w);
    /* One tap on a button in the unfocused pane: focuses it and presses the button. */
    in = button(b.x + b.w / 2, b.y + b.h / 2, 1);
    CHECK(canlin_shell_input(H.sh, H.out, &in) == CANLIN_SHELL_R_VIEW);
    CHECK(canlin_shell_focused(H.sh, NULL, &pane) == H.m[1] && pane == 1);
    in.down = 0;
    CHECK(canlin_shell_input(H.sh, H.out, &in) == CANLIN_SHELL_R_VIEW);
    settle();
    CHECK(count(1, "clicked") == 1);
    CHECK(count(0, "clicked") == 0);
    CHECK(H.buttons[0] == 0 && H.buttons[1] == 2);
    /* Pointer coordinates reach the view unchanged: output px are view space. */
    in = motion(v1.x + 7, v1.y + 9);
    CHECK(canlin_shell_input(H.sh, H.out, &in) == CANLIN_SHELL_R_VIEW);
    drain();
    CHECK(H.last_mx[1] == v1.x + 7 && H.last_my[1] == v1.y + 9);
    /* Keys and text go to the focused view only. */
    in = key(CANLIN_UI_KEY_LEFT, 1);
    CHECK(canlin_shell_input(H.sh, H.out, &in) == CANLIN_SHELL_R_VIEW);
    in.down = 0;
    CHECK(canlin_shell_input(H.sh, H.out, &in) == CANLIN_SHELL_R_VIEW);
    drain();
    CHECK(H.keys[1] == 2 && H.keys[0] == 0);
    /* Focus next reaches every pane without a pointer, and wraps. */
    in = key(CANLIN_SHELL_KEY_NEXT, 1);
    CHECK(canlin_shell_input(H.sh, H.out, &in) == CANLIN_SHELL_R_CHROME);
    CHECK(canlin_shell_focused(H.sh, NULL, &pane) == H.m[0] && pane == 0);
    CHECK(canlin_shell_input(H.sh, H.out, &in) == CANLIN_SHELL_R_CHROME);
    CHECK(canlin_shell_focused(H.sh, NULL, &pane) == H.m[1] && pane == 1);
    /* A press on the bar is chrome, focus unchanged. */
    CHECK(tap(part(CANLIN_SHELL_P_BAR, 0).w / 2, 2) == CANLIN_SHELL_R_CHROME);
    CHECK(canlin_shell_focused(H.sh, NULL, NULL) == H.m[1]);
    h_done();
}

/* ---- §4 reserved keys ---- */

static void test_reserved_keys(void)
{
    canlin_ui_input in;
    int pane;
    h_init(1600, 900, 100, 2, CANLIN_SHELL_IN_KEYBOARD);
    settle();
    in = key(CANLIN_SHELL_KEY_SWITCHER, 1);
    CHECK(canlin_shell_input(H.sh, H.out, &in) == CANLIN_SHELL_R_CHROME);
    CHECK(canlin_shell_switcher_open(H.sh, H.out));
    in.down = 0;
    CHECK(canlin_shell_input(H.sh, H.out, &in) == CANLIN_SHELL_R_CHROME);
    in = key(CANLIN_SHELL_KEY_SWITCHER, 1);
    CHECK(canlin_shell_input(H.sh, H.out, &in) == CANLIN_SHELL_R_CHROME);
    CHECK(!canlin_shell_switcher_open(H.sh, H.out));
    in = key(CANLIN_SHELL_KEY_NEXT, 1);
    CHECK(canlin_shell_input(H.sh, H.out, &in) == CANLIN_SHELL_R_CHROME);
    in.down = 0;
    CHECK(canlin_shell_input(H.sh, H.out, &in) == CANLIN_SHELL_R_CHROME);
    CHECK(canlin_shell_focused(H.sh, NULL, &pane) == H.m[1]);
    drain();
    CHECK(H.keys[0] == 0 && H.keys[1] == 0);
    /* Ctrl+Tab is focus-next: Ctrl reaches the view, that Tab never does. */
    in = key(CANLIN_UI_KEY_CTRL, 1);
    CHECK(canlin_shell_input(H.sh, H.out, &in) == CANLIN_SHELL_R_VIEW); /* to m1 */
    in = key(CANLIN_UI_KEY_TAB, 1);
    CHECK(canlin_shell_input(H.sh, H.out, &in) == CANLIN_SHELL_R_CHROME);
    CHECK(canlin_shell_focused(H.sh, NULL, &pane) == H.m[0]);
    in.down = 0;
    CHECK(canlin_shell_input(H.sh, H.out, &in) == CANLIN_SHELL_R_CHROME);
    in = key(CANLIN_UI_KEY_CTRL, 0);
    CHECK(canlin_shell_input(H.sh, H.out, &in) == CANLIN_SHELL_R_VIEW); /* to m0 */
    drain();
    CHECK(H.keys[0] == 1 && H.keys[1] == 1);
    /* A plain Tab still is the module's (widget focus, UI spec §3). */
    in = key(CANLIN_UI_KEY_TAB, 1);
    CHECK(canlin_shell_input(H.sh, H.out, &in) == CANLIN_SHELL_R_VIEW);
    drain();
    CHECK(H.keys[0] == 2);
    /* The reserved codes are not canlin_ui keys: no module can receive or bind them. */
    in = key(CANLIN_SHELL_KEY_SWITCHER, 1);
    CHECK(!canlin_ui_input_valid(&in));
    CHECK(canlin_module_post_input(H.m[0], &in) == CANLIN_MODULE_EINVAL);
    in = key(CANLIN_SHELL_KEY_NEXT, 1);
    CHECK(canlin_module_post_input(H.m[0], &in) == CANLIN_MODULE_EINVAL);
    in = key(0x1ff, 1);
    CHECK(canlin_shell_input(H.sh, H.out, &in) == CANLIN_SHELL_EINVAL);
    h_done();
}

/* ---- §3 status bar ---- */

static int rect_has_color(canlin_shell_rect r, canlin_theme_rgb c)
{
    int x, y;
    for (y = r.y; y < r.y + r.h; y++)
        for (x = r.x; x < r.x + r.w; x++) {
            const unsigned char *p = H.px + (size_t)y * H.r.pitch + (size_t)x * 3;
            if (p[0] == c.r && p[1] == c.g && p[2] == c.b)
                return 1;
        }
    return 0;
}

static void test_status_bar(void)
{
    canlin_shell_status st;
    canlin_shell_rect bar, ind[3];
    canlin_theme t;
    unsigned char *snap;
    int i, k, lo, hi, n;
    canlin_shell_part parts[3] = {CANLIN_SHELL_P_STORAGE, CANLIN_SHELL_P_MIC, CANLIN_SHELL_P_FAULTED};

    canlin_theme_builtin(&t);
    h_init(1600, 900, 100, 3, CANLIN_SHELL_IN_POINTER);
    CHECK(has_part(CANLIN_SHELL_P_CLOCK, 0));
    CHECK(has_part(CANLIN_SHELL_P_STORAGE, 0));
    CHECK(!has_part(CANLIN_SHELL_P_MIC, 0) && !has_part(CANLIN_SHELL_P_FAULTED, 0));
    memset(&st, 0, sizeof st);
    st.net = CANLIN_SHELL_NET_ONLINE;
    st.storage = CANLIN_SHELL_STORE_VOLATILE;
    st.mic = 1;
    canlin_shell_set_status(H.sh, &st);
    CHECK(canlin_ev_signal(H.ev, canlin_module_gen(H.m[2]), CANLIN_EV_CTL_FAULT) == CANLIN_EV_OK);
    drain();
    CHECK(canlin_module_get_state(H.m[2]) == CANLIN_MODULE_FAULTED);
    settle();
    bar = part(CANLIN_SHELL_P_BAR, 0);
    CHECK(bar.y == 0 && bar.h == 40 && bar.w == 1600);
    for (i = 0; i < 3; i++) {
        ind[i] = part(parts[i], 0);
        CHECK(contains(bar, ind[i]));
        CHECK(rect_has_color(ind[i], t.color[CANLIN_THEME_DANGER]));
    }
    CHECK(has_part(CANLIN_SHELL_P_NET, 0));
    /* No view can cover the bar, in either mode. */
    for (k = 0; k < 2; k++) {
        CHECK(canlin_shell_set_mode(H.sh, H.out, k ? CANLIN_SHELL_FULL : CANLIN_SHELL_TILED) ==
              CANLIN_SHELL_OK);
        for (i = 0; i < canlin_shell_panes(H.sh, H.out); i++) {
            CHECK(!overlap(bar, part(CANLIN_SHELL_P_PANE, i)));
            CHECK(!overlap(bar, part(CANLIN_SHELL_P_VIEW, i)));
        }
    }
    CHECK(canlin_shell_set_mode(H.sh, H.out, CANLIN_SHELL_AUTO) == CANLIN_SHELL_OK);
    settle();
    /* A module redraw never writes a bar row; the bar pixels stay as they were. */
    snap = malloc((size_t)bar.h * H.r.pitch);
    memcpy(snap, H.px, (size_t)bar.h * H.r.pitch);
    CHECK(canlin_ev_invalidate(H.ev, canlin_module_gen(H.m[0])) == CANLIN_EV_OK);
    drain();
    n = frame(&lo, &hi);
    CHECK(n > 0 && lo >= bar.h);
    CHECK(memcmp(snap, H.px, (size_t)bar.h * H.r.pitch) == 0);
    free(snap);
    /* On a narrow output the mandatory indicators stay, optional ones give way. */
    CHECK(canlin_ui_output_set_size(H.out, 320, 900) == CANLIN_UI_OK);
    canlin_shell_update(H.sh);
    bar = part(CANLIN_SHELL_P_BAR, 0);
    for (i = 0; i < 3; i++)
        CHECK(contains(bar, part(parts[i], 0)));
    CHECK(!has_part(CANLIN_SHELL_P_NET, 0));
    /* Back to persistent storage and no capture: those indicators go. */
    st.storage = CANLIN_SHELL_STORE_PERSISTENT;
    st.mic = 0;
    canlin_shell_set_status(H.sh, &st);
    CHECK(!has_part(CANLIN_SHELL_P_MIC, 0));
    h_done();
}

/* ---- §3 faulted pane and restart ---- */

static void test_fault_restart(void)
{
    canlin_ev_gen g0;
    canlin_shell_rect rs;
    canlin_theme t;
    unsigned draws;
    canlin_ui_input in;
    canlin_theme_builtin(&t);
    h_init(1600, 900, 100, 2, CANLIN_SHELL_IN_POINTER | CANLIN_SHELL_IN_KEYBOARD);
    settle();
    CHECK(canlin_shell_restart(H.sh, H.m[1]) == CANLIN_SHELL_ESTATE);
    CHECK(!has_part(CANLIN_SHELL_P_RESTART, 1));
    g0 = canlin_module_gen(H.m[1]);
    CHECK(canlin_ev_signal(H.ev, g0, CANLIN_EV_CTL_FAULT) == CANLIN_EV_OK);
    drain();
    CHECK(canlin_module_get_state(H.m[1]) == CANLIN_MODULE_FAULTED);
    CHECK(canlin_shell_update(H.sh) == 1);
    settle();
    rs = part(CANLIN_SHELL_P_RESTART, 1);
    CHECK(contains(part(CANLIN_SHELL_P_VIEW, 1), rs));
    CHECK(rs.h >= 40);
    CHECK(rect_has_color(rs, t.color[CANLIN_THEME_ACCENT]));
    CHECK(has_part(CANLIN_SHELL_P_FAULTED, 0));
    /* Input to the faulted pane goes nowhere near the module; its press is chrome. */
    in = button(rs.x + rs.w / 2, rs.y - 10, 1);
    CHECK(canlin_shell_input(H.sh, H.out, &in) == CANLIN_SHELL_R_CHROME);
    in.down = 0;
    canlin_shell_input(H.sh, H.out, &in);
    CHECK(canlin_module_get_state(H.m[1]) == CANLIN_MODULE_FAULTED);
    /* The restart action is a user action and makes a new generation. */
    draws = H.draws[1];
    CHECK(tap(rs.x + rs.w / 2, rs.y + rs.h / 2) == CANLIN_SHELL_R_CHROME);
    CHECK(canlin_module_get_state(H.m[1]) == CANLIN_MODULE_RUNNING);
    CHECK(canlin_module_gen(H.m[1]) != 0 && canlin_module_gen(H.m[1]) != g0);
    settle();
    CHECK(H.draws[1] > draws);
    CHECK(!has_part(CANLIN_SHELL_P_RESTART, 1));
    CHECK(!has_part(CANLIN_SHELL_P_FAULTED, 0));
    /* Keyboard route: fault again, Enter on the focused faulted pane restarts it. */
    g0 = canlin_module_gen(H.m[1]);
    CHECK(canlin_ev_signal(H.ev, g0, CANLIN_EV_CTL_FAULT) == CANLIN_EV_OK);
    drain();
    settle();
    CHECK(canlin_shell_focused(H.sh, NULL, NULL) == H.m[1]);
    in = key(CANLIN_UI_KEY_ENTER, 1);
    CHECK(canlin_shell_input(H.sh, H.out, &in) == CANLIN_SHELL_R_CHROME);
    CHECK(canlin_module_get_state(H.m[1]) == CANLIN_MODULE_RUNNING);
    CHECK(canlin_module_gen(H.m[1]) != g0);
    h_done();
}

/* ---- §3 switcher ---- */

static void test_switcher(void)
{
    canlin_shell_rect row, btn, bar;
    canlin_ui_input in;
    int i, found = -1;
    h_init(1600, 900, 100, 3, CANLIN_SHELL_IN_POINTER | CANLIN_SHELL_IN_TOUCH);
    settle();
    CHECK(canlin_shell_pane_module(H.sh, H.out, 0) == H.m[0]);
    CHECK(canlin_shell_pane_module(H.sh, H.out, 1) == H.m[1]);
    btn = part(CANLIN_SHELL_P_SWITCH_BUTTON, 0);
    CHECK(btn.h >= 40);
    CHECK(tap(btn.x + 5, btn.y + 5) == CANLIN_SHELL_R_CHROME);
    CHECK(canlin_shell_switcher_open(H.sh, H.out));
    for (i = 0; i < NMOD; i++)
        if (canlin_shell_switcher_module(H.sh, H.out, i) == H.m[2])
            found = i;
    CHECK(found >= 0);
    CHECK(canlin_shell_switcher_module(H.sh, H.out, NMOD) == NULL);
    frame(NULL, NULL);
    /* Assign the hidden module to the focused pane (0): it swaps with alpha. */
    row = part(CANLIN_SHELL_P_SWITCHER_ROW, found);
    CHECK(row.h >= 40);
    CHECK(tap(row.x + 5, row.y + 5) == CANLIN_SHELL_R_CHROME);
    CHECK(!canlin_shell_switcher_open(H.sh, H.out));
    CHECK(canlin_shell_pane_module(H.sh, H.out, 0) == H.m[2]);
    CHECK(canlin_shell_focused(H.sh, NULL, NULL) == H.m[2]);
    settle();
    CHECK(canlin_ui_view_frames(canlin_module_view(H.m[2])) > 0);
    /* A tap outside the open switcher closes it and reaches no view. */
    in = key(CANLIN_SHELL_KEY_SWITCHER, 1);
    CHECK(canlin_shell_input(H.sh, H.out, &in) == CANLIN_SHELL_R_CHROME);
    CHECK(tap(1500, 800) == CANLIN_SHELL_R_CHROME);
    CHECK(!canlin_shell_switcher_open(H.sh, H.out));
    drain();
    CHECK(H.buttons[1] == 0);
    /* Keyboard: open, move down, Enter. */
    CHECK(canlin_shell_input(H.sh, H.out, &in) == CANLIN_SHELL_R_CHROME);
    in = key(CANLIN_UI_KEY_DOWN, 1);
    CHECK(canlin_shell_input(H.sh, H.out, &in) == CANLIN_SHELL_R_CHROME);
    in = key(CANLIN_UI_KEY_ENTER, 1);
    CHECK(canlin_shell_input(H.sh, H.out, &in) == CANLIN_SHELL_R_CHROME);
    CHECK(!canlin_shell_switcher_open(H.sh, H.out));
    CHECK(canlin_shell_pane_module(H.sh, H.out, 0) == H.m[1]); /* row 1 = beta */
    CHECK(canlin_shell_pane_module(H.sh, H.out, 1) == H.m[2]); /* swapped out of pane 0 */
    /* Touch: a swipe down from the bar opens it. */
    bar = part(CANLIN_SHELL_P_BAR, 0);
    in = button(800, bar.h / 2, 1);
    CHECK(canlin_shell_input(H.sh, H.out, &in) == CANLIN_SHELL_R_CHROME);
    in = motion(800, bar.h / 2 + 20);
    canlin_shell_input(H.sh, H.out, &in);
    CHECK(!canlin_shell_switcher_open(H.sh, H.out));
    in = motion(800, bar.h / 2 + 41);
    CHECK(canlin_shell_input(H.sh, H.out, &in) == CANLIN_SHELL_R_CHROME);
    CHECK(canlin_shell_switcher_open(H.sh, H.out));
    in = button(800, bar.h / 2 + 41, 0);
    CHECK(canlin_shell_input(H.sh, H.out, &in) == CANLIN_SHELL_R_CHROME);
    CHECK(canlin_shell_switcher_open(H.sh, H.out));
    drain();
    CHECK(H.buttons[0] == 0 && H.buttons[1] == 0 && H.buttons[2] == 0);
    h_done();
}

/* ---- §3 model-view frame ---- */

static void test_model_frame(void)
{
    canlin_shell_rect lab, v, p;
    h_init(1000, 800, 100, 1, CANLIN_SHELL_IN_POINTER);
    CHECK(!has_part(CANLIN_SHELL_P_MODEL_LABEL, 0));
    CHECK(canlin_shell_set_model_view(H.sh, H.m[0], 1) == CANLIN_SHELL_OK);
    lab = part(CANLIN_SHELL_P_MODEL_LABEL, 0);
    v = part(CANLIN_SHELL_P_VIEW, 0);
    p = part(CANLIN_SHELL_P_PANE, 0);
    CHECK(contains(p, lab) && contains(p, v) && !overlap(lab, v));
    CHECK(v.y >= lab.y + lab.h && v.x > p.x);
    settle();
    {
        canlin_ui_rect b = go_button(0);
        CHECK(b.y >= v.y); /* the model view lays out below its label */
    }
    CHECK(canlin_shell_set_model_view(H.sh, H.m[0], 0) == CANLIN_SHELL_OK);
    CHECK(!has_part(CANLIN_SHELL_P_MODEL_LABEL, 0));
    h_done();
}

/* ---- §5 drawing: idle clock ---- */

static void test_idle_clock(void)
{
    int lo, hi, n, i;
    unsigned draws[NMOD];
    canlin_shell_rect bar;
    h_init(1600, 900, 100, 2, CANLIN_SHELL_IN_POINTER);
    settle();
    bar = part(CANLIN_SHELL_P_BAR, 0);
    CHECK(canlin_shell_update(H.sh) == 0);
    CHECK(frame(NULL, NULL) == 0);
    CHECK(canlin_shell_next_tick_ms(H.sh) == 55000);
    for (i = 0; i < NMOD; i++)
        draws[i] = H.draws[i];
    /* Within the minute: nothing. */
    H.wall += 54;
    CHECK(canlin_shell_update(H.sh) == 0);
    CHECK(frame(NULL, NULL) == 0);
    /* The minute turns: the bar rows, and only those, once. */
    H.wall += 1;
    CHECK(canlin_shell_update(H.sh) == 1);
    n = frame(&lo, &hi);
    CHECK(n > 0 && lo >= 0 && hi < bar.h);
    CHECK(frame(NULL, NULL) == 0);
    H.wall += 59;
    CHECK(canlin_shell_update(H.sh) == 0);
    H.wall += 1;
    CHECK(canlin_shell_update(H.sh) == 1);
    n = frame(&lo, &hi);
    CHECK(n > 0 && hi < bar.h);
    drain();
    for (i = 0; i < NMOD; i++)
        CHECK(H.draws[i] == draws[i]);
    h_done();
}

/* A view that redraws touches its own rows only; a divider move redraws the two panes. */
static void test_dirty_regions(void)
{
    int lo, hi;
    canlin_shell_rect v0, bar;
    h_init(1000, 800, 100, 1, CANLIN_SHELL_IN_POINTER);
    CHECK(frame(&lo, &hi) == 800); /* first render: whole output */
    settle();
    bar = part(CANLIN_SHELL_P_BAR, 0);
    v0 = part(CANLIN_SHELL_P_VIEW, 0);
    CHECK(canlin_ev_invalidate(H.ev, canlin_module_gen(H.m[0])) == CANLIN_EV_OK);
    drain();
    CHECK(frame(&lo, &hi) > 0);
    CHECK(lo >= v0.y && hi < v0.y + v0.h && lo >= bar.h);
    h_done();
}

int main(void)
{
    test_mode_thresholds();
    test_fallback_restore();
    test_divider_drag();
    test_focus_tap_through();
    test_reserved_keys();
    test_status_bar();
    test_fault_restart();
    test_switcher();
    test_model_frame();
    test_idle_clock();
    test_dirty_regions();
    if (failures) {
        fprintf(stderr, "shell: %d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("shell: %d checks passed\n", checks);
    return 0;
}
