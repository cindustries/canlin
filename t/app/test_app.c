/* Minimal runtime assembly (canlin#72): the demo module on one touch output through the
 * native loop with a fake clock and wait. First frame presented whole, idle waits without
 * a timeout, a tap on the field brings up the on-screen keyboard, typed keys reach the
 * field, the button runs Lua (seen through print), a resize re-presents the whole frame.
 * Taps are made from Android motion values through the pure Android touch mapping, so the
 * mapping is exercised on the real shell/module route. Exits 0 on success. */
#include "canlin/app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "canlin/android.h"

static int failures, checks;

#define CHECK(c)                                                                   \
    do {                                                                           \
        checks++;                                                                  \
        if (!(c)) {                                                                \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c);  \
            failures++;                                                            \
        }                                                                          \
    } while (0)

#define W 800
#define H 480

typedef struct harness {
    canlin_ev_fake_clock clock;
    canlin_loop_fake_wait fw;
    canlin_app *a;
    canlin_loop *l;
    canlin_android_touch touch;
    unsigned presents;
    int max_rows, full_presents;
    char log[4096];
    size_t loglen;
    int errors;
} harness;

static void present(void *ctx, canlin_render *r, int rows)
{
    harness *h = ctx;
    h->presents++;
    if (rows > h->max_rows)
        h->max_rows = rows;
    if (rows == r->h)
        h->full_presents++;
}

static void logf_(void *ud, int level, const char *line)
{
    harness *h = ud;
    size_t n = strlen(line);
    if (level == CANLIN_APP_LOG_ERROR)
        h->errors++;
    if (h->loglen + n + 2 < sizeof h->log) {
        memcpy(h->log + h->loglen, line, n);
        h->loglen += n;
        h->log[h->loglen++] = '\n';
        h->log[h->loglen] = 0;
    }
}

/* Iterate until the loop would wait without a timeout (idle), at most 64 times. */
static void settle(harness *h)
{
    unsigned k = 0;
    do
        CHECK(canlin_loop_iterate(h->l) == CANLIN_LOOP_OK);
    while (++k < 64 && h->fw.last_timeout != -1);
    CHECK(h->fw.last_timeout == -1);
}

static void touch(harness *h, int action, float x, float y)
{
    canlin_android_motion m;
    canlin_ui_input in;
    memset(&m, 0, sizeof m);
    m.action = action;
    m.n = 1;
    m.id[0] = 0;
    m.x[0] = x;
    m.y[0] = y;
    if (canlin_android_touch_map(&h->touch, &m, &in))
        CHECK(canlin_app_input(h->a, &in) >= 0);
}

static void tap(harness *h, const canlin_ui_rect *r)
{
    CHECK(r != NULL);
    if (r == NULL)
        return;
    touch(h, CANLIN_ANDROID_ACTION_DOWN, (float)(r->x + r->w / 2), (float)(r->y + r->h / 2));
    settle(h);
    touch(h, CANLIN_ANDROID_ACTION_UP, (float)(r->x + r->w / 2), (float)(r->y + r->h / 2));
    settle(h);
}

static const canlin_ui_rect *find(harness *h, int kind, int part)
{
    size_t n, i;
    const canlin_ui_rect *r =
        canlin_ui_view_record(canlin_module_view(canlin_app_module(h->a)), &n, NULL);
    for (i = 0; r && i < n; i++)
        if (r[i].kind == kind && r[i].part == part)
            return &r[i];
    return NULL;
}

static const canlin_ui_rect *key(harness *h, uint32_t cp)
{
    size_t n, i;
    const canlin_ui_rect *r =
        canlin_ui_view_record(canlin_module_view(canlin_app_module(h->a)), &n, NULL);
    for (i = 0; r && i < n; i++)
        if (r[i].kind == CANLIN_UI_W_KEYBOARD && r[i].part == CANLIN_UI_P_KEY &&
            r[i].len == CANLIN_UI_KBD_CHAR && r[i].off == cp)
            return &r[i];
    return NULL;
}

static void test_demo(void)
{
    harness h;
    canlin_app_config ac;
    canlin_loop_config lc;
    canlin_render *r;
    unsigned p0;
    int err;

    memset(&h, 0, sizeof h);
    h.clock.now = 1000;
    canlin_loop_fake_wait_init(&h.fw, &h.clock);
    memset(&ac, 0, sizeof ac);
    ac.clock = canlin_ev_fake_clock_bind(&h.clock);
    ac.output_id = "test:main";
    ac.w = W;
    ac.h = H;
    ac.dpi = 1.0f;
    ac.input = CANLIN_SHELL_IN_TOUCH;
    ac.present = present;
    ac.present_ctx = &h;
    ac.log = logf_;
    ac.log_ud = &h;
    h.a = canlin_app_new(&ac, &err);
    CHECK(h.a != NULL && err == CANLIN_APP_OK);
    if (h.a == NULL)
        return;
    memset(&lc, 0, sizeof lc);
    canlin_app_loop_config(h.a, &lc);
    lc.wait = canlin_loop_fake_wait_poll;
    lc.wait_ctx = &h.fw;
    h.l = canlin_loop_new(&lc, &err);
    CHECK(h.l != NULL);
    if (h.l == NULL)
        return;
    canlin_android_touch_init(&h.touch, W, H);

    /* first frame: presented whole, then idle without a timeout (no wall clock) */
    settle(&h);
    CHECK(h.presents >= 1);
    CHECK(h.full_presents >= 1);
    CHECK(canlin_module_get_state(canlin_app_module(h.a)) == CANLIN_MODULE_RUNNING);
    CHECK(find(&h, CANLIN_UI_W_HEADING, CANLIN_UI_P_BOX) != NULL ||
          find(&h, CANLIN_UI_W_HEADING, CANLIN_UI_P_LINE) != NULL ||
          find(&h, CANLIN_UI_W_HEADING, CANLIN_UI_P_LABEL) != NULL);
    CHECK(find(&h, CANLIN_UI_W_KEYBOARD, CANLIN_UI_P_BOX) == NULL);
    p0 = h.presents;
    CHECK(canlin_loop_iterate(h.l) == CANLIN_LOOP_OK);
    CHECK(canlin_loop_iterate(h.l) == CANLIN_LOOP_OK);
    CHECK(h.presents == p0);          /* idle: nothing dirty, nothing presented */
    CHECK(h.fw.last_timeout == -1);

    /* tap on the field: the touch output shows the on-screen keyboard */
    h.max_rows = 0;
    tap(&h, find(&h, CANLIN_UI_W_FIELD, CANLIN_UI_P_BOX));
    CHECK(find(&h, CANLIN_UI_W_KEYBOARD, CANLIN_UI_P_BOX) != NULL);
    CHECK(h.presents > p0);
    CHECK(h.max_rows > 0 && h.max_rows < H); /* changed rows only, never the whole frame */

    /* type "hi" on it, then press the button */
    tap(&h, key(&h, 'h'));
    tap(&h, key(&h, 'i'));
    tap(&h, find(&h, CANLIN_UI_W_BUTTON, CANLIN_UI_P_BOX));
    CHECK(strstr(h.log, "demo: greet 1 hi\n") != NULL);
    tap(&h, find(&h, CANLIN_UI_W_BUTTON, CANLIN_UI_P_BOX));
    CHECK(strstr(h.log, "demo: greet 2 hi\n") != NULL);

    /* resize: new frame size, presented whole */
    h.full_presents = 0;
    CHECK(canlin_app_resize(h.a, 640, 400) == CANLIN_APP_OK);
    r = canlin_app_frame(h.a);
    CHECK(r->w == 640 && r->h == 400);
    settle(&h);
    CHECK(h.full_presents >= 1);
    CHECK(canlin_app_resize(h.a, 640, 400) == CANLIN_APP_OK); /* same size: nothing */
    p0 = h.presents;
    settle(&h);
    CHECK(h.presents == p0);
    CHECK(canlin_app_resize(h.a, 0, 400) == CANLIN_APP_EINVAL);
    CHECK(canlin_app_set_dpi(h.a, 1.5f) == CANLIN_APP_OK);
    settle(&h);
    CHECK(h.presents > p0);
    CHECK(canlin_app_set_dpi(h.a, 99.0f) == CANLIN_APP_EINVAL);

    CHECK(h.errors == 0);
    if (h.errors)
        fprintf(stderr, "log:\n%s", h.log);
    canlin_loop_free(h.l);
    canlin_app_free(h.a);
}

static void test_bad_config(void)
{
    canlin_app_config ac;
    int err = 0;
    memset(&ac, 0, sizeof ac);
    CHECK(canlin_app_new(NULL, &err) == NULL && err == CANLIN_APP_EINVAL);
    CHECK(canlin_app_new(&ac, &err) == NULL && err == CANLIN_APP_EINVAL);
    ac.clock = canlin_loop_clock_monotonic();
    ac.output_id = "x";
    ac.w = 100;
    ac.h = 100;
    ac.dpi = 99.0f; /* outside the output contract: the output refuses */
    CHECK(canlin_app_new(&ac, &err) == NULL && err == CANLIN_APP_ESETUP);
    canlin_app_free(NULL);
}

int main(void)
{
    test_demo();
    test_bad_config();
    printf("test_app: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
