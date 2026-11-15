/* CanLin assistant-view binding: the privileged Lua surface a module with the
 * assistant-view capability uses. See include/canlin/assistant_binding.h for the
 * contract.
 *
 * Isolation: the closures carry the binding as a light-userdata upvalue that Lua cannot
 * read (no debug library). The binding reaches the model view through the assistant
 * view - the model's widgets live in that view and in no other. The grant is the
 * loader's: revoked, every answer is nil, "denied", ... and never a pretend success.
 */
#include "canlin/assistant_binding.h"
#include "canlin/module.h"

#include "lua.h"
#include "lauxlib.h"

static canlin_assistant_binding *bind_of(lua_State *L)
{
    return lua_touserdata(L, lua_upvalueindex(1));
}

static int lfail(lua_State *L, const char *code, const char *msg)
{
    lua_pushnil(L);
    lua_pushstring(L, code);
    lua_pushstring(L, msg);
    return 3;
}

/* canlin.view.draw(): the model drawn into the model's own view. */
static int l_draw(lua_State *L)
{
    canlin_assistant_binding *b = bind_of(L);
    int rc;
    if (!b->granted)
        return lfail(L, "denied", "the assistant-view capability was revoked");
    /* The model has a view of its own (ADR 0001): its widgets live in their own
     * identity space, with their own focus, slot table and rectangle, and nothing the
     * module draws can reach into them. The frame of that view arises here, inside the
     * module's draw: the app asks for the redraw by invalidating the module's
     * generation, and the shell draws the model's frame when the frame is done. */
    rc = canlin_assistant_view_frame(b->view);
    switch (rc) {
    case CANLIN_AV_OK:
        lua_pushinteger(L, (lua_Integer)canlin_assistant_view_revision(b->view));
        return 1;
    case CANLIN_AV_EARG:
        return lfail(L, "unavailable", "the model has no view of its own to draw into");
    default:
        /* The frame could not start or a ui method raised: the assistant view's own
         * message says which. */
        return lfail(L, "unavailable", canlin_assistant_view_errmsg(b->view));
    }
}

/* canlin.view.submission(): the pending submission, taken: its own JSON bytes. */
static int l_submission(lua_State *L)
{
    canlin_assistant_binding *b = bind_of(L);
    int rc;
    if (!b->granted)
        return lfail(L, "denied", "the assistant-view capability was revoked");
    rc = canlin_assistant_view_take_json(b->view, L);
    switch (rc) {
    case CANLIN_AV_OK:
        return 1; /* the submission's JSON */
    case CANLIN_AV_ESTALE:
        lua_pushnil(L);
        return 1; /* no submission waits */
    case CANLIN_AV_ENOMEM:
        return lfail(L, "unavailable", "out of memory");
    default:
        /* EARG is C API misuse (a NULL view or state): canlin_av_code maps it to
         * "invalid" like every code the house does not name, and a module cannot cause
         * it. */
        return lfail(L, "invalid", "the submission could not be read");
    }
}

int canlin_assistant_binding_install(lua_State *L, canlin_module *m, void *ud)
{
    canlin_assistant_binding *b = ud;
    /* The module is not needed: the model has a view of its own, and the binding
     * reaches it through the assistant view. */
    (void)m;
    if (b == NULL || b->view == NULL)
        return 0;
    lua_createtable(L, 0, 2);
    lua_pushlightuserdata(L, b);
    lua_pushcclosure(L, l_draw, 1);
    lua_setfield(L, -2, "draw");
    lua_pushlightuserdata(L, b);
    lua_pushcclosure(L, l_submission, 1);
    lua_setfield(L, -2, "submission");
    lua_setfield(L, -2, "view"); /* the `canlin` table is on top of the stack */
    return 0;
}
