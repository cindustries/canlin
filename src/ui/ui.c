/* CanLin flow UI. See include/canlin/ui.h for the contract.
 *
 * Layout is owned here, not by Nuklear: each widget's rectangle is computed in float px
 * from em, rounded once, recorded, and handed to Nuklear through one NK_STATIC layout
 * space that covers the view (nk_layout_space_push). Nuklear draws the widget and
 * evaluates input inside that rectangle. Text wrapping and ellipsis are computed here with
 * the baked font's width function, so the record states exactly what is drawn.
 *
 * Error discipline inside the Lua methods: every check that can raise (type, limits,
 * UTF-8, identity, budget charge) runs before the first Nuklear call of that widget, so a
 * Lua error never leaves Nuklear in the middle of a widget.
 */
#include "canlin/ui.h"
#include "canlin/sandbox.h"
#include "canlin/sandbox_limits.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "graph.h"

enum { F_BODY = 0, F_H1 = 1, F_H2 = 2, F_COUNT = 3 };

typedef struct font_entry {
    int used;
    float em;                 /* key: em px size of size[F_BODY] */
    unsigned refs;            /* views pinning this entry */
    uint64_t last;            /* LRU tick */
    canlin_render_font font;  /* size[F_BODY], size[F_H1], size[F_H2] */
} font_entry;

struct canlin_ui {
    canlin_theme theme;
    font_entry fonts[CANLIN_UI_FONT_SIZES_MAX];
    uint64_t tick;
    unsigned bakes;
    canlin_ui_view *views;    /* every view bound to this ui, linked through unext */
};

struct canlin_ui_output {
    char id[CANLIN_UI_OUTPUT_ID_MAX];
    int w, h;
    float dpi;
    int scale;
    canlin_ui_view *views;    /* bound views, linked through onext */
};

typedef struct focus_ent {
    uint64_t id;
    unsigned char kind;       /* canlin_ui_kind */
} focus_ent;

typedef struct slot {
    uint64_t id;
    uint32_t frame;          /* frame that last used this identity */
    unsigned char used, init, open, expanded;
} slot;

struct canlin_ui_view {
    canlin_ui *ui;
    canlin_ui_output *out;
    canlin_ui_view *onext;    /* next view on out */
    canlin_ui_view *unext;    /* next view on ui */
    int efont_wait;           /* last frame_begin failed with EFONT: woken on a release */
    font_entry *fe;           /* pinned entry in ui->fonts */
    float factor;             /* DPI x scale of the current binding */
    float em;
    int dirty;
    int no_cmds;              /* the command list is stale (old atlas): render nothing */
    void (*dirty_fn)(void *);
    void *dirty_ud;
    struct nk_context nk;
    int nk_ok;
    int x, y, w, h;

    canlin_ui_input q[CANLIN_UI_INPUT_MAX];
    size_t qhead, qn;
    int overflow;
    unsigned btn_down;        /* bit per canlin_ui_button held */
    int last_x, last_y;
    unsigned char key_down[CANLIN_UI_KEY_LAST + 1];
    unsigned kpress;          /* bit per canlin_ui_key pressed in this frame's input */
    int space_in, press_in;   /* this frame's input: text U+0020, a primary press */

    /* Keyboard focus order (canlin#26): focusable widgets of the open frame and of the
     * last finished one, in draw order. */
    focus_ent order[CANLIN_UI_MAX_WIDGETS], order_prev[CANLIN_UI_MAX_WIDGETS];
    unsigned norder, norder_prev;
    int focus_kbd;            /* focus came from the keyboard: show the mark */
    int focus_pass;           /* Esc: the focused graph no longer takes Tab/Del */
    int tab_used;             /* this frame's Tab moved the focus (not the graph's) */

    float scroll;
    int content_h;

    slot slots[CANLIN_UI_STATE_SLOTS];
    slot tmp[CANLIN_UI_STATE_SLOTS];
    slot labels[CANLIN_UI_STATE_SLOTS]; /* per-frame label ordinals: open = count */
    uint32_t frame;
    int in_frame;
    uint64_t focus;

    /* per-frame layout state, content coordinates in px */
    float cx, cw, cy;
    float ox, oy;             /* layout space origin in screen px */
    unsigned depth, widgets;

    canlin_ui_rect rec[CANLIN_UI_RECORD_MAX];
    size_t nrec;
    int rec_over;

    char buf[CANLIN_UI_MAX_TEXT + 1];      /* edit scratch */
    char lbl[CANLIN_UI_MAX_LABEL + 8];     /* ellipsized label */

    canlin_ui_graph_state gstate[CANLIN_UI_GRAPH_STATES]; /* ui:graph pan/drag state */
    uint64_t btn_t;                        /* timestamp of the last applied button edge */
    uint64_t now;                          /* owner's monotonic time for this frame */
    uint64_t wake_at, wake_next;           /* time-driven redraw of the last / open frame */
    int has_wake, has_wake_next;
};

/* ---- shared ui and font cache ---- */

canlin_ui *canlin_ui_new(const canlin_theme *t)
{
    canlin_ui *ui;
    if (t == NULL)
        return NULL;
    ui = calloc(1, sizeof *ui);
    if (ui == NULL)
        return NULL;
    ui->theme = *t;
    return ui;
}

void canlin_ui_free(canlin_ui *ui)
{
    unsigned i;
    if (ui == NULL)
        return;
    for (i = 0; i < CANLIN_UI_FONT_SIZES_MAX; i++)
        if (ui->fonts[i].used)
            canlin_render_font_free(&ui->fonts[i].font);
    free(ui);
}

void canlin_ui_font_stats(const canlin_ui *ui, unsigned *held, unsigned *bakes)
{
    unsigned i, n = 0;
    for (i = 0; ui && i < CANLIN_UI_FONT_SIZES_MAX; i++)
        n += ui->fonts[i].used != 0;
    if (held)
        *held = n;
    if (bakes)
        *bakes = ui ? ui->bakes : 0;
}

/* Pins the entry for em px, baking it if needed. Evicts the least recently used unpinned
 * entry when the cache is full; never one with refs > 0. */
static font_entry *font_acquire(canlin_ui *ui, float em, int *err)
{
    font_entry *hit = NULL, *victim = NULL;
    float px[F_COUNT];
    unsigned i;
    for (i = 0; i < CANLIN_UI_FONT_SIZES_MAX; i++) {
        font_entry *e = &ui->fonts[i];
        if (e->used && e->em == em) {
            hit = e;
            break;
        }
        if (!e->used) {
            if (victim == NULL || victim->used)
                victim = e;
        } else if (e->refs == 0 && (victim == NULL || (victim->used && e->last < victim->last))) {
            victim = e;
        }
    }
    if (hit == NULL) {
        if (victim == NULL) {
            *err = CANLIN_UI_EFONT;
            return NULL;
        }
        if (victim->used)
            canlin_render_font_free(&victim->font);
        memset(victim, 0, sizeof *victim);
        px[F_BODY] = em;
        px[F_H1] = em * CANLIN_UI_HEADING1_SCALE;
        px[F_H2] = em * CANLIN_UI_HEADING2_SCALE;
        if (canlin_render_font_default_sizes(&victim->font, px, F_COUNT) != 0) {
            memset(victim, 0, sizeof *victim);
            *err = CANLIN_UI_ENOMEM;
            return NULL;
        }
        victim->used = 1;
        victim->em = em;
        ui->bakes++;
        hit = victim;
    }
    hit->refs++;
    hit->last = ++ui->tick;
    return hit;
}

/* A slot became evictable: every view of ui whose last frame was skipped with EFONT is
 * marked dirty and its owner told, so it retries without an external invalidate (canlin#30).
 * Only dirty callbacks run here (they schedule a draw, never a frame), so no frame is
 * started under the caller. */
static void font_wake(canlin_ui *ui)
{
    canlin_ui_view *v;
    for (v = ui->views; v; v = v->unext)
        if (v->efont_wait) {
            v->efont_wait = 0;
            v->dirty = 1;
            if (v->dirty_fn)
                v->dirty_fn(v->dirty_ud);
        }
}

static void font_release(canlin_ui *ui, font_entry *e)
{
    if (e && e->refs) {
        e->refs--; /* stays baked until LRU eviction or canlin_ui_free */
        if (e->refs == 0 && ui)
            font_wake(ui);
    }
}

static void ui_unlink(canlin_ui_view *v)
{
    canlin_ui_view **pp;
    for (pp = &v->ui->views; *pp; pp = &(*pp)->unext)
        if (*pp == v) {
            *pp = v->unext;
            break;
        }
    v->unext = NULL;
}

static void ui_link(canlin_ui_view *v)
{
    v->unext = v->ui->views;
    v->ui->views = v;
}

/* ---- outputs ---- */

int canlin_ui_scale_valid(int p)
{
    return p == 75 || p == 100 || p == 125 || p == 150 || p == 200;
}

static int dpi_valid(float dpi)
{
    return isfinite(dpi) && dpi >= CANLIN_UI_DPI_MIN && dpi <= CANLIN_UI_DPI_MAX;
}

canlin_ui_output *canlin_ui_output_new(const canlin_ui_output_desc *d, int *err)
{
    canlin_ui_output *o;
    size_t idlen;
    int dummy;
    if (err == NULL)
        err = &dummy;
    *err = CANLIN_UI_EINVAL;
    if (d == NULL || d->id == NULL || d->w <= 0 || d->h <= 0 || !dpi_valid(d->dpi) ||
        !canlin_ui_scale_valid(d->scale))
        return NULL;
    idlen = strlen(d->id);
    if (idlen == 0 || idlen >= CANLIN_UI_OUTPUT_ID_MAX)
        return NULL;
    o = calloc(1, sizeof *o);
    if (o == NULL) {
        *err = CANLIN_UI_ENOMEM;
        return NULL;
    }
    memcpy(o->id, d->id, idlen + 1);
    o->w = d->w;
    o->h = d->h;
    o->dpi = d->dpi;
    o->scale = d->scale;
    *err = CANLIN_UI_OK;
    return o;
}

int canlin_ui_output_free(canlin_ui_output *o)
{
    if (o == NULL)
        return CANLIN_UI_OK;
    if (o->views)
        return CANLIN_UI_EBUSY;
    free(o);
    return CANLIN_UI_OK;
}

const char *canlin_ui_output_id(const canlin_ui_output *o)
{
    return o->id;
}

int canlin_ui_output_scale(const canlin_ui_output *o)
{
    return o->scale;
}

float canlin_ui_output_dpi(const canlin_ui_output *o)
{
    return o->dpi;
}

void canlin_ui_output_size(const canlin_ui_output *o, int *w, int *h)
{
    if (w)
        *w = o->w;
    if (h)
        *h = o->h;
}

static float factor_of(const canlin_ui_output *o)
{
    return o->dpi * (float)o->scale / 100.0f;
}

float canlin_ui_output_em(const canlin_ui *ui, const canlin_ui_output *o)
{
    return canlin_theme_em(&ui->theme, factor_of(o));
}

static int output_busy(const canlin_ui_output *o)
{
    const canlin_ui_view *v;
    for (v = o->views; v; v = v->onext)
        if (v->in_frame)
            return 1;
    return 0;
}

/* Every view on o needs a re-layout; tell each owner. */
static void output_changed(canlin_ui_output *o)
{
    canlin_ui_view *v;
    for (v = o->views; v; v = v->onext) {
        v->dirty = 1;
        if (v->dirty_fn)
            v->dirty_fn(v->dirty_ud);
    }
}

int canlin_ui_output_set_scale(canlin_ui_output *o, int percent)
{
    if (o == NULL || !canlin_ui_scale_valid(percent))
        return CANLIN_UI_EINVAL;
    if (output_busy(o))
        return CANLIN_UI_EBUSY;
    if (o->scale != percent) {
        o->scale = percent;
        output_changed(o);
    }
    return CANLIN_UI_OK;
}

int canlin_ui_output_set_dpi(canlin_ui_output *o, float dpi)
{
    if (o == NULL || !dpi_valid(dpi))
        return CANLIN_UI_EINVAL;
    if (output_busy(o))
        return CANLIN_UI_EBUSY;
    if (o->dpi != dpi) {
        o->dpi = dpi;
        output_changed(o);
    }
    return CANLIN_UI_OK;
}

int canlin_ui_output_set_size(canlin_ui_output *o, int w, int h)
{
    if (o == NULL || w <= 0 || h <= 0)
        return CANLIN_UI_EINVAL;
    if (output_busy(o))
        return CANLIN_UI_EBUSY;
    if (o->w != w || o->h != h) {
        o->w = w;
        o->h = h;
        output_changed(o);
    }
    return CANLIN_UI_OK;
}

/* ---- UTF-8 ---- */

int canlin_ui_utf8_valid(const char *str, size_t len)
{
    const unsigned char *s = (const unsigned char *)str;
    size_t i = 0;
    while (i < len) {
        unsigned c = s[i];
        size_t n, k;
        uint32_t cp;
        if (c < 0x80) {
            i++;
            continue;
        }
        if (c >= 0xc2 && c <= 0xdf) {
            n = 2;
            cp = c & 0x1f;
        } else if (c >= 0xe0 && c <= 0xef) {
            n = 3;
            cp = c & 0x0f;
        } else if (c >= 0xf0 && c <= 0xf4) {
            n = 4;
            cp = c & 0x07;
        } else {
            return 0;
        }
        if (len - i < n)
            return 0;
        for (k = 1; k < n; k++) {
            if ((s[i + k] & 0xc0) != 0x80)
                return 0;
            cp = (cp << 6) | (s[i + k] & 0x3f);
        }
        if ((n == 3 && cp < 0x800) || (n == 4 && (cp < 0x10000 || cp > 0x10ffff)) ||
            (cp >= 0xd800 && cp <= 0xdfff))
            return 0;
        i += n;
    }
    return 1;
}

/* Length of the (valid) UTF-8 sequence starting at s[0]. */
static size_t cp_len(const char *s, size_t left)
{
    unsigned c = (unsigned char)s[0];
    size_t n = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
    return n > left ? left : n;
}

/* ---- view lifecycle ---- */

static void apply_style(canlin_ui_view *v)
{
    struct nk_style *s = &v->nk.style;
    canlin_theme_apply_nk(&v->ui->theme, v->factor, &v->nk, &v->fe->font.font->handle);
    /* Geometry is ours: the window adds no padding or spacing of its own. */
    s->window.padding = nk_vec2(0, 0);
    s->window.spacing = nk_vec2(0, 0);
    s->window.group_padding = nk_vec2(0, 0);
    s->window.border = 0;
    /* The view background covers its whole rect; rounding is for widgets, not views. */
    s->window.rounding = 0;
}

static void output_unlink(canlin_ui_view *v)
{
    canlin_ui_view **pp;
    if (v->out == NULL)
        return;
    for (pp = &v->out->views; *pp; pp = &(*pp)->onext)
        if (*pp == v) {
            *pp = v->onext;
            break;
        }
    v->onext = NULL;
    v->out = NULL;
}

static void output_link(canlin_ui_view *v, canlin_ui_output *o)
{
    v->out = o;
    v->onext = o->views;
    o->views = v;
}

canlin_ui_view *canlin_ui_view_new(canlin_ui *ui, canlin_ui_output *o, int *err)
{
    canlin_ui_view *v;
    int dummy;
    if (err == NULL)
        err = &dummy;
    *err = CANLIN_UI_EINVAL;
    if (ui == NULL || o == NULL)
        return NULL;
    v = calloc(1, sizeof *v);
    if (v == NULL) {
        *err = CANLIN_UI_ENOMEM;
        return NULL;
    }
    v->ui = ui;
    v->factor = factor_of(o);
    v->em = canlin_theme_em(&ui->theme, v->factor);
    v->fe = font_acquire(ui, v->em, err);
    if (v->fe == NULL) {
        free(v);
        return NULL;
    }
    if (!nk_init_default(&v->nk, &v->fe->font.font->handle)) {
        font_release(NULL, v->fe);
        free(v);
        *err = CANLIN_UI_ENOMEM;
        return NULL;
    }
    v->nk_ok = 1;
    apply_style(v);
    v->w = v->h = 1;
    v->dirty = 1;
    output_link(v, o);
    ui_link(v);
    *err = CANLIN_UI_OK;
    return v;
}

void canlin_ui_view_free(canlin_ui_view *v)
{
    if (v == NULL)
        return;
    output_unlink(v);
    ui_unlink(v); /* before the release: v itself is not woken */
    if (v->nk_ok)
        nk_free(&v->nk);
    font_release(v->ui, v->fe);
    free(v);
}

int canlin_ui_view_bind(canlin_ui_view *v, canlin_ui *ui, canlin_ui_output *o)
{
    font_entry *fe;
    float factor, em;
    int err = CANLIN_UI_OK;
    if (v == NULL || ui == NULL || o == NULL)
        return CANLIN_UI_EINVAL;
    if (v->in_frame)
        return CANLIN_UI_EBUSY;
    if (ui == v->ui && o == v->out)
        return CANLIN_UI_OK;
    factor = factor_of(o);
    em = canlin_theme_em(&ui->theme, factor);
    fe = font_acquire(ui, em, &err); /* before releasing: failure leaves v unchanged */
    if (fe == NULL)
        return err;
    ui_unlink(v);
    font_release(v->ui, v->fe);
    v->fe = fe;
    v->ui = ui;
    ui_link(v);
    v->efont_wait = 0; /* this binding has its font */
    v->factor = factor;
    v->em = em;
    if (o != v->out) {
        output_unlink(v);
        output_link(v, o);
    }
    /* The last command list points into the old atlas, which the caller may free now. */
    v->no_cmds = 1;
    apply_style(v);
    v->dirty = 1;
    return CANLIN_UI_OK;
}

void canlin_ui_view_set_dirty_fn(canlin_ui_view *v, void (*fn)(void *ud), void *ud)
{
    if (v == NULL)
        return;
    v->dirty_fn = fn;
    v->dirty_ud = ud;
}

int canlin_ui_view_dirty(const canlin_ui_view *v)
{
    return v ? v->dirty : 0;
}

float canlin_ui_view_em(const canlin_ui_view *v)
{
    return v->em;
}

const canlin_render_font *canlin_ui_view_font(const canlin_ui_view *v)
{
    return &v->fe->font;
}

void canlin_ui_view_set_rect(canlin_ui_view *v, int x, int y, int w, int h)
{
    if (v == NULL || w <= 0 || h <= 0 || v->in_frame)
        return;
    v->x = x;
    v->y = y;
    v->w = w;
    v->h = h;
}

void canlin_ui_view_reset(canlin_ui_view *v)
{
    if (v == NULL || v->in_frame)
        return;
    memset(v->slots, 0, sizeof v->slots);
    v->qhead = v->qn = 0;
    v->overflow = v->btn_down != 0;
    memset(v->key_down, 0, sizeof v->key_down);
    v->scroll = 0;
    v->content_h = 0;
    v->focus = 0;
    v->focus_kbd = v->focus_pass = 0;
    v->norder_prev = 0;
    v->nrec = 0;
    v->rec_over = 0;
    memset(v->gstate, 0, sizeof v->gstate);
}

struct nk_context *canlin_ui_view_nk(canlin_ui_view *v)
{
    return &v->nk;
}

/* ---- input ---- */

int canlin_ui_input_valid(const canlin_ui_input *in)
{
    if (in == NULL)
        return 0;
    switch (in->type) {
    case CANLIN_UI_IN_MOTION:
    case CANLIN_UI_IN_SCROLL:
        return 1;
    case CANLIN_UI_IN_BUTTON:
        return in->button >= CANLIN_UI_BUTTON_PRIMARY && in->button <= CANLIN_UI_BUTTON_MIDDLE;
    case CANLIN_UI_IN_KEY:
        return in->key >= CANLIN_UI_KEY_BACKSPACE && in->key <= CANLIN_UI_KEY_LAST;
    case CANLIN_UI_IN_TEXT:
        return !(in->cp > 0x10ffff || (in->cp >= 0xd800 && in->cp <= 0xdfff) || in->cp == 0);
    default:
        return 0;
    }
}

void canlin_ui_view_input_reset(canlin_ui_view *v)
{
    if (v == NULL || v->in_frame)
        return;
    v->qhead = v->qn = 0;
    v->overflow = 1; /* the next frame releases what is held */
    canlin_ui_graph_state_cancel(v->gstate);
}

int canlin_ui_view_input(canlin_ui_view *v, const canlin_ui_input *in)
{
    if (v == NULL || !canlin_ui_input_valid(in))
        return CANLIN_UI_EINVAL;
    if (v->qn == CANLIN_UI_INPUT_MAX) {
        canlin_ui_view_input_reset(v);
        return CANLIN_UI_EFULL;
    }
    v->q[(v->qhead + v->qn) % CANLIN_UI_INPUT_MAX] = *in;
    v->qn++;
    return CANLIN_UI_OK;
}

size_t canlin_ui_view_pending(const canlin_ui_view *v)
{
    return v ? v->qn : 0;
}

static enum nk_keys nk_key_of(int k)
{
    switch (k) {
    case CANLIN_UI_KEY_BACKSPACE: return NK_KEY_BACKSPACE;
    case CANLIN_UI_KEY_DELETE: return NK_KEY_DEL;
    case CANLIN_UI_KEY_ENTER: return NK_KEY_ENTER;
    case CANLIN_UI_KEY_TAB: return NK_KEY_TAB;
    case CANLIN_UI_KEY_LEFT: return NK_KEY_LEFT;
    case CANLIN_UI_KEY_RIGHT: return NK_KEY_RIGHT;
    case CANLIN_UI_KEY_UP: return NK_KEY_UP;
    case CANLIN_UI_KEY_DOWN: return NK_KEY_DOWN;
    case CANLIN_UI_KEY_HOME: return NK_KEY_TEXT_LINE_START;
    case CANLIN_UI_KEY_END: return NK_KEY_TEXT_LINE_END;
    case CANLIN_UI_KEY_SHIFT: return NK_KEY_SHIFT;
    case CANLIN_UI_KEY_CTRL: return NK_KEY_CTRL;
    default: return NK_KEY_NONE;
    }
}

static enum nk_buttons nk_button_of(int b)
{
    return b == CANLIN_UI_BUTTON_SECONDARY ? NK_BUTTON_RIGHT
         : b == CANLIN_UI_BUTTON_MIDDLE    ? NK_BUTTON_MIDDLE
                                           : NK_BUTTON_LEFT;
}

static int utf8_bytes(uint32_t cp)
{
    return cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
}

/* Apply queued input for one frame: motions and scrolls, then at most one edge group
 * (one button edge, one key edge, or a run of text that fits Nuklear's per-frame text
 * buffer). Nuklear evaluates a frame's input by category, not in order, so mixing
 * groups or repeating an edge in one frame would reorder or lose edges. */
static void apply_input(canlin_ui_view *v)
{
    struct nk_context *ctx = &v->nk;
    enum { G_NONE, G_BUTTON, G_KEY, G_TEXT } group = G_NONE;
    int text_bytes = 0, k;

    v->kpress = 0;
    v->space_in = v->press_in = 0;
    nk_input_begin(ctx);
    if (v->overflow) {
        /* Edges were lost: release what is held so no interaction stays stuck. */
        for (k = CANLIN_UI_BUTTON_PRIMARY; k <= CANLIN_UI_BUTTON_MIDDLE; k++)
            if (v->btn_down & (1u << k))
                nk_input_button(ctx, nk_button_of(k), v->last_x, v->last_y, 0);
        for (k = CANLIN_UI_KEY_BACKSPACE; k <= CANLIN_UI_KEY_LAST; k++)
            if (v->key_down[k] && nk_key_of(k) != NK_KEY_NONE)
                nk_input_key(ctx, nk_key_of(k), 0);
        memset(v->key_down, 0, sizeof v->key_down);
        v->btn_down = 0;
        v->overflow = 0;
        group = G_BUTTON; /* the release is this frame's edge */
    }
    while (v->qn) {
        const canlin_ui_input *e = &v->q[v->qhead];
        if (group != G_NONE && group != G_TEXT)
            break; /* one edge per frame; anything later waits (keeps order) */
        if (group == G_TEXT && e->type != CANLIN_UI_IN_TEXT)
            break;
        switch (e->type) {
        case CANLIN_UI_IN_MOTION:
            nk_input_motion(ctx, e->x, e->y);
            v->last_x = e->x;
            v->last_y = e->y;
            break;
        case CANLIN_UI_IN_SCROLL:
            v->scroll += (float)e->dy;
            break;
        case CANLIN_UI_IN_BUTTON:
            nk_input_motion(ctx, e->x, e->y);
            nk_input_button(ctx, nk_button_of(e->button), e->x, e->y, e->down ? 1 : 0);
            v->last_x = e->x;
            v->last_y = e->y;
            if (e->down && e->button == CANLIN_UI_BUTTON_PRIMARY)
                v->press_in = 1;
            if (e->down)
                v->btn_down |= 1u << e->button;
            else
                v->btn_down &= ~(1u << e->button);
            v->btn_t = e->t;
            group = G_BUTTON;
            break;
        case CANLIN_UI_IN_KEY:
            if (nk_key_of(e->key) != NK_KEY_NONE)
                nk_input_key(ctx, nk_key_of(e->key), e->down ? 1 : 0);
            v->key_down[e->key] = e->down != 0;
            if (e->down)
                v->kpress |= 1u << e->key;
            group = G_KEY;
            break;
        case CANLIN_UI_IN_TEXT:
            if (text_bytes + utf8_bytes(e->cp) >= NK_INPUT_MAX)
                goto done; /* Nuklear's per-frame text buffer is full */
            text_bytes += utf8_bytes(e->cp);
            nk_input_unicode(ctx, (nk_rune)e->cp);
            if (e->cp == ' ')
                v->space_in = 1;
            group = G_TEXT;
            break;
        }
        v->qhead = (v->qhead + 1) % CANLIN_UI_INPUT_MAX;
        v->qn--;
    }
done:
    nk_input_end(ctx);
}

/* ---- keyboard focus (canlin#26) ---- */

static int plain_kind(int kind)
{
    return kind == CANLIN_UI_W_BUTTON || kind == CANLIN_UI_W_TOGGLE ||
           kind == CANLIN_UI_W_CHOICE || kind == CANLIN_UI_W_SECTION;
}

/* Kind of the focused widget in the last finished frame, 0 if not there. */
static int focus_kind(const canlin_ui_view *v)
{
    unsigned i;
    for (i = 0; v->focus && i < v->norder_prev; i++)
        if (v->order_prev[i].id == v->focus)
            return v->order_prev[i].kind;
    return 0;
}

/* After this frame's input is applied: pointer press, Esc and Tab / Shift-Tab against the
 * last finished frame's draw order (the open frame's order is not known before Lua ran;
 * immediate-mode views draw the same order frame to frame). */
static void focus_keys(canlin_ui_view *v)
{
    int kind = focus_kind(v);
    unsigned i, n = v->norder_prev, at = n;
    v->tab_used = 0;
    if (v->press_in) {
        v->focus_kbd = v->focus_pass = 0;
        if (plain_kind(kind)) {
            v->focus = 0;
            kind = 0;
        }
    }
    if ((v->kpress & (1u << CANLIN_UI_KEY_ESCAPE)) && kind == CANLIN_UI_W_GRAPH)
        v->focus_pass = 1;
    if (!(v->kpress & (1u << CANLIN_UI_KEY_TAB)) || n == 0 ||
        (kind == CANLIN_UI_W_GRAPH && !v->focus_pass))
        return; /* a focused graph keeps Tab for its nodes */
    for (i = 0; kind && i < n; i++)
        if (v->order_prev[i].id == v->focus)
            at = i;
    if (v->key_down[CANLIN_UI_KEY_SHIFT])
        at = at == n ? n - 1 : (at + n - 1) % n;
    else
        at = at == n ? 0 : (at + 1) % n;
    v->focus = v->order_prev[at].id;
    v->focus_kbd = 1;
    v->focus_pass = 0;
    v->tab_used = 1;
}

/* A focusable widget drawn in this frame, in draw order. */
static void focusable(canlin_ui_view *v, uint64_t id, int kind)
{
    if (v->norder < CANLIN_UI_MAX_WIDGETS) {
        v->order[v->norder].id = id;
        v->order[v->norder].kind = (unsigned char)kind;
        v->norder++;
    }
}

/* Enter or Space activates the focused plain widget id in this frame. */
static int activated(const canlin_ui_view *v, uint64_t id)
{
    return v->focus == id &&
           ((v->kpress & (1u << CANLIN_UI_KEY_ENTER)) || v->space_in);
}

/* ---- identity ---- */

static uint64_t fnv(uint64_t h, const void *p, size_t n)
{
    const unsigned char *s = p;
    size_t i;
    for (i = 0; i < n; i++) {
        h ^= s[i];
        h *= 1099511628211ull;
    }
    return h;
}

#define FNV0 14695981039346656037ull

/* Finds or inserts id. Load stays <= 50% (ui_limits.h), so an empty slot exists. */
static slot *slot_get(slot *tab, uint64_t id)
{
    size_t mask = CANLIN_UI_STATE_SLOTS - 1, i = (size_t)(id ^ (id >> 32)) & mask;
    for (;;) {
        slot *s = &tab[i];
        if (!s->used) {
            memset(s, 0, sizeof *s);
            s->used = 1;
            s->id = id;
            return s;
        }
        if (s->id == id)
            return s;
        i = (i + 1) & mask;
    }
}

/* Drop identities not used in the frame that just ended. */
static void slots_evict(canlin_ui_view *v)
{
    size_t i, n = 0;
    for (i = 0; i < CANLIN_UI_STATE_SLOTS; i++)
        if (v->slots[i].used && v->slots[i].frame == v->frame)
            v->tmp[n++] = v->slots[i];
    memset(v->slots, 0, sizeof v->slots);
    for (i = 0; i < n; i++)
        *slot_get(v->slots, v->tmp[i].id) = v->tmp[i];
}

/* ---- frames ---- */

int canlin_ui_view_frame_begin(canlin_ui_view *v)
{
    struct nk_context *ctx;
    struct nk_rect sb;
    float maxs;
    font_entry *old;

    if (v == NULL || v->ui == NULL || v->out == NULL)
        return CANLIN_UI_EINVAL;
    if (v->in_frame)
        return CANLIN_UI_EBUSY;
    old = NULL;
    {
        /* Metrics are read from the output every frame (spec §2.1). */
        float factor = factor_of(v->out), em = canlin_theme_em(&v->ui->theme, factor);
        if (em != v->fe->em || factor != v->factor) {
            int err = CANLIN_UI_OK;
            font_entry *fe = em == v->fe->em ? v->fe : font_acquire(v->ui, em, &err);
            if (fe == NULL) {
                /* previous frame, record and font stay intact; a later release wakes v */
                v->efont_wait = err == CANLIN_UI_EFONT;
                return err;
            }
            if (fe != v->fe)
                old = v->fe;
            v->fe = fe;
            v->factor = factor;
            v->em = em;
            apply_style(v);
        }
    }
    ctx = &v->nk;
    nk_clear(ctx);
    v->efont_wait = 0;
    font_release(v->ui, old); /* after nk_clear: no command points into its atlas any more */
    v->no_cmds = 0;
    apply_input(v);
    focus_keys(v);
    maxs = (float)(v->content_h - v->h);
    if (maxs < 0)
        maxs = 0;
    if (v->scroll > maxs)
        v->scroll = maxs;
    if (v->scroll < 0)
        v->scroll = 0;
    v->scroll = floorf(v->scroll);

    v->frame++;
    v->in_frame = 1;
    v->has_wake_next = 0;
    v->norder = 0;
    v->widgets = 0;
    v->depth = 0;
    v->nrec = 0;
    v->rec_over = 0;
    memset(v->labels, 0, sizeof v->labels);
    {
        float em = v->em, unit = v->ui->theme.spacing * 0.25f * em;
        float pad = 2.0f * unit;
        v->cx = pad;
        v->cw = (float)v->w - 2.0f * pad;
        if (v->cw < 1.0f)
            v->cw = 1.0f;
        v->cy = pad;
    }
    nk_begin(ctx, "canlin.view", nk_rect((float)v->x, (float)v->y, (float)v->w, (float)v->h),
             NK_WINDOW_NO_SCROLLBAR);
    nk_layout_space_begin(ctx, NK_STATIC, (float)v->h, 1 << 30);
    sb = nk_layout_space_bounds(ctx);
    v->ox = sb.x;
    v->oy = sb.y;
    return 0;
}

void canlin_ui_view_frame_end(canlin_ui_view *v)
{
    struct nk_context *ctx;
    float em, unit;
    if (v == NULL || !v->in_frame)
        return;
    ctx = &v->nk;
    em = v->em;
    unit = v->ui->theme.spacing * 0.25f * em;
    /* cy ends with one inter-widget gap; the view ends with its padding instead. */
    v->content_h = v->widgets ? (int)ceilf(v->cy) : 0;
    if (v->content_h > v->h) {
        /* Scroll position indicator (the view scrolls by input; no scrollbar widget). */
        struct nk_command_buffer *out = nk_window_get_canvas(ctx);
        float th = (float)v->h * (float)v->h / (float)v->content_h;
        float ty = (float)v->y + v->scroll * (float)v->h / (float)v->content_h;
        canlin_theme_rgb m = v->ui->theme.color[CANLIN_THEME_MUTED];
        float bw = unit > 1.0f ? unit : 1.0f;
        nk_fill_rect(out, nk_rect((float)(v->x + v->w) - bw, ty, bw, th), 0,
                     nk_rgb(m.r, m.g, m.b));
    }
    nk_layout_space_end(ctx);
    nk_end(ctx);
    slots_evict(v);
    canlin_ui_graph_state_evict(v->gstate, v->frame);
    v->has_wake = v->has_wake_next;
    v->wake_at = v->wake_next;
    memcpy(v->order_prev, v->order, v->norder * sizeof v->order[0]);
    v->norder_prev = v->norder;
    {
        /* Focus of a field that was not drawn this frame is gone. */
        size_t mask = CANLIN_UI_STATE_SLOTS - 1, i = (size_t)(v->focus ^ (v->focus >> 32)) & mask;
        int found = 0;
        while (v->focus && v->slots[i].used) {
            if (v->slots[i].id == v->focus) {
                found = 1;
                break;
            }
            i = (i + 1) & mask;
        }
        if (!found)
            v->focus = 0;
    }
    v->dirty = 0;
    v->in_frame = 0;
}

void canlin_ui_view_render(canlin_ui_view *v, canlin_render *r)
{
    if (v == NULL || r == NULL || v->in_frame || v->no_cmds)
        return;
    canlin_render_set_atlas(r, v->fe->font.alpha, v->fe->font.w, v->fe->font.h);
    canlin_render_commands(r, &v->nk);
}

void canlin_ui_view_set_now(canlin_ui_view *v, uint64_t now)
{
    if (v && !v->in_frame)
        v->now = now;
}

int canlin_ui_view_wake(const canlin_ui_view *v, uint64_t *at)
{
    if (v == NULL || !v->has_wake)
        return 0;
    if (at)
        *at = v->wake_at;
    return 1;
}

void canlin_ui_graph_wake(canlin_ui_view *v, uint64_t at)
{
    if (!v->has_wake_next || at < v->wake_next)
        v->wake_next = at;
    v->has_wake_next = 1;
}

const canlin_ui_rect *canlin_ui_view_record(const canlin_ui_view *v, size_t *n, int *overflow)
{
    if (n)
        *n = v ? v->nrec : 0;
    if (overflow)
        *overflow = v ? v->rec_over : 0;
    return v ? v->rec : NULL;
}

int canlin_ui_view_content_height(const canlin_ui_view *v)
{
    return v->content_h;
}

int canlin_ui_view_scroll(const canlin_ui_view *v)
{
    return (int)v->scroll;
}

/* ---- geometry ---- */

static float em_of(const canlin_ui_view *v)
{
    return v->em;
}

static float unit_of(const canlin_ui_view *v)
{
    return v->ui->theme.spacing * 0.25f * v->em;
}

static float gap_of(const canlin_ui_view *v)
{
    return 2.0f * unit_of(v);
}

static float touch_of(const canlin_ui_view *v)
{
    return CANLIN_UI_MIN_TOUCH_EM * v->em;
}

static const struct nk_user_font *font_of(const canlin_ui_view *v, int which)
{
    return &v->fe->font.size[which]->handle;
}

static float line_of(const struct nk_user_font *f)
{
    return CANLIN_UI_LINE_EM * f->height;
}

/* Rounds a content-space float rect once to whole screen pixels (edges rounded, so
 * neighbours abut) and records it. Interactive rects never end up below 2.5 em. */
static struct nk_rect place(canlin_ui_view *v, int kind, int part, float fx, float fy,
                            float fw, float fh, int interactive, size_t off, size_t len,
                            int ellipsized)
{
    float sx = (float)v->x + fx, sy = (float)v->y + fy - v->scroll;
    long x0 = lroundf(sx), x1 = lroundf(sx + fw), y0 = lroundf(sy), y1 = lroundf(sy + fh);
    if (interactive) {
        long min = (long)ceilf(touch_of(v) - 1e-3f);
        if (y1 - y0 < min)
            y1 = y0 + min;
    }
    if (x1 <= x0)
        x1 = x0 + 1;
    if (y1 < y0)
        y1 = y0;
    if (v->nrec < CANLIN_UI_RECORD_MAX) {
        canlin_ui_rect *r = &v->rec[v->nrec++];
        r->kind = (unsigned char)kind;
        r->part = (unsigned char)part;
        r->ellipsized = (unsigned char)(ellipsized != 0);
        r->widget = (unsigned short)(v->widgets ? v->widgets - 1 : 0);
        r->x = (int)x0;
        r->y = (int)y0;
        r->w = (int)(x1 - x0);
        r->h = (int)(y1 - y0);
        r->off = (uint32_t)off;
        r->len = (uint32_t)len;
    } else {
        v->rec_over = 1;
    }
    return nk_rect((float)x0, (float)y0, (float)(x1 - x0), (float)(y1 - y0));
}

/* Hand a screen rect to Nuklear as the next widget's bounds. */
static void push(canlin_ui_view *v, struct nk_rect r)
{
    nk_layout_space_push(&v->nk, nk_rect(r.x - v->ox, r.y - v->oy, r.w, r.h));
}

static void advance(canlin_ui_view *v, float h)
{
    v->cy += h + gap_of(v);
}

static struct nk_color col(const canlin_ui_view *v, int role)
{
    canlin_theme_rgb c = v->ui->theme.color[role];
    return nk_rgb(c.r, c.g, c.b);
}

static float text_w(const struct nk_user_font *f, const char *s, size_t len)
{
    return len ? f->width(f->userdata, f->height, s, (int)len) : 0.0f;
}

/* Single-line label: fits s into avail px, else a prefix plus "..." (v->lbl). Returns the
 * shown byte count of s in *shown; out and outlen are what to draw. */
static int ellipsize(canlin_ui_view *v, const struct nk_user_font *f, const char *s,
                     size_t len, float avail, const char **out, size_t *outlen, size_t *shown)
{
    float dots, acc = 0;
    size_t i = 0;
    if (text_w(f, s, len) <= avail) {
        *out = s;
        *outlen = len;
        *shown = len;
        return 0;
    }
    dots = text_w(f, "...", 3);
    while (i < len) {
        size_t n = cp_len(s + i, len - i);
        float cw = text_w(f, s + i, n);
        if (acc + cw + dots > avail)
            break;
        acc += cw;
        i += n;
    }
    memcpy(v->lbl, s, i);
    memcpy(v->lbl + i, "...", 3);
    *out = v->lbl;
    *outlen = i + 3;
    *shown = i;
    return 1;
}

/* Word wrap (spec §2): break at spaces, inside a word only when the word is wider than
 * the line; '\n' forces a break. Calls fn per line with its byte range; returns the line
 * count (at least 1). O(len) width calls. */
typedef void (*line_fn)(canlin_ui_view *v, size_t off, size_t len, size_t idx, void *ud);

static size_t wrap(canlin_ui_view *v, const struct nk_user_font *f, const char *s,
                   size_t len, float width, line_fn fn, void *ud)
{
    size_t start = 0, i = 0, lines = 0, brk = 0;
    int have_brk = 0;
    float w = 0, w_since = 0;

    while (i < len) {
        size_t n;
        float cw;
        if (s[i] == '\n') {
            if (fn)
                fn(v, start, i - start, lines, ud);
            lines++;
            i++;
            start = i;
            w = 0;
            have_brk = 0;
            continue;
        }
        n = cp_len(s + i, len - i);
        cw = text_w(f, s + i, n);
        if (w + cw > width && i > start) {
            if (s[i] == ' ') {
                if (fn)
                    fn(v, start, i - start, lines, ud);
                lines++;
                while (i < len && s[i] == ' ')
                    i++;
                start = i;
                w = 0;
                have_brk = 0;
                continue;
            }
            if (have_brk && brk > start) {
                if (fn)
                    fn(v, start, brk - start, lines, ud);
                lines++;
                start = brk + 1;
                w = w_since;
                have_brk = 0;
                continue; /* re-measure this code point on the new line */
            }
            if (fn)
                fn(v, start, i - start, lines, ud);
            lines++;
            start = i;
            w = 0;
            have_brk = 0;
            continue;
        }
        w += cw;
        if (s[i] == ' ') {
            brk = i;
            have_brk = 1;
            w_since = 0;
        } else if (have_brk) {
            w_since += cw;
        }
        i += n;
    }
    if (start < len || lines == 0 || (len && s[len - 1] == '\n')) {
        if (fn)
            fn(v, start, len - start, lines, ud);
        lines++;
    }
    return lines;
}

/* ---- the Lua object ---- */

typedef struct text_ctx {
    const char *s;
    const struct nk_user_font *f;
    struct nk_color c;
    int kind;
    float x, y, w, lh;
} text_ctx;

static void emit_line(canlin_ui_view *v, size_t off, size_t len, size_t idx, void *ud)
{
    text_ctx *t = ud;
    struct nk_rect r = place(v, t->kind, CANLIN_UI_P_LINE, t->x, t->y + (float)idx * t->lh,
                             t->w, t->lh, 0, off, len, 0);
    push(v, r);
    nk_text_colored(&v->nk, t->s + off, (int)len, NK_TEXT_LEFT, t->c);
}

static canlin_ui_view *check_ui(lua_State *L)
{
    canlin_ui_view *v = lua_touserdata(L, lua_upvalueindex(1));
    if (!lua_rawequal(L, 1, lua_upvalueindex(3)))
        luaL_error(L, "ui: call widget methods as ui:name(...)");
    if (v == NULL)
        luaL_error(L, "ui: no view attached to this module (headless)");
    if (!v->in_frame || (lua_Integer)v->frame != lua_tointeger(L, lua_upvalueindex(2)))
        luaL_error(L, "ui: object used outside the draw it was passed to");
    return v;
}

/* Counts one widget against CANLIN_UI_MAX_WIDGETS and charges its base cost. */
static void count_widget(lua_State *L, canlin_ui_view *v)
{
    if (v->widgets >= CANLIN_UI_MAX_WIDGETS)
        luaL_error(L, "ui: more than %d widgets in one draw", (int)CANLIN_UI_MAX_WIDGETS);
    canlin_sandbox_charge(L, CANLIN_SB_COST_ELEM);
    v->widgets++;
}

/* String argument: type, byte limit, charge, UTF-8. */
static const char *get_str(lua_State *L, int idx, size_t max, const char *what, size_t *len)
{
    const char *s;
    luaL_checktype(L, idx, LUA_TSTRING);
    s = lua_tolstring(L, idx, len);
    if (*len > max)
        luaL_error(L, "ui: %s exceeds %d bytes", what, (int)max);
    canlin_sandbox_charge(L, *len / CANLIN_SB_COST_BYTES_PER_UNIT + 1u);
    if (!canlin_ui_utf8_valid(s, *len))
        luaL_error(L, "ui: %s is not valid UTF-8", what);
    return s;
}

/* opts table at idx: nil or table. */
static int has_opts(lua_State *L, int idx)
{
    int t = lua_type(L, idx);
    if (t != LUA_TNIL && t != LUA_TNONE && t != LUA_TTABLE)
        luaL_typeerror(L, idx, "nil or table");
    return t == LUA_TTABLE;
}

/* Pushes opts[key] (nil if no opts) and returns its type. */
static int opt_field(lua_State *L, int idx, const char *key)
{
    if (!has_opts(L, idx)) {
        lua_pushnil(L);
        return LUA_TNIL;
    }
    return lua_getfield(L, idx, key);
}

/* Identity (spec §3): opts.id, else label plus its ordinal among equal labels in this
 * draw. Duplicate identities in one draw raise. Returns the state slot. */
static slot *identity(lua_State *L, canlin_ui_view *v, int optidx, const char *label,
                      size_t llen)
{
    uint64_t id;
    slot *s;
    int t = opt_field(L, optidx, "id");
    if (t != LUA_TNIL) {
        size_t n;
        const char *is = get_str(L, lua_gettop(L), CANLIN_UI_MAX_LABEL, "opts.id", &n);
        id = fnv(fnv(FNV0, "i", 1), is, n);
    } else {
        uint64_t lh = fnv(fnv(FNV0, "l", 1), label, llen);
        slot *ls = slot_get(v->labels, lh);
        uint32_t ord = ls->frame++; /* labels table is per frame: frame = count */
        id = fnv(lh, &ord, sizeof ord);
    }
    lua_pop(L, 1);
    if (id == 0)
        id = 1; /* 0 means "no focus" */
    s = slot_get(v->slots, id);
    if (s->frame == v->frame)
        luaL_error(L, "ui: two stateful widgets with identity '%s' in one draw",
                   t != LUA_TNIL ? "opts.id" : label);
    s->frame = v->frame;
    return s;
}

static float label_avail_button(const canlin_ui_view *v, float w)
{
    const struct nk_style_button *b = &v->nk.style.button;
    return w - 2.0f * (b->padding.x + b->border + b->rounding);
}

/* ui:heading(text [, level]) */
static int l_heading(lua_State *L)
{
    canlin_ui_view *v = check_ui(L);
    size_t len;
    lua_Integer level;
    const char *s;
    text_ctx t;
    size_t n;
    count_widget(L, v);
    s = get_str(L, 2, CANLIN_UI_MAX_TEXT, "heading text", &len);
    level = luaL_optinteger(L, 3, 1);
    if (level < 1 || level > 3)
        luaL_error(L, "ui:heading: level must be 1-3");
    t.f = font_of(v, level == 1 ? F_H1 : level == 2 ? F_H2 : F_BODY);
    t.s = s;
    t.c = col(v, CANLIN_THEME_TEXT);
    t.kind = CANLIN_UI_W_HEADING;
    t.x = v->cx;
    t.y = v->cy;
    t.w = v->cw;
    t.lh = line_of(t.f);
    n = wrap(v, t.f, s, len, v->cw, NULL, NULL);
    canlin_sandbox_charge(L, n * CANLIN_SB_COST_ELEM);
    nk_style_set_font(&v->nk, t.f);
    wrap(v, t.f, s, len, v->cw, emit_line, &t);
    nk_style_set_font(&v->nk, font_of(v, F_BODY));
    advance(v, (float)n * t.lh);
    return 0;
}

/* ui:text(text [, opts]) */
static int l_text(lua_State *L)
{
    static const char *const styles[] = {"normal", "muted", "danger", "success", NULL};
    static const int roles[] = {CANLIN_THEME_TEXT, CANLIN_THEME_MUTED, CANLIN_THEME_DANGER,
                                CANLIN_THEME_SUCCESS};
    canlin_ui_view *v = check_ui(L);
    size_t len, n;
    const char *s;
    int style = 0;
    text_ctx t;
    count_widget(L, v);
    s = get_str(L, 2, CANLIN_UI_MAX_TEXT, "text", &len);
    if (opt_field(L, 3, "style") != LUA_TNIL)
        style = luaL_checkoption(L, -1, NULL, styles);
    lua_pop(L, 1);
    t.f = font_of(v, F_BODY);
    t.s = s;
    t.c = col(v, roles[style]);
    t.kind = CANLIN_UI_W_TEXT;
    t.x = v->cx;
    t.y = v->cy;
    t.w = v->cw;
    t.lh = line_of(t.f);
    n = wrap(v, t.f, s, len, v->cw, NULL, NULL);
    canlin_sandbox_charge(L, n * CANLIN_SB_COST_ELEM);
    wrap(v, t.f, s, len, v->cw, emit_line, &t);
    advance(v, (float)n * t.lh);
    return 0;
}

/* ui:bubble(role, text) */
static int l_bubble(lua_State *L)
{
    static const char *const rolenames[] = {"user", "assistant", "system", NULL};
    canlin_ui_view *v = check_ui(L);
    size_t len, n;
    const char *s;
    int role;
    text_ctx t;
    float em, bpad, bw, bh, bx;
    struct nk_rect box;
    count_widget(L, v);
    role = luaL_checkoption(L, 2, NULL, rolenames);
    s = get_str(L, 3, CANLIN_UI_MAX_TEXT, "bubble text", &len);
    em = em_of(v);
    bpad = 0.5f * em;
    bw = 0.85f * v->cw;
    t.f = font_of(v, F_BODY);
    t.lh = line_of(t.f);
    n = wrap(v, t.f, s, len, bw - 2.0f * bpad, NULL, NULL);
    canlin_sandbox_charge(L, n * CANLIN_SB_COST_ELEM);
    bh = (float)n * t.lh + 2.0f * bpad;
    bx = role == 0 ? v->cx + v->cw - bw : v->cx;
    box = place(v, CANLIN_UI_W_BUBBLE, CANLIN_UI_P_BOX, bx, v->cy, bw, bh, 0, 0, len, 0);
    {
        canlin_theme_rgb sf = v->ui->theme.color[CANLIN_THEME_SURFACE];
        canlin_theme_rgb ac = v->ui->theme.color[CANLIN_THEME_ACCENT];
        struct nk_color bg = role == 0 ? nk_rgb((sf.r * 3 + ac.r) / 4, (sf.g * 3 + ac.g) / 4,
                                                (sf.b * 3 + ac.b) / 4)
                                       : nk_rgb(sf.r, sf.g, sf.b);
        nk_fill_rect(nk_window_get_canvas(&v->nk), box, v->ui->theme.radius * em, bg);
    }
    t.s = s;
    t.c = col(v, role == 2 ? CANLIN_THEME_MUTED : CANLIN_THEME_TEXT);
    t.kind = CANLIN_UI_W_BUBBLE;
    t.x = bx + bpad;
    t.y = v->cy + bpad;
    t.w = bw - 2.0f * bpad;
    wrap(v, t.f, s, len, t.w, emit_line, &t);
    advance(v, bh);
    return 0;
}

/* Appends a record entry for a screen rect (already whole pixels). */
static void rec_screen(canlin_ui_view *v, int kind, int part, struct nk_rect r)
{
    canlin_ui_rect *d;
    if (v->nrec >= CANLIN_UI_RECORD_MAX) {
        v->rec_over = 1;
        return;
    }
    d = &v->rec[v->nrec++];
    memset(d, 0, sizeof *d);
    d->kind = (unsigned char)kind;
    d->part = (unsigned char)part;
    d->widget = (unsigned short)(v->widgets ? v->widgets - 1 : 0);
    d->x = (int)r.x;
    d->y = (int)r.y;
    d->w = (int)r.w;
    d->h = (int)r.h;
}

/* Keyboard focus mark (canlin#26): an accent stroke inside the widget's box, shown while
 * the focus came from the keyboard. */
static void focus_mark(canlin_ui_view *v, int kind, struct nk_rect r, uint64_t id)
{
    float th, rad;
    canlin_theme_rgb c;
    if (v->focus != id || !v->focus_kbd)
        return;
    th = floorf(v->em / 8.0f) < 1.0f ? 1.0f : floorf(v->em / 8.0f);
    rad = v->ui->theme.radius * v->em;
    c = v->ui->theme.color[CANLIN_THEME_ACCENT];
    nk_stroke_rect(nk_window_get_canvas(&v->nk), r, rad, th, nk_rgb(c.r, c.g, c.b));
    rec_screen(v, kind, CANLIN_UI_P_FOCUS, r);
}

/* One single-line label row above a widget; returns its height. */
static float label_row(canlin_ui_view *v, int kind, const char *s, size_t len)
{
    const struct nk_user_font *f = font_of(v, F_BODY);
    const char *out;
    size_t outlen, shown;
    float lh = line_of(f);
    int el = ellipsize(v, f, s, len, v->cw, &out, &outlen, &shown);
    struct nk_rect r = place(v, kind, CANLIN_UI_P_LABEL, v->cx, v->cy, v->cw, lh, 0, 0,
                             shown, el);
    push(v, r);
    nk_text_colored(&v->nk, out, (int)outlen, NK_TEXT_LEFT, col(v, CANLIN_THEME_TEXT));
    return lh;
}

/* Keeps Nuklear's single edit focus on the widget with identity `id` (Nuklear tracks it
 * by draw ordinal, which shifts when widgets are inserted above). */
static void edit_focus_sync(canlin_ui_view *v, uint64_t id, size_t len)
{
    struct nk_window *win = v->nk.current;
    int nk_has = win->edit.active && win->edit.name == win->edit.seq;
    if (v->focus == id && !nk_has) {
        nk_edit_focus(&v->nk, 0);
        win->edit.cursor = (int)len; /* byte length >= glyph count: clamp below */
        win->edit.sel_start = win->edit.sel_end = win->edit.cursor;
    } else if (v->focus != id && nk_has) {
        nk_edit_unfocus(&v->nk);
    }
    if (win->edit.active && win->edit.name == win->edit.seq) {
        int glyphs = nk_utf_len(v->buf, (int)len);
        if (win->edit.cursor > glyphs)
            win->edit.cursor = glyphs;
        if (win->edit.sel_start > glyphs)
            win->edit.sel_start = glyphs;
        if (win->edit.sel_end > glyphs)
            win->edit.sel_end = glyphs;
    }
}

static int do_edit(lua_State *L, int multiline)
{
    canlin_ui_view *v = check_ui(L);
    size_t llen, vlen, plen = 0, max = CANLIN_UI_MAX_TEXT;
    const char *label, *value, *ph = NULL;
    lua_Integer rows = 4;
    slot *st;
    float lh, h;
    struct nk_rect box;
    int len, kind = multiline ? CANLIN_UI_W_AREA : CANLIN_UI_W_FIELD;
    nk_flags fl;
    count_widget(L, v);
    label = get_str(L, 2, CANLIN_UI_MAX_LABEL, "label", &llen);
    value = get_str(L, 3, CANLIN_UI_MAX_TEXT, "value", &vlen);
    has_opts(L, 4);
    if (!multiline) {
        if (opt_field(L, 4, "placeholder") != LUA_TNIL)
            ph = get_str(L, lua_gettop(L), CANLIN_UI_MAX_LABEL, "opts.placeholder", &plen);
        lua_pop(L, 1);
        if (opt_field(L, 4, "max") != LUA_TNIL) {
            lua_Integer m = luaL_checkinteger(L, -1);
            if (m < 0 || m > (lua_Integer)CANLIN_UI_MAX_TEXT)
                luaL_error(L, "ui:field: opts.max must be 0-%d", (int)CANLIN_UI_MAX_TEXT);
            max = (size_t)m;
        }
        lua_pop(L, 1);
        if (vlen > max)
            luaL_error(L, "ui:field: value exceeds opts.max");
    } else {
        if (opt_field(L, 4, "rows") != LUA_TNIL) {
            rows = luaL_checkinteger(L, -1);
            if (rows < 2 || rows > 20)
                luaL_error(L, "ui:area: opts.rows must be 2-20");
        }
        lua_pop(L, 1);
    }
    st = identity(L, v, 4, label, llen);
    focusable(v, st->id, kind);

    lh = label_row(v, kind, label, llen);
    v->cy += lh;
    h = multiline ? (float)rows * line_of(font_of(v, F_BODY)) +
                        2.0f * v->nk.style.edit.padding.y
                  : touch_of(v);
    if (h < touch_of(v))
        h = touch_of(v);
    box = place(v, kind, CANLIN_UI_P_BOX, v->cx, v->cy, v->cw, h, 1, 0, vlen, 0);
    memcpy(v->buf, value, vlen);
    len = (int)vlen;
    edit_focus_sync(v, st->id, vlen);
    push(v, box);
    fl = nk_edit_string(&v->nk, multiline ? NK_EDIT_BOX : NK_EDIT_FIELD, v->buf, &len,
                        (int)max + 1, nk_filter_default);
    if (fl & NK_EDIT_ACTIVE)
        v->focus = st->id;
    else if (v->focus == st->id)
        v->focus = 0;
    focus_mark(v, kind, box, st->id);
    if (ph && len == 0 && !(fl & NK_EDIT_ACTIVE)) {
        const struct nk_style_edit *e = &v->nk.style.edit;
        const struct nk_user_font *f = font_of(v, F_BODY);
        struct nk_rect pr = nk_rect(box.x + e->padding.x + e->border, box.y, box.w, box.h);
        const char *out;
        size_t outlen, shown;
        ellipsize(v, f, ph, plen, box.w - 2.0f * (e->padding.x + e->border), &out, &outlen,
                  &shown);
        pr.y = box.y + (box.h - f->height) / 2.0f;
        pr.h = f->height;
        nk_draw_text(nk_window_get_canvas(&v->nk), pr, out, (int)outlen, f,
                     nk_rgba(0, 0, 0, 0), col(v, CANLIN_THEME_MUTED));
    }
    advance(v, h);
    if (len < 0 || (size_t)len > max || !canlin_ui_utf8_valid(v->buf, (size_t)len))
        lua_pushvalue(L, 3); /* cannot happen with nk_filter_default; keep the old value */
    else
        lua_pushlstring(L, v->buf, (size_t)len);
    return 1;
}

static int l_field(lua_State *L)
{
    return do_edit(L, 0);
}

static int l_area(lua_State *L)
{
    return do_edit(L, 1);
}

/* ui:button(label [, opts]) */
static int l_button(lua_State *L)
{
    canlin_ui_view *v = check_ui(L);
    size_t llen, outlen, shown;
    const char *label, *out;
    struct nk_rect r;
    int el, hit;
    slot *st;
    count_widget(L, v);
    label = get_str(L, 2, CANLIN_UI_MAX_LABEL, "label", &llen);
    st = identity(L, v, 3, label, llen);
    focusable(v, st->id, CANLIN_UI_W_BUTTON);
    el = ellipsize(v, font_of(v, F_BODY), label, llen, label_avail_button(v, v->cw), &out,
                   &outlen, &shown);
    r = place(v, CANLIN_UI_W_BUTTON, CANLIN_UI_P_BOX, v->cx, v->cy, v->cw, touch_of(v), 1, 0,
              shown, el);
    push(v, r);
    hit = nk_button_text(&v->nk, out, (int)outlen) || activated(v, st->id);
    focus_mark(v, CANLIN_UI_W_BUTTON, r, st->id);
    advance(v, touch_of(v));
    lua_pushboolean(L, hit != 0);
    return 1;
}

/* ui:toggle(label, on) */
static int l_toggle(lua_State *L)
{
    canlin_ui_view *v = check_ui(L);
    size_t llen, outlen, shown;
    const char *label, *out;
    struct nk_rect r;
    int on, el;
    slot *st;
    count_widget(L, v);
    label = get_str(L, 2, CANLIN_UI_MAX_LABEL, "label", &llen);
    luaL_checktype(L, 3, LUA_TBOOLEAN);
    on = lua_toboolean(L, 3);
    st = identity(L, v, 4, label, llen);
    focusable(v, st->id, CANLIN_UI_W_TOGGLE);
    {
        /* Box and gap derive from em and the spacing unit, so they scale together with
         * the label; the whole row (box + gap + label) is the hit target. */
        struct nk_command_buffer *cv = nk_window_get_canvas(&v->nk);
        struct nk_style_toggle saved = v->nk.style.checkbox, *cs = &v->nk.style.checkbox;
        struct nk_style_item none = nk_style_item_color(nk_rgba(0, 0, 0, 0));
        const struct nk_user_font *f = font_of(v, F_BODY);
        float em = em_of(v), bs = roundf(1.25f * em), gap = gap_of(v);
        float th = floorf(em / 8.0f) < 1.0f ? 1.0f : floorf(em / 8.0f);
        float rad = v->ui->theme.radius * em, inset = th + floorf(unit_of(v) / 2.0f);
        struct nk_rect box, cur;
        el = ellipsize(v, f, label, llen, v->cw - bs - gap, &out, &outlen, &shown);
        r = place(v, CANLIN_UI_W_TOGGLE, CANLIN_UI_P_BOX, v->cx, v->cy, v->cw, touch_of(v), 1,
                  0, shown, el);
        push(v, r);
        /* Nuklear's check box behaviour on r, drawn with transparent colours (Nuklear emits
         * no command for alpha 0); its selector is fixed to the font height, so the
         * visible box is drawn here. No Lua runs between the swap and the restore. */
        cs->normal = cs->hover = cs->active = none;
        cs->cursor_normal = cs->cursor_hover = none;
        cs->border_color = cs->text_normal = cs->text_hover = cs->text_active = none.data.color;
        on = nk_check_text(&v->nk, "", 0, on ? nk_true : nk_false);
        *cs = saved;
        if (activated(v, st->id))
            on = !on;
        box = nk_rect(r.x, r.y + floorf((r.h - bs) / 2.0f), bs, bs);
        if (rad > bs / 4.0f)
            rad = floorf(bs / 4.0f);
        nk_fill_rect(cv, box, rad,
                     nk_input_is_mouse_hovering_rect(&v->nk.input, r) ? saved.hover.data.color
                                                                       : saved.normal.data.color);
        nk_stroke_rect(cv, box, rad, th, col(v, CANLIN_THEME_MUTED));
        if (on) {
            cur = nk_rect(box.x + inset, box.y + inset, bs - 2.0f * inset, bs - 2.0f * inset);
            nk_fill_rect(cv, cur, rad > inset ? rad - inset : 0, col(v, CANLIN_THEME_ACCENT));
        }
        nk_draw_text(cv, nk_rect(box.x + bs + gap, r.y + floorf((r.h - f->height) / 2.0f),
                                 r.w - bs - gap, f->height),
                     out, (int)outlen, f, nk_rgba(0, 0, 0, 0), col(v, CANLIN_THEME_TEXT));
        focus_mark(v, CANLIN_UI_W_TOGGLE, r, st->id);
    }
    advance(v, touch_of(v));
    lua_pushboolean(L, on != 0);
    return 1;
}

/* ui:choice(label, options, index): header shows the current option; activating it
 * expands the options inline below (state per identity); picking one returns its index
 * and collapses. Inline instead of a Nuklear popup: identity-keyed, touch-sized rows. */
static int l_choice(lua_State *L)
{
    canlin_ui_view *v = check_ui(L);
    size_t llen, n, i, outlen, shown;
    const char *label, *out;
    const char *opt[CANLIN_UI_MAX_OPTIONS];
    size_t optlen[CANLIN_UI_MAX_OPTIONS];
    lua_Integer idx, newidx;
    slot *st;
    struct nk_rect r;
    int el;
    float touch;
    count_widget(L, v);
    label = get_str(L, 2, CANLIN_UI_MAX_LABEL, "label", &llen);
    luaL_checktype(L, 3, LUA_TTABLE);
    n = (size_t)lua_rawlen(L, 3);
    if (n < 1 || n > CANLIN_UI_MAX_OPTIONS)
        luaL_error(L, "ui:choice: 1-%d options required", (int)CANLIN_UI_MAX_OPTIONS);
    canlin_sandbox_charge(L, n * CANLIN_SB_COST_ELEM);
    idx = luaL_checkinteger(L, 4);
    if (idx < 1 || (size_t)idx > n)
        luaL_error(L, "ui:choice: index out of range");
    for (i = 0; i < n; i++) {
        lua_rawgeti(L, 3, (lua_Integer)i + 1);
        opt[i] = get_str(L, lua_gettop(L), CANLIN_UI_MAX_LABEL, "option", &optlen[i]);
        lua_pop(L, 1); /* strings stay alive: the options table holds them */
    }
    st = identity(L, v, 5, label, llen);
    focusable(v, st->id, CANLIN_UI_W_CHOICE);
    touch = touch_of(v);
    newidx = idx;

    v->cy += label_row(v, CANLIN_UI_W_CHOICE, label, llen);
    el = ellipsize(v, font_of(v, F_BODY), opt[idx - 1], optlen[idx - 1],
                   label_avail_button(v, v->cw) - em_of(v), &out, &outlen, &shown);
    r = place(v, CANLIN_UI_W_CHOICE, CANLIN_UI_P_BOX, v->cx, v->cy, v->cw, touch, 1, 0,
              shown, el);
    push(v, r);
    if (nk_button_symbol_text(&v->nk,
                              st->expanded ? NK_SYMBOL_TRIANGLE_UP : NK_SYMBOL_TRIANGLE_DOWN,
                              out, (int)outlen, NK_TEXT_LEFT) ||
        activated(v, st->id))
        st->expanded = !st->expanded;
    if (v->focus == st->id) {
        if ((v->kpress & (1u << CANLIN_UI_KEY_UP)) && newidx > 1)
            newidx--;
        if ((v->kpress & (1u << CANLIN_UI_KEY_DOWN)) && (size_t)newidx < n)
            newidx++;
    }
    focus_mark(v, CANLIN_UI_W_CHOICE, r, st->id);
    v->cy += touch;
    if (st->expanded) {
        for (i = 0; i < n; i++) {
            nk_bool sel = (lua_Integer)i + 1 == idx;
            el = ellipsize(v, font_of(v, F_BODY), opt[i], optlen[i], v->cw - em_of(v), &out,
                           &outlen, &shown);
            r = place(v, CANLIN_UI_W_CHOICE, CANLIN_UI_P_OPTION, v->cx, v->cy, v->cw, touch,
                      1, 0, shown, el);
            push(v, r);
            if (nk_selectable_text(&v->nk, out, (int)outlen, NK_TEXT_LEFT, &sel)) {
                newidx = (lua_Integer)i + 1;
                st->expanded = 0;
            }
            v->cy += touch;
        }
    }
    v->cy += gap_of(v);
    lua_pushinteger(L, newidx);
    return 1;
}

/* ui:meter(label, value) */
static int l_meter(lua_State *L)
{
    canlin_ui_view *v = check_ui(L);
    size_t llen;
    const char *label;
    lua_Number x;
    nk_size cur;
    struct nk_rect r;
    count_widget(L, v);
    label = get_str(L, 2, CANLIN_UI_MAX_LABEL, "label", &llen);
    x = luaL_checknumber(L, 3);
    if (!(x > 0))
        x = 0; /* also NaN */
    if (x > 1)
        x = 1;
    v->cy += label_row(v, CANLIN_UI_W_METER, label, llen);
    r = place(v, CANLIN_UI_W_METER, CANLIN_UI_P_BOX, v->cx, v->cy, v->cw, em_of(v), 0, 0, 0, 0);
    push(v, r);
    cur = (nk_size)(x * 1000.0 + 0.5);
    nk_progress(&v->nk, &cur, 1000, nk_false);
    advance(v, em_of(v));
    return 0;
}

/* ui:separator() */
static int l_separator(lua_State *L)
{
    canlin_ui_view *v = check_ui(L);
    float th;
    struct nk_rect r;
    count_widget(L, v);
    th = floorf(v->factor + 0.5f);
    if (th < 1)
        th = 1;
    r = place(v, CANLIN_UI_W_SEPARATOR, CANLIN_UI_P_BOX, v->cx, v->cy, v->cw, th, 0, 0, 0, 0);
    nk_fill_rect(nk_window_get_canvas(&v->nk), r, 0, col(v, CANLIN_THEME_MUTED));
    advance(v, th);
    return 0;
}

static int l_icon(lua_State *L)
{
    check_ui(L);
    return luaL_error(L, "ui:icon: unsupported (no image decoding yet)");
}

/* The one place graph.c gets display metrics (the view's per-output em) and view
 * geometry from. */
static void graph_env(canlin_ui_view *v, canlin_ui_graph_env *e)
{
    e->view = v;
    e->nk = &v->nk;
    e->theme = &v->ui->theme;
    e->font = font_of(v, F_BODY);
    e->em = em_of(v);
    e->vis = nk_rect((float)v->x, (float)v->y, (float)v->w, (float)v->h);
    e->focus = &v->focus;
    e->btn_t = v->btn_t;
    e->now = v->now;
    e->keys = !v->focus_pass && !v->tab_used; /* Esc, or this Tab moved the focus in */
}

void canlin_ui_graph_record(canlin_ui_view *v, int part, struct nk_rect r, uint32_t off,
                            uint32_t len)
{
    canlin_ui_rect *d;
    if (v->nrec >= CANLIN_UI_RECORD_MAX) {
        v->rec_over = 1;
        return;
    }
    d = &v->rec[v->nrec++];
    memset(d, 0, sizeof *d);
    d->kind = CANLIN_UI_W_GRAPH;
    d->part = (unsigned char)part;
    d->widget = (unsigned short)(v->widgets ? v->widgets - 1 : 0);
    d->x = (int)r.x;
    d->y = (int)r.y;
    d->w = (int)r.w;
    d->h = (int)r.h;
    d->off = off;
    d->len = len;
}

/* ui:graph(g [, opts]) — spec §5; validation, state and drawing live in graph.c. */
static int l_graph(lua_State *L)
{
    canlin_ui_view *v = check_ui(L);
    canlin_ui_graph *g;
    canlin_ui_graph_env e;
    slot *st;
    float hem;
    int fit;
    count_widget(L, v);
    luaL_checktype(L, 2, LUA_TTABLE);
    /* identity() may run opts metamethods; the parse below reads raw and runs no Lua, so
     * the parsed string pointers stay anchored in g until the edits are pushed. */
    st = identity(L, v, 3, "\0graph", 6); /* own label namespace in practice */
    focusable(v, st->id, CANLIN_UI_W_GRAPH);
    graph_env(v, &e);
    e.id = st->id;
    e.st = canlin_ui_graph_state_get(v->gstate, st->id, v->frame);
    if (e.st == NULL)
        luaL_error(L, "ui:graph: more than %d graphs in one draw",
                   (int)CANLIN_UI_GRAPH_MAX_PER_DRAW);
    g = canlin_ui_graph_parse(L, 2, 3, &hem, &fit); /* raises; userdata stays on stack */
    e.box = place(v, CANLIN_UI_W_GRAPH, CANLIN_UI_P_BOX, v->cx, v->cy, v->cw, hem * e.em, 1,
                  0, 0, 0);
    canlin_ui_graph_run(L, g, fit, &e);
    focus_mark(v, CANLIN_UI_W_GRAPH, e.box, st->id); /* after run: a press may move it */
    advance(v, hem * e.em);
    return 1;
}

/* Runs the function at idx; restores the column state and re-raises on error. */
static void run_fn(lua_State *L, canlin_ui_view *v, int idx, float cx, float cw, unsigned depth)
{
    lua_pushvalue(L, idx);
    if (canlin_sandbox_pcall(L, 0, 0) != LUA_OK) {
        v->cx = cx;
        v->cw = cw;
        v->depth = depth;
        lua_error(L);
    }
}

static void enter(lua_State *L, canlin_ui_view *v, const char *what)
{
    if (v->depth >= CANLIN_UI_MAX_DEPTH)
        luaL_error(L, "ui:%s: nesting deeper than %d", what, (int)CANLIN_UI_MAX_DEPTH);
}

/* ui:columns(left_fn, right_fn) */
static int l_columns(lua_State *L)
{
    canlin_ui_view *v = check_ui(L);
    float cx, cw, y0, yl, colgap;
    unsigned depth;
    count_widget(L, v);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    luaL_checktype(L, 3, LUA_TFUNCTION);
    enter(L, v, "columns");
    cx = v->cx;
    cw = v->cw;
    depth = v->depth;
    y0 = v->cy;
    v->depth++;
    if (cw >= CANLIN_UI_COLUMNS_EM * em_of(v)) {
        colgap = 2.0f * gap_of(v);
        v->cw = (cw - colgap) / 2.0f;
        run_fn(L, v, 2, cx, cw, depth);
        yl = v->cy;
        v->cy = y0;
        v->cx = cx + v->cw + colgap;
        run_fn(L, v, 3, cx, cw, depth);
        if (yl > v->cy)
            v->cy = yl;
        place(v, CANLIN_UI_W_COLUMNS, CANLIN_UI_P_BOX, cx, y0, cw, v->cy - y0, 0, 0, 2, 0);
    } else {
        run_fn(L, v, 2, cx, cw, depth);
        run_fn(L, v, 3, cx, cw, depth);
        place(v, CANLIN_UI_W_COLUMNS, CANLIN_UI_P_BOX, cx, y0, cw, v->cy - y0, 0, 0, 1, 0);
    }
    v->cx = cx;
    v->cw = cw;
    v->depth = depth;
    return 0;
}

/* ui:section(title, fn [, opts]) — open by default unless opts.open == false. */
static int l_section(lua_State *L)
{
    canlin_ui_view *v = check_ui(L);
    size_t tlen, outlen, shown;
    const char *title, *out;
    slot *st;
    struct nk_rect r;
    int el;
    float cx, cw, indent;
    unsigned depth;
    count_widget(L, v);
    title = get_str(L, 2, CANLIN_UI_MAX_LABEL, "title", &tlen);
    luaL_checktype(L, 3, LUA_TFUNCTION);
    enter(L, v, "section");
    st = identity(L, v, 4, title, tlen);
    focusable(v, st->id, CANLIN_UI_W_SECTION);
    if (!st->init) {
        int t = opt_field(L, 4, "open");
        if (t != LUA_TNIL && t != LUA_TBOOLEAN)
            luaL_error(L, "ui:section: opts.open must be a boolean");
        st->open = t == LUA_TNIL ? 1 : (unsigned char)lua_toboolean(L, -1);
        st->init = 1;
        lua_pop(L, 1);
    }
    {
        /* A heading-style row, unlike choice's input box: disclosure arrow in the first
         * em (where the indented content starts, the title), no box background, a
         * separator along the bottom. The whole row is the hit target. */
        struct nk_command_buffer *cv = nk_window_get_canvas(&v->nk);
        struct nk_style_button bs = v->nk.style.button;
        struct nk_style_item none = nk_style_item_color(nk_rgba(0, 0, 0, 0));
        const struct nk_user_font *f = font_of(v, F_BODY);
        float em = em_of(v), a = roundf(0.5f * em), ax, cy;
        float th = floorf(v->factor + 0.5f) < 1.0f ? 1.0f : floorf(v->factor + 0.5f);
        struct nk_color ac;
        el = ellipsize(v, f, title, tlen, v->cw - em, &out, &outlen, &shown);
        r = place(v, CANLIN_UI_W_SECTION, CANLIN_UI_P_BOX, v->cx, v->cy, v->cw, touch_of(v), 1,
                  0, shown, el);
        push(v, r);
        bs.normal = bs.hover = bs.active = none;
        bs.border = 0;
        bs.border_color = bs.text_normal = bs.text_hover = bs.text_active = none.data.color;
        if (nk_button_text_styled(&v->nk, &bs, "", 0) || activated(v, st->id))
            st->open = !st->open;
        ac = col(v, nk_input_is_mouse_hovering_rect(&v->nk.input, r) ? CANLIN_THEME_ACCENT
                                                                      : CANLIN_THEME_TEXT);
        ax = r.x + floorf((em - a) / 2.0f);
        cy = r.y + floorf(r.h / 2.0f);
        if (st->open) /* pointing down */
            nk_fill_triangle(cv, ax, cy - floorf(a * 0.4f), ax + a, cy - floorf(a * 0.4f),
                             ax + a / 2.0f, cy + floorf(a * 0.4f), ac);
        else /* pointing right */
            nk_fill_triangle(cv, ax + floorf(a * 0.1f), cy - a / 2.0f, ax + floorf(a * 0.1f),
                             cy + a / 2.0f, ax + floorf(a * 0.9f), cy, ac);
        nk_draw_text(cv, nk_rect(r.x + em, r.y + floorf((r.h - f->height) / 2.0f), r.w - em,
                                 f->height),
                     out, (int)outlen, f, nk_rgba(0, 0, 0, 0), col(v, CANLIN_THEME_TEXT));
        nk_fill_rect(cv, nk_rect(r.x, r.y + r.h - th, r.w, th), 0, col(v, CANLIN_THEME_MUTED));
        focus_mark(v, CANLIN_UI_W_SECTION, r, st->id);
    }
    advance(v, touch_of(v));
    if (st->open) {
        cx = v->cx;
        cw = v->cw;
        depth = v->depth;
        indent = em_of(v);
        if (indent > cw / 2.0f)
            indent = 0;
        v->cx += indent;
        v->cw -= indent;
        v->depth++;
        run_fn(L, v, 3, cx, cw, depth);
        v->cx = cx;
        v->cw = cw;
        v->depth = depth;
    }
    return 0;
}

static int ui_newindex(lua_State *L)
{
    return luaL_error(L, "ui is read-only");
}

static const luaL_Reg methods[] = {
    {"heading", l_heading}, {"text", l_text},       {"bubble", l_bubble},
    {"field", l_field},     {"area", l_area},       {"button", l_button},
    {"toggle", l_toggle},   {"choice", l_choice},   {"meter", l_meter},
    {"icon", l_icon},       {"separator", l_separator}, {"columns", l_columns},
    {"section", l_section}, {"graph", l_graph},     {NULL, NULL}};

int canlin_ui_push(lua_State *L, canlin_ui_view *v)
{
    const luaL_Reg *r;
    int proxy;
    canlin_sandbox_charge(L, (sizeof methods / sizeof methods[0] + 3) * CANLIN_SB_COST_ELEM);
    luaL_checkstack(L, 8, "ui");
    lua_createtable(L, 0, 0); /* proxy */
    proxy = lua_gettop(L);
    lua_createtable(L, 0, 3); /* metatable */
    lua_createtable(L, 0, (int)(sizeof methods / sizeof methods[0]));
    for (r = methods; r->name; r++) {
        lua_pushlightuserdata(L, v);
        lua_pushinteger(L, v ? (lua_Integer)v->frame : 0);
        lua_pushvalue(L, proxy);
        lua_pushcclosure(L, r->func, 3);
        lua_setfield(L, -2, r->name);
    }
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, ui_newindex);
    lua_setfield(L, -2, "__newindex");
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "__metatable");
    lua_setmetatable(L, proxy);
    return 1;
}

/* ==== shell support (canlin#35) ==========================================================
 * Read-only accessors the shell (src/shell) needs to draw chrome with the same theme and
 * to redraw a view region only when its module finished a new frame. */

const canlin_theme *canlin_ui_theme(const canlin_ui *ui)
{
    return ui ? &ui->theme : NULL;
}

uint32_t canlin_ui_view_frames(const canlin_ui_view *v)
{
    return v ? v->frame : 0;
}
