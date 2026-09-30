/* CanLin Android device layer (canlin#72): NativeActivity through android_native_app_glue.
 *
 * Contract: docs/superpowers/specs/2026-09-30-canlin-android-adapter-design.md §3–§6.1, §7.
 * The glue thread runs android_main and is CanLin's owner thread: it runs canlin_loop with
 * the Android wait below. Glue commands and input are drained by a loop source in step 3
 * (bounded), never inside a Lua callback. What is decided here beyond the Android calls
 * lives in the pure part (src/android/adapter.c, host-tested).
 *
 * Android wait (§3): ALooper_pollOnce on the owner thread's looper is the one blocking
 * primitive. The glue registers its command pipe (LOOPER_ID_MAIN) and input queue
 * (LOOPER_ID_INPUT). The loop's descriptors go into one private epoll set, and that epoll
 * descriptor is registered once with ALooper_addFd (LOOPER_ID_USER). Deviation from the
 * spec's per-descriptor ALooper_addFd: loop sources (canlin_http) close their descriptors
 * on their own, before any removeFd could run, and the KitKat Looper (as read from its
 * source, not reproduced on a device) keeps its record of an fd whose EPOLL_CTL_DEL fails,
 * so a reused fd number would then fail ALooper_addFd. The private set is ours to keep
 * straight: every descriptor is re-armed before each wait (MOD, ADD on ENOENT), which also
 * covers a closed-and-reused number.
 *
 * Immersive sticky mode (§7) is the one thing done on the Android UI thread: the entry
 * canlin_onCreate (named by the manifest's android.app.func_name) runs the glue's
 * ANativeActivity_onCreate and then wraps its onResume and onWindowFocusChanged callbacks,
 * which NativeActivity calls on the UI thread. See "immersive" below.
 */
#define _POSIX_C_SOURCE 200809L
#include <android/configuration.h>
#include <android/input.h>
#include <android/log.h>
#include <android/looper.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>

#include <errno.h>
#include <fcntl.h>
#include <jni.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

#include "canlin/android.h"
#include "canlin/app.h"
#include "canlin/loop.h"
#include "canlin/shell.h"

#define TAG "canlin"
#define LOGI(...) ((void)__android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__))
#define LOGW(...) ((void)__android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__))
#define LOGE(...) ((void)__android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__))

typedef struct droid {
    struct android_app *app;
    canlin_android_life life;
    canlin_android_touch touch;
    canlin_app *core;
    canlin_loop *loop;
    ANativeWindow *win;
    int32_t fmt;             /* buffer format of the last lock, 0 = not locked yet */
    int fmt_bad;             /* unsupported format already logged */
    int need_full;           /* the next present writes every row */
    int metrics;             /* re-read window size and density in the next step */
    int more;                /* bounded draining left work: do not wait */
    unsigned lock_fails;     /* current failure streak of ANativeWindow_lock */
    canlin_ev_time now, retry_at;
    int epfd;
    int reg[CANLIN_LOOP_POLLFDS_MAX];
    size_t nreg;
} droid;

static void core_log(void *ud, int level, const char *line)
{
    (void)ud;
    __android_log_print(level == CANLIN_APP_LOG_ERROR  ? ANDROID_LOG_ERROR
                        : level == CANLIN_APP_LOG_WARN ? ANDROID_LOG_WARN
                                                       : ANDROID_LOG_INFO,
                        TAG, "%s", line);
}

/* ---- present (§5) ---- */

static void present_span(droid *d, int y0, int y1)
{
    canlin_render *r = canlin_app_frame(d->core);
    ANativeWindow_Buffer buf;
    ARect b;
    canlin_android_buffer cb;

    if (d->win == NULL || !canlin_android_life_presentable(&d->life)) {
        d->need_full = 1;
        return;
    }
    if (d->need_full) {
        y0 = 0;
        y1 = r->h;
    }
    b.left = 0;
    b.top = y0;
    b.right = r->w;
    b.bottom = y1;
    if (ANativeWindow_lock(d->win, &buf, &b) != 0) {
        if (d->lock_fails++ == 0)
            LOGW("present: window lock failed; frame kept, retrying");
        d->need_full = 1;
        d->retry_at = d->now + CANLIN_ANDROID_LOCK_RETRY_MS;
        return;
    }
    if (d->lock_fails) {
        LOGI("present: window lock works again after %u failures", d->lock_fails);
        d->lock_fails = 0;
    }
    if (buf.format != d->fmt) {
        LOGI("window buffer %dx%d stride %d format %d", buf.width, buf.height, buf.stride,
             buf.format);
        d->fmt = buf.format;
    }
    if (b.bottom <= b.top) { /* no bounds reported back: the whole buffer is ours */
        b.top = 0;
        b.bottom = buf.height;
    }
    cb.bits = buf.bits;
    cb.w = buf.width;
    cb.h = buf.height;
    cb.stride = buf.stride;
    cb.format = buf.format;
    if (canlin_android_blit(&cb, b.top, b.bottom, r->px, r->pitch, r->w, r->h) != 0 &&
        !d->fmt_bad) {
        LOGE("present: window format %d not supported; nothing drawn", buf.format);
        d->fmt_bad = 1;
    }
    ANativeWindow_unlockAndPost(d->win);
    d->need_full = 0;
    if (buf.width != r->w || buf.height != r->h)
        d->metrics = 1; /* the window changed under us; its resize command follows */
}

/* canlin_app present hook (display step 4, after canlin_render_end). */
static void on_present(void *ctx, canlin_render *r, int rows)
{
    droid *d = ctx;
    int y0 = 0, y1 = 0;
    if (rows <= 0 && !d->need_full)
        return;
    if (!canlin_android_dirty_span(r->dirty, r->h, &y0, &y1) && !d->need_full)
        return;
    present_span(d, y0, y1);
}

/* Re-reads density and window size. Returns 1 if the frame size changed (the display
 * step then draws and presents the whole new frame). */
static int read_metrics(droid *d)
{
    float dpi = canlin_android_dpi(AConfiguration_getDensity(d->app->config));
    canlin_render *r = canlin_app_frame(d->core);
    int32_t w, h;
    int changed;
    if (canlin_app_set_dpi(d->core, dpi) != CANLIN_APP_OK)
        LOGW("output: DPI factor %.2f refused", (double)dpi);
    if (d->win == NULL)
        return 0;
    w = ANativeWindow_getWidth(d->win);
    h = ANativeWindow_getHeight(d->win);
    if (w <= 0 || h <= 0) {
        LOGW("output: window reports %dx%d, size kept", w, h);
        return 0;
    }
    changed = r->w != w || r->h != h;
    if (canlin_app_resize(d->core, w, h) != CANLIN_APP_OK) {
        LOGE("output: resize to %dx%d failed, size kept", w, h);
        return 0;
    }
    d->touch.w = w;
    d->touch.h = h;
    d->need_full = 1;
    LOGI("output android:main %dx%d, DPI factor %.2f", w, h, (double)dpi);
    return changed;
}

/* ---- lifecycle (§4) ---- */

static int cmd_of(int32_t cmd)
{
    switch (cmd) {
    case APP_CMD_INPUT_CHANGED: return CANLIN_ANDROID_CMD_INPUT_CHANGED;
    case APP_CMD_INIT_WINDOW: return CANLIN_ANDROID_CMD_INIT_WINDOW;
    case APP_CMD_TERM_WINDOW: return CANLIN_ANDROID_CMD_TERM_WINDOW;
    case APP_CMD_WINDOW_RESIZED: return CANLIN_ANDROID_CMD_WINDOW_RESIZED;
    case APP_CMD_WINDOW_REDRAW_NEEDED: return CANLIN_ANDROID_CMD_WINDOW_REDRAW_NEEDED;
    case APP_CMD_CONTENT_RECT_CHANGED: return CANLIN_ANDROID_CMD_CONTENT_RECT_CHANGED;
    case APP_CMD_GAINED_FOCUS: return CANLIN_ANDROID_CMD_GAINED_FOCUS;
    case APP_CMD_LOST_FOCUS: return CANLIN_ANDROID_CMD_LOST_FOCUS;
    case APP_CMD_CONFIG_CHANGED: return CANLIN_ANDROID_CMD_CONFIG_CHANGED;
    case APP_CMD_LOW_MEMORY: return CANLIN_ANDROID_CMD_LOW_MEMORY;
    case APP_CMD_START: return CANLIN_ANDROID_CMD_START;
    case APP_CMD_RESUME: return CANLIN_ANDROID_CMD_RESUME;
    case APP_CMD_SAVE_STATE: return CANLIN_ANDROID_CMD_SAVE_STATE;
    case APP_CMD_PAUSE: return CANLIN_ANDROID_CMD_PAUSE;
    case APP_CMD_STOP: return CANLIN_ANDROID_CMD_STOP;
    case APP_CMD_DESTROY: return CANLIN_ANDROID_CMD_DESTROY;
    default: return -1;
    }
}

static const char *cmd_name(int c)
{
    static const char *const names[] = {
        "input changed", "init window", "term window", "window resized", "redraw needed",
        "content rect changed", "gained focus", "lost focus", "config changed",
        "low memory", "start", "resume", "save state", "pause", "stop", "destroy"
    };
    return c >= 0 && c < (int)(sizeof names / sizeof names[0]) ? names[c] : "unknown";
}

static void log_stats(droid *d, const char *when)
{
    canlin_loop_stats st;
    if (d->loop == NULL)
        return;
    canlin_loop_get_stats(d->loop, &st);
    LOGI("%s: loop iterations %llu waits %llu frames %llu", when,
         (unsigned long long)st.iterations, (unsigned long long)st.waits,
         (unsigned long long)st.frames);
}

/* Glue onAppCmd: runs between the glue's pre- and post-processing, on the owner thread,
 * from src_step (or the teardown drain, where core and loop are gone). */
static void on_cmd(struct android_app *app, int32_t cmd)
{
    droid *d = app->userData;
    int c = cmd_of(cmd);
    unsigned act;
    canlin_ui_input in;

    if (d == NULL)
        return;
    act = canlin_android_life_cmd(&d->life, c);
    if (c != CANLIN_ANDROID_CMD_CONTENT_RECT_CHANGED)
        LOGI("lifecycle: %s", cmd_name(c));
    if (act & CANLIN_ANDROID_DO_WINDOW) {
        d->win = app->window;
        if (d->win && ANativeWindow_setBuffersGeometry(d->win, 0, 0, WINDOW_FORMAT_RGBX_8888) != 0)
            LOGW("window: RGBX_8888 geometry refused; converting to what the lock reports");
        d->fmt = 0;
        d->fmt_bad = 0;
        d->lock_fails = 0;
        d->metrics = 1;
    }
    if (act & CANLIN_ANDROID_DO_NO_WINDOW)
        d->win = NULL; /* before the glue's post-processing hands the window back */
    if (act & CANLIN_ANDROID_DO_METRICS)
        d->metrics = 1;
    if ((act & CANLIN_ANDROID_DO_RELEASE) && d->core &&
        canlin_android_touch_release(&d->touch, &in))
        canlin_app_input(d->core, &in);
    if (act & CANLIN_ANDROID_DO_SUSPEND)
        log_stats(d, "suspend");
    if (act & CANLIN_ANDROID_DO_RESUME)
        log_stats(d, "resume");
    if (act & CANLIN_ANDROID_DO_TRIM)
        LOGW("low memory: no releasable caches in this build");
    if (act & CANLIN_ANDROID_DO_PRESENT)
        d->need_full = 1;
    if ((act & CANLIN_ANDROID_DO_QUIT) && d->loop)
        canlin_loop_quit(d->loop);
}

/* ---- input (§6.1) ---- */

/* 1 if handled (the event is finished as handled), 0 to leave it to Android. */
static int on_input(droid *d, AInputEvent *ev)
{
    canlin_ui_input in;
    int32_t type = AInputEvent_getType(ev);
    if (type == AINPUT_EVENT_TYPE_MOTION) {
        canlin_android_motion m;
        int32_t a = AMotionEvent_getAction(ev);
        size_t i, n = AMotionEvent_getPointerCount(ev);
        if (!(AInputEvent_getSource(ev) & AINPUT_SOURCE_CLASS_POINTER))
            return 0;
        m.action = a & AMOTION_EVENT_ACTION_MASK;
        m.index = (size_t)((a & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >>
                           AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
        m.n = n < CANLIN_ANDROID_POINTERS_MAX ? n : CANLIN_ANDROID_POINTERS_MAX;
        for (i = 0; i < m.n; i++) {
            m.id[i] = AMotionEvent_getPointerId(ev, i);
            m.x[i] = AMotionEvent_getX(ev, i);
            m.y[i] = AMotionEvent_getY(ev, i);
        }
        if (canlin_android_touch_map(&d->touch, &m, &in))
            canlin_app_input(d->core, &in);
        return 1;
    }
    if (type == AINPUT_EVENT_TYPE_KEY) {
        int r = canlin_android_key_map(AKeyEvent_getKeyCode(ev), AKeyEvent_getAction(ev), &in);
        if (r < 0)
            return 0;
        if (r > 0)
            canlin_app_input(d->core, &in);
        return 1;
    }
    return 0;
}

/* ---- the Android loop source: glue commands, input, pending present ---- */

static int readable(int fd)
{
    struct pollfd p;
    p.fd = fd;
    p.events = POLLIN;
    p.revents = 0;
    return poll(&p, 1, 0) > 0 && (p.revents & POLLIN);
}

static size_t src_pollfds(void *ctx, struct pollfd *out, size_t max, canlin_ev_time *deadline)
{
    droid *d = ctx;
    (void)out;
    (void)max;
    if (d->more || d->metrics)
        *deadline = 0;
    else if (d->need_full && d->win && canlin_android_life_presentable(&d->life))
        *deadline = d->lock_fails ? d->retry_at : 0;
    return 0;
}

static void src_step(void *ctx, canlin_ev_time now)
{
    droid *d = ctx;
    struct android_app *app = d->app;
    AInputEvent *ev;
    unsigned k;
    int resized = 0;

    d->now = now;
    d->more = 0;
    for (k = 0; k < CANLIN_ANDROID_CMDS_MAX && readable(app->msgread); k++)
        app->cmdPollSource.process(app, &app->cmdPollSource);
    if (k == CANLIN_ANDROID_CMDS_MAX)
        d->more = 1;
    for (k = 0; app->inputQueue && k < CANLIN_ANDROID_INPUTS_MAX &&
                AInputQueue_getEvent(app->inputQueue, &ev) >= 0;
         k++) {
        if (AInputQueue_preDispatchEvent(app->inputQueue, ev))
            continue; /* taken by the IME path; Android hands it back later */
        AInputQueue_finishEvent(app->inputQueue, ev, on_input(d, ev));
    }
    if (k == CANLIN_ANDROID_INPUTS_MAX)
        d->more = 1;
    if (d->metrics) {
        d->metrics = 0;
        resized = read_metrics(d);
    }
    /* After a size change the display step draws and presents the whole new frame;
     * otherwise a pending full present (new window, resume, failed lock) is done here
     * from the current frame, which is always complete. */
    if (!resized && d->need_full && (!d->lock_fails || now >= d->retry_at))
        present_span(d, 0, canlin_app_frame(d->core)->h);
    if (app->destroyRequested)
        canlin_loop_quit(d->loop);
}

/* ---- the Android wait (§3) ---- */

static int ep_add(int epfd, int fd, uint32_t events)
{
    struct epoll_event e;
    memset(&e, 0, sizeof e);
    e.events = events;
    e.data.fd = fd;
    if (epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &e) == 0)
        return 0;
    if (errno == ENOENT && epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &e) == 0)
        return 0;
    return -1;
}

/* Brings the epoll set in line with fds; returns descriptors that could not be armed
 * (marked POLLNVAL, like poll(2) does for a bad descriptor). */
static int ep_sync(droid *d, struct pollfd *fds, size_t n)
{
    size_t i, j;
    int bad = 0;
    for (j = 0; j < d->nreg;) {
        for (i = 0; i < n && fds[i].fd != d->reg[j]; i++)
            ;
        if (i == n) {
            struct epoll_event e;
            memset(&e, 0, sizeof e);
            (void)epoll_ctl(d->epfd, EPOLL_CTL_DEL, d->reg[j], &e); /* EBADF: closed */
            d->reg[j] = d->reg[--d->nreg];
        } else {
            j++;
        }
    }
    for (i = 0; i < n; i++) {
        uint32_t ev = 0;
        if (fds[i].fd < 0)
            continue;
        if (fds[i].events & POLLIN)
            ev |= EPOLLIN;
        if (fds[i].events & POLLOUT)
            ev |= EPOLLOUT;
        if (fds[i].events & POLLPRI)
            ev |= EPOLLPRI;
        /* MOD first: re-arms a number that was closed and reused (the kernel dropped it). */
        if (ep_add(d->epfd, fds[i].fd, ev) != 0) {
            fds[i].revents = POLLNVAL;
            bad++;
            continue;
        }
        for (j = 0; j < d->nreg && d->reg[j] != fds[i].fd; j++)
            ;
        if (j == d->nreg && d->nreg < CANLIN_LOOP_POLLFDS_MAX)
            d->reg[d->nreg++] = fds[i].fd;
    }
    return bad;
}

static int droid_wait(void *ctx, struct pollfd *fds, size_t n, int timeout_ms)
{
    droid *d = ctx;
    struct epoll_event ev[CANLIN_LOOP_POLLFDS_MAX];
    size_t i;
    int r, k, j, ready;

    for (i = 0; i < n; i++)
        fds[i].revents = 0;
    ready = ep_sync(d, fds, n);
    r = ALooper_pollOnce(ready ? 0 : timeout_ms, NULL, NULL, NULL);
    if (r == ALOOPER_POLL_ERROR)
        return -1;
    if (r == LOOPER_ID_MAIN || r == LOOPER_ID_INPUT)
        ready++; /* drained by src_step in the next iteration */
    if (r >= 0) {
        k = epoll_wait(d->epfd, ev, (int)CANLIN_LOOP_POLLFDS_MAX, 0);
        for (j = 0; j < k; j++) {
            short re = 0;
            if (ev[j].events & EPOLLIN)
                re |= POLLIN;
            if (ev[j].events & EPOLLOUT)
                re |= POLLOUT;
            if (ev[j].events & EPOLLPRI)
                re |= POLLPRI;
            if (ev[j].events & EPOLLERR)
                re |= POLLERR;
            if (ev[j].events & EPOLLHUP)
                re |= POLLHUP;
            for (i = 0; i < n; i++)
                if (fds[i].fd == ev[j].data.fd) {
                    fds[i].revents = (short)(re & (fds[i].events | POLLERR | POLLHUP));
                    if (fds[i].revents)
                        ready++;
                }
        }
    }
    return ready;
}

/* ---- immersive sticky mode (§7), on the Android UI thread ----
 *
 * View.setSystemUiVisibility must run on the UI thread (ViewRootImpl.checkThread throws
 * otherwise), and without Java code nothing can be posted to it. NativeActivity calls the
 * ANativeActivity callbacks on that thread with activity->env valid, so the flags are set
 * from wrappers around the glue's onResume and onWindowFocusChanged: the same places the
 * Android documentation names for immersive mode. The wrappers call the glue first, so the
 * owner thread sees RESUME / GAINED_FOCUS exactly as before; the owner itself has nothing
 * to do for immersive mode (CANLIN_ANDROID_DO_IMMERSIVE). The window growing to the full
 * screen arrives on the owner thread as APP_CMD_WINDOW_RESIZED (§5 resize path). The
 * LAYOUT_* flags keep the window at full size while the bars show transiently after an
 * edge swipe, so a swipe does not resize the output. */

static void (*glue_on_resume)(ANativeActivity *a);
static void (*glue_on_focus)(ANativeActivity *a, int focused);

/* 1 and the exception cleared if the last JNI call threw. */
static int jni_threw(JNIEnv *env)
{
    if (!(*env)->ExceptionCheck(env))
        return 0;
    (*env)->ExceptionClear(env);
    return 1;
}

/* activity.getWindow().getDecorView().setSystemUiVisibility(CANLIN_ANDROID_UI_IMMERSIVE) */
static void apply_immersive(ANativeActivity *a, const char *when)
{
    JNIEnv *env = a->env;
    jclass cls;
    jmethodID m;
    jobject win, view;
    int ok = 0;

    if ((*env)->PushLocalFrame(env, 8) != 0) {
        (void)jni_threw(env);
        LOGW("immersive (%s): no JNI local frame", when);
        return;
    }
    cls = (*env)->GetObjectClass(env, a->clazz);
    m = (*env)->GetMethodID(env, cls, "getWindow", "()Landroid/view/Window;");
    if (m == NULL || jni_threw(env))
        goto out;
    win = (*env)->CallObjectMethod(env, a->clazz, m);
    if (win == NULL || jni_threw(env))
        goto out;
    cls = (*env)->GetObjectClass(env, win);
    m = (*env)->GetMethodID(env, cls, "getDecorView", "()Landroid/view/View;");
    if (m == NULL || jni_threw(env))
        goto out;
    view = (*env)->CallObjectMethod(env, win, m);
    if (view == NULL || jni_threw(env))
        goto out;
    cls = (*env)->GetObjectClass(env, view);
    m = (*env)->GetMethodID(env, cls, "setSystemUiVisibility", "(I)V");
    if (m == NULL || jni_threw(env))
        goto out;
    (*env)->CallVoidMethod(env, view, m, (jint)CANLIN_ANDROID_UI_IMMERSIVE);
    ok = !jni_threw(env);
out:
    (void)jni_threw(env); /* never return to Java with an exception pending */
    (*env)->PopLocalFrame(env, NULL);
    if (ok)
        LOGI("immersive (%s): system UI flags 0x%x", when, (unsigned)CANLIN_ANDROID_UI_IMMERSIVE);
    else
        LOGW("immersive (%s): setting the system UI flags failed; bars stay", when);
}

static void ui_on_resume(ANativeActivity *a)
{
    glue_on_resume(a);
    apply_immersive(a, "resume");
}

static void ui_on_focus(ANativeActivity *a, int focused)
{
    glue_on_focus(a, focused);
    if (focused)
        apply_immersive(a, "focus");
}

/* ---- entry ---- */

/* NativeActivity's entry (manifest meta-data android.app.func_name), on the UI thread. */
JNIEXPORT void canlin_onCreate(ANativeActivity *a, void *saved, size_t saved_size);
JNIEXPORT void canlin_onCreate(ANativeActivity *a, void *saved, size_t saved_size)
{
    ANativeActivity_onCreate(a, saved, saved_size);
    glue_on_resume = a->callbacks->onResume;
    glue_on_focus = a->callbacks->onWindowFocusChanged;
    a->callbacks->onResume = ui_on_resume;
    a->callbacks->onWindowFocusChanged = ui_on_focus;
}

/* After a failed start or loop: let the activity finish and serve the glue until it
 * destroys us, so its UI thread never waits on a dead owner. */
static void drain_until_destroy(struct android_app *app)
{
    ANativeActivity_finish(app->activity);
    while (!app->destroyRequested) {
        struct android_poll_source *s = NULL;
        if (ALooper_pollOnce(-1, NULL, NULL, (void **)&s) >= 0 && s != NULL)
            s->process(app, s);
    }
}

void android_main(struct android_app *app)
{
    droid *d = calloc(1, sizeof *d);
    canlin_app_config ac;
    canlin_loop_config lc;
    canlin_loop_source src;
    float dpi;
    int err = 0, w, h, rc;

    if (d == NULL) {
        LOGE("start: out of memory");
        drain_until_destroy(app);
        return;
    }
    d->app = app;
    d->epfd = -1;
    canlin_android_life_init(&d->life);
    app->userData = d;
    app->onAppCmd = on_cmd;
    app->onInputEvent = NULL; /* input is drained by src_step, bounded */
    LOGI("start: API %d, density %d", AConfiguration_getSdkVersion(app->config),
         AConfiguration_getDensity(app->config));

    d->epfd = epoll_create(8);
    if (d->epfd < 0 || fcntl(d->epfd, F_SETFD, FD_CLOEXEC) != 0 ||
        ALooper_addFd(app->looper, d->epfd, LOOPER_ID_USER, ALOOPER_EVENT_INPUT, NULL, NULL) != 1) {
        LOGE("start: epoll/looper setup failed: %s", strerror(errno));
        goto out;
    }

    /* The first window's size is not known yet: start from the screen size in dp; the
     * window's own size replaces it at APP_CMD_INIT_WINDOW. */
    dpi = canlin_android_dpi(AConfiguration_getDensity(app->config));
    w = (int)((float)AConfiguration_getScreenWidthDp(app->config) * dpi);
    h = (int)((float)AConfiguration_getScreenHeightDp(app->config) * dpi);
    if (w <= 0 || h <= 0) {
        w = 640;
        h = 480;
    }
    canlin_android_touch_init(&d->touch, w, h);

    memset(&ac, 0, sizeof ac);
    ac.clock = canlin_loop_clock_monotonic();
    ac.output_id = "android:main";
    ac.w = w;
    ac.h = h;
    ac.dpi = dpi;
    ac.scale = 100;
    ac.input = CANLIN_SHELL_IN_TOUCH;
    ac.wall = canlin_app_local_clock;
    ac.present = on_present;
    ac.present_ctx = d;
    ac.log = core_log;
    d->core = canlin_app_new(&ac, &err);
    if (d->core == NULL) {
        LOGE("start: runtime assembly failed (%d)", err);
        goto out;
    }
    memset(&lc, 0, sizeof lc);
    canlin_app_loop_config(d->core, &lc);
    lc.wait = droid_wait;
    lc.wait_ctx = d;
    d->loop = canlin_loop_new(&lc, &err);
    memset(&src, 0, sizeof src);
    src.pollfds = src_pollfds;
    src.step = src_step;
    src.ctx = d;
    if (d->loop == NULL || canlin_loop_add_source(d->loop, &src) != CANLIN_LOOP_OK) {
        LOGE("start: loop setup failed (%d)", err);
        goto out;
    }

    rc = canlin_loop_run(d->loop);
    if (rc != CANLIN_LOOP_OK)
        LOGE("loop ended with %d", rc);
    log_stats(d, "exit");

out:
    canlin_loop_free(d->loop);
    d->loop = NULL;
    canlin_app_free(d->core);
    d->core = NULL;
    if (d->epfd >= 0) {
        ALooper_removeFd(app->looper, d->epfd);
        close(d->epfd);
    }
    if (!app->destroyRequested)
        drain_until_destroy(app);
    app->userData = NULL;
    free(d);
    LOGI("exit");
}
