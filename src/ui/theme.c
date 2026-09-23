/* CanLin theme resolution (UI spec §6). See include/canlin/theme.h for the contract. */
#include "canlin/theme.h"
#include "canlin/nk.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static const char *const key_names[CANLIN_THEME_KEYS] = {
    "theme#color#background", "theme#color#surface", "theme#color#text",
    "theme#color#muted", "theme#color#accent", "theme#color#danger",
    "theme#color#success",
    "theme#font#body", "theme#font#heading", "theme#font#size", "theme#logo",
    "theme#spacing", "theme#radius",
};

const char *canlin_theme_key_name(int key)
{
    return key >= 0 && key < CANLIN_THEME_KEYS ? key_names[key] : NULL;
}

/* Placeholder palette: dark, text/background about 13:1. To be replaced by the
 * design's built-in theme. */
void canlin_theme_builtin(canlin_theme *t)
{
    static const canlin_theme_rgb pal[CANLIN_THEME_ROLES] = {
        { 0x1e, 0x21, 0x26 },   /* background */
        { 0x2a, 0x2e, 0x35 },   /* surface */
        { 0xe6, 0xe8, 0xeb },   /* text */
        { 0x8a, 0x91, 0x9c },   /* muted */
        { 0x3d, 0x8b, 0xfd },   /* accent */
        { 0xe5, 0x53, 0x4b },   /* danger */
        { 0x3f, 0xb9, 0x50 },   /* success */
    };
    memset(t, 0, sizeof *t);
    memcpy(t->color, pal, sizeof pal);
    t->font_size = 16;
    t->spacing = 1.0f;
    t->radius = 0.25f;
    memset(t->from, CANLIN_THEME_FROM_BUILTIN, sizeof t->from);
}

static size_t key_max(int key)
{
    switch (key) {
    case CANLIN_THEME_KEY_FONT_BODY:
    case CANLIN_THEME_KEY_FONT_HEADING: return CANLIN_THEME_FONT_MAX;
    case CANLIN_THEME_KEY_LOGO:         return CANLIN_THEME_LOGO_MAX;
    case CANLIN_THEME_KEY_FONT_SIZE:    return CANLIN_THEME_SIZE_MAXLEN;
    case CANLIN_THEME_KEY_SPACING:
    case CANLIN_THEME_KEY_RADIUS:       return CANLIN_THEME_DECIMAL_MAXLEN;
    default:                            return CANLIN_THEME_COLOR_LEN;
    }
}

static int hexval(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static const char *parse_color(const unsigned char *v, size_t len, canlin_theme_rgb *out)
{
    unsigned char b[3];
    int i;
    if (len != CANLIN_THEME_COLOR_LEN || v[0] != '#')
        return "not #RRGGBB";
    for (i = 0; i < 3; i++) {
        int hi = hexval(v[1 + 2 * i]), lo = hexval(v[2 + 2 * i]);
        if (hi < 0 || lo < 0)
            return "not #RRGGBB";
        b[i] = (unsigned char)(hi * 16 + lo);
    }
    out->r = b[0]; out->g = b[1]; out->b = b[2];
    return NULL;
}

static const char *parse_size(const unsigned char *v, size_t len, int *out)
{
    int n = 0;
    size_t i;
    if (len == 0)
        return "empty";
    for (i = 0; i < len; i++) {
        if (v[i] < '0' || v[i] > '9')
            return "not a decimal integer";
        n = n * 10 + (v[i] - '0');
    }
    if (n < 10 || n > 40)
        return "out of range 10-40";
    *out = n;
    return NULL;
}

/* "D[.DDDD]" -> value in 1/10000 units. No sign, exponent or locale. */
static const char *parse_decimal(const unsigned char *v, size_t len, long lo, long hi,
                                 float *out)
{
    long ip = 0, fp = 0, scale = 10000;
    size_t i = 0, nd = 0;
    if (len == 0)
        return "empty";
    for (; i < len && v[i] >= '0' && v[i] <= '9'; i++, nd++)
        if ((ip = ip * 10 + (v[i] - '0')) > 1000)
            return "out of range";
    if (nd == 0)
        return "not a decimal number";
    if (i < len) {
        size_t fd = 0;
        if (v[i++] != '.')
            return "not a decimal number";
        for (; i < len && v[i] >= '0' && v[i] <= '9'; i++, fd++) {
            if (fd == 4)
                return "more than 4 fraction digits";
            scale /= 10;
            fp += (v[i] - '0') * scale;
        }
        if (fd == 0 || i != len)
            return "not a decimal number";
    }
    fp += ip * 10000;
    if (fp < lo || fp > hi)
        return "out of range";
    *out = (float)fp / 10000.0f;
    return NULL;
}

static unsigned long be32(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) |
           ((unsigned long)p[2] << 8) | (unsigned long)p[3];
}

/* Magic numbers stb_truetype accepts (stbtt__isfont), checked before any baking. */
static const char *check_font(const unsigned char *v, size_t len)
{
    static const unsigned char tags[][4] = {
        { 0x00, 0x01, 0x00, 0x00 }, { 't', 'r', 'u', 'e' }, { 'O', 'T', 'T', 'O' },
        { 't', 'y', 'p', '1' }, { '1', 0x00, 0x00, 0x00 },
    };
    size_t i;
    if (len < 12)
        return "too short for a font";
    for (i = 0; i < sizeof tags / sizeof tags[0]; i++)
        if (memcmp(v, tags[i], 4) == 0)
            return NULL;
    return "not a TrueType/OpenType font";
}

/* Size limit plus PNG signature and IHDR dimensions. TODO(canlin#17 follow-up): full
 * decode (CRC, zlib, colour types) once a PNG decoder is vendored. */
static const char *check_logo(const unsigned char *v, size_t len)
{
    static const unsigned char sig[8] = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
    unsigned long w, h;
    if (len < 33 || memcmp(v, sig, 8) != 0 || be32(v + 8) != 13 ||
        memcmp(v + 12, "IHDR", 4) != 0)
        return "not a PNG";
    w = be32(v + 16);
    h = be32(v + 20);
    if (w == 0 || h == 0 || w > CANLIN_THEME_LOGO_MAX_SIDE || h > CANLIN_THEME_LOGO_MAX_SIDE)
        return "PNG dimensions out of range";
    return NULL;
}

/* Validates one value and stores it on success; t is untouched on failure. */
static const char *take(canlin_theme *t, int key, const unsigned char *v, size_t len)
{
    const char *err;
    canlin_theme_blob b;
    b.data = v;
    b.len = len;
    if (key < CANLIN_THEME_ROLES)
        return parse_color(v, len, &t->color[key]);
    switch (key) {
    case CANLIN_THEME_KEY_FONT_SIZE:
        return parse_size(v, len, &t->font_size);
    case CANLIN_THEME_KEY_SPACING:
        return parse_decimal(v, len, 2500, 40000, &t->spacing);
    case CANLIN_THEME_KEY_RADIUS:
        return parse_decimal(v, len, 0, 20000, &t->radius);
    case CANLIN_THEME_KEY_FONT_BODY:
        if (!(err = check_font(v, len)))
            t->font_body = b;
        return err;
    case CANLIN_THEME_KEY_FONT_HEADING:
        if (!(err = check_font(v, len)))
            t->font_heading = b;
        return err;
    case CANLIN_THEME_KEY_LOGO:
        if (!(err = check_logo(v, len)))
            t->logo = b;
        return err;
    default:
        return "unknown key";
    }
}

static int logf_(canlin_theme_log_fn log, void *ud, const char *src, const char *key,
                 const char *why)
{
    char line[192];
    if (!log)
        return 1;
    snprintf(line, sizeof line, "theme: %s: %s: %s, using the next layer",
             src ? src : "?", key, why);
    log(ud, line);
    return 1;
}

int canlin_theme_resolve(canlin_theme *t, const canlin_theme_source *srcs, size_t n,
                         canlin_theme_log_fn log, void *log_ud)
{
    int key, lines = 0;
    size_t i;
    float cr;

    canlin_theme_builtin(t);
    if (n > CANLIN_THEME_FROM_BUILTIN - 1u)
        n = CANLIN_THEME_FROM_BUILTIN - 1u;
    for (key = 0; key < CANLIN_THEME_KEYS; key++) {
        size_t max = key_max(key);
        for (i = 0; i < n; i++) {
            const unsigned char *v = NULL;
            size_t len = 0;
            const char *why;
            int rc;
            if (!srcs[i].get)
                continue;
            rc = srcs[i].get(srcs[i].ud, key_names[key], max, &v, &len);
            if (rc == CANLIN_THEME_SRC_ABSENT)
                continue;
            if (rc == CANLIN_THEME_SRC_TOOBIG || (rc == CANLIN_THEME_SRC_FOUND && len > max))
                why = "oversized";
            else if (rc != CANLIN_THEME_SRC_FOUND || (!v && len))
                why = "unreadable";
            else if (!v)
                why = "empty";
            else
                why = take(t, key, v, len);
            if (!why) {
                t->from[key] = (unsigned char)i;
                break;
            }
            lines += logf_(log, log_ud, srcs[i].name, key_names[key], why);
        }
    }
    cr = canlin_theme_contrast(t->color[CANLIN_THEME_TEXT], t->color[CANLIN_THEME_BACKGROUND]);
    if (cr < CANLIN_THEME_MIN_CONTRAST) {
        if (log) {
            char line[128];
            snprintf(line, sizeof line,
                     "theme: warning: text/background contrast %.2f:1 is below %.1f:1",
                     (double)cr, (double)CANLIN_THEME_MIN_CONTRAST);
            log(log_ud, line);
        }
        lines++;
    }
    return lines;
}

static double lin(unsigned char c)
{
    double s = c / 255.0;
    return s <= 0.03928 ? s / 12.92 : pow((s + 0.055) / 1.055, 2.4);
}

static double luminance(canlin_theme_rgb c)
{
    return 0.2126 * lin(c.r) + 0.7152 * lin(c.g) + 0.0722 * lin(c.b);
}

float canlin_theme_contrast(canlin_theme_rgb a, canlin_theme_rgb b)
{
    double la = luminance(a), lb = luminance(b);
    if (la < lb) {
        double s = la; la = lb; lb = s;
    }
    return (float)((la + 0.05) / (lb + 0.05));
}

float canlin_theme_em(const canlin_theme *t, float dpi)
{
    if (!(dpi > 0.0f) || !isfinite(dpi))
        dpi = 1.0f;
    return (float)t->font_size * dpi;
}

static struct nk_color nkc(canlin_theme_rgb c)
{
    return nk_rgb(c.r, c.g, c.b);
}

static struct nk_color mix(canlin_theme_rgb a, canlin_theme_rgb b, int pct)
{
    return nk_rgb((a.r * (100 - pct) + b.r * pct) / 100, (a.g * (100 - pct) + b.g * pct) / 100,
                  (a.b * (100 - pct) + b.b * pct) / 100);
}

/* Role -> Nuklear colour table. Provisional spacing mapping (one "unit" = spacing x
 * 0.25 em) until the layout ticket fixes geometry rules (UI spec §2). */
void canlin_theme_apply_nk(const canlin_theme *t, float dpi, struct nk_context *ctx,
                           const struct nk_user_font *font)
{
    struct nk_color tab[NK_COLOR_COUNT];
    const canlin_theme_rgb *c = t->color;
    struct nk_color bg = nkc(c[CANLIN_THEME_BACKGROUND]), sf = nkc(c[CANLIN_THEME_SURFACE]);
    struct nk_color tx = nkc(c[CANLIN_THEME_TEXT]), mu = nkc(c[CANLIN_THEME_MUTED]);
    struct nk_color ac = nkc(c[CANLIN_THEME_ACCENT]);
    struct nk_color hv = mix(c[CANLIN_THEME_SURFACE], c[CANLIN_THEME_ACCENT], 25);
    struct nk_style *s = &ctx->style;
    float em = canlin_theme_em(t, dpi), rad = t->radius * em;
    struct nk_vec2 unit;

    tab[NK_COLOR_TEXT] = tx;
    tab[NK_COLOR_WINDOW] = bg;
    tab[NK_COLOR_HEADER] = sf;
    tab[NK_COLOR_BORDER] = mu;
    tab[NK_COLOR_BUTTON] = sf;
    tab[NK_COLOR_BUTTON_HOVER] = hv;
    tab[NK_COLOR_BUTTON_ACTIVE] = ac;
    tab[NK_COLOR_TOGGLE] = sf;
    tab[NK_COLOR_TOGGLE_HOVER] = hv;
    tab[NK_COLOR_TOGGLE_CURSOR] = ac;
    tab[NK_COLOR_SELECT] = sf;
    tab[NK_COLOR_SELECT_ACTIVE] = ac;
    tab[NK_COLOR_SLIDER] = sf;
    tab[NK_COLOR_SLIDER_CURSOR] = ac;
    tab[NK_COLOR_SLIDER_CURSOR_HOVER] = hv;
    tab[NK_COLOR_SLIDER_CURSOR_ACTIVE] = ac;
    tab[NK_COLOR_PROPERTY] = sf;
    tab[NK_COLOR_EDIT] = sf;
    tab[NK_COLOR_EDIT_CURSOR] = tx;
    tab[NK_COLOR_COMBO] = sf;
    tab[NK_COLOR_CHART] = sf;
    tab[NK_COLOR_CHART_COLOR] = ac;
    tab[NK_COLOR_CHART_COLOR_HIGHLIGHT] = nkc(c[CANLIN_THEME_DANGER]);
    tab[NK_COLOR_SCROLLBAR] = bg;
    tab[NK_COLOR_SCROLLBAR_CURSOR] = mu;
    tab[NK_COLOR_SCROLLBAR_CURSOR_HOVER] = hv;
    tab[NK_COLOR_SCROLLBAR_CURSOR_ACTIVE] = ac;
    tab[NK_COLOR_TAB_HEADER] = sf;
    tab[NK_COLOR_KNOB] = sf;
    tab[NK_COLOR_KNOB_CURSOR] = ac;
    tab[NK_COLOR_KNOB_CURSOR_HOVER] = hv;
    tab[NK_COLOR_KNOB_CURSOR_ACTIVE] = ac;
    nk_style_from_table(ctx, tab);

    unit = nk_vec2(t->spacing * 0.25f * em, t->spacing * 0.25f * em);
    s->window.padding = unit;
    s->window.spacing = unit;
    s->window.group_padding = unit;
    s->button.padding = unit;
    s->window.rounding = rad;
    s->button.rounding = rad;
    s->edit.rounding = rad;
    s->property.rounding = rad;
    s->combo.rounding = rad;
    s->progress.rounding = rad;
    s->selectable.rounding = rad;
    if (font)
        nk_style_set_font(ctx, font);
}
