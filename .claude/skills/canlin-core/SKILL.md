---
name: canlin-core
description: "Use when implementing, reviewing or designing CanLin runtime code, Lua modules, provider/GUI-tool integration, event scheduling, platform adapters, storage/sync, audio, sandbox boundaries or browser/WASM support."
user-invocable: false
allowed-tools: Read, Grep, Glob
model: sonnet
---

# CanLin — architecture and invariants

CanLin is a **standalone-capable thin-client runtime**: portable C and sandboxed Lua. Choose
provider/compatible URL, API key and model; the built-in assistant connects to a remote
LLM and renders model-produced views. No canlin-server deployment is required. Inference
stays remote; local data is authoritative unless explicitly opted into a sync service.
Gateway, speech, sync and hosted services are independent options.

## Read the owning spec

Authoritative docs belong to the **canlin checkout**, also when loaded from canlin-os:

- `docs/superpowers/specs/2026-09-22-canlin-system-design.md` — family/subsystem contracts.
- `docs/superpowers/specs/2026-09-22-canlin-event-runtime-design.md` — scheduling/platform seam.
- `docs/superpowers/specs/2026-09-22-canlin-llm-ui-design.md` — direct providers and GUI tools.

A more specific spec controls its seam. Conflicting specs need review, not silent choice.
This skill is a briefing summary. Exact APIs, numeric budgets and build commands need the
relevant implementation/spec; this repository starts with design, not an existing build.

## Hardware floor and targets

P2 i686 without SSE sets CPU/resources, not automatic platform compatibility.

- Use `-march=i686 -mno-sse`, PUC Lua 5.4, no LuaJIT or mandatory SIMD.
- Test portable suites under `qemu-system-i386 -cpu pentium2 -m 1024` via canlin-os.
  Host success is not P2 evidence; QEMU is not a real-hardware timing measurement.
- Native images use static musl and software Nuklear rendering, without X11/browser.

| Target | Backend / delivery |
|---|---|
| P2 laptop | rawfb + evdev, canlin-os Buildroot image |
| x86 tablet | rawfb + evdev/touch, canlin-os UEFI image |
| Desktop | SDL2 binary |
| Android | SDL2 + NDK, sideloaded APK |
| iOS | SDL2 + Xcode, signed app |
| Browser | Emscripten/WASM, SDL2/Canvas and browser I/O adapters |

WASM is the actual runtime, not another web UI. Whole-PC/v86 image demos are separate.

## Core, modules and events

Core owns stable mechanisms; features are Lua modules/BeePacks. Each module has its own
Lua state, storage and handles. One owner thread enters Lua and owns UI/module state.
Backends drive bounded turns: native wait/wake, browser callback/yield. `poll()` is only
a native implementation. Audio callbacks/isolated workers may exist but never enter Lua.

- Dispatch `init`, `event`, snapshot `sync(info)` and `draw` without re-entrancy.
- Bound queue items/bytes, in-flight work, results and callbacks per module/process.
  Rotate fairly; native C work needs limits beyond Lua hooks and allocation budgets.
- Coalesce latest state, not ordered input edges. Reserve bounded terminal outcome/result
  capacity before starting finite operations. Stream data has separate backpressure.
- Use monotonic timers and generation-scoped handles. Cancel is not rollback. Late work
  still gets cleanup but never calls a replacement Lua state or reuses its handles.
- Redraw only when dirty; idle waits/yields instead of spinning. No generic broker or
  unrestricted cross-module pub/sub in v1.

Sandbox all enabled coroutine/native paths too; catching hook errors must not evade
scheduler termination. SQLite/extensions need bounded inputs/results and interruptible
or isolated work. Read the event spec for ordering, cancellation and lifetime details.

## Direct LLM and model-produced UI

The built-in assistant/provider adapters own conversation and provider dialects; the
core provides bounded transport/parsing, credentials and UI validation. No model-name
heuristics in the scheduler. HTTP/SSE streams are incremental; SSE means Server-Sent
Events, not CPU instructions. A blocking callback-based request is not sufficient.

- Credentials are opaque caller/endpoint-bound capabilities; core injects auth. No raw
  API key in Lua, prompts, tools, generic stores or logs. Redirects cannot change binding.
- Browser-direct access requires provider CORS support. Use a user-chosen gateway when
  needed, never a hidden proxy. Browser secrets share the page/origin trust boundary.
- Partial tool arguments never execute. Validate complete calls and semantic outcomes;
  HTTP EOF is not proof of success. Bound assembly and automatic continuation rounds.
- GUI tool v1 atomically replaces an assistant-owned declarative flow view. Allowlisted
  widgets/actions only, no generated Lua/HTML/JS/shell. Keep trusted settings/chrome out.
- Input includes view revision; stale clicks cannot target reused widget ids. Local edits
  are not transmitted until explicit submit. Tool results reflect actual acceptance.
- Normalised text/tool/outcome events do not discard required provider continuation data;
  adapters retain it within bounds. Request/turn identity is separate from sync identity.

## Storage, optional sync and trust

```
local/         device-only data overrides
state.bee      optional validated/versioned server snapshot, never executable Lua
module.bee     verified signed Lua/assets/defaults
data.db        local SQLite; core-owned outbox/metadata when sync is enabled
```

Data lookup: `local/` → `state.bee` → `module.bee`. Local overrides visibly mask only
the device-effective value; expose provenance and leave canonical synced state untouched.
Executable lookup: verified bundle,
optionally preceded by an **operator-enabled** local override, visibly modified/unsigned.
Snapshots cannot supply Lua or overwrite verified identity/compatibility metadata. Module
APIs cannot enable/write executable overrides. Tinkering still obeys the sandbox.

SQLite stays for volume. Authorizer/API protects other files and core outbox metadata.
Sync opt-in is separate from gateway/speech use and never silently replaces local truth.
At-least-once sync deduplicates `(device, module, origin, id)`; `AUTOINCREMENT` avoids id
reuse, DB recreation gets a new origin, retry/reload does not. ACK and versioned snapshot
installation are separate. No client merge or stale snapshot replacement.

Admission is not durable completion. Require a platform persistence barrier, not merely
rename or an in-memory SQLite commit. Browser VFS/flush and exclusive writer ownership
need tests; tab close is no flush guarantee. RAM mode is explicitly volatile. Quotas
fail visibly; retention never silently evicts unacknowledged outbox work.

## UI, capabilities and audio

Flow UI via constrained `canlin.ui`, not arbitrary pixel positions. Below 40 em: one column;
at or above: permits two under core layout rules. Theme fonts/colors/logo/spacing live in
`theme.bee` with device overrides; DPI and minimum touch sizes are centrally enforced.

Query capabilities, not OS names; unsupported, denied and unavailable differ. Browser
Fetch/Streams/WebSocket uses browser TLS, not arbitrary native sockets. MQTT needs a WS
broker/gateway there; serial/USB is optional and permission-gated. Speech is optional.
Audio uses bounded rings, 16 kHz mono 16-bit PCM at its protocol boundary and adapter-owned
rate conversion. Capture overflow fails the stream; underrun emits silence/status.
Tests use fake clocks and file/memory devices, the real Lua sandbox and actual WASM for
browser integration. Report host, P2-QEMU, browser and physical-device evidence separately.

Related: `beepack-format` (libbee format), `canlin-coordination` (ownership/serialised handoff).
