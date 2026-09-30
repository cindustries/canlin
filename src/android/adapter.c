/* CanLin Android adapter, pure part (canlin#72): lifecycle state machine, input mapping,
 * row conversion. See include/canlin/android.h and the Android adapter spec §4–§6.1. No
 * Android headers: built and tested on every target. */
#include "canlin/android.h"

#include <string.h>

#include "canlin/shell.h"

/* ---- lifecycle ---- */

void canlin_android_life_init(canlin_android_life *l)
{
    memset(l, 0, sizeof *l);
}

unsigned canlin_android_life_cmd(canlin_android_life *l, int cmd)
{
    if (l == NULL || l->quit)
        return 0;
    switch (cmd) {
    case CANLIN_ANDROID_CMD_INIT_WINDOW:
        l->window = 1;
        return CANLIN_ANDROID_DO_WINDOW | CANLIN_ANDROID_DO_PRESENT;
    case CANLIN_ANDROID_CMD_TERM_WINDOW:
        l->window = 0;
        return CANLIN_ANDROID_DO_NO_WINDOW | CANLIN_ANDROID_DO_RELEASE;
    case CANLIN_ANDROID_CMD_WINDOW_RESIZED:
    case CANLIN_ANDROID_CMD_CONFIG_CHANGED:
        return CANLIN_ANDROID_DO_METRICS | CANLIN_ANDROID_DO_PRESENT;
    case CANLIN_ANDROID_CMD_WINDOW_REDRAW_NEEDED:
        return CANLIN_ANDROID_DO_PRESENT;
    case CANLIN_ANDROID_CMD_GAINED_FOCUS:
        l->focused = 1;
        return CANLIN_ANDROID_DO_IMMERSIVE;
    case CANLIN_ANDROID_CMD_LOST_FOCUS:
        l->focused = 0;
        return 0;
    case CANLIN_ANDROID_CMD_START:
        l->started = 1;
        return 0;
    case CANLIN_ANDROID_CMD_RESUME:
        l->resumed = 1;
        return CANLIN_ANDROID_DO_RESUME | CANLIN_ANDROID_DO_PRESENT;
    case CANLIN_ANDROID_CMD_PAUSE:
        l->resumed = 0;
        return CANLIN_ANDROID_DO_SUSPEND | CANLIN_ANDROID_DO_RELEASE;
    case CANLIN_ANDROID_CMD_STOP:
        l->started = 0;
        l->resumed = 0;
        return CANLIN_ANDROID_DO_SUSPEND | CANLIN_ANDROID_DO_RELEASE;
    case CANLIN_ANDROID_CMD_LOW_MEMORY:
        return CANLIN_ANDROID_DO_TRIM;
    case CANLIN_ANDROID_CMD_DESTROY:
        l->quit = 1;
        return CANLIN_ANDROID_DO_QUIT;
    default: /* INPUT_CHANGED (the glue attaches the queue), CONTENT_RECT_CHANGED,
              * SAVE_STATE (nothing depends on a final flush), unknown */
        return 0;
    }
}

int canlin_android_life_presentable(const canlin_android_life *l)
{
    return l != NULL && l->window && l->resumed && !l->quit;
}

/* ---- input ---- */

void canlin_android_touch_init(canlin_android_touch *t, int w, int h)
{
    memset(t, 0, sizeof *t);
    t->w = w;
    t->h = h;
}

static int px_of(float v, int max)
{
    int p;
    if (!(v == v) || v < 0.0f) /* NaN or negative */
        return 0;
    if (v > 1e6f)
        v = 1e6f;
    p = (int)(v + 0.5f);
    if (max > 0 && p > max - 1)
        p = max - 1;
    return p;
}

static void edge(canlin_ui_input *out, canlin_ui_input_type type, int x, int y, int down)
{
    memset(out, 0, sizeof *out);
    out->type = type;
    out->x = x;
    out->y = y;
    out->down = down;
    out->button = CANLIN_UI_BUTTON_PRIMARY;
}

/* Index of pointer id in m, or m->n. */
static size_t find_id(const canlin_android_motion *m, size_t n, int32_t id)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (m->id[i] == id)
            break;
    return i;
}

int canlin_android_touch_map(canlin_android_touch *t, const canlin_android_motion *m,
                             canlin_ui_input *out)
{
    size_t n, i;
    if (t == NULL || m == NULL || out == NULL)
        return 0;
    n = m->n < CANLIN_ANDROID_POINTERS_MAX ? m->n : CANLIN_ANDROID_POINTERS_MAX;
    switch (m->action) {
    case CANLIN_ANDROID_ACTION_DOWN:
        if (m->index >= n)
            return 0;
        if (t->active) {
            /* A DOWN while a touch is held (an UP was lost): the held press continues
             * as the new pointer, so no second press edge is produced. */
            t->id = m->id[m->index];
            t->x = px_of(m->x[m->index], t->w);
            t->y = px_of(m->y[m->index], t->h);
            edge(out, CANLIN_UI_IN_MOTION, t->x, t->y, 0);
            return 1;
        }
        t->active = 1;
        t->id = m->id[m->index];
        t->x = px_of(m->x[m->index], t->w);
        t->y = px_of(m->y[m->index], t->h);
        edge(out, CANLIN_UI_IN_BUTTON, t->x, t->y, 1);
        return 1;
    case CANLIN_ANDROID_ACTION_MOVE:
        if (!t->active)
            return 0;
        i = find_id(m, n, t->id);
        if (i == n)
            return 0;
        {
            int x = px_of(m->x[i], t->w), y = px_of(m->y[i], t->h);
            if (x == t->x && y == t->y)
                return 0;
            t->x = x;
            t->y = y;
        }
        edge(out, CANLIN_UI_IN_MOTION, t->x, t->y, 0);
        return 1;
    case CANLIN_ANDROID_ACTION_UP:
    case CANLIN_ANDROID_ACTION_POINTER_UP:
        if (!t->active || m->index >= n || m->id[m->index] != t->id)
            return 0;
        t->x = px_of(m->x[m->index], t->w);
        t->y = px_of(m->y[m->index], t->h);
        t->active = 0;
        edge(out, CANLIN_UI_IN_BUTTON, t->x, t->y, 0);
        return 1;
    case CANLIN_ANDROID_ACTION_CANCEL:
        return canlin_android_touch_release(t, out);
    default: /* POINTER_DOWN (further pointers are ignored in v1), OUTSIDE, unknown */
        return 0;
    }
}

int canlin_android_touch_release(canlin_android_touch *t, canlin_ui_input *out)
{
    if (t == NULL || out == NULL || !t->active)
        return 0;
    t->active = 0;
    edge(out, CANLIN_UI_IN_BUTTON, t->x, t->y, 0);
    return 1;
}

static int key_of(int keycode)
{
    switch (keycode) {
    case CANLIN_ANDROID_KEY_BACK:
    case CANLIN_ANDROID_KEY_ESCAPE:
        return CANLIN_UI_KEY_ESCAPE;
    case CANLIN_ANDROID_KEY_DEL:
        return CANLIN_UI_KEY_BACKSPACE;
    case CANLIN_ANDROID_KEY_FORWARD_DEL:
        return CANLIN_UI_KEY_DELETE;
    case CANLIN_ANDROID_KEY_ENTER:
    case CANLIN_ANDROID_KEY_NUMPAD_ENTER:
        return CANLIN_UI_KEY_ENTER;
    case CANLIN_ANDROID_KEY_TAB:
        return CANLIN_UI_KEY_TAB;
    case CANLIN_ANDROID_KEY_DPAD_LEFT:
        return CANLIN_UI_KEY_LEFT;
    case CANLIN_ANDROID_KEY_DPAD_RIGHT:
        return CANLIN_UI_KEY_RIGHT;
    case CANLIN_ANDROID_KEY_DPAD_UP:
        return CANLIN_UI_KEY_UP;
    case CANLIN_ANDROID_KEY_DPAD_DOWN:
        return CANLIN_UI_KEY_DOWN;
    case CANLIN_ANDROID_KEY_MOVE_HOME:
        return CANLIN_UI_KEY_HOME;
    case CANLIN_ANDROID_KEY_MOVE_END:
        return CANLIN_UI_KEY_END;
    case CANLIN_ANDROID_KEY_SHIFT_LEFT:
    case CANLIN_ANDROID_KEY_SHIFT_RIGHT:
        return CANLIN_UI_KEY_SHIFT;
    case CANLIN_ANDROID_KEY_CTRL_LEFT:
    case CANLIN_ANDROID_KEY_CTRL_RIGHT:
        return CANLIN_UI_KEY_CTRL;
    case CANLIN_ANDROID_KEY_MENU:
        return CANLIN_SHELL_KEY_SWITCHER;
    default: /* volume keys stay Android's; everything else is not mapped in v1 */
        return 0;
    }
}

int canlin_android_key_map(int keycode, int action, canlin_ui_input *out)
{
    int k = key_of(keycode);
    if (k == 0 || out == NULL)
        return -1;
    if (action != CANLIN_ANDROID_KEY_ACTION_DOWN && action != CANLIN_ANDROID_KEY_ACTION_UP)
        return 0;
    memset(out, 0, sizeof *out);
    out->type = CANLIN_UI_IN_KEY;
    out->key = k;
    out->down = action == CANLIN_ANDROID_KEY_ACTION_DOWN;
    return 1;
}

float canlin_android_dpi(int32_t density)
{
    float f;
    if (density <= 0 || density >= 0xfffe)
        density = 160;
    f = (float)density / 160.0f;
    if (f < CANLIN_UI_DPI_MIN)
        f = CANLIN_UI_DPI_MIN;
    if (f > CANLIN_UI_DPI_MAX)
        f = CANLIN_UI_DPI_MAX;
    return f;
}

/* ---- present ---- */

int canlin_android_dirty_span(const unsigned char *dirty, int h, int *y0, int *y1)
{
    int a = -1, b = -1, y;
    if (dirty == NULL)
        return 0;
    for (y = 0; y < h; y++)
        if (dirty[y]) {
            if (a < 0)
                a = y;
            b = y;
        }
    if (a < 0)
        return 0;
    *y0 = a;
    *y1 = b + 1;
    return 1;
}

int canlin_android_blit(const canlin_android_buffer *b, int y0, int y1,
                        const unsigned char *src, size_t pitch, int sw, int sh)
{
    int y, x, cw;
    if (b == NULL || b->bits == NULL || src == NULL || b->w <= 0 || b->h <= 0 ||
        b->stride < b->w || sw < 0 || sh < 0 || pitch < (size_t)sw * 3)
        return -1;
    if (b->format != CANLIN_ANDROID_FMT_RGBA_8888 && b->format != CANLIN_ANDROID_FMT_RGBX_8888 &&
        b->format != CANLIN_ANDROID_FMT_RGB_565)
        return -1;
    if (y0 < 0)
        y0 = 0;
    if (y1 > b->h)
        y1 = b->h;
    cw = sw < b->w ? sw : b->w;
    for (y = y0; y < y1; y++) {
        int covered = y < sh ? cw : 0;
        const unsigned char *s = covered ? src + (size_t)y * pitch : src;
        if (b->format == CANLIN_ANDROID_FMT_RGB_565) {
            uint16_t *d = (uint16_t *)b->bits + (size_t)y * (size_t)b->stride;
            for (x = 0; x < covered; x++, s += 3)
                d[x] = (uint16_t)(((s[0] & 0xf8u) << 8) | ((s[1] & 0xfcu) << 3) | (s[2] >> 3));
            for (; x < b->w; x++)
                d[x] = 0;
        } else {
            /* byte order R, G, B, A in memory; built bytewise, so no alignment or
             * endianness assumption beyond the format's own */
            unsigned char *d = (unsigned char *)b->bits + (size_t)y * (size_t)b->stride * 4u;
            for (x = 0; x < covered; x++, s += 3, d += 4) {
                d[0] = s[0];
                d[1] = s[1];
                d[2] = s[2];
                d[3] = 255;
            }
            for (; x < b->w; x++, d += 4) {
                d[0] = d[1] = d[2] = 0;
                d[3] = 255;
            }
        }
    }
    return 0;
}
