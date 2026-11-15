/* UI budget measurement (canlin#96, UI spec §6.3 and §5).
 *
 * Two numbers the UI spec quotes but no card owns:
 *
 *   1. The font atlas: the memory of the rasterized glyphs of one em size (the
 *      built-in Inter subset, the three faces body/H1/H2 in one atlas). This is
 *      a SIZE, not a time: it follows from the em size (the resolution and the
 *      scale), not from the CPU. The i686 run therefore yields the same bytes
 *      as the host run - the number is exact, not an order of magnitude.
 *
 *   2. The graph at its limits (256 nodes, 1024 edges): the visible nodes, the
 *      drawn edges, the draw commands per frame and the time of a frame. The
 *      counts are exact and the same on the host and on the target. The TIME
 *      under the emulated P2 is an UPPER BOUND, never the P2's own time: the
 *      emulation makes every instruction dearer.
 *
 * Nothing here is a limit. The limits (CANLIN_UI_*) are set for the slowest
 * target and stay set; this tool only says what the numbers are.
 *
 * Build and run:
 *   make ui-budget BUILD=build/host
 *   make TARGET=i686 BUILD=build/i686-ui ui-budget
 *   qemu-i386 -cpu pentium2 ./build/i686-ui/tools/ui_budget
 */
#define _POSIX_C_SOURCE 200809L /* clock_gettime() */

#include "canlin/ev.h"
#include "canlin/module.h"
#include "canlin/render.h"
#include "canlin/theme.h"
#include "canlin/ui.h"
#include "canlin/ui_limits.h"
#include "font_builtin.h" /* the built-in subset's blob size (src/render) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/* ---- the font atlas ---- */

/* One configuration of the limits: the theme's font size at DPI 1.0, the
 * output's DPI factor and its UI scale step. The em of the view is their
 * product (canlin_theme_em), and the font cache holds one atlas per em size. */
struct em_case {
    const char *label;
    float font_px;
    float dpi;
    int scale;
};

/* The cases the limits allow: the smallest, the default, the scaled ones and
 * the largest. CANLIN_THEME_FONT_SIZE_MIN/MAX, CANLIN_UI_DPI_MIN/MAX and
 * CANLIN_UI_SCALE_STEP_* are the bounds these cases walk. */
static const struct em_case em_cases[] = {
    {"smallest (10 px, 0.5 DPI, 75 %)", 10.0f, 0.5f, CANLIN_UI_SCALE_STEP_75},
    {"default (16 px, 1.0 DPI, 100 %)", 16.0f, 1.0f, CANLIN_UI_SCALE_STEP_100},
    {"200 % (16 px, 1.0 DPI, 200 %)", 16.0f, 1.0f, CANLIN_UI_SCALE_STEP_200},
    {"200 %, 2.0 DPI (16 px, 2.0 DPI, 200 %)", 16.0f, 2.0f, CANLIN_UI_SCALE_STEP_200},
    {"200 %, 4.0 DPI (16 px, 4.0 DPI, 200 %)", 16.0f, 4.0f, CANLIN_UI_SCALE_STEP_200},
    {"largest (40 px, 4.0 DPI, 200 %)", 40.0f, 4.0f, CANLIN_UI_SCALE_STEP_200},
};

/* The three faces of one em size, as src/ui/ui.c bakes them: the body font and
 * the two heading sizes. */
static void face_sizes(float em, float *px)
{
    px[0] = em;
    px[1] = em * CANLIN_UI_HEADING1_SCALE;
    px[2] = em * CANLIN_UI_HEADING2_SCALE;
}

/* The largest em size the limits allow: the theme's font size at DPI 1.0 (the
 * theme key's own range), the output's DPI factor and the biggest UI scale
 * step, multiplied (canlin_theme_em). */
static float em_max(void)
{
    return (float)CANLIN_THEME_FONT_SIZE_MAX * CANLIN_UI_DPI_MAX *
           (float)CANLIN_UI_SCALE_STEP_200 / (float)CANLIN_UI_SCALE_BASE_PERCENT;
}

/* Resident bytes of one baked font size (one cache entry): the alpha8 pixels,
 * Nuklear's glyph table and Nuklear's own copy of the TTF blob (one per face,
 * because the built-in blob is static and read-only). */
typedef struct atlas_cost {
    int w, h;
    unsigned long pixels; /* the alpha8 atlas: w * h bytes */
    unsigned long glyphs; /* the glyph table: glyph_count * sizeof(nk_font_glyph) */
    unsigned long ttf;    /* Nuklear's copy of the blob, one per face */
    unsigned long total;
    unsigned n_glyphs;    /* the glyphs of the atlas */
    unsigned n_bitmap;    /* the ones that got a bitmap (the rest were dropped) */
} atlas_cost;

static void cost_of(const canlin_render_font *f, unsigned nfaces, atlas_cost *c)
{
    unsigned i;

    memset(c, 0, sizeof *c);
    c->w = f->w;
    c->h = f->h;
    c->pixels = (unsigned long)f->w * (unsigned long)f->h;
    c->glyphs = (unsigned long)f->atlas.glyph_count * (unsigned long)sizeof(struct nk_font_glyph);
    c->ttf = (unsigned long)nfaces * canlin_font_builtin_ttf_size;
    c->total = c->pixels + c->glyphs + c->ttf;
    c->n_glyphs = (unsigned)f->atlas.glyph_count;
    for (i = 0; i < c->n_glyphs; i++)
        if (f->atlas.glyphs[i].w > 0.0f && f->atlas.glyphs[i].h > 0.0f)
            c->n_bitmap++;
}

/* Bakes one em size and reports it. Returns 0 on success, -1 if the atlas does
 * not fit (the packer runs out of height) or the memory is gone. */
static int measure_em(float em, atlas_cost *out, double *bake_ms)
{
    canlin_render_font f;
    float px[3];
    double t0;

    face_sizes(em, px);
    t0 = now_ms();
    if (canlin_render_font_default_sizes(&f, px, 3) != 0)
        return -1;
    *bake_ms = now_ms() - t0;
    cost_of(&f, 3, out);
    canlin_render_font_free(&f);
    return 0;
}

static void report_atlas(void)
{
    size_t i;
    unsigned reference = 0; /* the glyphs that have a bitmap when nothing is dropped */

    printf("=== the font atlas (the built-in Inter subset, three faces per em size) ===\n");
    printf("  the em size follows from the limits, not from the CPU: the atlas is a\n");
    printf("  size in bytes, so the i686 run gives the same bytes as the host run.\n");
    printf("\n");
    printf("  %-36s %6s %12s %11s %11s %11s %8s %s\n", "case", "em px", "atlas px", "pixels",
           "glyph table", "ttf copies", "bake ms", "glyphs");
    for (i = 0; i < sizeof em_cases / sizeof em_cases[0]; i++) {
        const struct em_case *c = &em_cases[i];
        float em = c->font_px * c->dpi * (float)c->scale / (float)CANLIN_UI_SCALE_BASE_PERCENT;
        atlas_cost cost;
        double bake_ms = 0.0;
        if (measure_em(em, &cost, &bake_ms) != 0) {
            printf("  %-36s %6.2f %12s %11s %11s %11s %8s %s\n", c->label, em, "-", "-", "-", "-",
                   "NO ATLAS", "-");
            continue;
        }
        if (reference == 0)
            reference = cost.n_bitmap;
        printf("  %-36s %6.2f %5d x %5d %11lu %11lu %11lu %8.1f %u/%u%s\n", c->label, em, cost.w,
               cost.h, cost.pixels, cost.glyphs, cost.ttf, bake_ms, cost.n_bitmap, cost.n_glyphs,
               cost.n_bitmap >= reference ? "" : "  DROPPED");
    }
    printf("\n");
    printf("  one cache entry = the alpha8 pixels + Nuklear's glyph table + Nuklear's\n");
    printf("  copy of the TTF blob (%lu bytes, one copy per face)\n",
           (unsigned long)canlin_font_builtin_ttf_size);
    printf("  the font cache holds at most %u entries (CANLIN_UI_FONT_SIZES_MAX)\n",
           (unsigned)CANLIN_UI_FONT_SIZES_MAX);
    printf("  the bake time is an upper bound under the emulation, never a P2 time\n");

    /* The packer has a height limit, so the atlas stops growing at 1024 x 32768.
     * Above the em size where the glyphs no longer all fit, the packer drops
     * them silently: the bake succeeds and the text comes out with holes. */
    {
        float em = em_max();
        atlas_cost cost;
        double bake_ms = 0.0;
        printf("\n");
        printf("  the largest em size the limits allow: %.0f px", em);
        printf(" (font %d px, DPI %.1f, scale %d %%)\n", (int)CANLIN_THEME_FONT_SIZE_MAX,
               (double)CANLIN_UI_DPI_MAX, (int)CANLIN_UI_SCALE_STEP_200);
        if (measure_em(em, &cost, &bake_ms) == 0) {
            printf("  its atlas: %d x %d px = %lu bytes pixels, %lu bytes per cache entry\n",
                   cost.w, cost.h, cost.pixels, cost.total);
            printf("  glyphs with a bitmap: %u of %u%s\n", cost.n_bitmap, cost.n_glyphs,
                   cost.n_bitmap >= reference ? "" : "  <- the packer dropped the rest");
        } else {
            printf("  its atlas: the bake fails\n");
        }
    }
}

/* ---- the graph at its limits ---- */

/* The fixture of t/ui/test_graph.c (test_limits): 256 nodes, 1024 edges, one
 * graph per draw, in a 1024 x 1000 px view at DPI 1.0 (em 16 px). */
static const char graph_src[] =
    "local M = {}\n"
    "local n, e = {}, {}\n"
    "for i = 1, 256 do\n"
    "  n[i] = { id = 'n' .. i, title = 'Node ' .. i, x = (i % 16) * 14, y = (i // 16) * 12,\n"
    "    inputs = { { id = 'i1', type = 't' }, { id = 'i2' } },\n"
    "    outputs = { { id = 'o1', type = 't' }, { id = 'o2' } },\n"
    "    body = { { kind = 'text', text = 'status' }, { kind = 'meter', value = 0.3 } } }\n"
    "end\n"
    "for k = 1, 1024 do\n"
    "  local a, b = (k - 1) % 256 + 1, (k * 7) % 256 + 1\n"
    "  e[k] = { from = { 'n' .. a, 'o' .. (k % 2 + 1) }, to = { 'n' .. b, 'i' .. (k % 2 + 1) } }\n"
    "end\n"
    "local g = { nodes = n, edges = e, selected = 'n1' }\n"
    "function M.draw(ui)\n"
    "  local ed = ui:graph(g, { height = 60 })\n"
    "  print('drawn ' .. #ed)\n"
    "end\n"
    "return M\n";

typedef struct harness {
    canlin_ev_fake_clock clock;
    canlin_ev *ev;
    canlin_ui *ui;
    canlin_ui_output *out;
    canlin_module *m;
    unsigned long log_lines;
    unsigned long errors; /* the log lines that name a fault or a skipped draw */
    char last_log[256];
} harness;

static void log_fn(void *ud, const char *msg, size_t len)
{
    harness *h = ud;
    h->log_lines++;
    if (strstr(msg, "draw:") || strstr(msg, "fault"))
        h->errors++;
    if (len >= sizeof h->last_log)
        len = sizeof h->last_log - 1;
    memcpy(h->last_log, msg, len);
    h->last_log[len] = 0;
}

static void on_deliver(void *ud, canlin_ev *ev, const canlin_ev_delivery *d)
{
    harness *h = ud;
    (void)ev;
    if (h->m && canlin_module_gen(h->m) == d->gen)
        canlin_module_deliver(h->m, d);
}

static int h_init(harness *h, const char *src, int w, int vh)
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
    d.id = "graph-budget";
    d.w = w;
    d.h = vh;
    d.dpi = 1.0f;
    d.scale = CANLIN_UI_SCALE_STEP_100;
    h->out = canlin_ui_output_new(&d, NULL);
    if (!h->ev || !h->ui || !h->out)
        return -1;
    memset(&s, 0, sizeof s);
    memset(&cfg, 0, sizeof cfg);
    s.name = "graphbudget";
    s.version = "1";
    s.main = src;
    s.main_len = strlen(src);
    cfg.log = log_fn;
    cfg.ud = h;
    h->m = canlin_module_load(h->ev, &s, &cfg, &err);
    if (!h->m) {
        fprintf(stderr, "ui_budget: module load failed (%d): %s\n", err, h->last_log);
        return -1;
    }
    if (canlin_module_set_ui(h->m, h->ui, h->out, 0, 0, w, vh) != CANLIN_MODULE_OK)
        return -1;
    return 0;
}

static void h_done(harness *h)
{
    if (h->m)
        canlin_module_free(h->m);
    if (h->ev)
        canlin_ev_free(h->ev);
    if (h->out)
        canlin_ui_output_free(h->out);
    if (h->ui)
        canlin_ui_free(h->ui);
}

/* The command list of the last finished frame: the draw budget in commands. */
typedef struct cmd_count {
    unsigned total;
    unsigned curve; /* the drawn edges (a cubic Bezier per edge) */
    unsigned line;
    unsigned rect;
    unsigned circle;
    unsigned text;
} cmd_count;

static void count_commands(canlin_ui_view *v, cmd_count *c)
{
    const struct nk_command *cmd;
    struct nk_context *ctx = canlin_ui_view_nk(v);

    memset(c, 0, sizeof *c);
    nk_foreach(cmd, ctx)
    {
        c->total++;
        switch (cmd->type) {
        case NK_COMMAND_CURVE: c->curve++; break;
        case NK_COMMAND_LINE: c->line++; break;
        case NK_COMMAND_RECT:
        case NK_COMMAND_RECT_FILLED: c->rect++; break;
        case NK_COMMAND_CIRCLE:
        case NK_COMMAND_CIRCLE_FILLED: c->circle++; break;
        case NK_COMMAND_TEXT: c->text++; break;
        default: break;
        }
    }
}

/* The visible nodes of the graph widget in the layout record. */
static unsigned visible_nodes(canlin_ui_view *v)
{
    size_t n, i;
    unsigned nodes = 0;
    const canlin_ui_rect *r = canlin_ui_view_record(v, &n, NULL);

    for (i = 0; i < n; i++)
        if (r[i].kind == CANLIN_UI_W_GRAPH && r[i].part == CANLIN_UI_P_NODE)
            nodes++;
    return nodes;
}

#define DRAWS 20

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static void report_graph(void)
{
    harness h;
    canlin_ui_view *v;
    cmd_count cmds;
    unsigned nodes;
    double draw_ms[DRAWS];
    double raster_ms[DRAWS];
    double t0, t1;
    int i;
    const int W = 1024, H = 1000;

    if (h_init(&h, graph_src, W, H) != 0) {
        fprintf(stderr, "ui_budget: the graph harness did not come up\n");
        h_done(&h);
        exit(2);
    }
    /* The first frame bakes the font and warms the interpreter; it is not a
     * steady-state frame. */
    for (i = 0; i < 8; i++) {
        canlin_ev_turn_result r;
        canlin_ev_turn(h.ev, &r);
    }
    v = canlin_module_view(h.m);
    if (!v) {
        fprintf(stderr, "ui_budget: the module has no view\n");
        h_done(&h);
        exit(2);
    }
    count_commands(v, &cmds);
    nodes = visible_nodes(v);

    for (i = 0; i < DRAWS; i++) {
        canlin_ev_turn_result r;
        canlin_ev_invalidate(h.ev, canlin_module_gen(h.m));
        t0 = now_ms();
        canlin_ev_turn(h.ev, &r);
        t1 = now_ms();
        draw_ms[i] = t1 - t0;
    }
    /* The counts again, after the measured draws: the frame must be the same. */
    {
        cmd_count again;
        count_commands(v, &again);
        if (again.total != cmds.total || again.curve != cmds.curve) {
            fprintf(stderr, "ui_budget: the frame is not stable (%u vs %u commands)\n",
                    cmds.total, again.total);
            h_done(&h);
            exit(2);
        }
    }
    {
        unsigned char *px = malloc((size_t)W * (size_t)H * CANLIN_RENDER_BPP);
        canlin_render r;
        if (!px) {
            fprintf(stderr, "ui_budget: no pixel buffer\n");
            h_done(&h);
            exit(2);
        }
        canlin_render_init(&r, px, W, H, (size_t)W * CANLIN_RENDER_BPP, NULL, NULL);
        for (i = 0; i < DRAWS; i++) {
            t0 = now_ms();
            canlin_ui_view_render(v, &r);
            t1 = now_ms();
            raster_ms[i] = t1 - t0;
        }
        free(px);
    }
    if (h.errors) {
        fprintf(stderr, "ui_budget: the module logged %lu errors (last: %s)\n", h.errors,
                h.last_log);
        h_done(&h);
        exit(2);
    }
    h_done(&h);

    qsort(draw_ms, DRAWS, sizeof draw_ms[0], cmp_double);
    qsort(raster_ms, DRAWS, sizeof raster_ms[0], cmp_double);

    printf("\n");
    printf("=== the graph at its limits (256 nodes, 1024 edges, 1024 x 1000 px) ===\n");
    printf("  visible nodes:      %u of %u (the rest are culled)\n", nodes,
           (unsigned)CANLIN_UI_GRAPH_MAX_NODES);
    printf("  drawn edges:        %u of %u (the curves of the command list)\n", cmds.curve,
           (unsigned)CANLIN_UI_GRAPH_MAX_EDGES);
    printf("  commands per frame: %u (curve %u, line %u, rect %u, circle %u, text %u)\n",
           cmds.total, cmds.curve, cmds.line, cmds.rect, cmds.circle, cmds.text);
    printf("  the counts are exact and the same on the host and on the target.\n");
    printf("\n");
    printf("  draw  (Lua, graph parse, command build): min %8.2f ms  median %8.2f ms\n",
           draw_ms[0], draw_ms[DRAWS / 2]);
    printf("  raster (the command list into pixels):    min %8.2f ms  median %8.2f ms\n",
           raster_ms[0], raster_ms[DRAWS / 2]);
    printf("  the times under the emulation are an UPPER BOUND: the emulation makes\n");
    printf("  every instruction dearer, so they are not the P2's own times.\n");
}

int main(void)
{
    printf("ui_budget: the UI numbers canlin#96 asks for\n");
#if defined(__i386__) || defined(__i686__)
    printf("build: i686 (run it under qemu-i386 -cpu pentium2: the time is an upper bound)\n");
#else
    printf("build: host\n");
#endif
    report_atlas();
    report_graph();
    return 0;
}
