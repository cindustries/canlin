/* CanLin assistant-view binding: the privileged Lua API that a module holding the
 * assistant-view capability uses to render the model view and take its submission.
 *
 * Contract: module roles spec §3 (the assistant-view capability grants "the model-view
 * binding (set_view rendering, submissions)"; a capability that is not granted makes the
 * API absent (nil) or answers denied - never a pretend success), UI spec §7 (the submit
 * contract {revision, action, values}), core API §3.1 (nil, code, message).
 *
 * The binding is installed by the trusted loader (the assistant hook of
 * canlin_module_source), never by a module: a module without the capability never sees
 * canlin.view, the field is absent. A revoked grant makes every function answer
 * nil, "denied", <message> - never a pretend success.
 *
 * The binding reaches nothing but the model view it holds and this module's own view:
 * no other module's view, no credentials, no network, no files. It cannot change the
 * model view either: only the response gate does that, on a turn that ended ok.
 *
 * Lua surface (canlin.view):
 *   canlin.view.draw()       -> revision
 *         The model view drawn into this module's own view (the ui of M.draw). The
 *         revision drawn (0: no model view yet). nil, "unavailable", <message> when
 *         this module has no view to draw into.
 *   canlin.view.submission() -> {revision = N, action = "id", values = {...}}
 *         The pending submission, taken: it waits for one consumer. nil when none
 *         waits. nil, "denied", <message> when the capability was revoked.
 */
#ifndef CANLIN_ASSISTANT_BINDING_H
#define CANLIN_ASSISTANT_BINDING_H

#include "canlin/assistant_view.h"

#ifdef __cplusplus
extern "C" {
#endif

struct lua_State;
struct canlin_module;

/* The binding of one module. The loader owns it and it must outlive every generation
 * of the module that uses it. The view is owned by the loader too. */
typedef struct canlin_assistant_binding {
    canlin_assistant_view *view; /* the model view; not owned */
    int granted;                 /* 0: the assistant-view capability was revoked */
} canlin_assistant_binding;

/* The assistant hook of canlin_module_source: installs canlin.view into the `canlin`
 * table on top of the stack. ud is the canlin_assistant_binding *. */
int canlin_assistant_binding_install(struct lua_State *L, struct canlin_module *m, void *ud);

#ifdef __cplusplus
}
#endif

#endif
