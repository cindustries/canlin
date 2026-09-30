/* Android adapter, pure part (Android adapter spec §4, §5, §6.1; canlin#72): lifecycle state
 * machine, Android input values -> normalised input, DPI factor, dirty span, and the RGB ->
 * window-format row conversion including enlarged bounds, RGB_565 and an unsupported
 * format. Host-side; no Android headers. Exits 0 on success. */
#include "canlin/android.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "canlin/shell.h"

static int failures, checks;

#define CHECK(c)                                                                   \
    do {                                                                           \
        checks++;                                                                  \
        if (!(c)) {                                                                \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c);  \
            failures++;                                                            \
        }                                                                          \
    } while (0)

/* ---- lifecycle ---- */

static void test_life(void)
{
    canlin_android_life l;
    unsigned a;

    canlin_android_life_init(&l);
    CHECK(!canlin_android_life_presentable(&l));

    /* launch as seen on 4.4: start, resume, input queue, window, focus */
    CHECK(canlin_android_life_cmd(&l, CANLIN_ANDROID_CMD_START) == 0);
    a = canlin_android_life_cmd(&l, CANLIN_ANDROID_CMD_RESUME);
    CHECK(a == (CANLIN_ANDROID_DO_RESUME | CANLIN_ANDROID_DO_PRESENT));
    CHECK(!canlin_android_life_presentable(&l)); /* no window yet */
    CHECK(canlin_android_life_cmd(&l, CANLIN_ANDROID_CMD_INPUT_CHANGED) == 0);
    a = canlin_android_life_cmd(&l, CANLIN_ANDROID_CMD_INIT_WINDOW);
    CHECK(a == (CANLIN_ANDROID_DO_WINDOW | CANLIN_ANDROID_DO_PRESENT));
    CHECK(canlin_android_life_presentable(&l));
    CHECK(canlin_android_life_cmd(&l, CANLIN_ANDROID_CMD_GAINED_FOCUS) ==
          CANLIN_ANDROID_DO_IMMERSIVE);
    CHECK(l.focused && l.started && l.resumed && l.window);

    /* power button off: pause (touch released, no presenting), window may stay */
    a = canlin_android_life_cmd(&l, CANLIN_ANDROID_CMD_PAUSE);
    CHECK(a == (CANLIN_ANDROID_DO_SUSPEND | CANLIN_ANDROID_DO_RELEASE));
    CHECK(!canlin_android_life_presentable(&l));
    CHECK(canlin_android_life_cmd(&l, CANLIN_ANDROID_CMD_LOST_FOCUS) == 0);
    CHECK(canlin_android_life_cmd(&l, CANLIN_ANDROID_CMD_SAVE_STATE) == 0);
    a = canlin_android_life_cmd(&l, CANLIN_ANDROID_CMD_STOP);
    CHECK(a & CANLIN_ANDROID_DO_SUSPEND);
    CHECK(!l.started && l.window);
    /* power on: start, resume -> the kept window gets a full present */
    CHECK(canlin_android_life_cmd(&l, CANLIN_ANDROID_CMD_START) == 0);
    a = canlin_android_life_cmd(&l, CANLIN_ANDROID_CMD_RESUME);
    CHECK(a & CANLIN_ANDROID_DO_PRESENT);
    CHECK(canlin_android_life_presentable(&l));

    /* Home: pause, stop, window gone; return: new window */
    canlin_android_life_cmd(&l, CANLIN_ANDROID_CMD_PAUSE);
    canlin_android_life_cmd(&l, CANLIN_ANDROID_CMD_STOP);
    a = canlin_android_life_cmd(&l, CANLIN_ANDROID_CMD_TERM_WINDOW);
    CHECK(a == (CANLIN_ANDROID_DO_NO_WINDOW | CANLIN_ANDROID_DO_RELEASE));
    CHECK(!l.window);
    canlin_android_life_cmd(&l, CANLIN_ANDROID_CMD_START);
    canlin_android_life_cmd(&l, CANLIN_ANDROID_CMD_RESUME);
    CHECK(!canlin_android_life_presentable(&l)); /* resumed, but no window */
    canlin_android_life_cmd(&l, CANLIN_ANDROID_CMD_INIT_WINDOW);
    CHECK(canlin_android_life_presentable(&l));

    /* metrics, redraw, memory, unknown */
    CHECK(canlin_android_life_cmd(&l, CANLIN_ANDROID_CMD_WINDOW_RESIZED) ==
          (CANLIN_ANDROID_DO_METRICS | CANLIN_ANDROID_DO_PRESENT));
    CHECK(canlin_android_life_cmd(&l, CANLIN_ANDROID_CMD_CONFIG_CHANGED) ==
          (CANLIN_ANDROID_DO_METRICS | CANLIN_ANDROID_DO_PRESENT));
    CHECK(canlin_android_life_cmd(&l, CANLIN_ANDROID_CMD_WINDOW_REDRAW_NEEDED) ==
          CANLIN_ANDROID_DO_PRESENT);
    CHECK(canlin_android_life_cmd(&l, CANLIN_ANDROID_CMD_CONTENT_RECT_CHANGED) == 0);
    CHECK(canlin_android_life_cmd(&l, CANLIN_ANDROID_CMD_LOW_MEMORY) == CANLIN_ANDROID_DO_TRIM);
    CHECK(canlin_android_life_cmd(&l, 99) == 0);
    CHECK(canlin_android_life_cmd(&l, -1) == 0);

    /* destroy: quit, nothing presentable, every later command is ignored */
    CHECK(canlin_android_life_cmd(&l, CANLIN_ANDROID_CMD_DESTROY) == CANLIN_ANDROID_DO_QUIT);
    CHECK(!canlin_android_life_presentable(&l));
    CHECK(canlin_android_life_cmd(&l, CANLIN_ANDROID_CMD_INIT_WINDOW) == 0);
    CHECK(canlin_android_life_cmd(&l, CANLIN_ANDROID_CMD_RESUME) == 0);
    CHECK(canlin_android_life_cmd(NULL, CANLIN_ANDROID_CMD_RESUME) == 0);
    CHECK(!canlin_android_life_presentable(NULL));
}

/* ---- input ---- */

static canlin_android_motion mv(int action, size_t index, size_t n, const int32_t *id,
                                const float *x, const float *y)
{
    canlin_android_motion m;
    size_t i;
    memset(&m, 0, sizeof m);
    m.action = action;
    m.index = index;
    m.n = n;
    for (i = 0; i < n; i++) {
        m.id[i] = id[i];
        m.x[i] = x[i];
        m.y[i] = y[i];
    }
    return m;
}

static int is_btn(const canlin_ui_input *in, int down, int x, int y)
{
    return in->type == CANLIN_UI_IN_BUTTON && in->button == CANLIN_UI_BUTTON_PRIMARY &&
           in->down == down && in->x == x && in->y == y;
}

static int is_motion(const canlin_ui_input *in, int x, int y)
{
    return in->type == CANLIN_UI_IN_MOTION && in->x == x && in->y == y;
}

/* ---- immersive mode (§7) ---- */

/* The flag set is View.SYSTEM_UI_FLAG_* of API 19, and the command sequence recorded on the
 * A7600-H (k74) when the flags land only at focus gain: the window is created at 1280x752
 * and grows to 1280x800 behind a WINDOW_RESIZED. Every step keeps the output presentable
 * and the grow asks for new metrics plus a full present; each focus gain (also after the
 * notification shade took it) asks for immersive mode again. Touch clamping follows the
 * size the device layer reads. */
static void test_immersive(void)
{
    canlin_android_life l;
    canlin_android_touch t;
    canlin_ui_input in;
    canlin_android_motion m;

    CHECK(CANLIN_ANDROID_UI_IMMERSIVE == 0x1706);
    CHECK((CANLIN_ANDROID_UI_IMMERSIVE & 0x0800) == 0); /* not plain IMMERSIVE */

    canlin_android_life_init(&l);
    canlin_android_life_cmd(&l, CANLIN_ANDROID_CMD_START);
    canlin_android_life_cmd(&l, CANLIN_ANDROID_CMD_RESUME);
    canlin_android_life_cmd(&l, CANLIN_ANDROID_CMD_INPUT_CHANGED);
    CHECK(canlin_android_life_cmd(&l, CANLIN_ANDROID_CMD_INIT_WINDOW) ==
          (CANLIN_ANDROID_DO_WINDOW | CANLIN_ANDROID_DO_PRESENT));
    CHECK(canlin_android_life_cmd(&l, CANLIN_ANDROID_CMD_WINDOW_RESIZED) ==
          (CANLIN_ANDROID_DO_METRICS | CANLIN_ANDROID_DO_PRESENT));
    CHECK(canlin_android_life_presentable(&l));
    CHECK(canlin_android_life_cmd(&l, CANLIN_ANDROID_CMD_GAINED_FOCUS) ==
          CANLIN_ANDROID_DO_IMMERSIVE);
    CHECK(canlin_android_life_cmd(&l, CANLIN_ANDROID_CMD_WINDOW_REDRAW_NEEDED) ==
          CANLIN_ANDROID_DO_PRESENT);
    CHECK(canlin_android_life_presentable(&l));
    /* notification shade: focus goes and comes back, the output stays presentable */
    CHECK(canlin_android_life_cmd(&l, CANLIN_ANDROID_CMD_LOST_FOCUS) == 0);
    CHECK(canlin_android_life_presentable(&l));
    CHECK(canlin_android_life_cmd(&l, CANLIN_ANDROID_CMD_GAINED_FOCUS) ==
          CANLIN_ANDROID_DO_IMMERSIVE);

    /* a touch in the former navigation bar area reaches the grown output */
    canlin_android_touch_init(&t, 1280, 752);
    {
        int32_t id[1] = {0};
        float x[1] = {100.0f}, y[1] = {790.0f};
        m = mv(CANLIN_ANDROID_ACTION_DOWN, 0, 1, id, x, y);
        CHECK(canlin_android_touch_map(&t, &m, &in) == 1 && in.y == 751);
        canlin_android_touch_release(&t, &in);
        t.w = 1280; /* what read_metrics sets after the grow */
        t.h = 800;
        CHECK(canlin_android_touch_map(&t, &m, &in) == 1 && is_btn(&in, 1, 100, 790));
    }
}

static void test_touch(void)
{
    canlin_android_touch t;
    canlin_android_motion m;
    canlin_ui_input in;
    int32_t id1[1] = {0}, id2[2] = {0, 1}, id2r[2] = {1, 0};
    float x1[1] = {10.4f}, y1[1] = {20.6f};
    float x2[2] = {30.0f, 500.0f}, y2[2] = {40.0f, 300.0f};

    canlin_android_touch_init(&t, 800, 480);

    /* first pointer down -> primary press at the rounded position */
    m = mv(CANLIN_ANDROID_ACTION_DOWN, 0, 1, id1, x1, y1);
    CHECK(canlin_android_touch_map(&t, &m, &in) == 1);
    CHECK(is_btn(&in, 1, 10, 21));
    CHECK(in.t == 0); /* stamped at post by canlin_module_post_input, not here */
    /* a move to the same px is no event (latest-state) */
    m = mv(CANLIN_ANDROID_ACTION_MOVE, 0, 1, id1, x1, y1);
    CHECK(canlin_android_touch_map(&t, &m, &in) == 0);
    /* second pointer down: ignored */
    m = mv(CANLIN_ANDROID_ACTION_POINTER_DOWN, 1, 2, id2, x2, y2);
    CHECK(canlin_android_touch_map(&t, &m, &in) == 0);
    /* move of both: only the tracked pointer (id 0) counts */
    m = mv(CANLIN_ANDROID_ACTION_MOVE, 0, 2, id2, x2, y2);
    CHECK(canlin_android_touch_map(&t, &m, &in) == 1);
    CHECK(is_motion(&in, 30, 40));
    /* the other pointer lifts: nothing */
    m = mv(CANLIN_ANDROID_ACTION_POINTER_UP, 1, 2, id2, x2, y2);
    CHECK(canlin_android_touch_map(&t, &m, &in) == 0);
    CHECK(t.active);
    /* the tracked pointer lifts while another stays: release */
    m = mv(CANLIN_ANDROID_ACTION_POINTER_DOWN, 1, 2, id2, x2, y2);
    CHECK(canlin_android_touch_map(&t, &m, &in) == 0);
    m = mv(CANLIN_ANDROID_ACTION_POINTER_UP, 0, 2, id2, x2, y2);
    CHECK(canlin_android_touch_map(&t, &m, &in) == 1);
    CHECK(is_btn(&in, 0, 30, 40));
    CHECK(!t.active);
    /* the remaining pointer does not become a new touch */
    m = mv(CANLIN_ANDROID_ACTION_MOVE, 0, 2, id2r, x2, y2);
    CHECK(canlin_android_touch_map(&t, &m, &in) == 0);
    m = mv(CANLIN_ANDROID_ACTION_UP, 0, 1, id2r, x2, y2);
    CHECK(canlin_android_touch_map(&t, &m, &in) == 0);

    /* tracking by id, not by index: pointer id 1 is at index 1 after reordering */
    {
        int32_t a[1] = {1}, b[2] = {0, 1};
        float ax[1] = {100.0f}, ay[1] = {100.0f}, bx[2] = {7.0f, 120.0f}, by[2] = {7.0f, 130.0f};
        m = mv(CANLIN_ANDROID_ACTION_DOWN, 0, 1, a, ax, ay);
        CHECK(canlin_android_touch_map(&t, &m, &in) == 1 && is_btn(&in, 1, 100, 100));
        m = mv(CANLIN_ANDROID_ACTION_MOVE, 0, 2, b, bx, by);
        CHECK(canlin_android_touch_map(&t, &m, &in) == 1 && is_motion(&in, 120, 130));
        m = mv(CANLIN_ANDROID_ACTION_UP, 1, 2, b, bx, by);
        CHECK(canlin_android_touch_map(&t, &m, &in) == 1 && is_btn(&in, 0, 120, 130));
    }

    /* cancel releases the held press at its last position; without one it is nothing */
    m = mv(CANLIN_ANDROID_ACTION_DOWN, 0, 1, id1, x1, y1);
    CHECK(canlin_android_touch_map(&t, &m, &in) == 1);
    m = mv(CANLIN_ANDROID_ACTION_CANCEL, 0, 1, id1, x2, y2);
    CHECK(canlin_android_touch_map(&t, &m, &in) == 1);
    CHECK(is_btn(&in, 0, 10, 21));
    CHECK(canlin_android_touch_map(&t, &m, &in) == 0);
    CHECK(canlin_android_touch_release(&t, &in) == 0);

    /* release by the device layer (pause, window loss) */
    m = mv(CANLIN_ANDROID_ACTION_DOWN, 0, 1, id1, x1, y1);
    canlin_android_touch_map(&t, &m, &in);
    CHECK(canlin_android_touch_release(&t, &in) == 1 && is_btn(&in, 0, 10, 21));
    CHECK(canlin_android_touch_release(&t, &in) == 0);

    /* clamping into the output, NaN and negatives */
    {
        float bx[1] = {5000.0f}, by[1] = {-3.0f}, nx[1] = {NAN}, ny[1] = {479.6f};
        m = mv(CANLIN_ANDROID_ACTION_DOWN, 0, 1, id1, bx, by);
        CHECK(canlin_android_touch_map(&t, &m, &in) == 1 && is_btn(&in, 1, 799, 0));
        m = mv(CANLIN_ANDROID_ACTION_MOVE, 0, 1, id1, nx, ny);
        CHECK(canlin_android_touch_map(&t, &m, &in) == 1 && is_motion(&in, 0, 479));
        /* a DOWN while held (lost UP) continues as motion, no second press */
        m = mv(CANLIN_ANDROID_ACTION_DOWN, 0, 1, id1, x1, y1);
        CHECK(canlin_android_touch_map(&t, &m, &in) == 1 && is_motion(&in, 10, 21));
        m = mv(CANLIN_ANDROID_ACTION_UP, 0, 1, id1, x1, y1);
        CHECK(canlin_android_touch_map(&t, &m, &in) == 1 && is_btn(&in, 0, 10, 21));
    }

    /* malformed: action index outside the pointers, OUTSIDE, unknown actions */
    m = mv(CANLIN_ANDROID_ACTION_DOWN, 3, 1, id1, x1, y1);
    CHECK(canlin_android_touch_map(&t, &m, &in) == 0 && !t.active);
    m = mv(CANLIN_ANDROID_ACTION_OUTSIDE, 0, 1, id1, x1, y1);
    CHECK(canlin_android_touch_map(&t, &m, &in) == 0);
    m = mv(7 /* HOVER_MOVE */, 0, 1, id1, x1, y1);
    CHECK(canlin_android_touch_map(&t, &m, &in) == 0);
    CHECK(canlin_android_touch_map(NULL, &m, &in) == 0);
}

static void test_keys(void)
{
    canlin_ui_input in;

    /* Back = Escape, both edges, always handled */
    CHECK(canlin_android_key_map(CANLIN_ANDROID_KEY_BACK, CANLIN_ANDROID_KEY_ACTION_DOWN, &in) == 1);
    CHECK(in.type == CANLIN_UI_IN_KEY && in.key == CANLIN_UI_KEY_ESCAPE && in.down == 1);
    CHECK(canlin_android_key_map(CANLIN_ANDROID_KEY_BACK, CANLIN_ANDROID_KEY_ACTION_UP, &in) == 1);
    CHECK(in.key == CANLIN_UI_KEY_ESCAPE && in.down == 0);
    CHECK(canlin_android_key_map(CANLIN_ANDROID_KEY_BACK, CANLIN_ANDROID_KEY_ACTION_MULTIPLE,
                                 &in) == 0);
    /* volume stays Android's */
    CHECK(canlin_android_key_map(CANLIN_ANDROID_KEY_VOLUME_UP, CANLIN_ANDROID_KEY_ACTION_DOWN,
                                 &in) == -1);
    CHECK(canlin_android_key_map(CANLIN_ANDROID_KEY_VOLUME_DOWN, CANLIN_ANDROID_KEY_ACTION_UP,
                                 &in) == -1);
    CHECK(canlin_android_key_map(CANLIN_ANDROID_KEY_VOLUME_MUTE, CANLIN_ANDROID_KEY_ACTION_DOWN,
                                 &in) == -1);
    /* editing and navigation keys of a hardware keyboard */
    CHECK(canlin_android_key_map(CANLIN_ANDROID_KEY_DEL, 0, &in) == 1 &&
          in.key == CANLIN_UI_KEY_BACKSPACE);
    CHECK(canlin_android_key_map(CANLIN_ANDROID_KEY_FORWARD_DEL, 0, &in) == 1 &&
          in.key == CANLIN_UI_KEY_DELETE);
    CHECK(canlin_android_key_map(CANLIN_ANDROID_KEY_NUMPAD_ENTER, 0, &in) == 1 &&
          in.key == CANLIN_UI_KEY_ENTER);
    CHECK(canlin_android_key_map(CANLIN_ANDROID_KEY_TAB, 0, &in) == 1 && in.key == CANLIN_UI_KEY_TAB);
    CHECK(canlin_android_key_map(CANLIN_ANDROID_KEY_DPAD_DOWN, 0, &in) == 1 &&
          in.key == CANLIN_UI_KEY_DOWN);
    CHECK(canlin_android_key_map(CANLIN_ANDROID_KEY_MOVE_END, 0, &in) == 1 &&
          in.key == CANLIN_UI_KEY_END);
    CHECK(canlin_android_key_map(CANLIN_ANDROID_KEY_CTRL_RIGHT, 0, &in) == 1 &&
          in.key == CANLIN_UI_KEY_CTRL);
    /* Menu opens the shell's switcher */
    CHECK(canlin_android_key_map(CANLIN_ANDROID_KEY_MENU, 0, &in) == 1 &&
          in.key == CANLIN_SHELL_KEY_SWITCHER);
    /* letters are not mapped (no text from hardware keyboards yet) */
    CHECK(canlin_android_key_map(29 /* A */, 0, &in) == -1);
    CHECK(canlin_android_key_map(CANLIN_ANDROID_KEY_BACK, 0, NULL) == -1);
}

static void test_dpi(void)
{
    CHECK(canlin_android_dpi(160) == 1.0f);
    CHECK(canlin_android_dpi(240) == 1.5f);
    CHECK(canlin_android_dpi(320) == 2.0f);
    CHECK(canlin_android_dpi(0) == 1.0f);
    CHECK(canlin_android_dpi(0xfffe) == 1.0f);
    CHECK(canlin_android_dpi(0xffff) == 1.0f);
    CHECK(canlin_android_dpi(-5) == 1.0f);
    CHECK(canlin_android_dpi(40) == CANLIN_UI_DPI_MIN);
    CHECK(canlin_android_dpi(2000) == CANLIN_UI_DPI_MAX);
}

/* ---- present ---- */

static void test_span(void)
{
    unsigned char d[10];
    int y0 = -1, y1 = -1;
    memset(d, 0, sizeof d);
    CHECK(canlin_android_dirty_span(d, 10, &y0, &y1) == 0);
    CHECK(canlin_android_dirty_span(NULL, 10, &y0, &y1) == 0);
    d[3] = d[7] = 1;
    CHECK(canlin_android_dirty_span(d, 10, &y0, &y1) == 1 && y0 == 3 && y1 == 8);
    d[9] = 1;
    d[0] = 1;
    CHECK(canlin_android_dirty_span(d, 10, &y0, &y1) == 1 && y0 == 0 && y1 == 10);
}

#define FW 4
#define FH 3

/* Frame pixel (x, y) = (10y + x, 100 + y, 200 + x). */
static void frame(unsigned char *px)
{
    int x, y;
    for (y = 0; y < FH; y++)
        for (x = 0; x < FW; x++) {
            unsigned char *p = px + (size_t)(y * FW + x) * 3;
            p[0] = (unsigned char)(10 * y + x);
            p[1] = (unsigned char)(100 + y);
            p[2] = (unsigned char)(200 + x);
        }
}

static int row_ok(const unsigned char *buf, int stride, int y)
{
    int x;
    for (x = 0; x < FW; x++) {
        const unsigned char *p = buf + ((size_t)y * (size_t)stride + (size_t)x) * 4;
        if (p[0] != 10 * y + x || p[1] != 100 + y || p[2] != 200 + x || p[3] != 255)
            return 0;
    }
    return 1;
}

static int untouched(const unsigned char *p, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (p[i] != 0xaa)
            return 0;
    return 1;
}

static void test_blit(void)
{
    unsigned char px[FW * FH * 3];
    unsigned char buf[6 * 5 * 4];     /* stride 6 px (2 px padding), up to 5 rows */
    uint16_t b16[8 * FH];
    canlin_android_buffer b;

    frame(px);

    /* RGBX, stride with padding: only the rows of [1, 2) are written */
    memset(buf, 0xaa, sizeof buf);
    b.bits = buf;
    b.w = FW;
    b.h = FH;
    b.stride = 6;
    b.format = CANLIN_ANDROID_FMT_RGBX_8888;
    CHECK(canlin_android_blit(&b, 1, 2, px, FW * 3, FW, FH) == 0);
    CHECK(untouched(buf, 6 * 4));                 /* row 0 */
    CHECK(row_ok(buf, 6, 1));
    CHECK(untouched(buf + 6 * 4 + FW * 4, 2 * 4)); /* row 1 padding is the platform's */
    CHECK(untouched(buf + 2 * 6 * 4, 6 * 4));     /* row 2 */

    /* enlarged returned bounds (first frame, other back buffer): every row inside them is
     * written from the full frame; bounds past the buffer are clamped */
    memset(buf, 0xaa, sizeof buf);
    CHECK(canlin_android_blit(&b, -2, 99, px, FW * 3, FW, FH) == 0);
    CHECK(row_ok(buf, 6, 0) && row_ok(buf, 6, 1) && row_ok(buf, 6, 2));
    CHECK(untouched(buf + 3 * 6 * 4, 2 * 6 * 4)); /* rows past b.h */
    /* an empty span writes nothing */
    memset(buf, 0xaa, sizeof buf);
    CHECK(canlin_android_blit(&b, 2, 2, px, FW * 3, FW, FH) == 0);
    CHECK(untouched(buf, sizeof buf));

    /* RGBA_8888 (what this device's app surfaces default to): same bytes, alpha opaque */
    memset(buf, 0xaa, sizeof buf);
    b.format = CANLIN_ANDROID_FMT_RGBA_8888;
    CHECK(canlin_android_blit(&b, 0, FH, px, FW * 3, FW, FH) == 0);
    CHECK(row_ok(buf, 6, 0) && row_ok(buf, 6, 2));

    /* a window larger than the frame (resize race): the rest is black, opaque */
    memset(buf, 0xaa, sizeof buf);
    b.w = 6;
    b.h = 5;
    b.format = CANLIN_ANDROID_FMT_RGBX_8888;
    CHECK(canlin_android_blit(&b, 0, 5, px, FW * 3, FW, FH) == 0);
    CHECK(row_ok(buf, 6, 2));
    CHECK(buf[(2 * 6 + 4) * 4] == 0 && buf[(2 * 6 + 5) * 4 + 3] == 255);
    CHECK(buf[(4 * 6 + 0) * 4] == 0 && buf[(4 * 6 + 0) * 4 + 1] == 0 &&
          buf[(4 * 6 + 0) * 4 + 3] == 255);

    /* RGB_565: packed 5-6-5, stride 8 px */
    {
        unsigned char c[FW * 3] = {255, 0, 0, 0, 255, 0, 0, 0, 255, 8, 4, 8};
        memset(b16, 0xaa, sizeof b16);
        b.bits = b16;
        b.w = FW;
        b.h = 1;
        b.stride = 8;
        b.format = CANLIN_ANDROID_FMT_RGB_565;
        CHECK(canlin_android_blit(&b, 0, 1, c, sizeof c, FW, 1) == 0);
        CHECK(b16[0] == 0xf800 && b16[1] == 0x07e0 && b16[2] == 0x001f && b16[3] == 0x0821);
        CHECK(b16[4] == 0xaaaa); /* padding */
        /* wider window: black past the frame */
        b.w = 6;
        CHECK(canlin_android_blit(&b, 0, 1, c, sizeof c, FW, 1) == 0);
        CHECK(b16[4] == 0 && b16[5] == 0 && b16[6] == 0xaaaa);
    }

    /* unsupported format (3 = RGB_888) and bad arguments: nothing written */
    memset(buf, 0xaa, sizeof buf);
    b.bits = buf;
    b.w = FW;
    b.h = FH;
    b.stride = 6;
    b.format = 3;
    CHECK(canlin_android_blit(&b, 0, FH, px, FW * 3, FW, FH) == -1);
    CHECK(untouched(buf, sizeof buf));
    b.format = CANLIN_ANDROID_FMT_RGBX_8888;
    b.stride = FW - 1;
    CHECK(canlin_android_blit(&b, 0, FH, px, FW * 3, FW, FH) == -1);
    b.stride = 6;
    CHECK(canlin_android_blit(&b, 0, FH, px, FW * 3 - 1, FW, FH) == -1);
    CHECK(canlin_android_blit(&b, 0, FH, NULL, FW * 3, FW, FH) == -1);
    CHECK(canlin_android_blit(NULL, 0, FH, px, FW * 3, FW, FH) == -1);
    CHECK(untouched(buf, sizeof buf));
}

int main(void)
{
    test_life();
    test_immersive();
    test_touch();
    test_keys();
    test_dpi();
    test_span();
    test_blit();
    printf("test_android: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
