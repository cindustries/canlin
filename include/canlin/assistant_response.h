/* CanLin assistant response gate: the tool calls of one model response are acted on only
 * after that response completed successfully.
 *
 * Contract: LLM providers spec §2.3 (decided 2026-09-23, canlin#38): a tool call is acted
 * on only after `done` with ok = true; if the stream fails after a tool event, the
 * assistant discards that call. LLM/UI design §4 (text already shown on a failed stream
 * stays and is marked partial), §5 (tool-call identity, no success for rejected or stale
 * work). Dialect-independent: it consumes the normalised events of LLM providers §4
 * ("llm" text/tool, "done"), not a wire protocol. Owner thread only.
 *
 * Life of one response (one canlin.llm request of the conversation):
 *   begin(gen, turn)   open; forgets the previous response
 *   text(n)*, tool()*  in stream order; tool calls are copied and held, never executed
 *   finish(ok, ...)    exactly once. ok: every held call is dispatched in arrival order
 *                      (set_view to the assistant view, with the response's gen/turn and
 *                      the call id), its result recorded. Not ok: every held call is
 *                      discarded unexecuted, and the outcome says so (partial, discarded).
 * Events after finish (late tool, second finish) are refused as ESTALE and change
 * nothing, so a call is acted on at most once per response. A bound hit while holding
 * (count, size) fails the response at once ("full"): no subset of a response is applied.
 *
 * The caller owns scope: canlin_assistant_view_set_scope(av, gen, turn) with the same
 * gen/turn as begin, before finish. A response whose scope is no longer the view's gets
 * "stale" results from the view and changes nothing. Discarded calls are not part of the
 * conversation: they get no tool result and must not be sent back to the provider as
 * assistant tool_calls.
 */
#ifndef CANLIN_ASSISTANT_RESPONSE_H
#define CANLIN_ASSISTANT_RESPONSE_H

#include <stddef.h>
#include <stdint.h>

#include "canlin/assistant_view.h"
#include "canlin/llm_limits.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum canlin_ar_state {
    CANLIN_AR_IDLE = 0,    /* no response begun */
    CANLIN_AR_OPEN,        /* collecting */
    CANLIN_AR_COMPLETE,    /* done ok = true: held calls dispatched */
    CANLIN_AR_FAILED,      /* done ok = false or a bound hit: held calls discarded */
    CANLIN_AR_CANCELLED    /* cancelled by the conversation: held calls discarded */
} canlin_ar_state;

typedef struct canlin_ar_call {
    const char *id, *name, *args;   /* owned by the gate, valid until the next begin/free */
    size_t id_len, name_len, args_len;
    int dispatched;                 /* 1: handed to its tool (set_view) after done ok */
    canlin_av_result res;           /* complete: the tool result (unknown tool: EINVALID) */
} canlin_ar_call;

typedef struct canlin_ar_outcome {
    canlin_ar_state state;
    /* 1 when the response did not complete but something of it was received: the
     * conversation marks the shown text partial and states the discarded calls. */
    int partial;
    size_t text_bytes;              /* text received (shown by the conversation) */
    unsigned calls;                 /* tool calls received */
    unsigned dispatched;            /* of them handed to their tool (complete only) */
    unsigned discarded;             /* of them dropped unexecuted (failed/cancelled) */
    char code[16];                  /* failed: core API §3.1 code; else "" */
    char message[CANLIN_LLM_MSG_MAX]; /* failed: diagnostic (never parsed); else "" */
} canlin_ar_outcome;

typedef struct canlin_assistant_response canlin_assistant_response;

canlin_assistant_response *canlin_assistant_response_new(void);
void canlin_assistant_response_free(canlin_assistant_response *r);

/* Opens a response for scope (gen, turn), turn >= 1. Any earlier response and its held
 * calls are forgotten. CANLIN_AV_OK or CANLIN_AV_EARG. */
int canlin_assistant_response_begin(canlin_assistant_response *r, uint64_t gen,
                                    uint64_t turn);
/* A text fragment of n bytes was received (and shown). ESTALE unless open. */
int canlin_assistant_response_text(canlin_assistant_response *r, size_t n);
/* A complete tool call was received: copied and held, nothing executed. ESTALE unless
 * open; EFULL beyond CANLIN_LLM_TOOL_CALLS_MAX calls, an id beyond
 * CANLIN_LLM_TOOL_ID_MAX, a name beyond CANLIN_LLM_TOOL_NAME_MAX or arguments beyond
 * CANLIN_LLM_TOOL_ARGS_MAX (the response is then failed with "full"); ENOMEM likewise
 * ("unavailable"); EARG for a missing id or name. */
int canlin_assistant_response_tool(canlin_assistant_response *r, const char *id,
                                   size_t id_len, const char *name, size_t name_len,
                                   const char *args, size_t args_len);
/* The response's `done`. ok != 0: dispatch every held call to av in arrival order
 * (av required when calls are held). ok == 0: discard them; code/message (may be NULL)
 * are recorded bounded. ESTALE unless open: a second done dispatches nothing. Returns
 * CANLIN_AV_OK when the finish was taken (per-call results are in the calls). */
int canlin_assistant_response_finish(canlin_assistant_response *r,
                                     canlin_assistant_view *av, int ok, const char *code,
                                     const char *message);
/* The conversation cancelled the request: like a failed finish with state CANCELLED.
 * ESTALE unless open (cancel after done changes nothing: cancel is not rollback). */
int canlin_assistant_response_cancel(canlin_assistant_response *r);

const canlin_ar_outcome *canlin_assistant_response_outcome(const canlin_assistant_response *r);
/* Held/dispatched calls of the current response in arrival order (NULL past the end).
 * After a failed or cancelled finish there are none: discarded calls are dropped. */
unsigned canlin_assistant_response_ncalls(const canlin_assistant_response *r);
const canlin_ar_call *canlin_assistant_response_call(const canlin_assistant_response *r,
                                                     unsigned i);

#ifdef __cplusplus
}
#endif

#endif
