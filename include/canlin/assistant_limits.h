/* CanLin assistant view (set_view GUI tool) — numeric limits.
 *
 * The contract limits are in ui_limits.h (UI spec §8: CANLIN_UI_TOOL_MAX_BYTES,
 * CANLIN_UI_TOOL_MAX_WIDGETS, CANLIN_UI_MAX_DEPTH, the graph limits). The values below
 * are not in the §8 table: chosen for canlin#20 (provisional, to be checked on P2).
 */
#ifndef CANLIN_ASSISTANT_LIMITS_H
#define CANLIN_ASSISTANT_LIMITS_H

#include "canlin/ui_limits.h"

/* JSON values (containers, keys excluded, scalars) in one set_view argument. A value
 * needs at least ~4 source bytes in any view that fits CANLIN_UI_TOOL_MAX_BYTES and is
 * made of real widgets, so this bounds the transient parse tree, not real views. */
#define CANLIN_AV_MAX_VALUES        (CANLIN_UI_TOOL_MAX_BYTES / 4u)
/* Members of one JSON object (the richest widget has 8 keys). */
#define CANLIN_AV_MAX_KEYS          16u
/* Characters of a JSON number (no view needs more). */
#define CANLIN_AV_MAX_NUMBER        32u
/* Widget and graph-element ids: [a-z0-9_-]{1,CANLIN_AV_ID_MAX}. */
#define CANLIN_AV_ID_MAX            32u
/* Tool-call id bytes (provider-assigned, opaque). */
#define CANLIN_AV_CALL_ID_MAX       128u
/* Recorded tool-call results per turn scope (duplicate receipt returns them). A turn
 * with more calls is refused with "full"; results are never evicted within a scope. */
#define CANLIN_AV_CALLS_MAX         16u
/* Structured error: JSON path and message bytes including the NUL. */
#define CANLIN_AV_PATH_MAX          160u
#define CANLIN_AV_MSG_MAX           128u
/* One submission's JSON bytes. A submit whose values do not fit is refused visibly
 * (canlin_assistant_view_submit_refused), never truncated. */
#define CANLIN_AV_SUBMIT_MAX        (256u * 1024u)
/* Heap of the private Lua state that drives the ui object for C-driven frames. */
#define CANLIN_AV_LUA_MEM           (8u * 1024u * 1024u)

#endif
