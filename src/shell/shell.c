/* CanLin shell. See include/canlin/shell.h for the contract.
 *
 * Geometry is computed in px from the output's em whenever its metrics or the
 * arrangement change (layout()); views are rebound only when their rectangle changed
 * (sync_views()), so a divider move touches the two adjacent views only. Chrome is drawn
 * with its own Nuklear context per output (canvas commands only, no Nuklear input) and a
 * font baked at the output's em, styled by the same resolved theme as the views.
 */
#include "canlin/shell.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    D_FULL = 1u << 0,
    D_BAR = 1u << 1,
    D_OVERLAY = 1u << 2,
    D_PANE0 = 1u << 3 /* D_PANE0 << pane */
};

enum { IND_FAULTED, IND_MIC, IND_STORAGE, IND_CLOCK, IND_NET, IND_COUNT };

typedef struct mod_ent {
    canlin_module *m;
    const canlin_module_source *src;
    int model;
    canlin_module_state seen;
    int bound;                  /* set_ui succeeded with bout/br */
    uint32_t bframe;            /* view frames at that bind: older command lists are stale */
    canlin_ui_output *bout;
    canlin_shell_rect br;
} mod_ent;

typedef struct shell_out {
    canlin_ui_output *o;
    unsigned input;
    canlin_shell_mode want, mode;
    int orient, n;
    int slot[CANLIN_SHELL_PANES_MAX];     /* module index or -1 */
    float frac[CANLIN_SHELL_PANES_MAX - 1];
    int cur;                              /* last focused slot; the full-screen slot */

    int w, h, laid;
    float em;
    int bar_h, ring;
    canlin_shell_rect bar, swbtn, ind[IND_COUNT];
    int ind_on[IND_COUNT];
    char ind_txt[IND_COUNT][32];
    int vn;                               /* visible panes */
    int vslot[CANLIN_SHELL_PANES_MAX];
    canlin_shell_rect pane[CANLIN_SHELL_PANES_MAX], view[CANLIN_SHELL_PANES_MAX];
    canlin_shell_rect label[CANLIN_SHELL_PANES_MAX], restart[CANLIN_SHELL_PANES_MAX];
    canlin_shell_rect div[CANLIN_SHELL_PANES_MAX - 1];
    int bound[CANLIN_SHELL_PANES_MAX + 1]; /* pane boundaries along the split, px */
    int minpx;

    int sw_open, sw_sel, sw_rows;
    canlin_shell_rect sw, sw_row[CANLIN_SHELL_MODULES_MAX];
    int sw_mod[CANLIN_SHELL_MODULES_MAX];

    int drag;                             /* divider being dragged, -1 none */
    int drag_off;
    int cap;                              /* module capturing the pointer, -1 none */
    unsigned cap_btns;                    /* buttons held in the captured view (bit each) */
    int hover;                            /* module last sent a motion, -1 none */
    int bar_press, bar_py;
    int chrome_press;                     /* a press consumed by chrome: eat its release */

    unsigned dirty;
    uint32_t vframe[CANLIN_SHELL_PANES_MAX];
    int vmod[CANLIN_SHELL_PANES_MAX];

    struct nk_context nk;
    int nk_ok;
    canlin_render_font font;
    int font_ok;
    float font_em;
} shell_out;

struct canlin_shell {
    canlin_ui *ui;
    canlin_shell_clock_fn clock;
    void *clock_ud;
    int64_t minute;
    int have_minute;
    canlin_shell_status st;
    shell_out out[CANLIN_SHELL_OUTPUTS_MAX];
    int nout;
    mod_ent mods[CANLIN_SHELL_MODULES_MAX];
    int focus_out;
    int ctrl, tab_eaten;
};

/* ---- small helpers ---- */

static int ceil_px(float v)
{
    return (int)ceilf(v - 0.001f);
}

static int inside(const canlin_shell_rect *r, int x, int y)
{
    return r->w > 0 && r->h > 0 && x >= r->x && y >= r->y && x < r->x + r->w &&
           y < r->y + r->h;
}

static int rect_eq(const canlin_shell_rect *a, const canlin_shell_rect *b)
{
    return a->x == b->x && a->y == b->y && a->w == b->w && a->h == b->h;
}

static canlin_shell_rect mkrect(int x, int y, int w, int h)
{
    canlin_shell_rect r;
    r.x = x;
    r.y = y;
    r.w = w < 0 ? 0 : w;
    r.h = h < 0 ? 0 : h;
    return r;
}

static shell_out *find_out(canlin_shell *sh, const canlin_ui_output *o)
{
    int i;
    if (sh == NULL || o == NULL)
        return NULL;
    for (i = 0; i < sh->nout; i++)
        if (sh->out[i].o == o)
            return &sh->out[i];
    return NULL;
}

static int find_mod(canlin_shell *sh, const canlin_module *m)
{
    int i;
    if (m == NULL)
        return -1;
    for (i = 0; i < CANLIN_SHELL_MODULES_MAX; i++)
        if (sh->mods[i].m == m)
            return i;
    return -1;
}

static const canlin_theme *theme(canlin_shell *sh)
{
    return canlin_ui_theme(sh->ui);
}

static struct nk_color role(canlin_shell *sh, int r)
{
    canlin_theme_rgb c = theme(sh)->color[r];
    return nk_rgb(c.r, c.g, c.b);
}

static int mod_state(canlin_shell *sh, int k)
{
    return canlin_module_get_state(sh->mods[k].m);
}

static int faulted_count(canlin_shell *sh)
{
    int i, n = 0;
    for (i = 0; i < CANLIN_SHELL_MODULES_MAX; i++)
        if (sh->mods[i].m && mod_state(sh, i) == CANLIN_MODULE_FAULTED)
            n++;
    return n;
}

/* Output and slot of module k, or -1. */
static int slot_of(canlin_shell *sh, int k, int *slot)
{
    int i, s;
    for (i = 0; i < sh->nout; i++)
        for (s = 0; s < sh->out[i].n; s++)
            if (sh->out[i].slot[s] == k) {
                if (slot)
                    *slot = s;
                return i;
            }
    return -1;
}

static void dirty_all(canlin_shell *sh, unsigned bits)
{
    int i;
    for (i = 0; i < sh->nout; i++)
        sh->out[i].dirty |= bits;
}

/* ---- chrome font ---- */

static int chrome_font(canlin_shell *sh, shell_out *so, float em)
{
    float factor = canlin_ui_output_dpi(so->o) * (float)canlin_ui_output_scale(so->o) / 100.0f;
    struct nk_style *s;
    if (so->font_ok && so->font_em == em)
        return 0;
    if (so->nk_ok)
        nk_clear(&so->nk); /* no command may point into the old atlas */
    if (so->font_ok)
        canlin_render_font_free(&so->font);
    so->font_ok = 0;
    memset(&so->font, 0, sizeof so->font);
    if (canlin_render_font_default(&so->font, em) != 0)
        return -1;
    so->font_ok = 1;
    so->font_em = em;
    if (!so->nk_ok) {
        if (!nk_init_default(&so->nk, &so->font.font->handle))
            return -1;
        so->nk_ok = 1;
    }
    canlin_theme_apply_nk(theme(sh), factor, &so->nk, &so->font.font->handle);
    s = &so->nk.style;
    s->window.fixed_background = nk_style_item_color(nk_rgba(0, 0, 0, 0));
    s->window.padding = nk_vec2(0, 0);
    s->window.spacing = nk_vec2(0, 0);
    s->window.border = 0;
    return 0;
}

static const struct nk_user_font *ufont(shell_out *so)
{
    return &so->font.font->handle;
}

static float text_w(shell_out *so, const char *s)
{
    const struct nk_user_font *f;
    if (!so->font_ok)
        return 0.0f;
    f = ufont(so);
    return f->width(f->userdata, f->height, s, (int)strlen(s));
}

/* ---- layout ---- */

static int tiling_fits(shell_out *so, float em, int w, int ch)
{
    int ext = so->orient == CANLIN_SHELL_ROW ? w : ch;
    int minpx = ceil_px(CANLIN_SHELL_PANE_MIN * em);
    return so->n * minpx <= ext;
}

static canlin_shell_mode choose(shell_out *so, float em, int w, int h)
{
    float wem = (float)w / em;
    int ch = h - ceil_px(CANLIN_SHELL_BAR_HEIGHT * em);
    int fits = tiling_fits(so, em, w, ch);
    if (so->want == CANLIN_SHELL_FULL)
        return CANLIN_SHELL_FULL;
    if (so->want == CANLIN_SHELL_TILED)
        return wem >= CANLIN_SHELL_TILED_MIN - 1e-4f && fits ? CANLIN_SHELL_TILED
                                                             : CANLIN_SHELL_FULL;
    return wem >= CANLIN_SHELL_TILED_DEFAULT - 1e-4f && fits ? CANLIN_SHELL_TILED
                                                             : CANLIN_SHELL_FULL;
}

/* Pane boundaries from the fractions, clamped so every pane keeps its minimum. */
static void boundaries(shell_out *so, int ext)
{
    int k, n = so->n;
    so->bound[0] = 0;
    so->bound[n] = ext;
    for (k = 1; k < n; k++)
        so->bound[k] = (int)lroundf(so->frac[k - 1] * (float)ext);
    for (k = 1; k < n; k++)
        if (so->bound[k] < so->bound[k - 1] + so->minpx)
            so->bound[k] = so->bound[k - 1] + so->minpx;
    for (k = n - 1; k >= 1; k--)
        if (so->bound[k] > so->bound[k + 1] - so->minpx)
            so->bound[k] = so->bound[k + 1] - so->minpx;
}

static void layout_bar(canlin_shell *sh, shell_out *so)
{
    float em = so->em;
    int pad = (int)lroundf(0.5f * em), x, i, avail;
    int order[IND_COUNT] = {IND_CLOCK, IND_NET, IND_STORAGE, IND_MIC, IND_FAULTED};
    int mandatory[IND_COUNT];
    int nf = faulted_count(sh);

    so->bar = mkrect(0, 0, so->w, so->bar_h);
    so->swbtn = mkrect(0, 0, ceil_px(CANLIN_SHELL_SWITCH_BUTTON * em), so->bar_h);
    if (so->swbtn.w > so->w)
        so->swbtn.w = so->w;

    memset(so->ind_on, 0, sizeof so->ind_on);
    memset(mandatory, 0, sizeof mandatory);
    if (nf > 0) {
        snprintf(so->ind_txt[IND_FAULTED], sizeof so->ind_txt[0], "%d faulted", nf);
        so->ind_on[IND_FAULTED] = mandatory[IND_FAULTED] = 1;
    }
    if (sh->st.mic) {
        snprintf(so->ind_txt[IND_MIC], sizeof so->ind_txt[0], "MIC");
        so->ind_on[IND_MIC] = mandatory[IND_MIC] = 1;
    }
    snprintf(so->ind_txt[IND_STORAGE], sizeof so->ind_txt[0], "%s",
             sh->st.storage == CANLIN_SHELL_STORE_VOLATILE ? "VOLATILE"
             : sh->st.storage == CANLIN_SHELL_STORE_ERROR  ? "STORAGE ERROR"
                                                           : "stored");
    so->ind_on[IND_STORAGE] = 1;
    mandatory[IND_STORAGE] = sh->st.storage != CANLIN_SHELL_STORE_PERSISTENT;
    if (sh->clock) {
        int64_t mins = sh->minute % 1440;
        if (mins < 0)
            mins += 1440;
        snprintf(so->ind_txt[IND_CLOCK], sizeof so->ind_txt[0], "%02d:%02d", (int)(mins / 60),
                 (int)(mins % 60));
        so->ind_on[IND_CLOCK] = 1;
    }
    if (sh->st.net != CANLIN_SHELL_NET_UNKNOWN) {
        snprintf(so->ind_txt[IND_NET], sizeof so->ind_txt[0], "%s",
                 sh->st.net == CANLIN_SHELL_NET_ONLINE ? "online" : "offline");
        so->ind_on[IND_NET] = 1;
    }
    /* Drop optional items (net, clock, persistent storage) until the rest fits. */
    avail = so->w - so->swbtn.w;
    for (;;) {
        int need = 0;
        for (i = 0; i < IND_COUNT; i++)
            if (so->ind_on[i])
                need += ceil_px(text_w(so, so->ind_txt[i])) + 2 * pad;
        if (need <= avail)
            break;
        if (so->ind_on[IND_NET])
            so->ind_on[IND_NET] = 0;
        else if (so->ind_on[IND_CLOCK])
            so->ind_on[IND_CLOCK] = 0;
        else if (so->ind_on[IND_STORAGE] && !mandatory[IND_STORAGE])
            so->ind_on[IND_STORAGE] = 0;
        else
            break; /* mandatory items stay; the switcher button is overdrawn */
    }
    x = so->w;
    for (i = 0; i < IND_COUNT; i++) {
        int k = order[i], wpx;
        if (!so->ind_on[k]) {
            so->ind[k] = mkrect(0, 0, 0, 0);
            continue;
        }
        wpx = ceil_px(text_w(so, so->ind_txt[k])) + 2 * pad;
        x -= wpx;
        so->ind[k] = mkrect(x, 0, wpx, so->bar_h);
    }
}

static void layout_switcher(canlin_shell *sh, shell_out *so)
{
    int rowh = ceil_px(CANLIN_SHELL_ROW_HEIGHT * so->em), k, maxrows;
    int w = ceil_px(CANLIN_SHELL_SWITCHER_WIDTH * so->em);
    if (w > so->w)
        w = so->w;
    maxrows = rowh > 0 ? (so->h - so->bar_h) / rowh : 0;
    so->sw_rows = 0;
    for (k = 0; k < CANLIN_SHELL_MODULES_MAX && so->sw_rows < maxrows; k++) {
        if (sh->mods[k].m == NULL)
            continue;
        so->sw_mod[so->sw_rows] = k;
        so->sw_row[so->sw_rows] = mkrect(0, so->bar_h + so->sw_rows * rowh, w, rowh);
        so->sw_rows++;
    }
    so->sw = mkrect(0, so->bar_h, w, so->sw_rows * rowh);
    if (so->sw_sel >= so->sw_rows)
        so->sw_sel = so->sw_rows ? so->sw_rows - 1 : 0;
}

static void layout_pane(canlin_shell *sh, shell_out *so, int i)
{
    int k = so->slot[so->vslot[i]], r = so->ring;
    canlin_shell_rect p = so->pane[i], v;
    int model = k >= 0 && sh->mods[k].model;
    int rowh = ceil_px(CANLIN_SHELL_ROW_HEIGHT * so->em);
    int bw;
    if (model && r == 0)
        r = so->ring > 0 ? so->ring : (int)fmaxf(2.0f, roundf(so->em / 8.0f));
    v = mkrect(p.x + r, p.y + r, p.w - 2 * r, p.h - 2 * r);
    so->label[i] = mkrect(0, 0, 0, 0);
    if (model) {
        int lh = ceil_px(CANLIN_SHELL_MODEL_LABEL * so->em);
        if (lh > v.h - 1)
            lh = v.h > 1 ? v.h - 1 : 0;
        so->label[i] = mkrect(v.x, v.y, v.w, lh);
        v.y += lh;
        v.h -= lh;
    }
    if (v.w < 1)
        v.w = 1;
    if (v.h < 1)
        v.h = 1;
    so->view[i] = v;
    bw = ceil_px(10.0f * so->em);
    if (bw > v.w)
        bw = v.w;
    so->restart[i] = mkrect(v.x + (v.w - bw) / 2, v.y + v.h / 2, bw, rowh);
}

static void layout(canlin_shell *sh, shell_out *so)
{
    int i, ch;
    canlin_ui_output_size(so->o, &so->w, &so->h);
    so->em = canlin_ui_output_em(sh->ui, so->o);
    chrome_font(sh, so, so->em);
    so->bar_h = ceil_px(CANLIN_SHELL_BAR_HEIGHT * so->em);
    if (so->bar_h > so->h)
        so->bar_h = so->h;
    ch = so->h - so->bar_h;
    so->minpx = ceil_px(CANLIN_SHELL_PANE_MIN * so->em);
    so->mode = choose(so, so->em, so->w, so->h);
    memset(so->pane, 0, sizeof so->pane);
    memset(so->div, 0, sizeof so->div);
    if (so->mode == CANLIN_SHELL_TILED) {
        int ext = so->orient == CANLIN_SHELL_ROW ? so->w : ch;
        int hit = ceil_px(CANLIN_SHELL_DIVIDER_HIT * so->em);
        boundaries(so, ext);
        so->vn = so->n;
        so->ring = (int)fmaxf(2.0f, roundf(so->em / 8.0f));
        for (i = 0; i < so->n; i++) {
            int a = so->bound[i], b = so->bound[i + 1];
            so->vslot[i] = i;
            so->pane[i] = so->orient == CANLIN_SHELL_ROW ? mkrect(a, so->bar_h, b - a, ch)
                                                         : mkrect(0, so->bar_h + a, so->w, b - a);
            if (i > 0)
                so->div[i - 1] = so->orient == CANLIN_SHELL_ROW
                                     ? mkrect(a - hit / 2, so->bar_h, hit, ch)
                                     : mkrect(0, so->bar_h + a - hit / 2, so->w, hit);
        }
    } else {
        so->vn = 1;
        so->vslot[0] = so->cur;
        so->ring = 0;
        so->pane[0] = mkrect(0, so->bar_h, so->w, ch);
    }
    for (i = 0; i < so->vn; i++)
        layout_pane(sh, so, i);
    layout_bar(sh, so);
    layout_switcher(sh, so);
    so->laid = 1;
    so->dirty |= D_FULL;
}

/* Re-lay out after a divider moved: only the two adjacent panes change. */
static void move_divider(canlin_shell *sh, shell_out *so, int d, int pos)
{
    int lo = so->bound[d] + so->minpx, hi = so->bound[d + 2] - so->minpx, ext, ch;
    int k = d + 1;
    if (pos < lo)
        pos = lo;
    if (pos > hi)
        pos = hi;
    if (pos == so->bound[k])
        return;
    ch = so->h - so->bar_h;
    ext = so->orient == CANLIN_SHELL_ROW ? so->w : ch;
    so->bound[k] = pos;
    so->frac[d] = (float)pos / (float)ext;
    {
        int hit = ceil_px(CANLIN_SHELL_DIVIDER_HIT * so->em), i;
        for (i = d; i <= d + 1; i++) {
            int a = so->bound[i], b = so->bound[i + 1];
            so->pane[i] = so->orient == CANLIN_SHELL_ROW ? mkrect(a, so->bar_h, b - a, ch)
                                                         : mkrect(0, so->bar_h + a, so->w, b - a);
            layout_pane(sh, so, i);
            so->dirty |= D_PANE0 << i;
        }
        so->div[d] = so->orient == CANLIN_SHELL_ROW ? mkrect(pos - hit / 2, so->bar_h, hit, ch)
                                                    : mkrect(0, so->bar_h + pos - hit / 2, so->w, hit);
    }
    if (so->sw_open)
        so->dirty |= D_OVERLAY;
}

/* Rebind every visible module whose view rectangle or output changed. */
static void sync_views(canlin_shell *sh)
{
    int i, p;
    for (i = 0; i < sh->nout; i++) {
        shell_out *so = &sh->out[i];
        for (p = 0; p < so->vn; p++) {
            int k = so->slot[so->vslot[p]];
            mod_ent *e;
            if (k < 0)
                continue;
            e = &sh->mods[k];
            if (e->bound && e->bout == so->o && rect_eq(&e->br, &so->view[p]))
                continue;
            if (canlin_module_set_ui(e->m, sh->ui, so->o, so->view[p].x, so->view[p].y,
                                     so->view[p].w, so->view[p].h) == CANLIN_MODULE_OK) {
                e->bound = 1;
                e->bframe = canlin_ui_view_frames(canlin_module_view(e->m));
                e->bout = so->o;
                e->br = so->view[p];
            }
        }
    }
}

static int metrics_changed(canlin_shell *sh, shell_out *so)
{
    int w, h;
    canlin_ui_output_size(so->o, &w, &h);
    return !so->laid || w != so->w || h != so->h || canlin_ui_output_em(sh->ui, so->o) != so->em;
}

static void relayout_all(canlin_shell *sh)
{
    int i;
    for (i = 0; i < sh->nout; i++)
        layout(sh, &sh->out[i]);
    sync_views(sh);
}

/* ---- lifecycle ---- */

canlin_shell *canlin_shell_new(const canlin_shell_config *cfg, int *err)
{
    canlin_shell *sh;
    int dummy;
    if (err == NULL)
        err = &dummy;
    if (cfg == NULL || cfg->ui == NULL) {
        *err = CANLIN_SHELL_EINVAL;
        return NULL;
    }
    sh = calloc(1, sizeof *sh);
    if (sh == NULL) {
        *err = CANLIN_SHELL_ENOMEM;
        return NULL;
    }
    sh->ui = cfg->ui;
    sh->clock = cfg->clock;
    sh->clock_ud = cfg->clock_ud;
    if (sh->clock) {
        int64_t t = sh->clock(sh->clock_ud);
        sh->minute = t >= 0 ? t / 60 : -((-t + 59) / 60);
        sh->have_minute = 1;
    }
    *err = CANLIN_SHELL_OK;
    return sh;
}

void canlin_shell_free(canlin_shell *sh)
{
    int i;
    if (sh == NULL)
        return;
    for (i = 0; i < sh->nout; i++) {
        if (sh->out[i].nk_ok)
            nk_free(&sh->out[i].nk);
        if (sh->out[i].font_ok)
            canlin_render_font_free(&sh->out[i].font);
    }
    free(sh);
}

int canlin_shell_add_output(canlin_shell *sh, canlin_ui_output *o, unsigned input)
{
    shell_out *so;
    int i;
    if (sh == NULL || o == NULL || find_out(sh, o))
        return CANLIN_SHELL_EINVAL;
    if (sh->nout == CANLIN_SHELL_OUTPUTS_MAX)
        return CANLIN_SHELL_EFULL;
    {
        /* The views learn the input class from the output (on-screen keyboard, canlin#45). */
        unsigned ui_in = ((input & CANLIN_SHELL_IN_POINTER) ? CANLIN_UI_INPUT_POINTER : 0u) |
                         ((input & CANLIN_SHELL_IN_TOUCH) ? CANLIN_UI_INPUT_TOUCH : 0u) |
                         ((input & CANLIN_SHELL_IN_KEYBOARD) ? CANLIN_UI_INPUT_KEYBOARD : 0u);
        if (canlin_ui_output_set_input(o, ui_in) != CANLIN_UI_OK)
            return CANLIN_SHELL_EINVAL; /* a view on o is inside its frame */
    }
    so = &sh->out[sh->nout];
    memset(so, 0, sizeof *so);
    so->o = o;
    so->input = input;
    so->want = CANLIN_SHELL_AUTO;
    so->orient = CANLIN_SHELL_ROW;
    so->n = 2;
    for (i = 0; i < CANLIN_SHELL_PANES_MAX; i++) {
        so->slot[i] = -1;
        so->vmod[i] = -1;
    }
    so->frac[0] = 0.5f;
    so->frac[1] = 1.0f;
    so->drag = so->cap = so->hover = -1;
    so->bar_press = 0;
    if (chrome_font(sh, so, canlin_ui_output_em(sh->ui, o)) != 0) {
        if (so->nk_ok)
            nk_free(&so->nk);
        if (so->font_ok)
            canlin_render_font_free(&so->font);
        memset(so, 0, sizeof *so);
        return CANLIN_SHELL_ENOMEM;
    }
    sh->nout++;
    layout(sh, so);
    return CANLIN_SHELL_OK;
}

int canlin_shell_set_mode(canlin_shell *sh, canlin_ui_output *o, canlin_shell_mode mode)
{
    shell_out *so = find_out(sh, o);
    if (so == NULL || mode < CANLIN_SHELL_AUTO || mode > CANLIN_SHELL_TILED)
        return CANLIN_SHELL_EINVAL;
    so->want = mode;
    layout(sh, so);
    sync_views(sh);
    return CANLIN_SHELL_OK;
}

canlin_shell_mode canlin_shell_mode_of(canlin_shell *sh, canlin_ui_output *o)
{
    shell_out *so = find_out(sh, o);
    if (so == NULL)
        return CANLIN_SHELL_FULL;
    if (metrics_changed(sh, so)) {
        layout(sh, so);
        sync_views(sh);
    }
    return so->mode;
}

int canlin_shell_tiled_available(canlin_shell *sh, canlin_ui_output *o)
{
    shell_out *so = find_out(sh, o);
    int w, h, bar;
    float em;
    if (so == NULL)
        return 0;
    canlin_ui_output_size(o, &w, &h);
    em = canlin_ui_output_em(sh->ui, o);
    bar = ceil_px(CANLIN_SHELL_BAR_HEIGHT * em);
    return (float)w / em >= CANLIN_SHELL_TILED_MIN - 1e-4f && tiling_fits(so, em, w, h - bar);
}

/* First module in no slot at all, or -1. */
static int unslotted(canlin_shell *sh)
{
    int k;
    for (k = 0; k < CANLIN_SHELL_MODULES_MAX; k++)
        if (sh->mods[k].m && slot_of(sh, k, NULL) < 0)
            return k;
    return -1;
}

int canlin_shell_set_tiling(canlin_shell *sh, canlin_ui_output *o, canlin_shell_orient orient,
                            int panes)
{
    shell_out *so = find_out(sh, o);
    int i;
    if (so == NULL || panes < 2 || panes > CANLIN_SHELL_PANES_MAX ||
        (orient != CANLIN_SHELL_ROW && orient != CANLIN_SHELL_COLUMN))
        return CANLIN_SHELL_EINVAL;
    for (i = panes; i < CANLIN_SHELL_PANES_MAX; i++)
        so->slot[i] = -1;
    so->orient = orient;
    so->n = panes;
    for (i = 0; i < panes; i++)
        if (so->slot[i] < 0)
            so->slot[i] = unslotted(sh);
    for (i = 0; i < panes - 1; i++)
        so->frac[i] = (float)(i + 1) / (float)panes;
    if (so->cur >= panes)
        so->cur = panes - 1;
    so->cap = so->hover = so->drag = -1;
    layout(sh, so);
    sync_views(sh);
    return CANLIN_SHELL_OK;
}

/* ---- modules ---- */

int canlin_shell_add_module(canlin_shell *sh, canlin_module *m, const canlin_module_source *src)
{
    int k, i, s;
    if (sh == NULL || m == NULL || find_mod(sh, m) >= 0)
        return CANLIN_SHELL_EINVAL;
    for (k = 0; k < CANLIN_SHELL_MODULES_MAX; k++)
        if (sh->mods[k].m == NULL)
            break;
    if (k == CANLIN_SHELL_MODULES_MAX)
        return CANLIN_SHELL_EFULL;
    memset(&sh->mods[k], 0, sizeof sh->mods[k]);
    sh->mods[k].m = m;
    sh->mods[k].src = src;
    sh->mods[k].seen = canlin_module_get_state(m);
    /* Placement: the focused slot, then any visible slot, then any slot. */
    if (sh->nout > 0) {
        shell_out *fo = &sh->out[sh->focus_out];
        if (fo->slot[fo->cur] < 0)
            fo->slot[fo->cur] = k;
        else {
            int placed = 0;
            for (i = 0; i < sh->nout && !placed; i++)
                for (s = 0; s < sh->out[i].vn && !placed; s++)
                    if (sh->out[i].slot[sh->out[i].vslot[s]] < 0) {
                        sh->out[i].slot[sh->out[i].vslot[s]] = k;
                        placed = 1;
                    }
            for (i = 0; i < sh->nout && !placed; i++)
                for (s = 0; s < sh->out[i].n && !placed; s++)
                    if (sh->out[i].slot[s] < 0) {
                        sh->out[i].slot[s] = k;
                        placed = 1;
                    }
        }
    }
    relayout_all(sh);
    return CANLIN_SHELL_OK;
}

int canlin_shell_remove_module(canlin_shell *sh, canlin_module *m)
{
    int k = sh ? find_mod(sh, m) : -1, i, s;
    if (k < 0)
        return CANLIN_SHELL_EINVAL;
    for (i = 0; i < sh->nout; i++) {
        shell_out *so = &sh->out[i];
        for (s = 0; s < CANLIN_SHELL_PANES_MAX; s++) {
            if (so->slot[s] == k)
                so->slot[s] = -1;
            if (so->vmod[s] == k)
                so->vmod[s] = -1;
        }
        if (so->cap == k)
            so->cap = -1;
        if (so->hover == k)
            so->hover = -1;
    }
    memset(&sh->mods[k], 0, sizeof sh->mods[k]);
    relayout_all(sh);
    return CANLIN_SHELL_OK;
}

int canlin_shell_set_model_view(canlin_shell *sh, canlin_module *m, int on)
{
    int k = sh ? find_mod(sh, m) : -1;
    if (k < 0)
        return CANLIN_SHELL_EINVAL;
    if (sh->mods[k].model != (on != 0)) {
        sh->mods[k].model = on != 0;
        relayout_all(sh);
    }
    return CANLIN_SHELL_OK;
}

int canlin_shell_restart(canlin_shell *sh, canlin_module *m)
{
    int k = sh ? find_mod(sh, m) : -1, o, s;
    if (k < 0)
        return CANLIN_SHELL_EINVAL;
    if (canlin_module_get_state(m) != CANLIN_MODULE_FAULTED)
        return CANLIN_SHELL_ESTATE;
    if (sh->mods[k].src == NULL)
        return CANLIN_SHELL_EINVAL;
    /* A new generation (core API §5.3): the old one is gone, nothing is rolled back. */
    if (canlin_module_reload(m, sh->mods[k].src) != CANLIN_MODULE_OK)
        return CANLIN_SHELL_EMODULE;
    sh->mods[k].bound = 0; /* rebind: requests the first draw of the new generation */
    o = slot_of(sh, k, &s);
    if (o >= 0)
        sh->out[o].dirty |= D_FULL;
    dirty_all(sh, D_BAR);
    sync_views(sh);
    return CANLIN_SHELL_OK;
}

/* Put module k into slot `slot` of output oi; where k was, the slot's old module goes. */
static void assign(canlin_shell *sh, int oi, int slot, int k)
{
    shell_out *so = &sh->out[oi];
    int ps = -1, po = slot_of(sh, k, &ps), old = so->slot[slot];
    if (po == oi && ps == slot)
        return;
    so->slot[slot] = k;
    if (po >= 0) {
        sh->out[po].slot[ps] = old;
        layout(sh, &sh->out[po]);
    }
    so->cap = so->hover = -1;
    layout(sh, so);
    sync_views(sh);
}

int canlin_shell_assign(canlin_shell *sh, canlin_module *m)
{
    int k = sh ? find_mod(sh, m) : -1;
    if (k < 0 || sh->nout == 0)
        return CANLIN_SHELL_EINVAL;
    assign(sh, sh->focus_out, sh->out[sh->focus_out].cur, k);
    return CANLIN_SHELL_OK;
}

void canlin_shell_set_status(canlin_shell *sh, const canlin_shell_status *st)
{
    int i;
    if (sh == NULL || st == NULL)
        return;
    if (memcmp(&sh->st, st, sizeof *st) == 0)
        return;
    sh->st = *st;
    for (i = 0; i < sh->nout; i++) {
        layout_bar(sh, &sh->out[i]);
        sh->out[i].dirty |= D_BAR;
    }
}

/* ---- focus and input ---- */

static void set_focus(canlin_shell *sh, int oi, int slot)
{
    shell_out *old = &sh->out[sh->focus_out], *so = &sh->out[oi];
    int i;
    if (sh->focus_out == oi && so->cur == slot)
        return;
    for (i = 0; i < old->vn; i++)
        if (old->vslot[i] == old->cur)
            old->dirty |= D_PANE0 << i;
    sh->focus_out = oi;
    if (so->cur != slot && so->mode == CANLIN_SHELL_FULL) {
        so->cur = slot;
        layout(sh, so);
        sync_views(sh);
    }
    so->cur = slot;
    for (i = 0; i < so->vn; i++)
        if (so->vslot[i] == slot)
            so->dirty |= D_PANE0 << i;
}

static void focus_next(canlin_shell *sh)
{
    int oi = sh->focus_out, p = 0, i, tries;
    shell_out *so = &sh->out[oi];
    for (i = 0; i < so->vn; i++)
        if (so->vslot[i] == so->cur)
            p = i;
    for (tries = 0; tries < sh->nout + 1; tries++) {
        if (p + 1 < sh->out[oi].vn) {
            set_focus(sh, oi, sh->out[oi].vslot[p + 1]);
            return;
        }
        oi = (oi + 1) % sh->nout;
        p = -1;
        if (sh->out[oi].vn > 0) {
            set_focus(sh, oi, sh->out[oi].vslot[0]);
            return;
        }
    }
}

static void switcher_set(canlin_shell *sh, shell_out *so, int open)
{
    if (so->sw_open == open)
        return;
    so->sw_open = open;
    if (open) {
        layout_switcher(sh, so);
        so->sw_sel = 0;
        so->dirty |= D_OVERLAY;
    } else {
        so->dirty |= D_FULL; /* what the overlay covered */
    }
}

static int post(canlin_shell *sh, int k, const canlin_ui_input *in)
{
    if (k < 0 || mod_state(sh, k) != CANLIN_MODULE_RUNNING)
        return CANLIN_SHELL_R_DROPPED;
    return canlin_module_post_input(sh->mods[k].m, in) == CANLIN_MODULE_OK ? CANLIN_SHELL_R_VIEW
                                                                           : CANLIN_SHELL_R_DROPPED;
}

static int focused_mod(canlin_shell *sh)
{
    shell_out *so;
    if (sh->nout == 0)
        return -1;
    so = &sh->out[sh->focus_out];
    return so->slot[so->cur];
}

static int key_input(canlin_shell *sh, const canlin_ui_input *in)
{
    shell_out *fo = &sh->out[sh->focus_out];
    int k = focused_mod(sh);

    if (in->key == CANLIN_SHELL_KEY_SWITCHER || in->key == CANLIN_SHELL_KEY_NEXT) {
        if (in->down && in->key == CANLIN_SHELL_KEY_SWITCHER)
            switcher_set(sh, fo, !fo->sw_open);
        else if (in->down)
            focus_next(sh);
        return CANLIN_SHELL_R_CHROME;
    }
    if (in->key == CANLIN_UI_KEY_TAB && (sh->ctrl || sh->tab_eaten)) {
        if (in->down && !sh->tab_eaten)
            focus_next(sh);
        sh->tab_eaten = in->down;
        return CANLIN_SHELL_R_CHROME;
    }
    if (in->key == CANLIN_UI_KEY_CTRL)
        sh->ctrl = in->down;
    if (fo->sw_open) {
        if (in->down && in->key == CANLIN_UI_KEY_DOWN && fo->sw_sel + 1 < fo->sw_rows) {
            fo->sw_sel++;
            fo->dirty |= D_OVERLAY;
        } else if (in->down && in->key == CANLIN_UI_KEY_UP && fo->sw_sel > 0) {
            fo->sw_sel--;
            fo->dirty |= D_OVERLAY;
        } else if (in->down && in->key == CANLIN_UI_KEY_ENTER && fo->sw_rows > 0) {
            int m = fo->sw_mod[fo->sw_sel];
            switcher_set(sh, fo, 0);
            assign(sh, sh->focus_out, fo->cur, m);
        }
        return in->key == CANLIN_UI_KEY_CTRL ? post(sh, k, in) : CANLIN_SHELL_R_CHROME;
    }
    if (k < 0 || mod_state(sh, k) == CANLIN_MODULE_FAULTED) {
        /* Keyboard reaches the restart action of a focused faulted pane. */
        if (k >= 0 && in->down && in->key == CANLIN_UI_KEY_ENTER)
            canlin_shell_restart(sh, sh->mods[k].m);
        return CANLIN_SHELL_R_CHROME;
    }
    return post(sh, k, in);
}

/* Visible pane of so under (x, y), or -1. */
static int pane_at(shell_out *so, int x, int y)
{
    int i;
    for (i = 0; i < so->vn; i++)
        if (inside(&so->pane[i], x, y))
            return i;
    return -1;
}

/* Keep hover state honest: the view the pointer left gets one motion outside itself. */
static void hover_to(canlin_shell *sh, shell_out *so, int k, const canlin_ui_input *in)
{
    if (so->hover >= 0 && so->hover != k) {
        canlin_ui_input mv = *in;
        mv.type = CANLIN_UI_IN_MOTION;
        post(sh, so->hover, &mv);
    }
    so->hover = k;
}

static int pointer_input(canlin_shell *sh, int oi, const canlin_ui_input *in)
{
    shell_out *so = &sh->out[oi];
    int x = in->x, y = in->y, i, p, k;

    if (in->type == CANLIN_UI_IN_SCROLL) {
        if (so->sw_open)
            return CANLIN_SHELL_R_CHROME;
        return so->hover >= 0 ? post(sh, so->hover, in) : CANLIN_SHELL_R_DROPPED;
    }
    /* Captures first: a divider drag or a view press own the pointer until release. */
    if (so->drag >= 0) {
        int pos = (so->orient == CANLIN_SHELL_ROW ? x : y - so->bar_h) - so->drag_off;
        move_divider(sh, so, so->drag, pos);
        sync_views(sh);
        if (in->type == CANLIN_UI_IN_BUTTON && !in->down &&
            in->button == CANLIN_UI_BUTTON_PRIMARY)
            so->drag = -1;
        return CANLIN_SHELL_R_CHROME;
    }
    if (so->cap >= 0) {
        int k = so->cap, r = post(sh, k, in);
        /* Held until every button pressed into the view is released (canlin#32). */
        if (in->type == CANLIN_UI_IN_BUTTON) {
            if (in->down)
                so->cap_btns |= 1u << in->button;
            else
                so->cap_btns &= ~(1u << in->button);
            if (so->cap_btns == 0)
                so->cap = -1;
        }
        return r;
    }
    if (so->chrome_press) {
        if (so->bar_press && in->type == CANLIN_UI_IN_MOTION &&
            (so->input & CANLIN_SHELL_IN_TOUCH) &&
            y - so->bar_py >= ceil_px(CANLIN_SHELL_SWIPE * so->em)) {
            so->bar_press = 0;
            switcher_set(sh, so, 1);
        }
        if (in->type == CANLIN_UI_IN_BUTTON && !in->down &&
            in->button == CANLIN_UI_BUTTON_PRIMARY)
            so->chrome_press = so->bar_press = 0;
        return CANLIN_SHELL_R_CHROME;
    }

    if (in->type == CANLIN_UI_IN_MOTION) {
        if (so->sw_open && (inside(&so->sw, x, y) || inside(&so->bar, x, y)))
            return CANLIN_SHELL_R_CHROME;
        p = pane_at(so, x, y);
        i = p >= 0 ? so->slot[so->vslot[p]] : -1;
        if (i >= 0 && mod_state(sh, i) != CANLIN_MODULE_RUNNING)
            i = -1;
        hover_to(sh, so, i, in);
        return i >= 0 ? post(sh, i, in) : CANLIN_SHELL_R_DROPPED;
    }

    /* A button edge without capture. A release here belongs to nobody. */
    if (!in->down)
        return CANLIN_SHELL_R_DROPPED;
    if (in->button != CANLIN_UI_BUTTON_PRIMARY) {
        /* Secondary/middle: chrome has no binding for them (consumed, nothing happens);
         * over a running view they focus and capture like a primary press. */
        p = so->sw_open ? -1 : pane_at(so, x, y);
        i = p >= 0 ? so->slot[so->vslot[p]] : -1;
        if (i < 0 || mod_state(sh, i) != CANLIN_MODULE_RUNNING || inside(&so->bar, x, y))
            return CANLIN_SHELL_R_CHROME;
        if (so->mode == CANLIN_SHELL_TILED)
            for (k = 0; k < so->vn - 1; k++)
                if (inside(&so->div[k], x, y))
                    return CANLIN_SHELL_R_CHROME;
        set_focus(sh, oi, so->vslot[p]);
        hover_to(sh, so, i, in);
        so->cap = i;
        so->cap_btns = 1u << in->button;
        return post(sh, i, in);
    }
    so->chrome_press = 1;
    if (so->sw_open) {
        for (i = 0; i < so->sw_rows; i++)
            if (inside(&so->sw_row[i], x, y)) {
                int m = so->sw_mod[i];
                switcher_set(sh, so, 0);
                set_focus(sh, oi, so->cur);
                assign(sh, oi, so->cur, m);
                return CANLIN_SHELL_R_CHROME;
            }
        switcher_set(sh, so, 0); /* a tap outside closes it and goes nowhere else */
        return CANLIN_SHELL_R_CHROME;
    }
    if (inside(&so->bar, x, y)) {
        if (inside(&so->swbtn, x, y))
            switcher_set(sh, so, 1);
        else {
            so->bar_press = 1;
            so->bar_py = y;
        }
        return CANLIN_SHELL_R_CHROME;
    }
    if (so->mode == CANLIN_SHELL_TILED)
        for (i = 0; i < so->vn - 1; i++)
            if (inside(&so->div[i], x, y)) {
                int along = so->orient == CANLIN_SHELL_ROW ? x : y - so->bar_h;
                so->drag = i;
                so->drag_off = along - so->bound[i + 1];
                so->chrome_press = 0; /* the drag capture owns the release */
                return CANLIN_SHELL_R_CHROME;
            }
    p = pane_at(so, x, y);
    if (p < 0)
        return CANLIN_SHELL_R_CHROME; /* chrome gap: consumed */
    i = so->slot[so->vslot[p]];
    set_focus(sh, oi, so->vslot[p]);
    if (i < 0 || mod_state(sh, i) != CANLIN_MODULE_RUNNING) {
        if (i >= 0 && mod_state(sh, i) == CANLIN_MODULE_FAULTED && inside(&so->restart[p], x, y))
            canlin_shell_restart(sh, sh->mods[i].m);
        return CANLIN_SHELL_R_CHROME;
    }
    /* Tap-through: focus moved above, the press goes to the view in the same call. */
    so->chrome_press = 0;
    hover_to(sh, so, i, in);
    so->cap = i;
    so->cap_btns = 1u << CANLIN_UI_BUTTON_PRIMARY;
    return post(sh, i, in);
}

int canlin_shell_input(canlin_shell *sh, canlin_ui_output *o, const canlin_ui_input *in)
{
    shell_out *so = find_out(sh, o);
    if (so == NULL || in == NULL)
        return CANLIN_SHELL_EINVAL;
    if (metrics_changed(sh, so)) {
        layout(sh, so);
        sync_views(sh);
    }
    switch (in->type) {
    case CANLIN_UI_IN_KEY:
        if (in->key != CANLIN_SHELL_KEY_SWITCHER && in->key != CANLIN_SHELL_KEY_NEXT &&
            !canlin_ui_input_valid(in))
            return CANLIN_SHELL_EINVAL;
        return key_input(sh, in);
    case CANLIN_UI_IN_TEXT: {
        shell_out *fo = &sh->out[sh->focus_out];
        if (!canlin_ui_input_valid(in))
            return CANLIN_SHELL_EINVAL;
        if (fo->sw_open)
            return CANLIN_SHELL_R_CHROME;
        return post(sh, focused_mod(sh), in);
    }
    case CANLIN_UI_IN_MOTION:
    case CANLIN_UI_IN_BUTTON:
    case CANLIN_UI_IN_SCROLL:
        if (!canlin_ui_input_valid(in))
            return CANLIN_SHELL_EINVAL; /* e.g. a button outside canlin_ui_button */
        return pointer_input(sh, (int)(so - sh->out), in);
    default:
        return CANLIN_SHELL_EINVAL;
    }
}

canlin_module *canlin_shell_focused(canlin_shell *sh, canlin_ui_output **o, int *pane)
{
    int k;
    if (sh == NULL || sh->nout == 0)
        return NULL;
    if (o)
        *o = sh->out[sh->focus_out].o;
    if (pane) {
        shell_out *so = &sh->out[sh->focus_out];
        int i;
        *pane = 0;
        for (i = 0; i < so->vn; i++)
            if (so->vslot[i] == so->cur)
                *pane = i;
    }
    k = focused_mod(sh);
    return k >= 0 ? sh->mods[k].m : NULL;
}

int canlin_shell_switcher_open(canlin_shell *sh, canlin_ui_output *o)
{
    shell_out *so = find_out(sh, o);
    return so ? so->sw_open : 0;
}

/* ---- update and drawing ---- */

int canlin_shell_update(canlin_shell *sh)
{
    int i, k, any = 0;
    if (sh == NULL)
        return 0;
    for (i = 0; i < sh->nout; i++)
        if (metrics_changed(sh, &sh->out[i]))
            layout(sh, &sh->out[i]);
    if (sh->clock) {
        int64_t t = sh->clock(sh->clock_ud);
        int64_t mn = t >= 0 ? t / 60 : -((-t + 59) / 60);
        if (!sh->have_minute || mn != sh->minute) {
            sh->minute = mn;
            sh->have_minute = 1;
            for (i = 0; i < sh->nout; i++) {
                layout_bar(sh, &sh->out[i]);
                sh->out[i].dirty |= D_BAR;
            }
        }
    }
    for (k = 0; k < CANLIN_SHELL_MODULES_MAX; k++) {
        mod_ent *e = &sh->mods[k];
        canlin_module_state st;
        int s, o;
        if (e->m == NULL)
            continue;
        st = canlin_module_get_state(e->m);
        if (st == e->seen)
            continue;
        e->seen = st;
        o = slot_of(sh, k, &s);
        if (o >= 0) {
            shell_out *so = &sh->out[o];
            for (i = 0; i < so->vn; i++)
                if (so->vslot[i] == s)
                    so->dirty |= D_PANE0 << i;
            if (so->cap == k)
                so->cap = -1;
            if (so->hover == k)
                so->hover = -1;
        }
        for (i = 0; i < sh->nout; i++) {
            layout_bar(sh, &sh->out[i]);
            sh->out[i].dirty |= D_BAR;
            if (sh->out[i].sw_open)
                sh->out[i].dirty |= D_OVERLAY;
        }
    }
    sync_views(sh);
    for (i = 0; i < sh->nout; i++)
        any |= canlin_shell_dirty(sh, sh->out[i].o);
    return any;
}

static int view_new_frame(canlin_shell *sh, shell_out *so, int p)
{
    int k = so->slot[so->vslot[p]];
    canlin_ui_view *v;
    if (k < 0 || mod_state(sh, k) != CANLIN_MODULE_RUNNING)
        return 0;
    v = canlin_module_view(sh->mods[k].m);
    if (v == NULL || canlin_ui_view_frames(v) == sh->mods[k].bframe)
        return 0; /* no frame since the last (re)bind: its commands sit at the old rect */
    return so->vmod[p] != k || canlin_ui_view_frames(v) != so->vframe[p];
}

int canlin_shell_dirty(canlin_shell *sh, canlin_ui_output *o)
{
    shell_out *so = find_out(sh, o);
    int p;
    if (so == NULL)
        return 0;
    if (so->dirty)
        return 1;
    for (p = 0; p < so->vn; p++)
        if (view_new_frame(sh, so, p))
            return 1;
    return 0;
}

static struct nk_rect nkr(canlin_shell_rect r)
{
    return nk_rect((float)r.x, (float)r.y, (float)r.w, (float)r.h);
}

static void text_in(shell_out *so, struct nk_command_buffer *cv, canlin_shell_rect r,
                    const char *s, struct nk_color fg, int center)
{
    const struct nk_user_font *f = ufont(so);
    float tw = text_w(so, s), pad = roundf(0.5f * so->em);
    float x = center ? (float)r.x + ((float)r.w - tw) / 2.0f : (float)r.x + pad;
    float y = (float)r.y + ((float)r.h - f->height) / 2.0f;
    nk_draw_text(cv, nk_rect(roundf(x), roundf(y), tw + 1.0f, f->height), s, (int)strlen(s), f,
                 nk_rgba(0, 0, 0, 0), fg);
}

static void draw_bar(canlin_shell *sh, shell_out *so, struct nk_command_buffer *cv)
{
    int i;
    nk_fill_rect(cv, nkr(so->bar), 0, role(sh, CANLIN_THEME_SURFACE));
    nk_fill_rect(cv, nkr(so->swbtn), 0,
                 so->sw_open ? role(sh, CANLIN_THEME_ACCENT) : role(sh, CANLIN_THEME_SURFACE));
    text_in(so, cv, so->swbtn, "Modules", role(sh, CANLIN_THEME_TEXT), 1);
    for (i = 0; i < IND_COUNT; i++) {
        int danger = i == IND_FAULTED || i == IND_MIC ||
                     (i == IND_STORAGE && sh->st.storage != CANLIN_SHELL_STORE_PERSISTENT);
        if (!so->ind_on[i])
            continue;
        if (danger)
            nk_fill_rect(cv, nkr(so->ind[i]), 0, role(sh, CANLIN_THEME_DANGER));
        text_in(so, cv, so->ind[i], so->ind_txt[i],
                danger ? role(sh, CANLIN_THEME_BACKGROUND) : role(sh, CANLIN_THEME_TEXT), 1);
    }
}

static const char *state_name(int st)
{
    return st == CANLIN_MODULE_RUNNING ? "running" : st == CANLIN_MODULE_FAULTED ? "faulted"
                                                                                : "stopped";
}

/* Pane chrome: ring (divider line and focus mark), model label, fault or empty panel. */
static void draw_pane(canlin_shell *sh, shell_out *so, struct nk_command_buffer *cv, int p)
{
    int k = so->slot[so->vslot[p]];
    int focused = &sh->out[sh->focus_out] == so && so->vslot[p] == so->cur;
    canlin_shell_rect pr = so->pane[p], v = so->view[p];
    nk_fill_rect(cv, nkr(pr), 0,
                 focused && so->ring ? role(sh, CANLIN_THEME_ACCENT) : role(sh, CANLIN_THEME_MUTED));
    nk_fill_rect(cv, nkr(v), 0, role(sh, CANLIN_THEME_BACKGROUND));
    if (so->label[p].h > 0) {
        nk_fill_rect(cv, nkr(so->label[p]), 0, role(sh, CANLIN_THEME_SURFACE));
        text_in(so, cv, so->label[p], "Model-generated view", role(sh, CANLIN_THEME_TEXT), 0);
    }
    if (k < 0) {
        text_in(so, cv, mkrect(v.x, v.y, v.w, ceil_px(2.5f * so->em)),
                "Empty pane: open Modules", role(sh, CANLIN_THEME_MUTED), 0);
    } else if (mod_state(sh, k) != CANLIN_MODULE_RUNNING) {
        char line[96];
        snprintf(line, sizeof line, "Module %s", state_name(mod_state(sh, k)));
        text_in(so, cv, mkrect(v.x, v.y, v.w, ceil_px(2.5f * so->em)), line,
                role(sh, CANLIN_THEME_DANGER), 0);
        text_in(so, cv, mkrect(v.x, v.y + ceil_px(2.5f * so->em), v.w, ceil_px(2.5f * so->em)),
                canlin_module_errmsg(sh->mods[k].m), role(sh, CANLIN_THEME_TEXT), 0);
        if (mod_state(sh, k) == CANLIN_MODULE_FAULTED) {
            nk_fill_rect(cv, nkr(so->restart[p]), 0, role(sh, CANLIN_THEME_ACCENT));
            text_in(so, cv, so->restart[p], "Restart", role(sh, CANLIN_THEME_BACKGROUND), 1);
        }
    }
}

static void draw_switcher(canlin_shell *sh, shell_out *so, struct nk_command_buffer *cv)
{
    int i;
    nk_fill_rect(cv, nkr(so->sw), 0, role(sh, CANLIN_THEME_SURFACE));
    for (i = 0; i < so->sw_rows; i++) {
        int k = so->sw_mod[i];
        char line[CANLIN_MODULE_NAME_MAX + 32];
        const canlin_module_source *src = sh->mods[k].src;
        if (i == so->sw_sel)
            nk_fill_rect(cv, nkr(so->sw_row[i]), 0, role(sh, CANLIN_THEME_ACCENT));
        snprintf(line, sizeof line, "%s  %s", src && src->name ? src->name : "module",
                 state_name(mod_state(sh, k)));
        text_in(so, cv, so->sw_row[i], line,
                mod_state(sh, k) == CANLIN_MODULE_FAULTED ? role(sh, CANLIN_THEME_DANGER)
                                                          : role(sh, CANLIN_THEME_TEXT), 0);
    }
}

/* One chrome batch: begin a window over the output, run fn, rasterize, clear. */
static struct nk_command_buffer *chrome_begin(shell_out *so)
{
    nk_begin(&so->nk, "canlin.shell", nk_rect(0, 0, (float)so->w, (float)so->h),
             NK_WINDOW_NO_SCROLLBAR | NK_WINDOW_NO_INPUT);
    return nk_window_get_canvas(&so->nk);
}

static void chrome_end(shell_out *so, canlin_render *r)
{
    nk_end(&so->nk);
    canlin_render_set_atlas(r, so->font.alpha, so->font.w, so->font.h);
    canlin_render_commands(r, &so->nk);
    nk_clear(&so->nk);
}

void canlin_shell_render(canlin_shell *sh, canlin_ui_output *o, canlin_render *r)
{
    shell_out *so = find_out(sh, o);
    struct nk_command_buffer *cv;
    unsigned d;
    int p, drew = 0;
    if (so == NULL || r == NULL)
        return;
    if (metrics_changed(sh, so)) {
        layout(sh, so);
        sync_views(sh);
    }
    d = so->dirty;
    if (d & D_FULL) {
        struct nk_color bg = role(sh, CANLIN_THEME_BACKGROUND);
        canlin_render_clear(r, bg);
        d = ~0u;
    }
    if (!so->font_ok || !so->nk_ok)
        return; /* chrome font could not be baked: nothing trustworthy to show */
    cv = chrome_begin(so);
    if (d & D_BAR)
        draw_bar(sh, so, cv);
    for (p = 0; p < so->vn; p++)
        if (d & (D_PANE0 << p))
            draw_pane(sh, so, cv, p);
    chrome_end(so, r);
    for (p = 0; p < so->vn; p++) {
        int k = so->slot[so->vslot[p]];
        if (!(d & (D_PANE0 << p)) && !view_new_frame(sh, so, p))
            continue;
        so->vmod[p] = k;
        if (k < 0 || mod_state(sh, k) != CANLIN_MODULE_RUNNING)
            continue;
        {
            canlin_ui_view *v = canlin_module_view(sh->mods[k].m);
            if (v == NULL || canlin_ui_view_frames(v) == sh->mods[k].bframe)
                continue;
            so->vframe[p] = canlin_ui_view_frames(v);
            canlin_ui_view_render(v, r);
            drew = 1;
        }
    }
    if (so->sw_open && ((d & (D_OVERLAY | D_BAR)) || drew)) {
        cv = chrome_begin(so);
        draw_switcher(sh, so, cv);
        chrome_end(so, r);
    }
    so->dirty = 0;
}

int64_t canlin_shell_next_tick_ms(canlin_shell *sh)
{
    int64_t t, s;
    if (sh == NULL || sh->clock == NULL)
        return -1;
    t = sh->clock(sh->clock_ud);
    s = t % 60;
    if (s < 0)
        s += 60;
    return (60 - s) * 1000;
}

/* ---- geometry ---- */

int canlin_shell_part_rect(canlin_shell *sh, canlin_ui_output *o, canlin_shell_part part,
                           int idx, canlin_shell_rect *r)
{
    shell_out *so = find_out(sh, o);
    canlin_shell_rect x;
    int k;
    if (so == NULL || r == NULL)
        return 0;
    if (metrics_changed(sh, so)) {
        layout(sh, so);
        sync_views(sh);
    }
    switch (part) {
    case CANLIN_SHELL_P_BAR: x = so->bar; break;
    case CANLIN_SHELL_P_SWITCH_BUTTON: x = so->swbtn; break;
    case CANLIN_SHELL_P_CLOCK: x = so->ind[IND_CLOCK]; if (!so->ind_on[IND_CLOCK]) return 0; break;
    case CANLIN_SHELL_P_NET: x = so->ind[IND_NET]; if (!so->ind_on[IND_NET]) return 0; break;
    case CANLIN_SHELL_P_STORAGE: x = so->ind[IND_STORAGE]; if (!so->ind_on[IND_STORAGE]) return 0; break;
    case CANLIN_SHELL_P_MIC: x = so->ind[IND_MIC]; if (!so->ind_on[IND_MIC]) return 0; break;
    case CANLIN_SHELL_P_FAULTED: x = so->ind[IND_FAULTED]; if (!so->ind_on[IND_FAULTED]) return 0; break;
    case CANLIN_SHELL_P_PANE:
    case CANLIN_SHELL_P_VIEW:
    case CANLIN_SHELL_P_RESTART:
    case CANLIN_SHELL_P_MODEL_LABEL:
        if (idx < 0 || idx >= so->vn)
            return 0;
        k = so->slot[so->vslot[idx]];
        if (part == CANLIN_SHELL_P_PANE)
            x = so->pane[idx];
        else if (part == CANLIN_SHELL_P_VIEW)
            x = so->view[idx];
        else if (part == CANLIN_SHELL_P_MODEL_LABEL) {
            if (so->label[idx].h <= 0)
                return 0;
            x = so->label[idx];
        } else {
            if (k < 0 || mod_state(sh, k) != CANLIN_MODULE_FAULTED)
                return 0;
            x = so->restart[idx];
        }
        break;
    case CANLIN_SHELL_P_DIVIDER:
        if (so->mode != CANLIN_SHELL_TILED || idx < 0 || idx >= so->vn - 1)
            return 0;
        x = so->div[idx];
        break;
    case CANLIN_SHELL_P_SWITCHER:
        if (!so->sw_open)
            return 0;
        x = so->sw;
        break;
    case CANLIN_SHELL_P_SWITCHER_ROW:
        if (!so->sw_open || idx < 0 || idx >= so->sw_rows)
            return 0;
        x = so->sw_row[idx];
        break;
    default:
        return 0;
    }
    *r = x;
    return 1;
}

int canlin_shell_panes(canlin_shell *sh, canlin_ui_output *o)
{
    shell_out *so = find_out(sh, o);
    if (so == NULL)
        return 0;
    if (metrics_changed(sh, so)) {
        layout(sh, so);
        sync_views(sh);
    }
    return so->vn;
}

canlin_module *canlin_shell_pane_module(canlin_shell *sh, canlin_ui_output *o, int idx)
{
    shell_out *so = find_out(sh, o);
    int k;
    if (so == NULL || idx < 0 || idx >= canlin_shell_panes(sh, o))
        return NULL;
    k = so->slot[so->vslot[idx]];
    return k >= 0 ? sh->mods[k].m : NULL;
}

canlin_module *canlin_shell_switcher_module(canlin_shell *sh, canlin_ui_output *o, int idx)
{
    shell_out *so = find_out(sh, o);
    if (so == NULL || !so->sw_open || idx < 0 || idx >= so->sw_rows)
        return NULL;
    return sh->mods[so->sw_mod[idx]].m;
}
