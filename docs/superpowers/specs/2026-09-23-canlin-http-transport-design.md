# CanLin — native HTTP/1.1 + TLS transport

Status: draft for canlin#41 (2026-09-23). It fills the "native" row of the
[LLM providers design](2026-09-23-canlin-llm-providers-design.md) §8 and is the transport
the `davs://` storage of the [accounts design](2026-09-23-canlin-accounts-design.md) §4
reuses. It obeys the [event runtime design](2026-09-22-canlin-event-runtime-design.md)
§3, §5 and §6. The browser Fetch/Streams adapter is a separate ticket.

## 1. Scope

In: one outgoing HTTP/1.1 request per connection over TCP, optionally TLS (mbedtls);
streamed response delivery; DNS; redirects; credential injection bound to an origin;
bounded buffers; cancellation; integration into the native (rawfb, SDL2 desktop) drive
loop. Out: keep-alive/connection reuse, HTTP/2, proxies, compression, client
certificates, WebSocket (own spec), the browser adapter, mobile trust stores.

## 2. Layers

```
src/net/http.c      canlin_http: request writer, response parser, redirects, streams
src/net/tls.c       thin mbedtls wrapper: config, trust store, handshake, read/write
src/net/sock.c      nonblocking TCP connect over resolved addresses
src/net/dns.c       resolver worker (§4)
src/llm/…           canlin_llm_transport adapter over canlin_http (llm.h seam unchanged)
```

`canlin_http` is generic (the storage backend is its second consumer); the LLM adapter
maps its callbacks one-to-one onto `canlin_llm_stream_cb`. The existing auth struct and
`canlin_llm_auth_applies` define the origin binding; `canlin_http` takes an equivalent
generic credential binding (header name, prefix, secret, origin).

## 3. Drive contract

The transport never waits. The native backend owns `poll()`:

```c
/* Descriptors the backend must wait on, plus the nearest transport deadline. */
size_t canlin_http_pollfds(canlin_http *h, struct pollfd *out, size_t max,
                           canlin_ev_time *deadline);
/* Bounded work: at most CANLIN_NET_TURN_BYTES read per stream, callbacks on the owner
 * thread. Called once per loop iteration after poll() returns (or on deadline). */
void canlin_http_step(canlin_http *h, canlin_ev_time now);
```

- `open()` only admits and queues; no callback is ever made from inside `open()`
  (llm.h contract). Admission fails immediately when `CANLIN_NET_STREAMS_MAX` streams
  are live.
- Callback order per stream: `status`, `header*`, `body*`, then exactly one of
  `end`/`error`. After `close()` returns, no callback for that stream.
- Read backpressure: `step` reads only while the consumer accepts; the LLM core's
  refused-post retry (`canlin_llm_pump`) maps to "stop reading this stream this turn",
  never to buffering beyond `CANLIN_NET_RX_MAX`.
- The fake clock of `canlin_ev` drives all transport timeouts in tests.

## 4. DNS

`getaddrinfo()` in **one resolver worker thread** with a bounded job queue
(`CANLIN_NET_DNS_JOBS_MAX`), waking the owner through a pipe included in
`canlin_http_pollfds`. Reason: it respects the platform's resolver configuration
(`/etc/resolv.conf`, `/etc/hosts`, Android Private DNS later) without an own resolver,
and the event runtime allows uninterruptible native work in a worker when bounded (§2 of
that spec). Cancelling a request marks its lookup as orphaned; the worker's result is
freed on arrival, never delivered. The lookup is bounded by the resolver's own timeout
and by `CANLIN_NET_DNS_TIMEOUT_MS` on the owner side (reported as `dns` error). Addresses
are tried in `getaddrinfo` order, one connect at a time, each with
`CANLIN_NET_CONNECT_TIMEOUT_MS`; no parallel "happy eyeballs" in v1.

## 5. TLS

- mbedtls **3.6 LTS**, vendored read-only under `third_party/mbedtls` with a CanLin
  config header in `src/net/` (not by editing vendored files). TLS 1.2 and 1.3,
  ECDHE (X25519, secp256r1, secp384r1), AES-GCM and ChaCha20-Poly1305, RSA and ECDSA
  certificates. **No AESNI, PadLock or SSE code paths** in any build (`TARGET=i686` is
  built `-mno-sse`; the host build uses the same config so tests cover the target code).
- Verification is always on: chain to the trust store, hostname (SNI + name check via
  `mbedtls_ssl_set_hostname`), validity dates. There is no "ignore certificate" switch.
  Plain `http://` is only for loopback/local profiles (providers spec §5).
- Trust store: a PEM bundle path from platform configuration, default
  `/etc/ssl/certs/ca-certificates.crt`; canlin-os ships that file. Loaded once, lazily,
  before the first TLS handshake. A missing/unparseable bundle fails every TLS request
  with `tls-trust`, not silently with verification off.
- Entropy: `getrandom()`/`/dev/urandom` via mbedtls' platform entropy.
- **Wall clock:** a P2 with a dead CMOS battery has a wrong date. A certificate failing
  only on validity dates is reported as `tls-clock` ("Systemzeit prüfen") so the UI can
  say so, distinct from `tls-verify`.
- Record buffers: `MBEDTLS_SSL_IN_CONTENT_LEN` 16 KiB (servers may send full records),
  `MBEDTLS_SSL_OUT_CONTENT_LEN` 4 KiB.

## 6. HTTP/1.1

Request: `METHOD path HTTP/1.1`, `Host`, `User-Agent: CanLin/<version>`,
`Accept-Encoding: identity`, `Connection: close`, `Content-Length` for a body, the
caller's non-secret headers, then the credential header only if the binding applies to
this URL. Header names/values from callers are validated (no CR/LF/NUL). The request
buffer holding the secret is zeroised after it is written.

Response parser (incremental, any split, bounded):

- Status line and headers: line ≤ `CANLIN_NET_HEADER_LINE_MAX`, count ≤
  `CANLIN_NET_HEADERS_MAX`, total ≤ `CANLIN_NET_HEADER_BYTES_MAX`; obsolete line folding
  rejected; `1xx` interim responses skipped (bounded count).
- Body framing: `Transfer-Encoding: chunked` (chunk-size hex bounded, extensions
  ignored, trailers read and discarded within the header bounds), else `Content-Length`,
  else read to close. Both headers present, conflicting `Content-Length`, or a
  `Content-Encoding` other than `identity` is a protocol error.
- EOF before the framing says the body is complete is an error (`eof`), never `end`.
- Response body bytes are not bounded in total by the transport (streams can be long);
  the consumer's limits apply (LLM §7), and the idle timeout covers stalls.

Redirects: at most `CANLIN_NET_REDIRECTS_MAX`. `301/302/303` followed for GET only (a
POST receiving them fails — no silent method change), `307/308` for GET and POST with
the same body. The target must be `https`, or loopback `http` when the original was
loopback `http`; downgrades fail. Each hop re-evaluates the credential binding: a
cross-origin hop drops the credential (already tested with the fixture transport).

## 7. Errors

`error(msg)` carries a short code prefix plus text without secrets or bodies:
`dns`, `connect`, `timeout`, `tls-trust`, `tls-verify`, `tls-clock`, `tls`, `protocol`,
`eof`, `redirect`, `limit`, `cancelled`. The LLM connection test (providers §6) maps
these to its user-facing categories.

## 8. Limits

New `include/canlin/net_limits.h`, initial values:

| constant | value |
|---|---|
| `CANLIN_NET_STREAMS_MAX` | 4 live requests (process) |
| `CANLIN_NET_DNS_JOBS_MAX` | 4 |
| `CANLIN_NET_DNS_TIMEOUT_MS` | 15 000 |
| `CANLIN_NET_CONNECT_TIMEOUT_MS` | 10 000 per address |
| `CANLIN_NET_HANDSHAKE_TIMEOUT_MS` | 15 000 |
| `CANLIN_NET_HEADER_LINE_MAX` | 8 KiB |
| `CANLIN_NET_HEADERS_MAX` | 64 |
| `CANLIN_NET_HEADER_BYTES_MAX` | 32 KiB |
| `CANLIN_NET_REDIRECTS_MAX` | 3 |
| `CANLIN_NET_RX_MAX` | 32 KiB buffered undelivered body per stream |
| `CANLIN_NET_TURN_BYTES` | 16 KiB read per stream per step |

Memory per TLS stream ≈ 16 + 4 KiB records + mbedtls session state + `RX_MAX`; four
streams stay well under 1 MiB.

## 9. Tests

No external network. A test server runs in-process (own thread or forked child) on
`127.0.0.1`/`::1` with fixture certificates under `t/net/fixtures/` generated by a
committed script (test CA, valid leaf for `localhost`, wrong-host leaf, expired leaf,
self-signed). The client trusts only the test CA bundle.

- HTTP parser unit tests fed at every split point: chunked, content-length,
  close-delimited, trailers, 1xx, oversize line/count/total, obs-fold, bad chunk size,
  both framing headers, gzip content-encoding, EOF mid-chunk.
- End-to-end over loopback, plain and TLS: streaming SSE body byte-by-byte; redirect
  chains (same origin keeps the credential, cross origin drops it, POST on 302 fails,
  https→http fails, > max fails); wrong host → `tls-verify`; expired leaf with a fake
  wall clock → `tls-clock`; untrusted CA → `tls-verify`; missing bundle → `tls-trust`;
  connect refused; handshake stall and idle stall under the fake monotonic clock; cancel
  during DNS, connect, handshake and body with no later callback; stream admission at
  `STREAMS_MAX`.
- Secret hygiene: the credential appears on the wire only for the bound origin (checked
  by the test server) and never in error messages.
- The LLM dialect tests run once more through the real transport against the loopback
  server replaying the existing fixtures.
- **Gates:** host `make test`; `make TARGET=i686` build plus the TLS loopback tests in
  `qemu-system-i386 -cpu pentium2` (catches SSE/AESNI instructions); handshake time on
  P2-QEMU recorded as a number, not a pass/fail. A skipped gate is named.

## 10. Open points

- Mobile (Android/iOS) trust stores and whether those builds use the platform TLS
  instead of mbedtls — with the mobile slices.
- Keep-alive/connection reuse — only if a measured handshake cost on P2 hurts.
- Proxy support (`HTTPS_PROXY`, CONNECT) — when a user needs it.
