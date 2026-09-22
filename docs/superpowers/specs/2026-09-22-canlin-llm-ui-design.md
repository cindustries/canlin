# CanLin — LLM connection and GUI design

Status: draft for review. This records the user's standalone-first direction: a person
can configure a remote LLM and let it populate a GUI without deploying a CanLin server.
It complements the [system design](2026-09-22-canlin-system-design.md) and
[event-runtime contract](2026-09-22-canlin-event-runtime-design.md). No runtime code or new
dependency is selected by this document.

## 1. Product contract

The initial path is: **choose provider or compatible endpoint → enter API key and model →
connect → converse and interact with model-produced views**. The built-in assistant,
settings and basic assets ship with CanLin. A signed-module service, sync account, operator
registration on canlin-server and STT/TTS service are not prerequisites.

There are two connection modes with the same application-facing stream and GUI tool:

- **Direct:** CanLin talks to a supported remote LLM provider using that provider's API.
- **Gateway:** CanLin talks to the user's server via a supported provider-compatible dialect
  or a separately specified adapter. The server can keep provider credentials off the
  device and optionally add speech, shared services or other features.

Choosing a gateway does not implicitly enable data sync. The assistant's local history,
view state and settings remain locally owned unless the user explicitly configures a
separate synced dataset. An LLM endpoint is not assumed to provide `/sync`, BeePack
snapshots or module distribution. Inference stays remote in both modes; no model weights
or inference engine are required on P2, mobile or WASM clients.

## 2. Smallest useful implementation boundary

| Concern | Owner |
|---|---|
| Endpoint profiles, trusted settings, credentials and authorised network access | CanLin core/platform services |
| Bounded HTTP body, JSON/SSE framing primitives, event delivery/cancellation | CanLin portable core plus native/browser transport adapters |
| Provider request/response dialect and bounded continuation state | trusted built-in provider adapters, expressed in Lua where suitable |
| Conversation, provider selection UX and declarative view rendering | built-in Lua assistant module |
| Layout enforcement, widget/action validation, scope and resource limits | core-owned constrained UI interface |
| Optional proxy, speech, synced data and hosted services | canlin-server or another configured compatible server |

Do not hardcode provider model-name heuristics into the renderer/scheduler. A profile
selects a known adapter/dialect and a user-configured model; advertised tool/stream support
is checked or explicitly configured, not inferred from branding. A custom base URL means
that selected dialect at another endpoint, not automatic support for an unknown protocol.

The first slice needs one supported tool-capable provider dialect and a deterministic
local fixture. More provider adapters are incremental work, not a prerequisite for that
slice. The Lua-facing protocol abstraction does not require a new agent framework.

## 3. Connection setup and credential boundary

Trusted settings gather provider/dialect, base URL, model and credential. An explicit
connection test distinguishes bad credentials, missing model/tool capability, network/TLS
failure and browser access restrictions. No silent fallback to a different endpoint or
provider. Settings remain usable when no LLM is reachable.

A credential is represented to authorised adapters by an opaque capability bound to the
caller, configured destination and permitted authentication policy. The core injects the
required header only for that binding. Arbitrary modules and model-produced views cannot
read the raw key, change its destination, create profiles or grant themselves a handle.
Cross-origin redirects never forward the credential; endpoint changes require trusted
configuration. Use HTTPS, except an explicitly enabled local development endpoint.

Secrets do not enter prompts, tool arguments/results, generic module stores, logs, crash
reports or exported conversation history. Masked display and deletion are trusted-settings
operations. User-facing submit paths cannot export credential fields from settings.

Session-only keys work without persistent secret storage. Saving a key is explicit and
uses an OS credential facility where available; native images need a clearly documented
local storage/protection choice. Do not describe a file readable by the device owner as
an encrypted vault unless it actually has a separate protection mechanism.

Browser-direct credentials necessarily share the browser page/origin's trust boundary.
Opaque handles protect the Lua/module API, not against compromised host JavaScript,
extensions or a compromised delivered app. Default to session-only storage there; an
explicit remember option must explain its storage/security properties. A user who does
not want provider keys on the browser device can use their own gateway. No key is embedded
in a WASM binary, URL or distributed asset.

## 4. Streaming contract

Transport progress is incremental and nonblocking: native HTTP body processing and
browser Fetch/Streams feed the event runtime in bounded chunks. SSE means Server-Sent
Events; it imposes no CPU-SSE requirement. A callback API backed by a blocking request is
not sufficient for the owner thread.

Adapters produce a provider-independent application view:

- text fragments;
- complete tool requests with stable request/turn/tool-call identity;
- optional usage/status metadata;
- explicit successful completion, failure or cancellation.

The adapter also retains bounded protocol-specific continuation data required by its
provider. Normalised UI events do not replace the full conversation representation if
that would discard information needed for the next turn. Protocol data is not rendered
as trusted UI or treated as a new source of permissions.

Bound parser buffers, line/event sizes, JSON depth, strings, tool arguments, transcript
storage, in-flight requests and automatic tool continuation rounds. Limits apply during
assembly, not only after JSON parsing. Correctly handle split UTF-8/JSON, JSON escapes,
SSE line endings/multiline data and incomplete final records for the chosen dialect.
Provider error events and truncation are errors, not normal end-of-response.

A successful HTTP transfer or EOF alone does not prove semantic completion. The adapter
must validate the provider's terminal state. Tool arguments are not executable while
streaming and are admitted only once complete, valid and authorised by that turn's
protocol state. Text already shown on a failed stream remains visibly marked partial.

Cancel closes/stops the scoped transport and rejects late delivery; it cannot undo a
request the provider already received or guarantee absence of billing. Do not blindly
retry an ambiguous provider POST after disconnect. Explicit retry is a new, visible
attempt; no automatic duplicate model actions or unbounded retry/continuation loop.

## 5. One constrained GUI tool

The first tool sets the **assistant-owned view** to a small declarative flow description.
It does not install a module or grant the model direct access to the `canlin.*` API. A full
bounded view replacement is enough for v1; a general patch language is not required.

The schema permits a fixed widget set, for example headings, text, fields, choices,
buttons and meters, with stable widget ids and bounded values. The core API spec fixes
the exact schema, limits and enum values before implementation. Descriptions are data:
no arbitrary Lua, JavaScript, HTML, shell, callback bodies or embedded action expressions.
Assets must refer to permitted installed resources, not trigger arbitrary URL fetches.

Validation covers widget/action allowlists, unique ids, types, depth/count/byte budgets,
module/generation scope and runtime capabilities. Only after full validation does the
owner thread atomically replace the view and advance its revision. Invalid arguments
leave the last valid view intact and produce a structured tool error for the provider.
Successful replacement returns a result containing the accepted revision. Neither
rejected nor stale work gets a success result.

A tool-call identity is scoped to its request/turn and module generation. Duplicate
receipt within that scope reuses the recorded result rather than repeating an action.
Counts and result retention are bounded. Outbox identity is unrelated: a GUI call is
not automatically a durable sync operation. Failed/cancelled turns cannot deliver new
GUI changes into a later generation or conversation.

### Interaction semantics

Model-defined actions are a fixed enum of implemented behaviours, not executable strings.
For the initial slice, local editing and an explicit **submit** action are sufficient.
Submit sends the declared form values/action id to that conversation; merely displaying,
typing into, or replacing a form does not automatically transmit its values to the LLM.
Other actions require separately specified implementations and permission rules.

Each input event names a view revision as well as widget id and module generation. A
queued click from a replaced view cannot act on a new widget that reused the same id.
The UI makes the model-produced area and submission boundary recognisable. Views cannot
impersonate trusted settings, cover system status, access stored credentials, change
provider profiles or suppress permission/error chrome. Credential entry exists only in
the trusted settings surface; model text remains untrusted and must not be presented as
a request from that surface. The renderer cannot prevent a user from manually typing a
secret into an ordinary text field, so the submission boundary must remain visible.

Tool results, including validation errors, are encoded back into the provider's own
continuation protocol by the adapter. Automatic follow-up turns have explicit finite
limits and remain cancellable by the user. Generic filesystem, process execution,
arbitrary network and device-control tools are not granted by the GUI tool; any future
tool needs its own authorisation and validation contract.

## 6. Platform behaviour

**Native:** the portable C/Lua path uses the runtime's nonblocking transport and TLS.
P2 requires no local inference and no mandatory SIMD in its linked dependencies. Provider
and prompt sizes still need limits suitable for that hardware.

**Browser:** Fetch streaming and WebSocket exist, but a provider must permit the origin,
headers and credential use involved. Browser-direct access is conditional on that API's
CORS/browser policy. Report an unsupported direct connection clearly; offer a user-chosen
compatible gateway rather than claiming that WASM bypasses browser restrictions. Do not
route keys through an arbitrary public proxy or disable browser security.

**Offline/paused:** settings, history and local view state can remain available; remote
inference cannot. Preserve committed local data, mark incomplete replies and stop or
re-establish streams according to their lifecycle. No model-generated GUI completion
is fabricated to conceal a missing connection.

## 7. llm-stream as reference, not selected dependency

The user-provided [Mattbusel/llm-stream](https://github.com/Mattbusel/llm-stream) illustrates
the desired direct-provider streaming interaction. The inspected revision was
[`2409866`](https://github.com/Mattbusel/llm-stream/tree/240986607f3222569d6fa1cad3b04955946ec673).
It is a C++17/libcurl client, not local inference or an OS event runtime. Its blocking
`curl_easy_perform()` path and lack of an event-loop/cancellation/backpressure interface
do not meet this runtime's transport contract as-is. P2/musl/WASM support was not verified.

The design adopts the use case, not an unreviewed dependency or parser implementation.
No C++/libcurl requirement is introduced here; library/parser selection belongs to the
implementation spec and must meet the bounded streaming and platform tests above.

## 8. Acceptance slice and adversarial cases

Without a real provider, secret, physical display or canlin-server deployment:

1. Boot the runtime with built-in assistant/assets and no configured services.
2. Configure a local fixture endpoint through trusted settings with a dummy credential.
3. Stream text and a complete valid GUI tool call; render the accepted view offscreen.
4. Edit locally without outbound model traffic; submit explicitly and verify the fixture
   receives only the declared fields. Return the tool result in the chosen dialect.
5. Restart and recover committed local state; never recover a secret from transcript data.

Also test malformed/oversized/split streams, escaped Unicode, provider error/truncation,
wrong-origin redirects, invalid widgets/actions, duplicates, old view clicks, cancelled
or old-generation calls, continuation limits and credential redaction. Browser tests add
CORS/permission failures, reload/persistence and the session-only credential path.

Run portable suites on host and P2-QEMU; run the actual WASM client against browser
fixtures. Provider live smoke tests are separate, explicitly authorised operations that
may transmit user data and incur API charges, never an automatic test-suite dependency.
