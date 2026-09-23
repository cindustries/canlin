/* CanLin native HTTP/1.1 + TLS transport — numeric limits.
 *
 * v1 values from docs/superpowers/specs/2026-09-23-canlin-http-transport-design.md §8.
 * Nothing outside this header may hard-code them. Values marked "not in the spec" were
 * chosen by the canlin#41 implementation where the spec is silent.
 */
#ifndef CANLIN_NET_LIMITS_H
#define CANLIN_NET_LIMITS_H

#define CANLIN_NET_STREAMS_MAX           4u      /* live requests (process) */
#define CANLIN_NET_DNS_JOBS_MAX          4u
#define CANLIN_NET_DNS_TIMEOUT_MS        15000u
#define CANLIN_NET_CONNECT_TIMEOUT_MS    10000u  /* per address */
#define CANLIN_NET_HANDSHAKE_TIMEOUT_MS  15000u
#define CANLIN_NET_HEADER_LINE_MAX       (8u * 1024u)
#define CANLIN_NET_HEADERS_MAX           64u
#define CANLIN_NET_HEADER_BYTES_MAX      (32u * 1024u)
#define CANLIN_NET_REDIRECTS_MAX         3u
#define CANLIN_NET_RX_MAX                (32u * 1024u) /* buffered undelivered body per stream */
#define CANLIN_NET_TURN_BYTES            (16u * 1024u) /* read per stream per step */

/* Not in the spec: no byte sent or received for this long after the handshake (request
 * write and response phases) is a `timeout` error. §6 says "the idle timeout covers
 * stalls" without naming a transport constant; this matches CANLIN_LLM_IDLE_TIMEOUT_MS. */
#define CANLIN_NET_IDLE_TIMEOUT_MS       60000u
/* Not in the spec: 1xx interim responses skipped before the final one ("bounded count"). */
#define CANLIN_NET_INTERIM_MAX           8u
/* Not in the spec: resolved addresses kept per lookup (tried in getaddrinfo order). */
#define CANLIN_NET_ADDRS_MAX             8u
/* Not in the spec: bytes of one request URL and of one host name. */
#define CANLIN_NET_URL_MAX               2048u
#define CANLIN_NET_HOST_MAX              255u
/* Not in the spec: request line + headers (+ credential) bytes, body excluded. */
#define CANLIN_NET_REQUEST_HEAD_MAX      (16u * 1024u)
/* Not in the spec: caller headers per request. */
#define CANLIN_NET_REQ_HEADERS_MAX       32u
/* Not in the spec: hex digits of one chunk-size (8 = chunks up to 4 GiB - 1). */
#define CANLIN_NET_CHUNK_HEX_MAX         8u
/* Not in the spec: bytes of one error message handed to the consumer. */
#define CANLIN_NET_MSG_MAX               128u

#endif
