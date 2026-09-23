/* CanLin graph editor widget (ui:graph) — internal seam between src/ui/ui.c and
 * src/ui/graph.c. Contract: docs/superpowers/specs/2026-09-23-canlin-ui-design.md §5, §8.
 *
 * ui.c owns the view (identity, layout box, input, focus); graph.c owns validation of
 * the module's graph table, the per-graph interaction state, drawing and the edits.
 * Everything graph.c needs from the view arrives in one canlin_ui_graph_env, filled by
 * one function in ui.c (graph_env), so display metrics are read in one place.
 */
#ifndef CANLIN_UI_GRAPH_H
#define CANLIN_UI_GRAPH_H

#include <stdint.h>

#include "canlin/nk.h"
#include "canlin/theme.h"
#include "canlin/ui.h"

struct lua_State;

/* Per graph identity, kept in the view across draws (core UI state, spec §5.3). */
typedef struct canlin_ui_graph_state {
    uint64_t id;             /* view identity of the graph; 0 = free entry */
    uint32_t frame;          /* last frame that drew it */
    float panx, pany;        /* pan offset, graph space em */
    unsigned char fit_prev;  /* opts.fit of the previous draw (fit is edge-triggered) */
    unsigned char drag;      /* G_DRAG_* of graph.c; 0 = no press in progress */
    unsigned char moved;     /* pointer left the slop since the press */
    unsigned char has_tap;   /* tap_t/tap_node describe the last tap on a node */
    float px0, py0;          /* press position, screen px */
    float ax, ay;            /* drag anchor: pan or node origin at press, em */
    uint64_t press_t;        /* timestamp of the press edge */
    uint64_t dnode, dport;   /* dragged/pressed node and port id hashes */
    int dport_out;           /* pressed port is an output */
    int dedge;               /* edge index taken off its input, or tapped edge; -1 */
    uint64_t tap_t, tap_node;
} canlin_ui_graph_state;

/* Twice the per-draw maximum: after a frame at most MAX_PER_DRAW entries are live. */
#define CANLIN_UI_GRAPH_STATES (2u * CANLIN_UI_GRAPH_MAX_PER_DRAW)

typedef struct canlin_ui_graph_env {
    canlin_ui_view *view;             /* for canlin_ui_graph_record */
    struct nk_context *nk;
    const canlin_theme *theme;
    const struct nk_user_font *font;  /* body font */
    float em;                         /* px per em of this view */
    struct nk_rect vis;               /* the view's visible screen rect */
    struct nk_rect box;               /* the canvas, screen px (whole pixels) */
    canlin_ui_graph_state *st;
    uint64_t id;                      /* the graph's identity */
    uint64_t *focus;                  /* the view's keyboard focus identity */
    uint64_t btn_t;                   /* timestamp of this frame's button edge */
} canlin_ui_graph_env;

typedef struct canlin_ui_graph canlin_ui_graph;

/* Validates the graph table at gidx and opts at oidx (nil or table) against spec §5.1
 * and §8, charging the running callback. Raises a Lua error on any violation, before
 * any Nuklear call. Leaves one userdata (the parsed graph) on the stack; it borrows the
 * strings of the graph table, which must stay on the stack. *height_em, *fit: opts. */
canlin_ui_graph *canlin_ui_graph_parse(struct lua_State *L, int gidx, int oidx,
                                       float *height_em, int *fit);

/* Finds or claims the state entry of identity id in states[CANLIN_UI_GRAPH_STATES]. NULL
 * when CANLIN_UI_GRAPH_MAX_PER_DRAW graphs were already drawn in this frame. */
canlin_ui_graph_state *canlin_ui_graph_state_get(canlin_ui_graph_state *states, uint64_t id,
                                                 uint32_t frame);
/* Frees the entries not drawn in `frame` (called when that frame ends). */
void canlin_ui_graph_state_evict(canlin_ui_graph_state *states, uint32_t frame);

/* Handles this frame's input, draws the graph into e->box and pushes the array of edits.
 * Every Nuklear call happens before the first Lua allocation. */
void canlin_ui_graph_run(struct lua_State *L, canlin_ui_graph *g, int fit,
                         canlin_ui_graph_env *e);

/* Defined in ui.c: appends a CANLIN_UI_W_GRAPH layout record entry (screen px). */
void canlin_ui_graph_record(canlin_ui_view *v, int part, struct nk_rect r, uint32_t off,
                            uint32_t len);

#endif
