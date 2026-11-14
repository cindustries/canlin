/* CanLin app internals: the assistant session the loader owns, shared by src/app/app.c
 * and src/app/assistant.c.
 *
 * The session is the app's answer to "who may set the model view": the loader owns the
 * view, the gate and the module, and routes the module's events into the gate. The
 * module never sets the model view (module roles spec §3). */
#ifndef CANLIN_APP_INT_H
#define CANLIN_APP_INT_H

#include "canlin/app.h"
#include "canlin/assistant_binding.h"
#include "canlin/assistant_response.h"
#include "canlin/assistant_view.h"
#include "canlin/llm.h"
#include "canlin/module.h"

typedef struct canlin_app_session {
    canlin_llm *llm;                    /* the provider context */
    canlin_assistant_view *av;          /* the model view */
    canlin_assistant_response *gate;    /* the response gate */
    canlin_module *m;                   /* the assistant module */
    canlin_assistant_binding bind;      /* the module's assistant-view capability */
    canlin_module_source src;           /* the module's source (the shell keeps it) */
    canlin_ui_view *attached;           /* the ui view the assistant view draws into */
    canlin_ev_handle cur;               /* the request handle of the open response */
    int have_cur;                       /* 1: cur names a request */
    uint64_t turn;                      /* the scope counter: one per response */
    char profile[CANLIN_LLM_ID_MAX + 1];/* the profile the assistant asks */
} canlin_app_session;

/* The session and its module (NULL without a transport: *err stays CANLIN_APP_OK). */
canlin_app_session *canlin_app_assistant_new(canlin_ev *ev, const canlin_app_config *cfg,
                                               canlin_sandbox_log_fn log, void *log_ud, int *err);
void canlin_app_assistant_free(canlin_app_session *s);

/* The module's deliveries: the gate first (so the module's done handler sees the tool
 * results), then the module. */
void canlin_app_assistant_deliver(canlin_app_session *s, const canlin_ev_delivery *d);

/* Attaches the assistant view to the module's view (the shell gives the module one
 * when it shows it). */
void canlin_app_assistant_update(canlin_app_session *s);

#endif
