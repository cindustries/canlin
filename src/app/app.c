/* CanLin minimal runtime assembly (canlin#72). See include/canlin/app.h. */
#define _POSIX_C_SOURCE 200809L
#include "canlin/app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "canlin/theme.h"

/* The built-in demo module: enough widgets to prove present, touch and the on-screen
 * keyboard (field focus on a touch output). print() goes to the backend's log. */
static const char DEMO_SRC[] =
    "local M = {}\n"
    "local name = ''\n"
    "local shown = 'Tap the field, type a name, then press Greet.'\n"
    "local presses = 0\n"
    "function M.init() end\n"
    "function M.draw(ui)\n"
    "  ui:heading('CanLin')\n"
    "  ui:text('First light: this view is a Lua module drawn by the CanLin runtime.',\n"
    "          {style = 'muted'})\n"
    "  name = ui:field('Name', name, {placeholder = 'Your name', max = 64})\n"
    "  if ui:button('Greet') then\n"
    "    presses = presses + 1\n"
    "    if name == '' then\n"
    "      shown = 'Hello, whoever you are! (' .. presses .. ')'\n"
    "    else\n"
    "      shown = 'Hello, ' .. name .. '! (' .. presses .. ')'\n"
    "    end\n"
    "    print('greet ' .. presses .. ' ' .. name)\n"
    "  end\n"
    "  ui:text(shown)\n"
    "end\n"
    "return M\n";

struct canlin_app {
    canlin_app_config cfg;
    canlin_ev *ev;
    canlin_ui *ui;
    canlin_ui_output *out;
    canlin_shell *sh;
    canlin_module *m;
    canlin_module_source src;
    canlin_render r;
    unsigned char *px, *dirty, *shadow;
    canlin_loop_shell ls;
    canlin_loop_display inner;   /* the shell display this one wraps */
    unsigned errors;             /* module error count last logged */
    canlin_module_state state;   /* module state last logged */
};

static void app_log(canlin_app *a, int level, const char *line)
{
    if (a->cfg.log)
        a->cfg.log(a->cfg.log_ud, level, line);
}

static void deliver(void *ud, canlin_ev *ev, const canlin_ev_delivery *d)
{
    canlin_app *a = ud;
    (void)ev;
    if (a->m && canlin_module_gen(a->m) == d->gen)
        canlin_module_deliver(a->m, d);
}

static void print_log(void *ud, const char *msg, size_t len)
{
    canlin_app *a = ud;
    char line[256];
    int n = len > 200 ? 200 : (int)len;
    snprintf(line, sizeof line, "demo: %.*s%s", n, msg, len > 200 ? "..." : "");
    app_log(a, CANLIN_APP_LOG_INFO, line);
}

static void theme_log(void *ud, const char *line)
{
    app_log(ud, CANLIN_APP_LOG_WARN, line);
}

/* (Re)allocates the frame for w x h; the render reports the next frame whole. */
static int frame_alloc(canlin_app *a, int w, int h)
{
    size_t pitch = (size_t)w * CANLIN_RENDER_BPP;
    unsigned char *px = calloc((size_t)h, pitch);
    unsigned char *dirty = calloc((size_t)h, 1);
    unsigned char *shadow = calloc((size_t)h, pitch);
    if (px == NULL || dirty == NULL || shadow == NULL ||
        canlin_render_init(&a->r, px, w, h, pitch, dirty, shadow) != 0) {
        free(px);
        free(dirty);
        free(shadow);
        return CANLIN_APP_ENOMEM;
    }
    free(a->px);
    free(a->dirty);
    free(a->shadow);
    a->px = px;
    a->dirty = dirty;
    a->shadow = shadow;
    return CANLIN_APP_OK;
}

/* Display wrapper: the shell's update, then one log line per new module error or state. */
static void d_update(void *ctx)
{
    canlin_app *a = ctx;
    canlin_module_state st;
    unsigned e;
    char line[320];
    a->inner.update(a->inner.ctx);
    st = canlin_module_get_state(a->m);
    e = canlin_module_errors(a->m);
    if (st != a->state || e > a->errors) {
        snprintf(line, sizeof line, "demo module %s: %s",
                 st == CANLIN_MODULE_RUNNING ? "error" :
                 st == CANLIN_MODULE_FAULTED ? "faulted" : "stopped",
                 canlin_module_errmsg(a->m));
        app_log(a, st == CANLIN_MODULE_RUNNING ? CANLIN_APP_LOG_WARN : CANLIN_APP_LOG_ERROR, line);
    }
    a->state = st;
    a->errors = e;
}

static size_t d_outputs(void *ctx)
{
    canlin_app *a = ctx;
    return a->inner.outputs(a->inner.ctx);
}

static int d_dirty(void *ctx, size_t i)
{
    canlin_app *a = ctx;
    return a->inner.dirty(a->inner.ctx, i);
}

static void d_frame(void *ctx, size_t i)
{
    canlin_app *a = ctx;
    a->inner.frame(a->inner.ctx, i);
}

static int64_t d_tick(void *ctx)
{
    canlin_app *a = ctx;
    return a->inner.next_tick_ms(a->inner.ctx);
}

canlin_app *canlin_app_new(const canlin_app_config *cfg, int *err)
{
    canlin_app *a;
    canlin_ev_callbacks cb;
    canlin_theme t;
    canlin_ui_output_desc od;
    canlin_shell_config sc;
    canlin_sandbox_config sb;
    int e = CANLIN_APP_OK, rc;

    if (err)
        *err = CANLIN_APP_OK;
    if (cfg == NULL || cfg->clock.now == NULL || cfg->output_id == NULL || cfg->w <= 0 ||
        cfg->h <= 0) {
        if (err)
            *err = CANLIN_APP_EINVAL;
        return NULL;
    }
    a = calloc(1, sizeof *a);
    if (a == NULL) {
        if (err)
            *err = CANLIN_APP_ENOMEM;
        return NULL;
    }
    a->cfg = *cfg;

    cb.deliver = deliver;
    a->ev = canlin_ev_new(&a->cfg.clock, &cb, a);
    canlin_theme_resolve(&t, NULL, 0, theme_log, a); /* built-in theme until canlin#2 */
    a->ui = a->ev ? canlin_ui_new(&t) : NULL;
    memset(&od, 0, sizeof od);
    od.id = cfg->output_id;
    od.w = cfg->w;
    od.h = cfg->h;
    od.dpi = cfg->dpi;
    od.scale = cfg->scale ? cfg->scale : 100;
    a->out = a->ui ? canlin_ui_output_new(&od, &rc) : NULL;
    memset(&sc, 0, sizeof sc);
    sc.ui = a->ui;
    sc.clock = cfg->wall;
    sc.clock_ud = cfg->wall_ud;
    a->sh = a->out ? canlin_shell_new(&sc, &rc) : NULL;
    if (a->sh == NULL || canlin_shell_add_output(a->sh, a->out, cfg->input) != CANLIN_SHELL_OK) {
        app_log(a, CANLIN_APP_LOG_ERROR, "setup: scheduler, ui, output or shell refused");
        e = CANLIN_APP_ESETUP;
        goto fail;
    }
    if (frame_alloc(a, cfg->w, cfg->h) != CANLIN_APP_OK) {
        e = CANLIN_APP_ENOMEM;
        goto fail;
    }

    a->src.name = "demo";
    a->src.version = "1";
    a->src.main = DEMO_SRC;
    a->src.main_len = sizeof DEMO_SRC - 1;
    memset(&sb, 0, sizeof sb);
    sb.log = print_log;
    sb.ud = a;
    a->m = canlin_module_load(a->ev, &a->src, &sb, &rc);
    if (a->m == NULL || canlin_shell_add_module(a->sh, a->m, &a->src) != CANLIN_SHELL_OK) {
        char line[128];
        snprintf(line, sizeof line, "setup: demo module refused (%d)", rc);
        app_log(a, CANLIN_APP_LOG_ERROR, line);
        e = CANLIN_APP_ESETUP;
        goto fail;
    }

    a->ls.sh = a->sh;
    a->ls.out[0].o = a->out;
    a->ls.out[0].r = &a->r;
    a->ls.out[0].present = cfg->present;
    a->ls.out[0].ctx = cfg->present_ctx;
    a->ls.n = 1;
    a->inner = canlin_loop_shell_display(&a->ls);
    return a;

fail:
    canlin_app_free(a);
    if (err)
        *err = e;
    return NULL;
}

void canlin_app_free(canlin_app *a)
{
    if (a == NULL)
        return;
    if (a->sh)
        canlin_shell_free(a->sh);
    canlin_module_free(a->m);
    if (a->ev)
        canlin_ev_free(a->ev);
    if (a->out)
        canlin_ui_output_free(a->out);
    if (a->ui)
        canlin_ui_free(a->ui);
    free(a->px);
    free(a->dirty);
    free(a->shadow);
    free(a);
}

void canlin_app_loop_config(canlin_app *a, canlin_loop_config *lc)
{
    lc->ev = a->ev;
    lc->display.update = d_update;
    lc->display.outputs = d_outputs;
    lc->display.dirty = d_dirty;
    lc->display.frame = d_frame;
    lc->display.next_tick_ms = d_tick;
    lc->display.ctx = a;
}

int canlin_app_resize(canlin_app *a, int w, int h)
{
    int cw, ch, rc;
    if (a == NULL || w <= 0 || h <= 0)
        return CANLIN_APP_EINVAL;
    canlin_ui_output_size(a->out, &cw, &ch);
    if (cw == w && ch == h && a->r.w == w && a->r.h == h)
        return CANLIN_APP_OK;
    rc = frame_alloc(a, w, h);
    if (rc != CANLIN_APP_OK)
        return rc;
    return canlin_ui_output_set_size(a->out, w, h) == CANLIN_UI_OK ? CANLIN_APP_OK
                                                                     : CANLIN_APP_EINVAL;
}

int canlin_app_set_dpi(canlin_app *a, float dpi)
{
    if (a == NULL)
        return CANLIN_APP_EINVAL;
    return canlin_ui_output_set_dpi(a->out, dpi) == CANLIN_UI_OK ? CANLIN_APP_OK
                                                                  : CANLIN_APP_EINVAL;
}

int canlin_app_input(canlin_app *a, const canlin_ui_input *in)
{
    if (a == NULL || in == NULL)
        return CANLIN_APP_EINVAL;
    return canlin_shell_input(a->sh, a->out, in);
}

canlin_render *canlin_app_frame(canlin_app *a)
{
    return a ? &a->r : NULL;
}

canlin_module *canlin_app_module(canlin_app *a)
{
    return a ? a->m : NULL;
}

canlin_ui_output *canlin_app_output(canlin_app *a)
{
    return a ? a->out : NULL;
}

int64_t canlin_app_local_clock(void *ud)
{
    time_t now = time(NULL);
    struct tm tm;
    (void)ud;
    if (localtime_r(&now, &tm) == NULL)
        return (int64_t)(now % 86400);
    return (int64_t)tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec;
}
