/* CanLin theme resolution (UI spec §6, system design §10).
 *
 * Each theme key is resolved on its own through an ordered list of key/value sources,
 * most specific first (theme/local/, then theme/theme.bee), then the built-in theme
 * compiled into the binary. An absent key falls through silently; an invalid or
 * oversized value falls through and logs one line naming the layer and key. Sizes are
 * checked before any parsing. Resolution never fails: the built-in theme is valid.
 *
 * Value encoding at the source seam (until libbee is consumed, canlin#2): bytes.
 *   theme#color#<role>   "#RRGGBB" (hex digits in either case)
 *   theme#font#size      decimal ASCII integer, 10-40 ("16")
 *   theme#spacing        decimal ASCII, 0.25-4.0, at most 4 fraction digits ("1.5")
 *   theme#radius         decimal ASCII, 0.0-2.0
 *   theme#font#body/heading  TTF/OTF blob, <= CANLIN_THEME_FONT_MAX
 *   theme#logo           PNG blob, <= CANLIN_THEME_LOGO_MAX, IHDR <= 1024x1024
 * A libbee-backed source presents MsgPack str/bin payloads as their bytes and a MsgPack
 * integer font size as its decimal text.
 */
#ifndef CANLIN_THEME_H
#define CANLIN_THEME_H

#include <stddef.h>

#include "canlin/ui_limits.h"

enum canlin_theme_role {
    CANLIN_THEME_BACKGROUND,
    CANLIN_THEME_SURFACE,
    CANLIN_THEME_TEXT,
    CANLIN_THEME_MUTED,
    CANLIN_THEME_ACCENT,
    CANLIN_THEME_DANGER,
    CANLIN_THEME_SUCCESS,
    CANLIN_THEME_ROLES
};

/* Keys in resolution order; the first CANLIN_THEME_ROLES are the colours by role. */
enum canlin_theme_key {
    CANLIN_THEME_KEY_FONT_BODY = CANLIN_THEME_ROLES,
    CANLIN_THEME_KEY_FONT_HEADING,
    CANLIN_THEME_KEY_FONT_SIZE,
    CANLIN_THEME_KEY_LOGO,
    CANLIN_THEME_KEY_SPACING,
    CANLIN_THEME_KEY_RADIUS,
    CANLIN_THEME_KEYS
};

/* Provenance value for "came from the built-in theme". */
#define CANLIN_THEME_FROM_BUILTIN 255u

/* Source result codes. */
#define CANLIN_THEME_SRC_ABSENT  0   /* no such key in this layer */
#define CANLIN_THEME_SRC_FOUND   1   /* *val / *len set */
#define CANLIN_THEME_SRC_TOOBIG  2   /* value longer than max; not read */
#define CANLIN_THEME_SRC_ERROR  (-1) /* layer unreadable for this key */

/* Looks up `key`. A value longer than `max` must be reported as TOOBIG without reading
 * or allocating it (a CDB lookup knows the length first). Returned bytes are owned by
 * the source and must stay valid as long as any theme resolved from it is in use. */
typedef int (*canlin_theme_get_fn)(void *ud, const char *key, size_t max,
                                   const unsigned char **val, size_t *len);

typedef struct canlin_theme_source {
    const char *name;           /* for log lines, e.g. "local", "theme.bee" */
    canlin_theme_get_fn get;
    void *ud;
} canlin_theme_source;

/* One complete line without newline. Never contains value bytes. */
typedef void (*canlin_theme_log_fn)(void *ud, const char *line);

typedef struct canlin_theme_rgb {
    unsigned char r, g, b;
} canlin_theme_rgb;

typedef struct canlin_theme_blob {
    const unsigned char *data;  /* NULL = none (fonts: use the built-in font) */
    size_t len;
} canlin_theme_blob;

typedef struct canlin_theme {
    canlin_theme_rgb color[CANLIN_THEME_ROLES];
    int font_size;              /* px at DPI 1.0 */
    float spacing;              /* em multiplier */
    float radius;               /* em */
    canlin_theme_blob font_body, font_heading;
    canlin_theme_blob logo;     /* size- and header-checked only; not decoded yet */
    unsigned char from[CANLIN_THEME_KEYS]; /* source index, or CANLIN_THEME_FROM_BUILTIN */
} canlin_theme;

/* The built-in theme (placeholder colours; the built-in DejaVu Sans subset as the font,
 * see render.h). */
void canlin_theme_builtin(canlin_theme *t);

/* Resolves every key through srcs[0..n) then the built-in theme. n <= 254; extra
 * sources are ignored. Logs a warning if text/background contrast is below 4.5:1.
 * log may be NULL. Returns the number of log lines emitted. */
int canlin_theme_resolve(canlin_theme *t, const canlin_theme_source *srcs, size_t n,
                         canlin_theme_log_fn log, void *log_ud);

/* WCAG 2 contrast ratio of two colours, 1.0-21.0. */
float canlin_theme_contrast(canlin_theme_rgb a, canlin_theme_rgb b);

/* em in px: base font size x factor, where src/ui passes the output's DPI factor x UI
 * scale (UI spec §2, §2.1). A factor that is not a positive finite number counts as 1.0. */
float canlin_theme_em(const canlin_theme *t, float dpi);

/* Key name, e.g. "theme#color#accent"; NULL for an out-of-range index. */
const char *canlin_theme_key_name(int key);

struct nk_context;
struct nk_user_font;
/* Maps the resolved theme onto ctx->style (the one owned mapping, UI spec §6.2):
 * colour table, rounding and spacing in px at `dpi`; sets `font` if non-NULL. */
void canlin_theme_apply_nk(const canlin_theme *t, float dpi, struct nk_context *ctx,
                           const struct nk_user_font *font);

#endif
