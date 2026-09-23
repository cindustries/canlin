/* canlin_llm_transport over the native HTTP/1.1 + TLS client (HTTP transport spec §2,
 * LLM providers spec §8, canlin#41). The llm.h seam is unchanged: requests, headers and
 * the credential binding map one-to-one onto canlin_http, its callbacks one-to-one onto
 * canlin_llm_stream_cb.
 *
 * Backpressure: canlin_llm_stream_cb.body cannot refuse bytes, so the adapter always
 * accepts; per step at most CANLIN_NET_TURN_BYTES are read per stream and the LLM core
 * bounds what it keeps pending by its own per-response limits (LLM providers spec §7).
 *
 * close() releases the stream at once (safe from inside its callbacks). llm.h calls
 * close "idempotent per stream"; here a second close of the same pointer is not
 * supported: the LLM core closes each stream exactly once (close_stream clears
 * r->stream before calling it).
 *
 * The native loop drives the canlin_http context (canlin_http_pollfds/step) next to
 * canlin_llm_pump and canlin_ev_turn; the context must outlive the canlin_llm using it.
 */
#ifndef CANLIN_LLM_HTTP_H
#define CANLIN_LLM_HTTP_H

#include "canlin/http.h"
#include "canlin/llm.h"

#ifdef __cplusplus
extern "C" {
#endif

canlin_llm_transport canlin_llm_http_transport(canlin_http *h);

#ifdef __cplusplus
}
#endif

#endif
