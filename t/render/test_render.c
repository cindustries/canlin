/* Software rasterizer pixel checks and a determinism/dirty-row check over a full
 * Nuklear frame (canlin#17, UI spec §9/§10). Exit 0 = pass. */
#include "canlin/render.h"
#include "canlin/ui_limits.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

#define W 96
#define H 64
static unsigned char px[W * H * 3];
static unsigned char dirty[H];
static canlin_render R;

static const struct nk_color BG = { 0, 0, 0, 255 };
static const struct nk_color RED = { 255, 0, 0, 255 };

static int lit(int x, int y)
{
    const unsigned char *p = px + ((size_t)y * W + (size_t)x) * 3;
    return p[0] || p[1] || p[2];
}

static int count_lit(void)
{
    int x, y, n = 0;
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++)
            n += lit(x, y);
    return n;
}

/* Every lit pixel inside [x0, x1] x [y0, y1] (inclusive)? */
static int lit_inside(int x0, int y0, int x1, int y1)
{
    int x, y;
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++)
            if (lit(x, y) && (x < x0 || x > x1 || y < y0 || y > y1))
                return 0;
    return 1;
}

static void fresh(void)
{
    CHECK(canlin_render_init(&R, px, W, H, W * 3, dirty, NULL) == 0, "init");
    canlin_render_clear(&R, BG);
}

static void scissor(int x, int y, int w, int h)
{
    struct nk_command_scissor s;
    memset(&s, 0, sizeof s);
    s.header.type = NK_COMMAND_SCISSOR;
    s.x = (short)x; s.y = (short)y; s.w = (unsigned short)w; s.h = (unsigned short)h;
    canlin_render_command(&R, &s.header);
}

static void fill(int x, int y, int w, int h, int rounding)
{
    struct nk_command_rect_filled c;
    memset(&c, 0, sizeof c);
    c.header.type = NK_COMMAND_RECT_FILLED;
    c.x = (short)x; c.y = (short)y; c.w = (unsigned short)w; c.h = (unsigned short)h;
    c.rounding = (unsigned short)rounding;
    c.color = RED;
    canlin_render_command(&R, &c.header);
}

static void test_init_rejects(void)
{
    canlin_render r;
    unsigned char sh[W * H * 3];
    CHECK(canlin_render_init(&r, px, 0, H, W * 3, NULL, NULL) == -1, "w=0");
    CHECK(canlin_render_init(&r, px, W, H, W * 3 - 1, NULL, NULL) == -1, "short pitch");
    CHECK(canlin_render_init(&r, px, W, H, W * 3, NULL, sh) == -1, "shadow w/o dirty");
    CHECK(canlin_render_init(&r, NULL, W, H, W * 3, NULL, NULL) == -1, "no buffer");
}

static void test_fill_rect_scissor(void)
{
    int x, y, bad = 0;
    fresh();
    fill(5, 6, 7, 8, 0);
    CHECK(count_lit() == 56, "rect 7x8 covers %d px", count_lit());
    CHECK(lit(5, 6) && lit(11, 13) && !lit(12, 13) && !lit(11, 14) && !lit(4, 6),
          "rect is half-open [x, x+w)");

    fresh();
    scissor(10, 12, 20, 15);
    fill(0, 0, W, H, 0);
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++)
            if (lit(x, y) != (x >= 10 && x < 30 && y >= 12 && y < 27))
                bad++;
    CHECK(bad == 0, "scissor: %d pixels wrong", bad);

    fresh();
    scissor(-20, -20, 30, 1000);        /* clamps to the buffer */
    fill(0, 0, W, H, 0);
    CHECK(count_lit() == 10 * H, "clamped scissor: %d", count_lit());

    fresh();
    canlin_render_begin(&R);            /* begin resets the scissor */
    scissor(0, 0, 1, 1);
    canlin_render_begin(&R);
    fill(0, 0, W, H, 0);
    CHECK(count_lit() == W * H, "begin resets scissor");
}

static void line(int x0, int y0, int x1, int y1, int t)
{
    struct nk_command_line c;
    memset(&c, 0, sizeof c);
    c.header.type = NK_COMMAND_LINE;
    c.begin.x = (short)x0; c.begin.y = (short)y0;
    c.end.x = (short)x1; c.end.y = (short)y1;
    c.line_thickness = (unsigned short)t;
    c.color = RED;
    canlin_render_command(&R, &c.header);
}

static void test_line(void)
{
    fresh();
    line(3, 4, 40, 20, 1);
    CHECK(lit(3, 4) && lit(40, 20), "both endpoints drawn");
    CHECK(count_lit() == 38, "x-major line has dx+1 pixels, got %d", count_lit());
    CHECK(lit_inside(3, 4, 40, 20), "line stays in its bbox");

    fresh();
    line(50, 60, 50, 2, 1);
    CHECK(lit(50, 60) && lit(50, 2) && count_lit() == 59, "vertical line: %d", count_lit());

    fresh();
    line(10, 30, 30, 30, 3);
    CHECK(count_lit() == 23 * 3, "thick line (x 9..31, 3 rows): %d", count_lit());
    CHECK(lit(9, 29) && lit(31, 31) && !lit(8, 30) && !lit(10, 32), "3px square brush");

    fresh();
    scissor(0, 0, 20, H);
    line(0, 10, 90, 10, 1);
    CHECK(count_lit() == 20, "line clipped by scissor: %d", count_lit());
}

static void test_rounded_rect(void)
{
    int x, y, sym = 1;
    struct nk_command_rect c;

    fresh();
    fill(10, 10, 40, 30, 8);
    CHECK(!lit(10, 10) && !lit(49, 10) && !lit(10, 39) && !lit(49, 39), "corners cut");
    CHECK(!lit(11, 11) && !lit(48, 38), "corners cut one in");
    CHECK(lit(30, 10) && lit(30, 39) && lit(10, 25) && lit(49, 25), "edge centres");
    CHECK(lit(18, 10) && lit(10, 18), "arc meets the straight edge");
    for (y = 10; y < 40; y++)
        for (x = 10; x < 50; x++)
            if (lit(x, y) != lit(59 - x, y) || lit(x, y) != lit(x, 49 - y))
                sym = 0;
    CHECK(sym, "rounded fill is symmetric");
    CHECK(lit_inside(10, 10, 49, 39), "rounded fill inside its rect");

    fresh();
    memset(&c, 0, sizeof c);
    c.header.type = NK_COMMAND_RECT;
    c.x = 10; c.y = 10; c.w = 40; c.h = 30; c.rounding = 8; c.line_thickness = 2;
    c.color = RED;
    canlin_render_command(&R, &c.header);
    CHECK(!lit(10, 10) && !lit(49, 39), "stroke corners cut");
    CHECK(lit(30, 10) && lit(30, 11) && !lit(30, 12), "top edge 2px thick");
    CHECK(lit(49, 25) && lit(48, 25) && !lit(47, 25), "right edge 2px thick");
    CHECK(!lit(30, 25), "stroke leaves the inside empty");
    CHECK(lit_inside(10, 10, 49, 39), "stroke inside its rect");

    fresh();                            /* rounding larger than half the size clamps */
    fill(0, 0, 10, 10, 200);
    CHECK(lit(5, 5) && !lit(0, 0) && lit_inside(0, 0, 9, 9), "clamped radius");
}

static void test_circle_triangle(void)
{
    struct nk_command_circle_filled c;
    struct nk_command_triangle_filled t;
    int x, y, sym = 1;

    fresh();
    memset(&c, 0, sizeof c);
    c.header.type = NK_COMMAND_CIRCLE_FILLED;
    c.x = 20; c.y = 10; c.w = 20; c.h = 20; c.color = RED;
    canlin_render_command(&R, &c.header);
    CHECK(lit(30, 20) && lit(20, 20) && lit(39, 20) && lit(30, 10) && lit(30, 29),
          "circle spans its box");
    CHECK(!lit(20, 10) && !lit(39, 29), "circle corners empty");
    CHECK(lit_inside(20, 10, 39, 29), "circle inside its box");
    for (y = 10; y < 30; y++)
        for (x = 20; x < 40; x++)
            if (lit(x, y) != lit(59 - x, y) || lit(x, y) != lit(x, 39 - y))
                sym = 0;
    CHECK(sym, "circle symmetric");

    fresh();
    memset(&t, 0, sizeof t);
    t.header.type = NK_COMMAND_TRIANGLE_FILLED;
    t.a.x = 10; t.a.y = 10; t.b.x = 50; t.b.y = 10; t.c.x = 10; t.c.y = 50;
    t.color = RED;
    canlin_render_command(&R, &t.header);
    CHECK(lit(12, 12) && lit(20, 30) && !lit(40, 40) && !lit(49, 49), "triangle area");
    CHECK(lit_inside(10, 10, 50, 50), "triangle inside its bbox");
}

static void test_curve(void)
{
    struct nk_command_curve c;
    memset(&c, 0, sizeof c);
    fresh();
    c.header.type = NK_COMMAND_CURVE;
    c.begin.x = 5; c.begin.y = 50;
    c.ctrl[0].x = 20; c.ctrl[0].y = 2;
    c.ctrl[1].x = 60; c.ctrl[1].y = 60;
    c.end.x = 90; c.end.y = 8;
    c.line_thickness = 1;
    c.color = RED;
    canlin_render_command(&R, &c.header);
    CHECK(lit(5, 50) && lit(90, 8), "curve endpoints");
    CHECK(lit_inside(5, 2, 90, 60), "curve inside the control-point bbox");
    CHECK(count_lit() > 85, "curve is connected, %d px", count_lit());
}

static void test_alpha(void)
{
    struct nk_command_rect_filled c;
    fresh();
    memset(&c, 0, sizeof c);
    c.header.type = NK_COMMAND_RECT_FILLED;
    c.x = 0; c.y = 0; c.w = 4; c.h = 4;
    c.color.r = 255; c.color.a = 0;
    canlin_render_command(&R, &c.header);
    CHECK(count_lit() == 0, "alpha 0 draws nothing");
    c.color.a = 128;
    canlin_render_command(&R, &c.header);
    CHECK(px[0] == 128 && px[1] == 0, "alpha 128 blends to %u", px[0]);
}

static canlin_render_font F;

static void test_text(void)
{
    union { struct nk_command_text t; char raw[sizeof(struct nk_command_text) + 32]; } u;
    const char *s = "Hello, CanLin";
    int len = (int)strlen(s), n;
    float tw, th;

    memset(&u, 0, sizeof u);
    u.t.header.type = NK_COMMAND_TEXT;
    u.t.font = &F.font->handle;
    u.t.foreground.r = 255; u.t.foreground.g = 255; u.t.foreground.b = 255;
    u.t.foreground.a = 255;
    u.t.x = 7; u.t.y = 20;
    u.t.height = F.font->handle.height;
    u.t.length = len;
    memcpy(u.t.string, s, (size_t)len);
    tw = F.font->handle.width(F.font->handle.userdata, u.t.height, s, len);
    th = u.t.height;
    u.t.w = (unsigned short)(tw + 1); u.t.h = (unsigned short)(th + 1);

    fresh();
    canlin_render_command(&R, &u.t.header);
    CHECK(count_lit() == 0, "no atlas, no text");

    canlin_render_set_atlas(&R, F.alpha, F.w, F.h);
    canlin_render_command(&R, &u.t.header);
    n = count_lit();
    CHECK(n > 40, "text drew %d px", n);
    CHECK(lit_inside(7, 20, 7 + (int)(tw + 0.999f), 20 + (int)(th + 0.999f)),
          "text stays inside its box %.1fx%.1f", tw, th);

    fresh();
    canlin_render_set_atlas(&R, F.alpha, F.w, F.h);
    scissor(0, 0, 7 + (int)(tw / 2), H);
    canlin_render_command(&R, &u.t.header);
    CHECK(count_lit() > 0 && count_lit() < n, "scissor halves the text");
    CHECK(lit_inside(0, 0, 6 + (int)(tw / 2), H), "text clipped by scissor");

    /* U+4E2D is outside the built-in font: the replacement box, no crash (the box shape
     * itself is checked in test_builtin_font). */
    fresh();
    canlin_render_set_atlas(&R, F.alpha, F.w, F.h);
    memcpy(u.t.string, "\xE4\xB8\xAD", 3);
    u.t.length = 3;
    canlin_render_command(&R, &u.t.header);
    CHECK(count_lit() > 0, "missing glyph draws the fallback");
    memcpy(u.t.string, "A\xFF", 2);     /* invalid UTF-8 stops, never overreads */
    u.t.length = 2;
    canlin_render_command(&R, &u.t.header);
}

/* Built-in font (canlin#22, UI spec §6.3) at 16/24/32 px, in its own larger buffer. */
#define BW 160
#define BH 64
static unsigned char bpx[BW * BH * 3];

/* Draws s at (4, 8) with font f, white on black. Returns the lit pixel count, sets the
 * lit bounding box (inclusive) and, if hist is given, a 256-bin red-channel histogram. */
static int draw_builtin(const canlin_render_font *f, const char *s, int *bx0, int *by0,
                        int *bx1, int *by1, int *hist)
{
    union { struct nk_command_text t; char raw[sizeof(struct nk_command_text) + 32]; } u;
    canlin_render r;
    int x, y, n = 0, len = (int)strlen(s);
    memset(&u, 0, sizeof u);
    u.t.header.type = NK_COMMAND_TEXT;
    u.t.font = &f->font->handle;
    u.t.foreground.r = u.t.foreground.g = u.t.foreground.b = u.t.foreground.a = 255;
    u.t.x = 4; u.t.y = 8;
    u.t.height = f->font->handle.height;
    u.t.length = len;
    memcpy(u.t.string, s, (size_t)len);
    u.t.w = BW; u.t.h = BH;
    CHECK(canlin_render_init(&r, bpx, BW, BH, BW * 3, NULL, NULL) == 0, "init");
    canlin_render_clear(&r, BG);
    canlin_render_set_atlas(&r, f->alpha, f->w, f->h);
    canlin_render_command(&r, &u.t.header);
    *bx0 = BW; *by0 = BH; *bx1 = -1; *by1 = -1;
    if (hist)
        memset(hist, 0, 256 * sizeof *hist);
    for (y = 0; y < BH; y++)
        for (x = 0; x < BW; x++) {
            unsigned char v = bpx[((size_t)y * BW + (size_t)x) * 3];
            if (hist)
                hist[v]++;
            if (!v)
                continue;
            n++;
            if (x < *bx0) *bx0 = x;
            if (x > *bx1) *bx1 = x;
            if (y < *by0) *by0 = y;
            if (y > *by1) *by1 = y;
        }
    return n;
}

static unsigned char bat(int x, int y)
{
    return bpx[((size_t)y * BW + (size_t)x) * 3];
}

static void test_builtin_font(void)
{
    static const float sizes[] = { 16.0f, 24.0f, 32.0f };
    /* U+00E4 U+00F6 U+00FC U+00DF U+00C4 U+00D6 U+00DC U+20AC U+2026 U+2013 U+25B6 U+0141 */
    static const char *const have[] = {
        "\xC3\xA4", "\xC3\xB6", "\xC3\xBC", "\xC3\x9F", "\xC3\x84", "\xC3\x96", "\xC3\x9C",
        "\xE2\x82\xAC", "\xE2\x80\xA6", "\xE2\x80\x93", "\xE2\x96\xB6", "\xC5\x81" };
    size_t k, i;
    for (k = 0; k < sizeof sizes / sizeof sizes[0]; k++) {
        canlin_render_font f;
        int hist[256], levels = 0, n, x0, y0, x1, y1, q, b[4], v;
        const struct nk_font_glyph *fb;
        if (canlin_render_font_default(&f, sizes[k]) != 0) {
            CHECK(0, "bake %.0f px", sizes[k]);
            continue;
        }
        /* anti-aliased: glyph edges carry intermediate coverage, not just on/off */
        n = draw_builtin(&f, "Sag", &x0, &y0, &x1, &y1, hist);
        for (v = 1; v < 255; v++)
            levels += hist[v] > 0;
        CHECK(n > 0 && levels > 2, "%.0f px: %d intermediate coverage levels", sizes[k], levels);
        /* ink height follows the size ('S' top to 'g' bottom is about 0.95 em) */
        CHECK(y1 - y0 + 1 >= (int)(0.75f * sizes[k]) && y1 - y0 + 1 <= (int)sizes[k] + 1,
              "%.0f px: ink height %d", sizes[k], y1 - y0 + 1);

        /* the replacement box is U+25A1, never '?' */
        fb = nk_font_find_glyph(f.font, 0x25A1);
        CHECK(fb && f.font->fallback == fb && f.font->fallback_codepoint == 0x25A1,
              "%.0f px: fallback is U+25A1", sizes[k]);
        CHECK(nk_font_find_glyph(f.font, 0x4E2D) == fb, "%.0f px: CJK maps to the box", sizes[k]);
        CHECK(nk_font_find_glyph(f.font, '?') != fb, "%.0f px: '?' is its own glyph", sizes[k]);
        /* drawn: hollow (centre unlit), all four sides lit, not what '?' draws */
        n = draw_builtin(&f, "\xE4\xB8\xAD", &b[0], &b[1], &b[2], &b[3], NULL);
        CHECK(n > 0 && b[2] - b[0] >= 4 && b[3] - b[1] >= 4, "%.0f px: box drawn %dx%d",
              sizes[k], b[2] - b[0] + 1, b[3] - b[1] + 1);
        if (n > 0) {
            int cx = (b[0] + b[2]) / 2, cy = (b[1] + b[3]) / 2;
            CHECK(bat(cx, cy) == 0, "%.0f px: box is hollow", sizes[k]);
            CHECK(bat(b[0], cy) && bat(b[2], cy) && bat(cx, b[1]) && bat(cx, b[3]),
                  "%.0f px: box has four sides", sizes[k]);
            q = draw_builtin(&f, "?", &x0, &y0, &x1, &y1, NULL);
            CHECK(q > 0 && (q != n || x0 != b[0] || x1 != b[2] || y0 != b[1] || y1 != b[3]),
                  "%.0f px: box differs from '?'", sizes[k]);
        }

        /* umlauts, sharp s, euro, ellipsis, dash, triangle, Latin Extended-A: own glyphs */
        for (i = 0; i < sizeof have / sizeof have[0]; i++) {
            nk_rune cp = 0;
            nk_utf_decode(have[i], &cp, (int)strlen(have[i]));
            CHECK(nk_font_find_glyph(f.font, cp) != fb, "%.0f px: U+%04X has a glyph",
                  sizes[k], (unsigned)cp);
            CHECK(draw_builtin(&f, have[i], &x0, &y0, &x1, &y1, NULL) > 0,
                  "%.0f px: U+%04X draws ink", sizes[k], (unsigned)cp);
        }
        canlin_render_font_free(&f);
    }
}

static void test_atlas_image(void)
{
    struct nk_command_image c;
    int x, y, bad = 0;

    fresh();
    canlin_render_set_atlas(&R, F.alpha, F.w, F.h);
    memset(&c, 0, sizeof c);
    c.header.type = NK_COMMAND_IMAGE;
    c.x = 0; c.y = 0; c.w = W; c.h = H;
    c.img = nk_image_ptr(F.alpha);
    c.img.region[0] = 0; c.img.region[1] = 0; c.img.region[2] = W; c.img.region[3] = H;
    c.col.r = c.col.g = c.col.b = c.col.a = 255;
    canlin_render_command(&R, &c.header);
    for (y = 0; y < H && y < F.h; y++)
        for (x = 0; x < W && x < F.w; x++)
            if (lit(x, y) != (F.alpha[(size_t)y * (size_t)F.w + (size_t)x] != 0))
                bad++;
    CHECK(bad == 0 && count_lit() > 0, "1:1 atlas image: %d px differ", bad);

    fresh();
    c.img.handle.ptr = px;              /* not the atlas: ignored in v1 */
    canlin_render_command(&R, &c.header);
    CHECK(count_lit() == 0, "foreign image ignored");
}

static void test_dirty_rows(void)
{
    int n;
    fresh();
    n = canlin_render_end(&R);
    CHECK(n == H, "first frame reports all rows, got %d", n);
    canlin_render_begin(&R);
    fill(3, 5, 10, 6, 0);
    n = canlin_render_end(&R);
    CHECK(n == 6 && dirty[5] && dirty[10] && !dirty[4] && !dirty[11], "touched rows %d", n);
    canlin_render_begin(&R);
    CHECK(canlin_render_end(&R) == 0, "empty frame, no dirty rows");
    canlin_render_invalidate(&R);
    canlin_render_begin(&R);
    CHECK(canlin_render_end(&R) == H, "invalidate reports all rows");
}

/* ---- a full Nuklear frame ---- */

#define FW 240
#define FH 160
static unsigned char fpx[FW * FH * 3], fdirty[FH], fshadow[FW * FH * 3];

static int frame(struct nk_context *ctx, canlin_render *r, int mx, int my)
{
    nk_input_begin(ctx);
    nk_input_motion(ctx, mx, my);
    nk_input_end(ctx);
    if (nk_begin(ctx, "demo", nk_rect(10, 10, 200, 130),
                 NK_WINDOW_BORDER | NK_WINDOW_TITLE)) {
        nk_layout_row_dynamic(ctx, 30, 1);
        nk_label(ctx, "Hello", NK_TEXT_LEFT);
        nk_button_label(ctx, "OK");
    }
    nk_end(ctx);
    canlin_render_begin(r);
    canlin_render_clear(r, nk_rgb(20, 20, 30));
    canlin_render_commands(r, ctx);
    nk_clear(ctx);
    return canlin_render_end(r);
}

static void test_nuklear_frame(void)
{
    static unsigned char first[sizeof fpx];
    struct nk_context ctx;
    canlin_render r;
    int n, y, lo = FH, hi = -1;

    CHECK(nk_init_default(&ctx, &F.font->handle), "nk_init");
    CHECK(canlin_render_init(&r, fpx, FW, FH, FW * 3, fdirty, fshadow) == 0, "init");
    canlin_render_set_atlas(&r, F.alpha, F.w, F.h);

    n = frame(&ctx, &r, 0, 0);
    CHECK(n == FH, "first frame: all rows, got %d", n);
    memcpy(first, fpx, sizeof fpx);
    n = frame(&ctx, &r, 0, 0);
    CHECK(memcmp(first, fpx, sizeof fpx) == 0, "same input renders identical buffers");
    CHECK(n == 0, "unchanged frame reports %d dirty rows", n);

    /* Hover the button: only its rows change. */
    n = frame(&ctx, &r, 100, 100);
    n = frame(&ctx, &r, 100, 100);
    n = frame(&ctx, &r, 100, 100);
    CHECK(n == 0, "stable hover: %d dirty", n);
    n = frame(&ctx, &r, 0, 0);
    for (y = 0; y < FH; y++)
        if (fdirty[y]) { if (y < lo) lo = y; hi = y; }
    CHECK(n > 0 && n < FH / 2 && lo > 10 && hi < 140, "hover change: %d rows %d..%d", n, lo, hi);
    CHECK(memcmp(first, fpx, sizeof fpx) == 0, "back to the first image");

    nk_free(&ctx);
}

int main(void)
{
    if (canlin_render_font_default(&F, 16.0f) != 0) {
        fprintf(stderr, "FAIL: font bake\n");
        return 1;
    }
    CHECK(canlin_render_font_default(&(canlin_render_font){0}, 0.0f) == -1, "px 0 refused");
    test_init_rejects();
    test_fill_rect_scissor();
    test_line();
    test_rounded_rect();
    test_circle_triangle();
    test_curve();
    test_alpha();
    test_text();
    test_builtin_font();
    test_atlas_image();
    test_dirty_rows();
    test_nuklear_frame();
    canlin_render_font_free(&F);
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("render: all passed\n");
    return 0;
}
