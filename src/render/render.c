/* CanLin software rasterizer (UI spec §9). See include/canlin/render.h for the contract.
 *
 * Adapted from Nuklear's demo/rawfb/nuklear_rawfb.h (third_party/nuklear, commit
 * 9f77502), MIT License, Copyright (c) 2016-2017 Patrick Rudolph
 * <siro@das-labor.org>: the command dispatch, Bresenham line and glyph blit follow it.
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this
 * software and associated documentation files (the "Software"), to deal in the Software
 * without restriction, including without limitation the rights to use, copy, modify,
 * merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to the following
 * conditions: The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software. THE SOFTWARE IS PROVIDED "AS
 * IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO
 * THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Differences from rawfb, on purpose: one fixed RGB888 format; int coordinates (rawfb
 * uses short and overflows on x+w); half-open coverage sampled at pixel centres
 * instead of Bresenham ellipse quadrants, so rounded rects and circles are symmetric
 * and stroke thickness is honoured; alpha blending for every primitive; glyph coverage
 * used as alpha instead of a threshold; per-row dirty tracking; bounded polygons and
 * curve segments; no malloc and no assert in the draw path.
 */
#include "canlin/render.h"
#include "canlin/ui_limits.h"
#include "font_builtin.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* Float pixel boundary -> first pixel whose centre is >= b. Clamped so the int
 * conversion is always defined. */
static int edge(const canlin_render *r, float b)
{
    float lim = (float)(r->w + r->h) + 2.0f;
    b -= 0.5f;
    if (!(b > -lim))
        return -(r->w + r->h) - 2;
    if (b > lim)
        return r->w + r->h + 2;
    return (int)ceilf(b);
}

static void mark(canlin_render *r, int y)
{
    if (r->dirty)
        r->dirty[y] = 1;
}

static void blend(unsigned char *p, struct nk_color c)
{
    unsigned a = c.a, ia = 255u - a;
    if (a == 255) {
        p[0] = c.r; p[1] = c.g; p[2] = c.b;
        return;
    }
    p[0] = (unsigned char)((c.r * a + p[0] * ia + 127u) / 255u);
    p[1] = (unsigned char)((c.g * a + p[1] * ia + 127u) / 255u);
    p[2] = (unsigned char)((c.b * a + p[2] * ia + 127u) / 255u);
}

/* Pixels [x0, x1) of row y, clipped to the scissor. */
static void hspan(canlin_render *r, int x0, int x1, int y, struct nk_color c)
{
    unsigned char *p;
    if (c.a == 0 || y < r->cy0 || y >= r->cy1)
        return;
    if (x0 < r->cx0) x0 = r->cx0;
    if (x1 > r->cx1) x1 = r->cx1;
    if (x0 >= x1)
        return;
    p = r->px + (size_t)y * r->pitch + (size_t)x0 * CANLIN_RENDER_BPP;
    for (; x0 < x1; x0++, p += CANLIN_RENDER_BPP)
        blend(p, c);
    mark(r, y);
}

static void put(canlin_render *r, int x, int y, struct nk_color c)
{
    hspan(r, x, x + 1, y, c);
}

/* Span between two float boundaries, pixel centres inside [xa, xb). */
static void fspan(canlin_render *r, float xa, float xb, int y, struct nk_color c)
{
    hspan(r, edge(r, xa), edge(r, xb), y, c);
}

static int thick(unsigned t)
{
    return t < 1 ? 1 : t > CANLIN_RENDER_MAX_THICKNESS ? CANLIN_RENDER_MAX_THICKNESS : (int)t;
}

/* A t x t brush centred on (x, y). */
static void plot(canlin_render *r, int x, int y, int t, struct nk_color c)
{
    int off = (t - 1) / 2, i;
    if (t == 1) {
        put(r, x, y, c);
        return;
    }
    for (i = 0; i < t; i++)
        hspan(r, x - off, x - off + t, y - off + i, c);
}

/* Bresenham, both endpoints drawn (rawfb nk_rawfb_stroke_line). */
static void line(canlin_render *r, int x0, int y0, int x1, int y1, unsigned th,
                 struct nk_color c)
{
    int dx = abs(x1 - x0), dy = abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx - dy, t = thick(th);

    if (c.a == 0)
        return;
    for (;;) {
        plot(r, x0, y0, t, c);
        if (x0 == x1 && y0 == y1)
            break;
        {
            int e2 = 2 * err;
            if (e2 > -dy) { err -= dy; x0 += sx; }
            if (e2 < dx)  { err += dx; y0 += sy; }
        }
    }
}

/* Horizontal inset of a rounded rect [y0, y1) with corner radius rad at row centre yc. */
static float inset(float yc, float y0, float y1, float rad)
{
    float d = 0.0f, s;
    if (rad <= 0.0f)
        return 0.0f;
    if (yc < y0 + rad)
        d = y0 + rad - yc;
    else if (yc > y1 - rad)
        d = yc - (y1 - rad);
    if (d <= 0.0f)
        return 0.0f;
    s = rad * rad - d * d;
    return rad - (s > 0.0f ? sqrtf(s) : 0.0f);
}

static float radius(float rounding, float w, float h)
{
    float m = (w < h ? w : h) / 2.0f;
    return rounding < m ? rounding : m;
}

static void rect_fill(canlin_render *r, int x, int y, int w, int h, unsigned rounding,
                      struct nk_color c)
{
    float x0 = (float)x, y0 = (float)y, x1 = (float)x + w, y1 = (float)y + h;
    float rad = radius((float)rounding, (float)w, (float)h);
    int yy, ya = clampi(y, r->cy0, r->cy1), yb = clampi(y + h, r->cy0, r->cy1);

    if (w <= 0 || h <= 0 || c.a == 0)
        return;
    for (yy = ya; yy < yb; yy++) {
        float in = inset((float)yy + 0.5f, y0, y1, rad);
        fspan(r, x0 + in, x1 - in, yy, c);
    }
}

static void rect_stroke(canlin_render *r, int x, int y, int w, int h, unsigned rounding,
                        unsigned th, struct nk_color c)
{
    float t = (float)thick(th);
    float x0 = (float)x, y0 = (float)y, x1 = (float)x + w, y1 = (float)y + h;
    float ix0 = x0 + t, iy0 = y0 + t, ix1 = x1 - t, iy1 = y1 - t;
    float rad = radius((float)rounding, (float)w, (float)h);
    float irad = rad - t > 0.0f ? rad - t : 0.0f;
    int yy, ya = clampi(y, r->cy0, r->cy1), yb = clampi(y + h, r->cy0, r->cy1);

    if (w <= 0 || h <= 0 || c.a == 0)
        return;
    if (ix0 >= ix1 || iy0 >= iy1) {
        rect_fill(r, x, y, w, h, rounding, c);
        return;
    }
    for (yy = ya; yy < yb; yy++) {
        float yc = (float)yy + 0.5f;
        float o = inset(yc, y0, y1, rad);
        if (yc > iy0 && yc < iy1) {
            float i = inset(yc, iy0, iy1, irad);
            fspan(r, x0 + o, ix0 + i, yy, c);
            fspan(r, ix1 - i, x1 - o, yy, c);
        } else {
            fspan(r, x0 + o, x1 - o, yy, c);
        }
    }
}

/* Half width of an ellipse at row centre yc, or -1 if the row misses it. */
static float ehalf(float yc, float cy, float rx, float ry)
{
    float d;
    if (rx <= 0.0f || ry <= 0.0f)
        return -1.0f;
    d = (yc - cy) / ry;
    if (d <= -1.0f || d >= 1.0f)
        return -1.0f;
    return rx * sqrtf(1.0f - d * d);
}

static void circle(canlin_render *r, int x, int y, int w, int h, int fill, unsigned th,
                   struct nk_color c)
{
    float cx = (float)x + w / 2.0f, cy = (float)y + h / 2.0f;
    float rx = w / 2.0f, ry = h / 2.0f, t = (float)thick(th);
    int yy, ya = clampi(y, r->cy0, r->cy1), yb = clampi(y + h, r->cy0, r->cy1);

    if (w <= 0 || h <= 0 || c.a == 0)
        return;
    for (yy = ya; yy < yb; yy++) {
        float yc = (float)yy + 0.5f;
        float o = ehalf(yc, cy, rx, ry), i;
        if (o < 0.0f)
            continue;
        i = fill ? -1.0f : ehalf(yc, cy, rx - t, ry - t);
        if (i < 0.0f) {
            fspan(r, cx - o, cx + o, yy, c);
        } else {
            fspan(r, cx - o, cx - i, yy, c);
            fspan(r, cx + i, cx + o, yy, c);
        }
    }
}

/* Even-odd scanline fill, pixel centres (after Darel Rex Finley's public-domain
 * algorithm, as used by rawfb). */
static void poly_fill(canlin_render *r, const struct nk_vec2i *p, int n, struct nk_color c)
{
    float xs[CANLIN_RENDER_MAX_POLY_POINTS];
    int i, j, k, yy, top, bot;

    if (n < 3 || n > CANLIN_RENDER_MAX_POLY_POINTS || c.a == 0)
        return;
    top = bot = p[0].y;
    for (i = 1; i < n; i++) {
        if (p[i].y < top) top = p[i].y;
        if (p[i].y > bot) bot = p[i].y;
    }
    top = clampi(top, r->cy0, r->cy1);
    bot = clampi(bot + 1, r->cy0, r->cy1);
    for (yy = top; yy < bot; yy++) {
        float yc = (float)yy + 0.5f;
        int m = 0;
        for (i = 0, j = n - 1; i < n; j = i++) {
            float yi = p[i].y, yj = p[j].y;
            if ((yi <= yc) != (yj <= yc))
                xs[m++] = p[i].x + (yc - yi) / (yj - yi) * (float)(p[j].x - p[i].x);
        }
        for (i = 1; i < m; i++) {           /* insertion sort, m <= n */
            float v = xs[i];
            for (k = i - 1; k >= 0 && xs[k] > v; k--)
                xs[k + 1] = xs[k];
            xs[k + 1] = v;
        }
        for (i = 0; i + 1 < m; i += 2)
            fspan(r, xs[i], xs[i + 1], yy, c);
    }
}

static void poly_stroke(canlin_render *r, const struct nk_vec2i *p, int n, unsigned th,
                        int closed, struct nk_color c)
{
    int i;
    if (n < 1 || n > CANLIN_RENDER_MAX_POLY_POINTS)
        return;
    for (i = 1; i < n; i++)
        line(r, p[i - 1].x, p[i - 1].y, p[i].x, p[i].y, th, c);
    if (closed && n > 2)
        line(r, p[n - 1].x, p[n - 1].y, p[0].x, p[0].y, th, c);
}

/* Cubic Bezier as CANLIN_UI_GRAPH_CURVE_SEGMENTS lines (rawfb nk_rawfb_stroke_curve). */
static void curve(canlin_render *r, const struct nk_command_curve *q)
{
    const unsigned segs = CANLIN_UI_GRAPH_CURVE_SEGMENTS;
    int lx = q->begin.x, ly = q->begin.y;
    unsigned i;
    for (i = 1; i <= segs; i++) {
        float t = (float)i / (float)segs, u = 1.0f - t;
        float w1 = u * u * u, w2 = 3 * u * u * t, w3 = 3 * u * t * t, w4 = t * t * t;
        float x = w1 * q->begin.x + w2 * q->ctrl[0].x + w3 * q->ctrl[1].x + w4 * q->end.x;
        float y = w1 * q->begin.y + w2 * q->ctrl[0].y + w3 * q->ctrl[1].y + w4 * q->end.y;
        int nx = (int)floorf(x + 0.5f), ny = (int)floorf(y + 0.5f);
        line(r, lx, ly, nx, ny, q->line_thickness, q->color);
        lx = nx;
        ly = ny;
    }
}

/* Nearest-neighbour blit of an atlas region used as coverage for colour c. */
static void atlas_blit(canlin_render *r, float sx, float sy, float sw, float sh,
                       int dx, int dy, int dw, int dh, struct nk_color c)
{
    int i, j;
    if (!r->atlas || dw <= 0 || dh <= 0 || dw > r->w + r->h || dh > r->w + r->h)
        return;
    for (j = 0; j < dh; j++) {
        int y = dy + j, ay;
        if (y < r->cy0 || y >= r->cy1)
            continue;
        ay = clampi((int)(sy + ((float)j + 0.5f) * sh / (float)dh), 0, r->atlas_h - 1);
        for (i = 0; i < dw; i++) {
            int ax = clampi((int)(sx + ((float)i + 0.5f) * sw / (float)dw), 0, r->atlas_w - 1);
            unsigned cov = r->atlas[(size_t)ay * (size_t)r->atlas_w + (size_t)ax];
            if (cov) {
                struct nk_color g = c;
                g.a = (unsigned char)((cov * c.a + 127u) / 255u);
                put(r, dx + i, y, g);
            }
        }
    }
}

/* rawfb nk_rawfb_draw_text: walk the UTF-8, look each glyph up in the baked font. */
static void text(canlin_render *r, const struct nk_command_text *t)
{
    const struct nk_font *font;
    float x = (float)t->x, scale;
    int pos = 0;

    if (!r->atlas || !t->font || t->length <= 0 || t->foreground.a == 0)
        return;
    font = (const struct nk_font *)t->font->userdata.ptr;
    if (!font || !font->glyphs || !(font->info.height > 0.0f) || !(t->height > 0.0f))
        return;
    scale = t->height / font->info.height;
    while (pos < t->length) {
        nk_rune cp = 0;
        const struct nk_font_glyph *g;
        int n = nk_utf_decode(t->string + pos, &cp, t->length - pos);
        if (n <= 0 || cp == NK_UTF_INVALID)
            break;
        g = nk_font_find_glyph(font, cp);
        if (!g)
            break;
        {
            float gw = (g->x1 - g->x0) * scale, gh = (g->y1 - g->y0) * scale;
            float lim = (float)(r->w + r->h);
            if (gw > 0.0f && gh > 0.0f && gw < lim && gh < lim && x > -lim && x < lim)
                atlas_blit(r, g->u0 * r->atlas_w, g->v0 * r->atlas_h,
                           (g->u1 - g->u0) * r->atlas_w, (g->v1 - g->v0) * r->atlas_h,
                           (int)floorf(x + g->x0 * scale + 0.5f),
                           (int)floorf((float)t->y + g->y0 * scale + 0.5f),
                           (int)ceilf(gw), (int)ceilf(gh), t->foreground);
        }
        x += g->xadvance * scale;
        pos += n;
    }
}

static void image(canlin_render *r, const struct nk_command_image *q)
{
    float sx = 0, sy = 0, sw = (float)r->atlas_w, sh = (float)r->atlas_h;
    if (!r->atlas || q->img.handle.ptr != (const void *)r->atlas)
        return;
    if (q->img.region[2] && q->img.region[3]) {
        sx = q->img.region[0]; sy = q->img.region[1];
        sw = q->img.region[2]; sh = q->img.region[3];
    }
    atlas_blit(r, sx, sy, sw, sh, q->x, q->y, q->w, q->h, q->col);
}

static unsigned char lerp8(unsigned char a, unsigned char b, float f)
{
    return (unsigned char)((float)a + ((float)b - (float)a) * f + 0.5f);
}

static struct nk_color lerpc(struct nk_color a, struct nk_color b, float f)
{
    struct nk_color c;
    c.r = lerp8(a.r, b.r, f); c.g = lerp8(a.g, b.g, f);
    c.b = lerp8(a.b, b.b, f); c.a = lerp8(a.a, b.a, f);
    return c;
}

/* Corners: left = top-left, top = top-right, right = bottom-right, bottom = bottom-left
 * (Nuklear's nk_fill_rect_multi_color vertex order). */
static void multi(canlin_render *r, const struct nk_command_rect_multi_color *q)
{
    int i, j;
    for (j = clampi(q->y, r->cy0, r->cy1); j < clampi(q->y + q->h, r->cy0, r->cy1); j++) {
        float fy = ((float)(j - q->y) + 0.5f) / (float)q->h;
        for (i = clampi(q->x, r->cx0, r->cx1); i < clampi(q->x + q->w, r->cx0, r->cx1); i++) {
            float fx = ((float)(i - q->x) + 0.5f) / (float)q->w;
            put(r, i, j, lerpc(lerpc(q->left, q->top, fx), lerpc(q->bottom, q->right, fx), fy));
        }
    }
}

int canlin_render_init(canlin_render *r, unsigned char *px, int w, int h, size_t pitch,
                       unsigned char *dirty, unsigned char *shadow)
{
    memset(r, 0, sizeof *r);
    if (!px || w <= 0 || h <= 0 || pitch < (size_t)w * CANLIN_RENDER_BPP ||
        (shadow && !dirty))
        return -1;
    r->px = px;
    r->w = w;
    r->h = h;
    r->pitch = pitch;
    r->dirty = dirty;
    r->shadow = shadow;
    r->full = 1;
    canlin_render_begin(r);
    return 0;
}

void canlin_render_set_atlas(canlin_render *r, const unsigned char *alpha8, int w, int h)
{
    if (!alpha8 || w <= 0 || h <= 0) {
        r->atlas = NULL;
        r->atlas_w = r->atlas_h = 0;
        return;
    }
    r->atlas = alpha8;
    r->atlas_w = w;
    r->atlas_h = h;
}

void canlin_render_invalidate(canlin_render *r)
{
    r->full = 1;
}

void canlin_render_begin(canlin_render *r)
{
    if (r->dirty)
        memset(r->dirty, 0, (size_t)r->h);
    r->cx0 = r->cy0 = 0;
    r->cx1 = r->w;
    r->cy1 = r->h;
}

void canlin_render_clear(canlin_render *r, struct nk_color c)
{
    int sx0 = r->cx0, sy0 = r->cy0, sx1 = r->cx1, sy1 = r->cy1, y;
    r->cx0 = r->cy0 = 0;
    r->cx1 = r->w;
    r->cy1 = r->h;
    c.a = 255;
    for (y = 0; y < r->h; y++)
        hspan(r, 0, r->w, y, c);
    r->cx0 = sx0; r->cy0 = sy0; r->cx1 = sx1; r->cy1 = sy1;
}

void canlin_render_command(canlin_render *r, const struct nk_command *cmd)
{
    switch (cmd->type) {
    case NK_COMMAND_SCISSOR: {
        const struct nk_command_scissor *s = (const struct nk_command_scissor *)cmd;
        r->cx0 = clampi(s->x, 0, r->w);
        r->cy0 = clampi(s->y, 0, r->h);
        r->cx1 = clampi(s->x + s->w, 0, r->w);
        r->cy1 = clampi(s->y + s->h, 0, r->h);
    } break;
    case NK_COMMAND_LINE: {
        const struct nk_command_line *l = (const struct nk_command_line *)cmd;
        line(r, l->begin.x, l->begin.y, l->end.x, l->end.y, l->line_thickness, l->color);
    } break;
    case NK_COMMAND_CURVE:
        curve(r, (const struct nk_command_curve *)cmd);
        break;
    case NK_COMMAND_RECT: {
        const struct nk_command_rect *q = (const struct nk_command_rect *)cmd;
        rect_stroke(r, q->x, q->y, q->w, q->h, q->rounding, q->line_thickness, q->color);
    } break;
    case NK_COMMAND_RECT_FILLED: {
        const struct nk_command_rect_filled *q = (const struct nk_command_rect_filled *)cmd;
        rect_fill(r, q->x, q->y, q->w, q->h, q->rounding, q->color);
    } break;
    case NK_COMMAND_RECT_MULTI_COLOR:
        multi(r, (const struct nk_command_rect_multi_color *)cmd);
        break;
    case NK_COMMAND_CIRCLE: {
        const struct nk_command_circle *q = (const struct nk_command_circle *)cmd;
        circle(r, q->x, q->y, q->w, q->h, 0, q->line_thickness, q->color);
    } break;
    case NK_COMMAND_CIRCLE_FILLED: {
        const struct nk_command_circle_filled *q = (const struct nk_command_circle_filled *)cmd;
        circle(r, q->x, q->y, q->w, q->h, 1, 1, q->color);
    } break;
    case NK_COMMAND_TRIANGLE: {
        const struct nk_command_triangle *q = (const struct nk_command_triangle *)cmd;
        struct nk_vec2i p[3];
        p[0] = q->a; p[1] = q->b; p[2] = q->c;
        poly_stroke(r, p, 3, q->line_thickness, 1, q->color);
    } break;
    case NK_COMMAND_TRIANGLE_FILLED: {
        const struct nk_command_triangle_filled *q =
            (const struct nk_command_triangle_filled *)cmd;
        struct nk_vec2i p[3];
        p[0] = q->a; p[1] = q->b; p[2] = q->c;
        poly_fill(r, p, 3, q->color);
    } break;
    case NK_COMMAND_POLYGON: {
        const struct nk_command_polygon *q = (const struct nk_command_polygon *)cmd;
        poly_stroke(r, q->points, q->point_count, q->line_thickness, 1, q->color);
    } break;
    case NK_COMMAND_POLYGON_FILLED: {
        const struct nk_command_polygon_filled *q = (const struct nk_command_polygon_filled *)cmd;
        poly_fill(r, q->points, q->point_count, q->color);
    } break;
    case NK_COMMAND_POLYLINE: {
        const struct nk_command_polyline *q = (const struct nk_command_polyline *)cmd;
        poly_stroke(r, q->points, q->point_count, q->line_thickness, 0, q->color);
    } break;
    case NK_COMMAND_TEXT:
        text(r, (const struct nk_command_text *)cmd);
        break;
    case NK_COMMAND_IMAGE:
        image(r, (const struct nk_command_image *)cmd);
        break;
    default:        /* NOP, ARC, ARC_FILLED, CUSTOM: not rendered in v1 */
        break;
    }
}

void canlin_render_commands(canlin_render *r, struct nk_context *ctx)
{
    const struct nk_command *cmd;
    nk_foreach(cmd, ctx)
        canlin_render_command(r, cmd);
}

int canlin_render_end(canlin_render *r)
{
    size_t row = (size_t)r->w * CANLIN_RENDER_BPP;
    int y, n = 0;

    if (!r->dirty)
        return 0;
    for (y = 0; y < r->h; y++) {
        const unsigned char *p = r->px + (size_t)y * r->pitch;
        if (r->full)
            r->dirty[y] = 1;
        if (!r->dirty[y])
            continue;
        if (r->shadow) {
            unsigned char *s = r->shadow + (size_t)y * r->pitch;
            if (!r->full && memcmp(p, s, row) == 0) {
                r->dirty[y] = 0;
                continue;
            }
            memcpy(s, p, row);
        }
        n++;
    }
    r->full = 0;
    return n;
}

int canlin_render_font_default(canlin_render_font *f, float px_height)
{
    return canlin_render_font_default_sizes(f, &px_height, 1);
}

int canlin_render_font_default_sizes(canlin_render_font *f, const float *px, int n)
{
    const void *img;
    int i, w = 0, h = 0;

    memset(f, 0, sizeof *f);
    if (px == NULL || n < 1 || n > CANLIN_RENDER_FONT_SIZES)
        return -1;
    for (i = 0; i < n; i++)
        if (!(px[i] > 0.0f))
            return -1;
    nk_font_atlas_init_default(&f->atlas);
    nk_font_atlas_begin(&f->atlas);
    for (i = 0; i < n; i++) {
        /* Built-in DejaVu Sans subset, 1:1 in the atlas (no oversampling: text() blits
         * nearest-neighbour, so the atlas must already hold the anti-aliased coverage
         * at the drawn size). Missing codepoints fall back to the U+25A1 box. The blob
         * is static and read-only; the atlas never owns or frees it. */
        struct nk_font_config cfg = nk_font_config(px[i]);
        cfg.ttf_blob = (void *)canlin_font_builtin_ttf;
        cfg.ttf_size = (nk_size)canlin_font_builtin_ttf_size;
        cfg.ttf_data_owned_by_atlas = 0;
        cfg.range = canlin_font_builtin_ranges;
        cfg.fallback_glyph = CANLIN_FONT_BUILTIN_BOX;
        cfg.oversample_h = 1;
        cfg.oversample_v = 1;
        f->size[i] = nk_font_atlas_add(&f->atlas, &cfg);
        if (!f->size[i])
            goto fail;
    }
    f->font = f->size[0];
    f->nsizes = n;
    img = nk_font_atlas_bake(&f->atlas, &w, &h, NK_FONT_ATLAS_ALPHA8);
    if (!img || w <= 0 || h <= 0)
        goto fail;
    f->alpha = malloc((size_t)w * (size_t)h);
    if (!f->alpha)
        goto fail;
    memcpy(f->alpha, img, (size_t)w * (size_t)h);
    f->w = w;
    f->h = h;
    nk_font_atlas_end(&f->atlas, nk_handle_ptr(f->alpha), NULL);
    return 0;
fail:
    nk_font_atlas_clear(&f->atlas);
    free(f->alpha);
    memset(f, 0, sizeof *f);
    return -1;
}

void canlin_render_font_free(canlin_render_font *f)
{
    if (f->font || f->alpha)
        nk_font_atlas_clear(&f->atlas);
    free(f->alpha);
    memset(f, 0, sizeof *f);
}
