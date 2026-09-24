/* The shell as the native loop's display (native loop spec §3 step 4, shell.h drawing):
 * canlin_shell_update, then per dirty output begin / canlin_shell_render / end and the
 * backend's present. */
#include "canlin/loop.h"
#include "canlin/render.h"
#include "canlin/shell.h"

static void sh_update(void *ctx)
{
    canlin_loop_shell *s = ctx;
    canlin_shell_update(s->sh);
}

static size_t sh_outputs(void *ctx)
{
    canlin_loop_shell *s = ctx;
    return s->n < CANLIN_LOOP_OUTPUTS_MAX ? s->n : CANLIN_LOOP_OUTPUTS_MAX;
}

static int sh_dirty(void *ctx, size_t i)
{
    canlin_loop_shell *s = ctx;
    return canlin_shell_dirty(s->sh, s->out[i].o);
}

static void sh_frame(void *ctx, size_t i)
{
    canlin_loop_shell *s = ctx;
    canlin_loop_shell_output *o = &s->out[i];
    int rows;
    canlin_render_begin(o->r);
    canlin_shell_render(s->sh, o->o, o->r);
    rows = canlin_render_end(o->r);
    if (o->present)
        o->present(o->ctx, o->r, rows);
}

static int64_t sh_tick(void *ctx)
{
    canlin_loop_shell *s = ctx;
    return canlin_shell_next_tick_ms(s->sh);
}

canlin_loop_display canlin_loop_shell_display(canlin_loop_shell *s)
{
    canlin_loop_display d;
    d.update = sh_update;
    d.outputs = sh_outputs;
    d.dirty = sh_dirty;
    d.frame = sh_frame;
    d.next_tick_ms = sh_tick;
    d.ctx = s;
    return d;
}
