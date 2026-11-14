/* CanLin assistant-view binding: the privileged Lua surface a module with the
 * assistant-view capability uses. See include/canlin/assistant_binding.h for the
 * contract.
 *
 * Isolation: the closures carry the binding and the module as light-userdata upvalues
 * that Lua cannot read (no debug library). The binding reaches the model view it holds
 * and the module's own view - nothing else. The grant is the loader's: revoked, every
 * answer is nil, "denied", ... and never a pretend success.
 */
#include "canlin/assistant_binding.h"
#include "canlin/module.h"
#include "canlin/ui.h"

#include "lua.h"
#include "lauxlib.h"

static canlin_assistant_binding *bind_of(lua_State *L)
{
    return lua_touserdata(L, lua_upvalueindex(1));
}

static canlin_module *mod_of(lua_State *L)
{
    return lua_touserdata(L, lua_upvalueindex(2));
}

static int lfail(lua_State *L, const char *code, const char *msg)
{
    lua_pushnil(L);
    lua_pushstring(L, code);
    lua_pushstring(L, msg);
    return 3;
}

/* canlin.view.draw(): the model view drawn into this module's own view. */
static int l_draw(lua_State *L)
{
    canlin_assistant_binding *b = bind_of(L);
    canlin_module *m = mod_of(L);
    canlin_ui_view *v;
    if (!b->granted)
        return lfail(L, "denied", "the assistant-view capability was revoked");
    v = canlin_module_view(m);
    if (v == NULL)
        return lfail(L, "unavailable", "this module has no view to draw into");
    /* The module's own view, pushed for this draw: the model view is drawn into the
     * module's view and into nothing else. */
    canlin_ui_push(L, v);
    canlin_assistant_view_draw(b->view, L, lua_gettop(L));
    lua_pop(L, 1);
    lua_pushinteger(L, (lua_Integer)canlin_assistant_view_revision(b->view));
    return 1;
}

/* canlin.view.submission(): the pending submission, taken. */
static int l_submission(lua_State *L)
{
    canlin_assistant_binding *b = bind_of(L);
    int rc;
    if (!b->granted)
        return lfail(L, "denied", "the assistant-view capability was revoked");
    rc = canlin_assistant_view_take_lua(b->view, L);
    switch (rc) {
    case CANLIN_AV_OK:
        return 1; /* the table */
    case CANLIN_AV_ESTALE:
        lua_pushnil(L);
        return 1; /* no submission waits */
    case CANLIN_AV_ENOMEM:
        return lfail(L, "unavailable", "out of memory");
    default:
        return lfail(L, "invalid", "the submission could not be read");
    }
}

int canlin_assistant_binding_install(lua_State *L, canlin_module *m, void *ud)
{
    canlin_assistant_binding *b = ud;
    if (b == NULL || b->view == NULL)
        return 0;
    lua_createtable(L, 0, 2);
    lua_pushlightuserdata(L, b);
    lua_pushlightuserdata(L, m);
    lua_pushcclosure(L, l_draw, 2);
    lua_setfield(L, -2, "draw");
    lua_pushlightuserdata(L, b);
    lua_pushlightuserdata(L, m);
    lua_pushcclosure(L, l_submission, 2);
    lua_setfield(L, -2, "submission");
    lua_setfield(L, -2, "view"); /* the `canlin` table is on top of the stack */
    return 0;
}
