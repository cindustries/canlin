/* Theme resolution: every key valid/invalid/oversized per layer, per-key fallback,
 * contrast warning, built-in only, em, Nuklear style mapping (canlin#17, UI spec §6).
 * Exit 0 = pass. */
#include "canlin/render.h"
#include "canlin/theme.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

/* ---- in-memory source ---- */

#define MAXENT 16
struct ent { const char *key; const unsigned char *v; size_t len; int rc; };
struct mem {
    struct ent e[MAXENT];
    int n;
    size_t max_seen[CANLIN_THEME_KEYS];
    int lie;                    /* return FOUND with len > max (misbehaving source) */
};

static int key_index(const char *k)
{
    int i;
    for (i = 0; i < CANLIN_THEME_KEYS; i++)
        if (!strcmp(canlin_theme_key_name(i), k))
            return i;
    return -1;
}

static int mem_get(void *ud, const char *key, size_t max, const unsigned char **v,
                   size_t *len)
{
    struct mem *m = ud;
    int i, k = key_index(key);
    if (k >= 0)
        m->max_seen[k] = max;
    for (i = 0; i < m->n; i++) {
        if (strcmp(m->e[i].key, key))
            continue;
        if (m->e[i].rc)
            return m->e[i].rc;
        if (m->e[i].len > max && !m->lie)
            return CANLIN_THEME_SRC_TOOBIG;
        *v = m->e[i].v;
        *len = m->e[i].len;
        return CANLIN_THEME_SRC_FOUND;
    }
    return CANLIN_THEME_SRC_ABSENT;
}

static void put(struct mem *m, const char *key, const void *v, size_t len)
{
    m->e[m->n].key = key;
    m->e[m->n].v = v;
    m->e[m->n].len = len;
    m->e[m->n].rc = 0;
    m->n++;
}

static void puts_(struct mem *m, const char *key, const char *s)
{
    put(m, key, s, strlen(s));
}

/* ---- log capture ---- */

static char logs[32][256];
static int nlogs;

static void on_log(void *ud, const char *line)
{
    (void)ud;
    if (nlogs < 32)
        snprintf(logs[nlogs], sizeof logs[0], "%s", line);
    nlogs++;
}

static int logged(const char *a, const char *b)
{
    int i;
    for (i = 0; i < nlogs && i < 32; i++)
        if (strstr(logs[i], a) && (!b || strstr(logs[i], b)))
            return 1;
    return 0;
}

static struct mem L, B;         /* layers: local, theme.bee */
static canlin_theme T;

static int resolve(void)
{
    canlin_theme_source s[2];
    s[0].name = "local"; s[0].get = mem_get; s[0].ud = &L;
    s[1].name = "theme.bee"; s[1].get = mem_get; s[1].ud = &B;
    nlogs = 0;
    return canlin_theme_resolve(&T, s, 2, on_log, NULL);
}

static void reset(void)
{
    memset(&L, 0, sizeof L);
    memset(&B, 0, sizeof B);
}

/* ---- sample values ---- */

static unsigned char font_ok[64], font_bad[64], font_short[11];
static unsigned char png_ok[40], png_big[40], png_notpng[40];
static unsigned char huge[CANLIN_THEME_FONT_MAX + 1];

static void be32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8); p[3] = (unsigned char)v;
}

static void mkpng(unsigned char *p, unsigned long w, unsigned long h)
{
    static const unsigned char sig[8] = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
    memset(p, 0, 40);
    memcpy(p, sig, 8);
    be32(p + 8, 13);
    memcpy(p + 12, "IHDR", 4);
    be32(p + 16, w);
    be32(p + 20, h);
}

static void samples(void)
{
    memset(font_ok, 0xAB, sizeof font_ok);
    font_ok[0] = 0; font_ok[1] = 1; font_ok[2] = 0; font_ok[3] = 0;
    memcpy(font_bad, "wOFF", 4);
    memcpy(font_short, "true", 4);
    mkpng(png_ok, 64, 32);
    mkpng(png_big, CANLIN_THEME_LOGO_MAX_SIDE + 1, 10);
    mkpng(png_notpng, 64, 32);
    png_notpng[1] = 'Q';
}

struct kcase {
    int key;
    const void *ok; size_t ok_len;
    const void *bad; size_t bad_len;
    size_t oversize_len;        /* length that exceeds the key's max */
};

static void test_builtin_only(void)
{
    canlin_theme b;
    int k, lines;
    reset();
    lines = resolve();
    canlin_theme_builtin(&b);
    CHECK(lines == 0 && nlogs == 0, "built-in resolves silently (%d)", nlogs);
    CHECK(!memcmp(T.color, b.color, sizeof b.color) && T.font_size == b.font_size &&
          T.spacing == b.spacing && T.radius == b.radius, "built-in values");
    for (k = 0; k < CANLIN_THEME_KEYS; k++)
        CHECK(T.from[k] == CANLIN_THEME_FROM_BUILTIN, "key %d from built-in", k);
    CHECK(!T.font_body.data && !T.font_heading.data && !T.logo.data, "no blobs built in");
    CHECK(canlin_theme_contrast(b.color[CANLIN_THEME_TEXT], b.color[CANLIN_THEME_BACKGROUND])
          >= CANLIN_THEME_MIN_CONTRAST, "built-in contrast");
    CHECK(canlin_theme_resolve(&T, NULL, 0, NULL, NULL) == 0, "no sources, no log fn");
}

/* For one key: valid/invalid/oversized in each layer, and fallback between them. */
static void test_key(const struct kcase *c)
{
    const char *name = canlin_theme_key_name(c->key);
    int layer;

    for (layer = 0; layer < 2; layer++) {
        struct mem *m = layer ? &B : &L;
        const char *lname = layer ? "theme.bee" : "local";

        reset();                                        /* valid */
        put(m, name, c->ok, c->ok_len);
        resolve();
        CHECK(T.from[c->key] == layer && nlogs == 0, "%s valid in %s: from %u, %d logs",
              name, lname, T.from[c->key], nlogs);
        CHECK(m->max_seen[c->key] == c->oversize_len - 1, "%s max passed %zu", name,
              m->max_seen[c->key]);

        reset();                                        /* invalid */
        put(m, name, c->bad, c->bad_len);
        resolve();
        CHECK(T.from[c->key] == CANLIN_THEME_FROM_BUILTIN && nlogs == 1 &&
              logged(lname, name), "%s invalid in %s: from %u, %d logs", name, lname,
              T.from[c->key], nlogs);

        reset();                                        /* oversized, honest source */
        put(m, name, huge, c->oversize_len);
        resolve();
        CHECK(T.from[c->key] == CANLIN_THEME_FROM_BUILTIN && nlogs == 1 &&
              logged(name, "oversized"), "%s oversized in %s", name, lname);

        reset();                                        /* oversized, lying source */
        m->lie = 1;
        put(m, name, huge, c->oversize_len);
        resolve();
        CHECK(T.from[c->key] == CANLIN_THEME_FROM_BUILTIN && logged(name, "oversized"),
              "%s oversized from a lying source", name);

        reset();                                        /* unreadable */
        put(m, name, c->ok, c->ok_len);
        m->e[0].rc = CANLIN_THEME_SRC_ERROR;
        resolve();
        CHECK(T.from[c->key] == CANLIN_THEME_FROM_BUILTIN && logged(name, "unreadable"),
              "%s unreadable", name);
    }

    reset();                                            /* local bad -> bee good */
    put(&L, name, c->bad, c->bad_len);
    put(&B, name, c->ok, c->ok_len);
    resolve();
    CHECK(T.from[c->key] == 1 && nlogs == 1 && logged("local", name),
          "%s falls back to theme.bee", name);

    reset();                                            /* local wins over bee */
    put(&L, name, c->ok, c->ok_len);
    put(&B, name, c->bad, c->bad_len);
    resolve();
    CHECK(T.from[c->key] == 0 && nlogs == 0, "%s local masks theme.bee", name);
}

static void test_every_key(void)
{
    static const char *const colors[CANLIN_THEME_ROLES] = {
        "#101010", "#202020", "#F0f0F0", "#808080", "#3366cc", "#cc3333", "#33cc33" };
    struct kcase c;
    int k;

    for (k = 0; k < CANLIN_THEME_ROLES; k++) {
        c.key = k;
        c.ok = colors[k]; c.ok_len = 7;
        c.bad = "#12G456"; c.bad_len = 7;
        c.oversize_len = CANLIN_THEME_COLOR_LEN + 1;
        test_key(&c);
    }
    c.key = CANLIN_THEME_KEY_FONT_SIZE;
    c.ok = "22"; c.ok_len = 2; c.bad = "41"; c.bad_len = 2;
    c.oversize_len = CANLIN_THEME_SIZE_MAXLEN + 1;
    test_key(&c);
    c.key = CANLIN_THEME_KEY_SPACING;
    c.ok = "1.5"; c.ok_len = 3; c.bad = "0.2"; c.bad_len = 3;
    c.oversize_len = CANLIN_THEME_DECIMAL_MAXLEN + 1;
    test_key(&c);
    c.key = CANLIN_THEME_KEY_RADIUS;
    c.ok = "0.5"; c.ok_len = 3; c.bad = "2.5"; c.bad_len = 3;
    c.oversize_len = CANLIN_THEME_DECIMAL_MAXLEN + 1;
    test_key(&c);
    c.key = CANLIN_THEME_KEY_FONT_BODY;
    c.ok = font_ok; c.ok_len = sizeof font_ok; c.bad = font_bad; c.bad_len = sizeof font_bad;
    c.oversize_len = CANLIN_THEME_FONT_MAX + 1;
    test_key(&c);
    c.key = CANLIN_THEME_KEY_FONT_HEADING;
    test_key(&c);
    c.key = CANLIN_THEME_KEY_LOGO;
    c.ok = png_ok; c.ok_len = sizeof png_ok; c.bad = png_big; c.bad_len = sizeof png_big;
    c.oversize_len = CANLIN_THEME_LOGO_MAX + 1;
    test_key(&c);
}

/* Values actually land in the struct. */
static void test_values(void)
{
    reset();
    puts_(&L, "theme#color#accent", "#aBcDeF");
    puts_(&L, "theme#font#size", "10");
    puts_(&B, "theme#spacing", "0.25");
    puts_(&B, "theme#radius", "0");
    put(&B, "theme#font#heading", font_ok, sizeof font_ok);
    put(&L, "theme#logo", png_ok, sizeof png_ok);
    resolve();
    CHECK(T.color[CANLIN_THEME_ACCENT].r == 0xab && T.color[CANLIN_THEME_ACCENT].g == 0xcd &&
          T.color[CANLIN_THEME_ACCENT].b == 0xef, "hex colour");
    CHECK(T.font_size == 10 && T.spacing == 0.25f && T.radius == 0.0f, "numbers");
    CHECK(T.font_heading.data == font_ok && T.font_heading.len == sizeof font_ok &&
          !T.font_body.data, "heading blob only");
    CHECK(T.logo.data == png_ok && T.logo.len == sizeof png_ok, "logo blob");
    CHECK(nlogs == 0, "no logs");
}

struct vcase { const char *key, *v; int ok; };

static void test_formats(void)
{
    static const struct vcase v[] = {
        { "theme#color#accent", "#000000", 1 }, { "theme#color#accent", "#FFFFFF", 1 },
        { "theme#color#accent", "#12345", 0 },  { "theme#color#accent", "123456#", 0 },
        { "theme#color#accent", "", 0 },         { "theme#color#accent", "#GGGGGG", 0 },
        { "theme#color#accent", " #12345", 0 },
        { "theme#font#size", "10", 1 },  { "theme#font#size", "40", 1 },
        { "theme#font#size", "9", 0 },   { "theme#font#size", "41", 0 },
        { "theme#font#size", "1a", 0 },  { "theme#font#size", "", 0 },
        { "theme#font#size", "+9", 0 },
        { "theme#spacing", "0.25", 1 },  { "theme#spacing", "4", 1 },
        { "theme#spacing", "4.0", 1 },   { "theme#spacing", "4.0000", 1 },
        { "theme#spacing", "0.2499", 0 }, { "theme#spacing", "4.0001", 0 },
        { "theme#spacing", "0.12345", 0 }, { "theme#spacing", "1.", 0 },
        { "theme#spacing", ".5", 0 },    { "theme#spacing", "1,5", 0 },
        { "theme#spacing", "1e0", 0 },   { "theme#spacing", "-1", 0 },
        { "theme#spacing", "99999999", 0 }, { "theme#spacing", "1.5 ", 0 },
        { "theme#radius", "0", 1 },      { "theme#radius", "0.0", 1 },
        { "theme#radius", "2.0", 1 },    { "theme#radius", "2.01", 0 },
    };
    size_t i;
    for (i = 0; i < sizeof v / sizeof v[0]; i++) {
        int k = key_index(v[i].key);
        reset();
        puts_(&L, v[i].key, v[i].v);
        resolve();
        CHECK((T.from[k] == 0) == v[i].ok && (nlogs == 0) == v[i].ok,
              "%s = \"%s\" expected %s", v[i].key, v[i].v, v[i].ok ? "valid" : "invalid");
    }

    reset();
    put(&L, "theme#font#body", font_short, sizeof font_short);
    put(&B, "theme#font#body", font_bad, sizeof font_bad);
    put(&L, "theme#logo", png_notpng, sizeof png_notpng);
    put(&B, "theme#logo", png_ok, 20);     /* truncated before IHDR data */
    resolve();
    CHECK(T.from[CANLIN_THEME_KEY_FONT_BODY] == CANLIN_THEME_FROM_BUILTIN &&
          T.from[CANLIN_THEME_KEY_LOGO] == CANLIN_THEME_FROM_BUILTIN && nlogs == 4,
          "short font, wOFF, not-PNG, truncated PNG: %d logs", nlogs);

    reset();                            /* zero-length values */
    put(&L, "theme#font#body", "", 0);
    put(&L, "theme#logo", "", 0);
    resolve();
    CHECK(nlogs == 2, "empty blobs rejected: %d", nlogs);
}

static void test_per_key_fallback(void)
{
    reset();
    puts_(&L, "theme#color#text", "#FAFAFA");
    puts_(&L, "theme#color#accent", "nope!!!");
    puts_(&B, "theme#color#accent", "#112233");
    puts_(&B, "theme#color#text", "#010101");
    puts_(&L, "theme#font#size", "99");
    resolve();
    CHECK(T.from[CANLIN_THEME_TEXT] == 0 && T.color[CANLIN_THEME_TEXT].r == 0xfa,
          "text from local");
    CHECK(T.from[CANLIN_THEME_ACCENT] == 1 && T.color[CANLIN_THEME_ACCENT].r == 0x11,
          "accent from theme.bee");
    CHECK(T.from[CANLIN_THEME_KEY_FONT_SIZE] == CANLIN_THEME_FROM_BUILTIN &&
          T.font_size == 16, "size from built-in");
    CHECK(T.from[CANLIN_THEME_BACKGROUND] == CANLIN_THEME_FROM_BUILTIN, "untouched key");
    CHECK(nlogs == 2 && logged("theme#color#accent", "local") &&
          logged("theme#font#size", "local"), "one line per invalid key (%d)", nlogs);
}

static void test_log_hygiene(void)
{
    int i;
    reset();
    puts_(&L, "theme#spacing", "SECRET!");
    puts_(&L, "theme#color#muted", "SECRETX");
    resolve();
    CHECK(nlogs == 2, "two lines");
    for (i = 0; i < nlogs && i < 32; i++)
        CHECK(!strstr(logs[i], "SECRET") && !strchr(logs[i], '\n'),
              "log line carries no value bytes: %s", logs[i]);
}

static void test_contrast(void)
{
    canlin_theme_rgb k = { 0, 0, 0 }, w = { 255, 255, 255 };
    float cr = canlin_theme_contrast(k, w);
    CHECK(fabsf(cr - 21.0f) < 0.01f && fabsf(canlin_theme_contrast(w, k) - cr) < 1e-6f,
          "black/white 21:1 (%.3f)", cr);
    CHECK(fabsf(canlin_theme_contrast(w, w) - 1.0f) < 1e-6f, "same colour 1:1");

    reset();
    puts_(&L, "theme#color#text", "#777777");
    puts_(&B, "theme#color#background", "#888888");
    resolve();
    CHECK(T.from[CANLIN_THEME_TEXT] == 0 && T.from[CANLIN_THEME_BACKGROUND] == 1,
          "low contrast is accepted, not refused");
    CHECK(nlogs == 1 && logged("warning", "contrast"), "contrast warning (%d)", nlogs);

    reset();                            /* #767676 on white is 4.54:1, just passes */
    puts_(&L, "theme#color#text", "#767676");
    puts_(&L, "theme#color#background", "#FFFFFF");
    resolve();
    CHECK(nlogs == 0, "4.54:1 has no warning");
}

static void test_em(void)
{
    canlin_theme t;
    canlin_theme_builtin(&t);
    t.font_size = 16;
    CHECK(canlin_theme_em(&t, 1.0f) == 16.0f, "em at 1.0");
    CHECK(canlin_theme_em(&t, 1.5f) == 24.0f, "em at 1.5");
    CHECK(canlin_theme_em(&t, 2.0f) == 32.0f, "em at 2.0");
    CHECK(canlin_theme_em(&t, 0.0f) == 16.0f && canlin_theme_em(&t, -1.0f) == 16.0f &&
          canlin_theme_em(&t, NAN) == 16.0f && canlin_theme_em(&t, INFINITY) == 16.0f,
          "bad DPI counts as 1.0");
}

static void test_apply_nk(void)
{
    canlin_render_font f;
    struct nk_context ctx;
    canlin_theme t;
    struct nk_color c;

    CHECK(canlin_render_font_default(&f, 24.0f) == 0, "font");
    CHECK(nk_init_default(&ctx, &f.font->handle), "nk_init");
    reset();
    puts_(&L, "theme#color#background", "#102030");
    puts_(&L, "theme#color#text", "#F0E0D0");
    puts_(&L, "theme#radius", "0.5");
    puts_(&L, "theme#spacing", "2");
    resolve();
    t = T;
    canlin_theme_apply_nk(&t, 1.5f, &ctx, &f.font->handle);
    c = ctx.style.window.fixed_background.data.color;
    CHECK(c.r == 0x10 && c.g == 0x20 && c.b == 0x30 && c.a == 255, "window background");
    c = ctx.style.text.color;
    CHECK(c.r == 0xf0 && c.g == 0xe0 && c.b == 0xd0, "text colour");
    c = ctx.style.button.active.data.color;
    CHECK(c.r == t.color[CANLIN_THEME_ACCENT].r && c.b == t.color[CANLIN_THEME_ACCENT].b,
          "button active = accent");
    /* em = 16 * 1.5 = 24 px; radius 0.5 em = 12 px; unit = 2 * 0.25 em = 12 px */
    CHECK(ctx.style.button.rounding == 12.0f && ctx.style.window.rounding == 12.0f,
          "rounding %f", (double)ctx.style.button.rounding);
    CHECK(ctx.style.window.padding.x == 12.0f && ctx.style.window.spacing.y == 12.0f,
          "spacing");
    CHECK(ctx.style.font == &f.font->handle, "font set");
    nk_free(&ctx);
    canlin_render_font_free(&f);
}

int main(void)
{
    samples();
    test_builtin_only();
    test_every_key();
    test_values();
    test_formats();
    test_per_key_fallback();
    test_log_hygiene();
    test_contrast();
    test_em();
    test_apply_nk();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("theme: all passed\n");
    return 0;
}
