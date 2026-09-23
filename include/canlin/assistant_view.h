/* CanLin assistant-owned view: the set_view GUI tool validator, the view model, its
 * local state, rendering and explicit submit.
 *
 * Contract: docs/superpowers/specs/2026-09-23-canlin-ui-design.md §3, §4, §5, §7, §8;
 * LLM/UI design §5; event runtime §4; core API §3 and D5. Dialect-independent: no
 * provider protocol, HTTP or stream lives here. Owner thread only.
 *
 * set_view argument (JSON object, at most CANLIN_UI_TOOL_MAX_BYTES):
 *   {"title": str?, "widgets": [widget...]}             unknown keys are rejected
 * widget = {"type": T, ...}; "id" is [a-z0-9_-]{1,32}, unique in the view, required for
 * input widgets (field, area, toggle, choice, graph) and buttons, optional elsewhere:
 *   heading   text, level? (1-3, default 1)
 *   text      text, style? ("normal"|"muted"|"danger"|"success")
 *   bubble    role ("user"|"assistant"|"system"), text
 *   field     id, label, value? (str), placeholder?, max? (bytes, 0..MAX_TEXT)
 *   area      id, label, value? (str), rows? (2-20)
 *   toggle    id, label, value? (bool)
 *   choice    id, label, options (1..64 str), index? (1-based, default 1)
 *   meter     label, value (number 0..1)
 *   separator (nothing)
 *   section   title, widgets, open? (bool, default true)
 *   columns   left, right (widget arrays)
 *   graph     id, nodes, edges, height? (em, 5-200)
 *               node = {id, title, x, y, inputs?, outputs?, body?}
 *               port = {id, label?, type?}   body row = {kind:"text", text} |
 *                                                       {kind:"meter", value 0..1}
 *               edge = {from: [node, port], to: [node, port]}
 *   button    id, label, action ("submit")
 *   icon      name (built-in assistant icons only; the list is empty in v1)
 * Strings: text/value up to CANLIN_UI_MAX_TEXT, everything else up to CANLIN_UI_MAX_LABEL
 * bytes; no control character (C0, DEL, C1) in any string (UI spec §7). Containers nest
 * at most CANLIN_UI_MAX_DEPTH deep; at most CANLIN_UI_TOOL_MAX_WIDGETS widgets in total
 * (containers included, graph nodes counted separately against the graph limits).
 *
 * Tool-call identity (LLM/UI §5): the conversation layer sets the scope (module
 * generation, turn). A call carries (gen, turn, call id). A call outside the current
 * scope is refused as "stale": nothing applied, nothing recorded, no success. A call id
 * seen before in the scope returns the recorded result (duplicate = 1) without applying
 * again; the same id with other argument bytes is refused as "invalid".
 *
 * Input (event runtime §4): canlin_assistant_view_input takes the view revision the
 * event was produced against. Events for another revision are dropped. Replacing the view
 * resets the attached ui view's queued input, and every stateful widget identity is
 * scoped by revision, so a queued click can never act on a new widget reusing its id.
 *
 * Submit: pressing a submit button produces exactly one pending submission
 * {revision, action, values} (JSON) with the input widgets of that view only. Typing,
 * toggling, choosing and graph edits stay local. While one submission is pending, further
 * presses are ignored (counted); the conversation layer takes it.
 */
#ifndef CANLIN_ASSISTANT_VIEW_H
#define CANLIN_ASSISTANT_VIEW_H

#include <stddef.h>
#include <stdint.h>

#include "canlin/assistant_limits.h"
#include "canlin/ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- status codes ---- */

enum {
    CANLIN_AV_OK = 0,
    CANLIN_AV_EINVALID = -1, /* argument outside the schema/limits ("invalid") */
    CANLIN_AV_ESTALE = -2,   /* call or input outside the current scope/revision ("stale") */
    CANLIN_AV_EFULL = -3,    /* a count bound refuses admission ("full") */
    CANLIN_AV_ENOMEM = -4,   /* host allocation failed ("unavailable") */
    CANLIN_AV_EARG = -5      /* C API misuse (NULL, called inside a frame) */
};

/* Core API §3.1 code string of a status ("invalid", "stale", "full", "unavailable"). */
const char *canlin_av_code(int status);

typedef struct canlin_av_error {
    int status;                       /* CANLIN_AV_E* */
    char path[CANLIN_AV_PATH_MAX];    /* JSON path of the first failure, e.g. $.widgets[3].id */
    char message[CANLIN_AV_MSG_MAX];  /* diagnostic, never parsed */
} canlin_av_error;

/* ---- the validated view model (immutable once built) ---- */

typedef enum canlin_av_type {
    CANLIN_AV_HEADING = 1, CANLIN_AV_TEXT, CANLIN_AV_BUBBLE, CANLIN_AV_FIELD,
    CANLIN_AV_AREA, CANLIN_AV_TOGGLE, CANLIN_AV_CHOICE, CANLIN_AV_METER,
    CANLIN_AV_SEPARATOR, CANLIN_AV_SECTION, CANLIN_AV_COLUMNS, CANLIN_AV_GRAPH,
    CANLIN_AV_BUTTON, CANLIN_AV_ICON
} canlin_av_type;

typedef struct canlin_av_str {
    const char *s;  /* NUL-terminated for convenience; len is authoritative */
    size_t len;     /* 0 and s == "" when absent */
} canlin_av_str;

typedef struct canlin_av_port {
    canlin_av_str id, label, type;
} canlin_av_port;

typedef struct canlin_av_body {
    int meter;           /* 0: text row, 1: meter row */
    canlin_av_str text;
    float value;
} canlin_av_body;

typedef struct canlin_av_node {
    canlin_av_str id, title;
    float x, y;
    unsigned nin, nout, nbody;
    const canlin_av_port *ports;   /* nin inputs, then nout outputs */
    canlin_av_body body[CANLIN_UI_GRAPH_MAX_BODY];
} canlin_av_node;

typedef struct canlin_av_edge {
    unsigned fn, fp, tn, tp;       /* node index, port index within that node's ports */
} canlin_av_edge;

typedef struct canlin_av_widget canlin_av_widget;

struct canlin_av_widget {
    canlin_av_type type;
    unsigned slot;                 /* flat index in the view, document order, from 0 */
    canlin_av_str id;              /* empty when absent */
    canlin_av_str text;            /* heading/text/bubble text */
    canlin_av_str label;           /* field/area/toggle/choice/meter/button label */
    canlin_av_str title;           /* section title */
    canlin_av_str value;           /* field/area initial text */
    canlin_av_str placeholder;     /* field */
    canlin_av_str name;            /* icon */
    int level;                     /* heading 1-3 */
    int style;                     /* text: 0 normal, 1 muted, 2 danger, 3 success */
    int role;                      /* bubble: 0 user, 1 assistant, 2 system */
    int rows;                      /* area 2-20 (default 4) */
    int on;                        /* toggle initial value */
    int open;                      /* section initially open */
    size_t max;                    /* field max bytes (default CANLIN_UI_MAX_TEXT) */
    unsigned index;                /* choice initial index, 1-based */
    float meter;                   /* meter value 0..1 */
    float height;                  /* graph height, em */
    const canlin_av_str *options;  /* choice */
    unsigned noptions;
    const canlin_av_widget *child; /* section widgets, columns left */
    unsigned nchild;
    const canlin_av_widget *child2;/* columns right */
    unsigned nchild2;
    const canlin_av_node *nodes;   /* graph */
    unsigned nnodes;
    const canlin_av_edge *edges;
    unsigned nedges;
};

typedef struct canlin_av_model {
    canlin_av_str title;
    const canlin_av_widget *widgets;
    unsigned nwidgets;             /* top level */
    unsigned total;                /* every widget, containers included */
} canlin_av_model;

/* ---- validator (push-fed; input may be split anywhere) ---- */

typedef struct canlin_av_parser canlin_av_parser;

canlin_av_parser *canlin_av_parser_new(void);
void canlin_av_parser_free(canlin_av_parser *p);
/* CANLIN_AV_OK or the sticky first error (details at finish). */
int canlin_av_parser_feed(canlin_av_parser *p, const void *data, size_t len);
/* Completes validation. OK: *out is an owned model (free with canlin_av_model_free).
 * Else *out = NULL and *err names the first failure. The parser is spent either way. */
int canlin_av_parser_finish(canlin_av_parser *p, canlin_av_model **out, canlin_av_error *err);
void canlin_av_model_free(canlin_av_model *m);

/* ---- the assistant view ---- */

typedef struct canlin_assistant_view canlin_assistant_view;

canlin_assistant_view *canlin_assistant_view_new(void);
void canlin_assistant_view_free(canlin_assistant_view *av);

/* Scope of the current request/turn (LLM/UI §5). turn 0 = no active turn: every call is
 * stale. A new scope forgets the recorded results of the previous one. */
void canlin_assistant_view_set_scope(canlin_assistant_view *av, uint64_t gen, uint64_t turn);

typedef struct canlin_av_call {
    uint64_t gen, turn;
    const char *id;                /* tool-call id bytes, 1..CANLIN_AV_CALL_ID_MAX */
    size_t id_len;
} canlin_av_call;

typedef struct canlin_av_result {
    int status;                    /* CANLIN_AV_OK or CANLIN_AV_E* */
    int duplicate;                 /* 1: the recorded result of an earlier receipt */
    uint32_t revision;             /* OK: the accepted revision */
    canlin_av_error err;           /* not OK: code/path/message */
} canlin_av_result;

/* The set_view tool: validate the complete argument bytes, then atomically replace the
 * view (revision + 1, fresh local state, attached ui view input reset). On any error the
 * last valid view stays. Returns res->status. EARG inside a frame. */
int canlin_assistant_view_set_view(canlin_assistant_view *av, const canlin_av_call *call,
                                   const void *json, size_t len, canlin_av_result *res);
/* Tool result as JSON for the adapter: {"revision":N} or
 * {"error":{"code":..,"path":..,"message":..}}. Bytes written (without NUL), or 0 if
 * cap is too small. */
size_t canlin_av_result_json(const canlin_av_result *res, char *buf, size_t cap);

/* Current model (NULL before the first accepted view) and revision (0 before). */
const canlin_av_model *canlin_assistant_view_model(const canlin_assistant_view *av);
uint32_t canlin_assistant_view_revision(const canlin_assistant_view *av);

/* ---- rendering and input ---- */

/* The ui view the assistant view is drawn into (NULL detaches). Not owned. */
void canlin_assistant_view_attach(canlin_assistant_view *av, canlin_ui_view *v);
/* Queues one input event produced against `revision` to the attached ui view. ESTALE
 * (dropped) for any other revision or before the first view; EARG without a view;
 * otherwise canlin_ui_view_input's result mapped (EFULL on overflow). */
int canlin_assistant_view_input(canlin_assistant_view *av, uint32_t revision,
                                const canlin_ui_input *in);

struct lua_State;
/* Draws the current model through the `ui` object at ui_idx of L (the per-draw object of
 * canlin_ui_push, inside its frame), updating local state from the widget results and
 * applying the view's own graph edits. Raises a Lua error only if a ui method raises
 * (not for a validated model within the limits). For a built-in assistant module's draw;
 * no Lua binding exists yet. */
void canlin_assistant_view_draw(canlin_assistant_view *av, struct lua_State *L, int ui_idx);
/* One C-driven frame on the attached view: frame_begin, draw on a private Lua state,
 * frame_end. Returns CANLIN_AV_OK, EARG (no view), or ENOMEM/EINVALID when the frame
 * could not start or a ui method raised (message in canlin_assistant_view_errmsg). */
int canlin_assistant_view_frame(canlin_assistant_view *av);
const char *canlin_assistant_view_errmsg(const canlin_assistant_view *av);

/* ---- submit ---- */

typedef struct canlin_av_submission {
    uint32_t revision;
    uint64_t gen, turn;            /* scope of the call that set this revision */
    char action[CANLIN_AV_ID_MAX + 1];
    char *json;                    /* owned: {"revision":N,"action":"id","values":{...}} */
    size_t len;
} canlin_av_submission;

/* 1 while a submission waits to be taken. */
int canlin_assistant_view_pending(const canlin_assistant_view *av);
/* Moves the pending submission into *out (1) or returns 0. Free with
 * canlin_av_submission_free. */
int canlin_assistant_view_take(canlin_assistant_view *av, canlin_av_submission *out);
void canlin_av_submission_free(canlin_av_submission *s);
/* Submit presses not turned into a submission: ignored while one was pending, or
 * refused because the values exceeded CANLIN_AV_SUBMIT_MAX. */
unsigned canlin_assistant_view_submit_refused(const canlin_assistant_view *av);

#ifdef __cplusplus
}
#endif

#endif
