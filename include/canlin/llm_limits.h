/* CanLin LLM providers — numeric limits.
 *
 * v1 values from docs/superpowers/specs/2026-09-23-canlin-llm-providers-design.md §7.
 * Nothing outside this header may hard-code them. Values marked "not in the spec" were
 * chosen by the canlin#38/#39 implementations where the spec is silent.
 */
#ifndef CANLIN_LLM_LIMITS_H
#define CANLIN_LLM_LIMITS_H

#include "canlin/ui_limits.h"

/* Streaming parser limits per response (spec §7). */
#define CANLIN_LLM_SSE_LINE_MAX      (64u * 1024u)
#define CANLIN_LLM_SSE_DATA_MAX      (256u * 1024u)
#define CANLIN_LLM_JSON_DEPTH        32u
#define CANLIN_LLM_JSON_STRING_MAX   (64u * 1024u)
#define CANLIN_LLM_TOOL_ARGS_MAX     CANLIN_UI_TOOL_MAX_BYTES /* 64 KiB */
#define CANLIN_LLM_TOOL_CALLS_MAX    8u
#define CANLIN_LLM_TEXT_MAX          (256u * 1024u)
#define CANLIN_LLM_REQUEST_MAX       (512u * 1024u)
#define CANLIN_LLM_INFLIGHT_MAX      2u
#define CANLIN_LLM_TOOL_ROUNDS_MAX   4u
#define CANLIN_LLM_IDLE_TIMEOUT_MS   60000u

/* Not in the spec: characters of one JSON number token in a stream chunk. */
#define CANLIN_LLM_JSON_NUMBER_MAX   32u
/* Not in the spec: tool call id and tool name bytes (a longer one is `full`). */
#define CANLIN_LLM_TOOL_ID_MAX       256u
#define CANLIN_LLM_TOOL_NAME_MAX     64u
/* Not in the spec: messages in one request (the body limit bounds their bytes). */
#define CANLIN_LLM_MESSAGES_MAX      256u
/* Not in the spec: largest max_tokens a module may ask for. */
#define CANLIN_LLM_MAX_TOKENS_MAX    (1u << 20)
/* Not in the spec: max_tokens sent by anthropic-messages when the request gives none
 * (the Messages API requires the field; Langertha's default_response_size). */
#define CANLIN_LLM_ANTHROPIC_MAX_TOKENS_DEFAULT 1024u
/* Not in the spec: text bytes per "llm" text event; longer pending text is split at a
 * UTF-8 boundary. Half of CANLIN_MARSHAL_MAX_BYTES leaves room for the other fields. */
#define CANLIN_LLM_EVENT_TEXT_MAX    (32u * 1024u)
/* Not in the spec: configured profiles and their string fields. */
#define CANLIN_LLM_PROFILES_MAX      16u
#define CANLIN_LLM_ID_MAX            32u   /* profile/preset id: [a-z0-9_-]{1,32} */
#define CANLIN_LLM_TITLE_MAX         64u
#define CANLIN_LLM_URL_MAX           512u
#define CANLIN_LLM_MODEL_MAX         128u
#define CANLIN_LLM_KEY_MAX           1024u /* credential bytes */
/* Not in the spec: diagnostic message bytes in a failed "done". */
#define CANLIN_LLM_MSG_MAX           200u

#endif
