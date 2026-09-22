# CanLin — system design spec

Status: draft for review, incorporating the architecture review and the browser target.
This is the family-level design. Each sub-project gets its own spec and plan; the first
one is `libbee` (`~/dev/libbee/docs/superpowers/specs/`). The runtime's scheduling and
platform contract is specified in [the event-runtime design](2026-09-22-canlin-event-runtime-design.md).
Direct LLM connections and model-driven views are specified in [the LLM/UI design](2026-09-22-canlin-llm-ui-design.md).
The specs are complementary; a conflicting implementation or skill needs a review,
not an inferred exception.

## 1. What CanLin is

A thin-client runtime and the operating-system images that carry it. Native images carry
one static C binary that renders screens, records and plays audio, talks to a server and
runs Lua modules. The same runtime sources also build as an SDL2 app or a WebAssembly
browser client. It works standalone: choose an LLM provider or target URL, supply an API
key and model, and use an assistant that can populate its own GUI through a constrained
tool. No self-hosted canlin-server is required. Inference stays remote; this is not a local
model runtime for the P2. A user's own server is an optional provider gateway and source
of additional services such as speech, shared data, sync or module distribution.

Local module data is authoritative in standalone mode. Modules opting into server sync
follow that service's explicit authority contract; not every local edit needs an outbox
or remote snapshot. Local UI, credentials and permission decisions remain device-owned.

The first hardware is two Pentium II laptops with 1 GB RAM. They set the CPU and resource
floor, not a guarantee of platform compatibility. One client source, six hosts; each host
still needs its own lifecycle, I/O and capability validation.

Principles that decide every later question:

- **Small core, features in modules, optional services.** The core provides rendering,
  storage, bounded I/O, credentials and event dispatch. A built-in Lua assistant and
  provider adapters connect to a remote LLM directly or through a user's server.
  Model-produced UI is validated data, not automatically executable code.
- **Usable without infrastructure.** Provider/URL + API key + model is a complete initial
  setup for a supported endpoint. Sync, STT/TTS, IoT gateways and a self-hosted server are
  optional capabilities, not prerequisites for opening a screen or starting a chat.
- **Fits everywhere before looks perfect.** Screens are described as flow and laid out
  per device. Branding is a theme file.
- **Own formats where they are small.** BeePack for bundles and configuration, MsgPack on
  the wire, SQLite where data has volume, Lua for logic.
- **The weakest host wins.** i686 without SSE, PUC Lua, no mandatory SIMD. No X11 or browser
  dependency on the native images; the optional browser target does not raise that floor.
- **One runtime, platform-driven events.** Lua and UI have one owner thread. Platform
  adapters drive bounded dispatch turns; POSIX `poll()` is an implementation, not the
  portable runtime contract.

## 2. Targets

| Target | Display / input | Delivery | Owner |
|---|---|---|---|
| Pentium II laptop | rawfb + evdev | canlin-os image, initramfs in RAM, syslinux | canlin-os |
| x86 tablet (Bay Trail / Surface era) | rawfb + evdev, touch via i2c-hid | canlin-os image, UEFI | canlin-os |
| Desktop (development, kiosks) | SDL2 | binary | CanLin |
| Android tablet, phone | SDL2 + NDK | sideloaded APK | CanLin |
| iPad, iPhone | SDL2 + Xcode | signed app (TestFlight or 7-day dev signing) | CanLin |
| Browser | Emscripten + SDL2/Canvas, browser I/O adapters | WASM + loader + assets over HTTPS | CanLin |

Compiler flags for every i686 build: `-march=i686 -mno-sse`. The i686 test suite runs
under `qemu-system-i386 -cpu pentium2 -m 1024` because the host silently executes SSE.
QEMU checks instruction compatibility and behaviour; real P2 hardware checks performance.

The browser build is **CanLin compiled to WASM**, including Lua and the portable libraries,
not a second JavaScript UI and not a PC emulator. It runs the same module format and Lua
API where capabilities are available. Building a v86-compatible canlin-os image is an
optional, separately scoped system demo; it is neither this target nor a substitute for
the P2-QEMU gate.

## 3. Decomposition and repos

| Part | Repo | Language |
|---|---|---|
| BeePack format and C library | `libbee` | C99 |
| Runtime core, Lua API, assistant/provider adapters, WASM build, these specs | `canlin` | C99, Lua 5.4; small browser glue |
| Images, kernels, boot media, QEMU harness | `canlin-os` | Buildroot |
| Fixture generation, `bee` CLI, later XS over libbee | `p5-beepack` | Perl |
| Reference IoT device, consumer of libbee | `sunriser8` | C, STM32 |
| Optional gateway, sync, speech and module-distribution services | `canlin-server` (not created yet) | Perl |

Order of work:

1. libbee, with the canlin-os i686 toolchain and minimal QEMU test-result path brought up
   early enough to test the first runtime slice, not after desktop completion.
2. Event-runtime, LLM/UI and core API contracts; then a small desktop/offscreen slice:
   load the built-in assistant through the real sandbox, configure a fixture endpoint,
   stream text and one complete GUI tool call, render its flow view and persist local
   state. Run it on host and P2-QEMU; no canlin-server or real provider is required for tests.
3. A browser slice of that same runtime, proving yield/wake, rendering, persistence,
   Fetch streaming, endpoint/CORS errors and credential handling. This is a portability
   gate, not a promise that every provider accepts browser-direct requests.
4. Optional sync against a deterministic fixture, rawfb image integration, audio and
   server services; mobile and IoT follow their own specs. A canlin-server implementation
   does not block direct-provider use.

## 4. The core

Vendored, read-only under `third_party/`: Nuklear, Lua 5.4, cmp, stb_truetype, libbee,
SQLite amalgamation + sqlite-vec, mbedtls, monocypher (Ed25519 for module signatures).
The P2 build cannot depend on optional SIMD paths. A browser transport uses the browser's
HTTP, WebSocket and TLS facilities rather than tunnelling the native socket/TLS stack.

MQTT 3.1.1 is a required transport, but its implementation is not settled by a line-count
estimate. The IoT spec must choose the required QoS/session/reconnect behaviour and memory
bounds, then compare a small portable library against an owned implementation. No
unverified claim that all existing clients are unsuitable.

Subsystems, each testable at its boundary:

- **events**: the owner-thread scheduler, bounded module queues, operation completion,
  monotonic timers, cancellation, lifecycle and invalidation. Native adapters wait/wake;
  the browser schedules callbacks and always returns control. No generic system daemon,
  distributed event bus or unrestricted cross-module publish/subscribe in v1.
- **render**: Nuklear with a software rasterizer into an RGB buffer in RAM. `rawfb` copies
  dirty rows to `/dev/fb0`; `sdl2` uploads the buffer as a texture, including through
  Emscripten in the browser. Redraw on invalidation, not an unconditional frame loop.
  One DPI scale factor from the theme or platform.
- **input**: evdev, SDL2 or browser input, normalised before delivery. The platform owns
  mobile text input/keyboard integration; rawfb touch targets use a Nuklear keyboard.
- **audio**: 16 kHz mono 16-bit PCM at the protocol boundary, ALSA on images, SDL2/native
  or browser adapters elsewhere. Device-rate conversion belongs to the adapter. Bounded
  capture/playback rings separate real-time callbacks from Lua. Push-to-talk first;
  voice activity detection and a codec are later work.
- **net**: native nonblocking sockets, mbedtls, streaming HTTP/1.1, WebSocket and MQTT;
  browser Fetch/Streams and WebSocket adapters instead. HTTP body/SSE decoding is
  incremental and bounded; SSE here means Server-Sent Events, not CPU instructions.
  Provider-specific message dialects belong in trusted adapters/modules, not hardcoded
  model-name switches in the event loop. DNS, handshake and I/O respect the event contract.
  MQTT in the browser needs a WebSocket-capable broker or server gateway, not raw TCP.
- **credentials**: trusted configuration binds opaque credential handles to endpoints
  and permitted adapters. The core injects authentication into authorised transport;
  arbitrary Lua, model content and logs never receive the raw API key.
- **store**: libbee stacks per module, theme and system; SQLite per module; outbox and
  persistent-storage completion. Disk and browser persistence have explicit contracts.
- **modules**: locate, verify signature (Ed25519 over the `.bee` bytes with the trusted
  public key), open the permitted layers, load `lua#main` in a fresh sandbox, wire `canlin.*`.
  Restart/unload invalidates that module instance's outstanding work.
- **sandbox**: Lua 5.4 state per module, without `os`, `io`, `package` or `debug`; binary
  chunk loading is forbidden. `require` resolves only this module's executable keys.
  Lua instruction and allocator budgets are supplemented by native-work/result budgets.

Only the owner thread enters Lua or changes UI/module state. Backend audio callbacks and
explicitly isolated workers may exist, but exchange owned data through bounded channels;
they never call Lua. A blocking or unbounded C call is not made safe by a Lua hook.

## 5. Modules

A module is a BeePack. Reserved keys inside `module.bee`:

```
meta#name, meta#version, meta#title, meta#min-core      identity
lua#main                                                entry point, returns a table
lua#<name>                                              require-able chunks
font#<name>, icon#<name>, image#<name>                  blobs
<anything else>                                         the module's default values
```

The returned table implements `init(ctx)`, `draw(ui)` and `event(name, payload)`.
`sync(info)` is the compatibility-shaped callback for a newly installed snapshot;
`info.version` identifies that snapshot, not a batch of acknowledged outbox rows.
Every callback is dispatched by the owner-thread scheduler, never inline from a backend
or nested inside another Lua callback. A draw consumes current state and may initiate a
bounded asynchronous operation; its completion cannot re-enter that draw.

Screens are immediate mode through a constrained `canlin.ui` flow API backed by Nuklear,
not an unrestricted one-to-one export of Nuklear's pixel-positioning primitives:

```lua
function M.draw(ui)
  ui:heading(M.title)
  ui:text(M.status)                      -- wraps
  for _, m in ipairs(M.messages) do ui:bubble(m.role, m.text) end
  M.input = ui:field("Nachricht", M.input)
  if ui:button("Senden") then M:send() end
  ui:meter("Mikrofon", canlin.audio.level())
end
```

Below 40 em of available content width the layout is one column; at or above 40 em it
permits two. Interactive elements are at least 2.5 em high. Font size is the theme's base
times the DPI factor; module code does not position in pixels. The core UI spec defines
column grouping, scrolling, rounding and text overflow. Test the boundary with a fixed
font/theme; arbitrary automatic two-column placement is not itself a layout contract.

Modules query runtime capabilities, not OS names. Permission-denied, temporarily
unavailable and unsupported are distinct results. No microphone, serial device or
persistent store is silently simulated as a working capability.

The built-in assistant renders a bounded declarative view model through this same flow
API. A provider-visible GUI tool updates only that assistant-owned view after full
argument validation; streamed partial tool arguments are never executed. Trusted settings,
credentials and shell chrome are outside the generated view. Widget actions are local UI
events that the assistant may send back to the conversation under its disclosure rules.
See the LLM/UI design for the tool boundary and provider-independent stream semantics.

### Executable layers and trust

Data lookup is `local/` → `state.bee` → `module.bee`. A local data override deliberately
masks the effective value on this device; it never changes the canonical server snapshot
or writes a sync operation. Reads expose local-override provenance, and the UI identifies
locally overridden synced configuration instead of presenting it as the server's value.
Server authority means canonical synced state, not prohibition of this visible device-local
mask. Executable lookup is narrower:
`local/` in an explicitly enabled local-code-override mode, then verified `module.bee`.
`state.bee` is never a source of `lua#…`, whether for the entry point or `require`.
A snapshot containing executable keys is rejected before installation. Synced state
cannot replace verified module identity, version or core compatibility metadata either.

Local tinkering remains a feature: enabling a local Lua override visibly marks the
module as locally modified/unsigned. The sandbox and resource limits still apply. Only
operator-controlled configuration enables this mode; synced state and a module's writable
APIs cannot grant it or write executable overrides. Ordinary local data edits remain
available without granting code authority.

## 6. Storage

Per module, on the writable data volume:

```
modules/<name>/module.bee     signed bundle, replaced on verified update
modules/<name>/state.bee      validated, versioned server snapshot
modules/<name>/local/         override dir, hashed .MP files, device-only
modules/<name>/data.db        SQLite: module tables plus core-owned outbox/metadata
theme/theme.bee, theme/local/
system/local/                 device settings, endpoint profiles, credential references
```

Standalone modules use local SQLite/state without a sync service. `state.bee` and the
outbox are present for modules with explicitly configured sync; their absence is normal.
The core owns secret storage, outside module-writable directories; `system/local/`
contains references, not an invitation to expose API keys through generic store lookups.
The credential design specifies session-only and persistent options per platform.

On images the data volume is a small ext4 partition on disk or USB, found by label
`CANLINDATA`; without it the runtime works from RAM and displays that it is volatile.
On Android and iOS it is the app's data directory. In the browser a storage adapter
provides a virtual filesystem/SQLite VFS backed by suitable browser storage such as
OPFS or IndexedDB. Its flush, quota, multi-tab ownership and recovery behaviour must be
proved in the browser slice; an in-memory SQLite commit is not browser durability.

Installing a file means writing and validating a temporary object, atomically publishing
it, and completing the platform's persistence barrier before reporting durable success.
On native persistent storage this includes the required file and directory sync, not
just `rename()`. Old or new complete content must survive a failure, never a partial
published pack. Volatile mode explicitly offers no restart durability.

SQLite holds sensor series (with module-declared retention), chat history, server-provided
embeddings (sqlite-vec without mandatory SIMD), and the outbox. A per-module authorizer
and constrained API prevent access to another file and direct modification of the
core-owned outbox/metadata. Native query cost, result sizes and extension workloads are
bounded separately from Lua allocation. Unsupported uninterruptible workloads must be
limited or isolated, not run indefinitely on the owner thread.

Per-module database, pack, queue and outbox quotas stop one module exhausting the shared
volume. Retention never silently deletes unacknowledged outbox operations. Full disk,
browser quota failure and loss of persistent storage produce explicit operation errors;
the runtime must not silently switch an accepted durable operation into volatile mode.

## 7. Optional sync

A user/module explicitly enables a sync service; the following authority contract applies
only to that service's synced state. Standalone writes stay local, and an LLM connection
is not itself a sync server. The core creates this outbox for a sync-enabled module:

```sql
CREATE TABLE outbox (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    ts INTEGER,
    kind TEXT,
    key TEXT,
    value BLOB
);
```

`value` is MsgPack. An outbox has a cryptographically random origin token stored in
core-owned metadata. Its stable operation identity is `(device, module, origin, id)`.
`AUTOINCREMENT` prevents sequence reuse after deletion; a newly created database gets a
new origin even on the same device. Restart, retry and module update keep the existing
origin. Active outboxes cannot be cloned to another writer; restoring an old outbox is
an explicit recovery problem, not a supported transparent database import in v1.

`canlin.sync.put(kind, key, value)` requests a local append. Admission returns a request
handle or an immediate bounded-resource/validation error. A later completion reports
that the row is committed with its operation identity, or reports failure. Persistent
mode completes only after the storage barrier; explicit volatile mode reports volatile
acceptance instead. The transport sends only accepted rows, never an uncommitted append.
No successful return merely promising future I/O is described as durable success.

The transport is **at least once**. It sends unacknowledged rows in order per outbox,
replaying them after reconnect with identical identities. The server deduplicates on the
full identity and durably records the processing outcome before ACK. Duplicate requests
return that outcome rather than applying the same operation again. Partial ACKs remove
only their matching rows; ACKs for another origin do not affect this outbox. A permanent
rejection is an explicit recorded outcome surfaced to the module, not silent data loss.
The server's spec owns transactions and idempotency of any downstream side effects.

Snapshots are separate from ACKs. The server offers a `state.bee` with a monotonic
version within the module's sync scope. Downloads have size limits, are validated and
installed through the storage barrier. Older or superseded versions cannot replace a
newer installed snapshot. The core then dispatches `sync({version = ...})` and invalidates
the screen. A failed snapshot does not undo an already acknowledged operation; retain
the previous snapshot and retry/report the failure. The client never merges data.

Module updates use the same bounded download/install machinery but require signature
and compatibility validation. Failed installation keeps the previous module. Successful
replacement restarts it with a new runtime generation; see the event-runtime spec for
cancellation and late completions. Schema migration and rollback belong in the core
module-lifecycle spec before updates that change `data.db` are implemented.

Outbox limits cover both rows and bytes. At capacity, `canlin.sync.put` fails visibly with
backpressure; it does not evict unacknowledged rows. Pending appends also consume bounded
admission capacity. Modules can show offline/pending/full/volatile states without
implementing a merge engine.

## 8. LLM, audio and optional speech services

Text-first setup connects directly to a supported provider endpoint or a user-selected
gateway. The built-in assistant handles conversation and validated GUI tool calls; it
does not require a canlin-server account, sync or audio. Provider adapters normalise streams
while retaining bounded provider-specific continuation state. Missing tool support is
reported as a capability limitation, not guessed from a model's name.

When a speech service is configured, a module can request `canlin.audio.record(true)`.
The core checks capability, permission and availability, then sends bounded PCM frames
to that service over an audio WebSocket associated with the module. The optional service
can perform STT/TTS or a complete STT → LLM → TTS flow. Its use does not transfer authority
over local assistant data or enable sync. `canlin.audio.level()` reads the latest coalesced
level. No speech model is required on the client, including the browser build.

Before implementation the audio wire spec fixes PCM byte order, frame duration/size,
stream identity, start/stop acknowledgements, end-of-stream and error framing. Capture
and playback backpressure must remain bounded. Capture overflow stops/reports the
stream rather than silently losing an unknown part of speech; playback underrun produces
silence plus status. Browser permission and user-activation requirements are normal
states, not startup failures for the entire runtime. Suspension cancels live capture;
resuming requires an explicit restart rather than replaying a backlog of recorded audio.

Audio tests use memory/file sources and sinks plus a fake clock, never a device.

## 9. IoT

Devices speak MQTT with MsgPack payloads; topics follow `canlin/<site>/<device>/<channel>`.
The core keeps one session per configured broker and dispatches messages to modules
through the event runtime. Subscriptions and serial handles belong to module instances
and are released on unload. Topic, device and I/O permissions are core-controlled.

A device's configuration is a BeePack delivered over MQTT or HTTP. `canlin.serial` makes a
native laptop a gateway for devices without network access. In the browser, serial/USB
is an optional permission-gated capability where supported, not a universal promise;
otherwise use the server/native gateway. MQTT over WebSocket and broker reconnect/QoS
semantics are part of the IoT contract, not an invisible TCP shim.

## 10. Theme

`theme.bee` keys: `theme#color#<role>` (background, surface, text, muted, accent,
danger, success), `theme#font#body`, `theme#font#heading` (TTF blobs), `theme#font#size`
(base in px at DPI 1.0), `theme#logo` (PNG), `theme#spacing` (em unit multiplier),
`theme#radius`. Loaded at startup into Nuklear's style table; a `theme/local/` override
allows per-device tweaks. Missing keys fall back to the built-in theme inside the binary.
Font, image and pack sizes are bounded before decoding/allocation.

## 11. Optional server contract (owned by canlin-server)

This is the additional CanLin service protocol, **not** the protocol a direct LLM provider
must implement. Provider presets/custom compatible endpoints follow the LLM/UI design.
A gateway may forward provider traffic without providing sync or becoming a data authority.
For explicitly synced datasets the service is authoritative; its wire contract must exist
before the first sync fixture, rather than being guessed by the client:

- `GET /modules` lists `{name, version, sha256}`; `GET /modules/<name>.bee` downloads a
  bundle. A hash in a listing does not replace signature verification.
- WebSocket `/sync` carries MsgPack control messages, including batches with
  `{device, module, origin, rows[]}`, scoped processing outcomes and snapshot offers
  `{module, version, state_bee_url}`. The protocol spec must fix message tags, field
  types, size limits and malformed/unsupported-version responses before golden fixtures.
- WebSocket `/audio` carries the audio stream contract described above; control/text
  events must be distinguishable from PCM. Browser and native clients use the same wire
  semantics even though their transport adapters differ.
- Auth for these optional services begins with a device key pair and operator registration;
  this is not required for a direct provider's URL/API-key onboarding. HTTP signing must cover
  a defined canonical request plus replay protection. Browser WebSocket constructors
  cannot set arbitrary auth headers: define a short-lived connection ticket or an
  authenticated first-message exchange, with no module data accepted before authentication.
  The server spec selects and tests one flow, including browser Origin and HTTP CORS policy.
  Durable browser identity is subject to storage loss and requires re-registration then.

Until `canlin-server` exists, record contract dependencies on the originating board. Do not
create a speculative server implementation in canlin or make up a remote repository.

## 12. Testing

- C unit tests per subsystem; an offscreen backend renders into memory. Assert geometry
  or Nuklear commands with fixed fonts/themes, plus targeted rasterizer pixel checks.
- Lua tests exercise the real sandbox. Server/broker, transport, storage and audio are
  local or in-memory fixtures. No normal automated test touches production services,
  physical audio devices or a physical display.
- Event tests use a fake monotonic clock and adapters that control delivery order,
  completion, pause/resume and faults. Cover fairness, queue saturation, cancellation,
  late completions after reload and bounded idle behaviour.
- Assistant/provider tests require no API key or real model: fixture streams split JSON,
  UTF-8 and SSE frames at arbitrary boundaries, submit valid/invalid GUI tools, lose the
  connection before semantic completion and deliver late/repeated tool calls. Verify
  view revisions, explicit submit disclosure, endpoint-bound credentials and no execution
  from model-generated code strings. Gateway use must not silently enable data sync.
- Sync tests include lost ACK, duplicate replay, partial ACK, empty-then-refilled outbox,
  database recreation, storage exhaustion and stale/failed snapshot installation.
- Security tests cover executable keys in snapshots, local-code-override authority,
  SQLite authorizer/native resource limits and per-module event/handle isolation.
- Every portable core suite runs on the host and i686 under P2-QEMU. Backend-specific
  suites run on their backend; the QEMU harness reports a machine-readable result and
  fails on hangs/illegal instructions. Build a minimal executable gate early.
- The WASM browser gate exercises the actual compiled runtime with local fixtures:
  yielding, input/render, Fetch streaming/WebSocket, persistence across reload, quota and
  permission denial, CORS failure, single-writer storage ownership and suspension. It cannot substitute for native/P2
  tests; nor does a host test prove browser persistence.
- Manual hardware qualification measures real P2 timing and checks native/mobile audio,
  drivers and lifecycle separately. Report this separately from automated fixture tests.

## 13. Follow-up specifications and decisions

- The core spec fixes exact `canlin.*` API shapes, admission/completion payloads, numeric
  resource budgets and recovery behaviour before implementation. Names include `ui`,
  `store`, `db`, `sync`, `net`, `mqtt`, `audio`, `serial`, `sys` and the event/timer surface.
  The event-runtime spec already fixes ownership, ordering and overload semantics.
- The LLM/UI implementation spec fixes the first supported provider dialect, bounded
  JSON/SSE parsing, exact GUI tool schema and platform credential storage. Custom URL does
  not mean arbitrary undocumented protocol. No llm-stream dependency is selected merely
  because its streaming demonstration illustrates the desired interaction.
- The browser storage slice selects and validates its persistent VFS, flush strategy and
  writer exclusion. Merely compiling SQLite to WASM does not settle those choices.
- Audio and IoT specs settle framing and required protocol subsets before implementation.
- The laptops' NIC, sound and graphics chips (canlin#3) decide the first real kernel
  fragment; QEMU is not a driver or performance substitute.
- canlin-os owns the musl i686 toolchain and QEMU harness; the same toolchain serves libbee's
  `test32`. Toolchain availability is an early dependency, not a presumed host feature.
- libbee consumption (submodule versus pinned snapshot) remains canlin#2.
- Licence for `canlin` and `canlin-os`: libbee is Perl (Artistic/GPL, like BeePack); the runtime
  and images remain undecided.
