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
 * The binding reaches nothing but the model view it holds: the model's widgets live in
 * that view and in no other (ADR 0001), no other module's view, no credentials, no
 * network, no files. It cannot change the model view either: only the response gate
 * does that, on a turn that ended ok.
 *
 * Lua surface (canlin.view):
 *   canlin.view.draw()       -> revision
 *         The model drawn into the model's own view: the model's widgets live in their
 *         own identity space, with their own focus, slot table and rectangle, and the
 *         frame of that view arises in the module's draw. The revision drawn (0: no
 *         model view yet). nil, "unavailable", <message> when the model has no view of
 *         its own to draw into.
 *   canlin.view.submission() -> "<the submission's JSON>"
 *         The pending submission, taken: it waits for one consumer. The bytes are the
 *         submission's own JSON (UI spec §7: {revision, action, values}), so the
 *         values the module hands back to the provider are the values the JSON
 *         carries. nil when none waits. nil, "denied", <message> when the capability
 *         was revoked.
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
