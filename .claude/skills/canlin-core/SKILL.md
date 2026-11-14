---
name: canlin-core
description: "Use when working in CanLin — implementing, reviewing or designing the runtime, the Lua modules, the provider/GUI-tool integration, the event scheduling, the platform adapters, the storage/sync, the audio, the sandbox boundaries or the browser/WASM support."
user-invocable: false
allowed-tools: Read, Grep, Glob
model: sonnet
---

# CanLin — architecture and invariants

CanLin is a **standalone-capable thin-client runtime**: portable C and sandboxed Lua. Choose
provider/compatible URL, API key and model; the built-in assistant connects to a remote LLM and
renders model-produced views. No canlin-server deployment is required. Inference stays remote;
local data is authoritative unless explicitly opted into a sync service. Gateway, speech, sync
and hosted services are independent options.

## The repo

```
src/ev/        event scheduler, fake clock, offscreen driver
src/module/    module lifecycle, the callback seam
src/sandbox/   PUC Lua 5.4 sandbox: budgets, hooks, metered calls
src/net/       HTTP/1.1, TLS (mbedtls), SSE, streaming JSON, sockets, DNS
src/llm/       provider dialects (openai-chat, anthropic-messages), fixture transport
src/assistant/ the built-in assistant: tool loop, response and view
src/ui/        flow widgets, graph editor, theme
src/render/    offscreen Nuklear rasterizer, the built-in font
src/shell/     arrangement, chrome, focus
src/loop/      the native main loop; the poll/wait seam
src/android/   NativeActivity adapter
src/app/       application bootstrap
include/canlin/ public headers; a <subsystem>_limits.h holds that subsystem's numbers
t/<part>/      one test binary per part; exit 0 means pass
third_party/   vendored, never edited in place
patches/lua/   applied to Lua at build time (the pattern step-counter)
docs/superpowers/specs/  the specs; docs/adr/ the decisions
```

Each subsystem adds itself to the build with `mk/<name>.mk` (objects into `LIBOBJ`, test
binaries into `TESTS`).

## Build and test

| Command | What it proves |
|---|---|
| `make` | host build, all suites |
| `make test` | host suites; each binary under `timeout(120)`, a hang fails instead of blocking |
| `make TARGET=i686` | the P2 target; needs `i686-linux-musl-gcc`, `-march=i686 -mno-sse` |
| `make TARGET=android-armv7 test-device` | on-device suites; `ANDROID_SERIAL` picks the device |
| `make TARGET=android-armv7 apk` | the NativeActivity APK |

The host runs SSE, the target does not: only a P2 run catches an illegal instruction, so a green
host test is not a green i686 test. QEMU user-mode (`qemu-i386 -cpu pentium2`) is not the
canlin-os full-system harness — name which one ran.

## Conventions

- Every public symbol is `canlin_` / `CANLIN_` plus the subsystem (`canlin_ev_`, `canlin_sse_`,
  `canlin_sandbox_`). Static functions are free. A number from a spec belongs in exactly one
  place: `include/canlin/<subsystem>_limits.h`.
- A Lua API function either succeeds with its documented returns or returns `nil, code, message`
  with `code` out of `full`, `invalid`, `unsupported`, `denied`, `unavailable`, `cancelled`.
- Vendored code is read-only. A fix is a patch file under `patches/` plus a ticket, never an
  edit inside `third_party/`.
- A more specific spec controls its seam. An actual contradiction between specs is a finding to
  return, not permission to choose silently. A failing test is a claim: state it before changing
  behaviour or weakening the assertion.

## The specs

Read the spec that owns the seam before editing it. The most specific one wins.

| Spec | Owns |
|---|---|
| `2026-09-22-canlin-system-design.md` | family, subsystem contracts, the split |
| `2026-09-22-canlin-event-runtime-design.md` | scheduling, platform seam, lifetimes |
| `2026-09-23-canlin-core-api-design.md` | C/Lua signatures, numeric budgets, cancel semantics |
| `2026-09-23-canlin-ui-design.md` | flow API, graph editor, theme, GUI tool schema |
| `2026-09-23-canlin-shell-design.md` | arrangement, chrome, focus |
| `2026-09-23-canlin-llm-providers-design.md` | dialects, presets, custom endpoints |
| `2026-09-23-canlin-http-transport-design.md` | HTTP/1.1, TLS, credential binding |
| `2026-09-23-canlin-llm-agency-design.md` | grants, model-written modules, versions |
| `2026-09-23-canlin-module-roles-design.md` | roles, capabilities, device modules |
| `2026-09-23-canlin-accounts-design.md` | accounts, instances, config storage |
| `2026-09-24-canlin-libs-design.md` | `libs/`, `require` |
| `2026-09-24-canlin-native-loop-design.md` | the main loop, poll/wait |
| `2026-09-30-canlin-android-adapter-design.md` | Android as a driver layer |

A spec is a sectioned document: a title, a status line, scope, numbered sections with the
decisions inline, and the open questions left open. It describes code that exists, not code
that is wished for.

## Hardware floor and targets

P2 i686 without SSE sets CPU and resources, not automatic platform compatibility.

- Use `-march=i686 -mno-sse`, PUC Lua 5.4, no LuaJIT, no mandatory SIMD.
- Native images use static musl and software Nuklear rendering, without X11.
- Targets: P2 laptop and x86 tablet via rawfb + evdev (canlin-os images), desktop via SDL2,
  Android via the NativeActivity APK, iOS via SDL2/Xcode, browser via Emscripten/WASM.

## Core, modules and events

Core owns stable mechanisms; features are Lua modules/BeePacks. Each module has its own Lua
state, storage and handles. One owner thread enters Lua and owns UI/module state. Backends drive
bounded turns: native wait/wake, browser callback/yield.

- Dispatch `init`, `event`, snapshot `sync(info)` and `draw` without re-entrancy.
- Bound queue items/bytes, in-flight work, results and callbacks per module/process. Rotate
  fairly; native C work needs limits beyond Lua hooks and allocation budgets.
- Coalesce latest state, not ordered input edges. Reserve bounded terminal outcome/result
  capacity before starting finite operations. Stream data has separate backpressure.
- Use monotonic timers and generation-scoped handles. Cancel is not rollback. Late work still
  gets cleanup but never calls a replacement Lua state or reuses its handles.
- Redraw only when dirty; idle waits/yields instead of spinning. No generic broker or
  unrestricted cross-module pub/sub in v1.

Sandbox all enabled coroutine/native paths too; catching hook errors must not evade scheduler
termination. SQLite/extensions need bounded inputs/results and interruptible or isolated work.

## Direct LLM and model-produced UI

- Credentials are opaque caller/endpoint-bound capabilities; core injects auth. No raw API key
  in Lua, prompts, tools, generic stores or logs. Redirects cannot change binding.
- Partial tool arguments never execute. Validate complete calls and semantic outcomes; HTTP EOF
  is not proof of success. Bound assembly and automatic continuation rounds.
- GUI tool v1 atomically replaces an assistant-owned declarative flow view. Allowlisted
  widgets/actions only, no generated Lua/HTML/JS/shell. Keep trusted settings/chrome out.
- Input includes view revision; stale clicks cannot target reused widget ids. Local edits are
  not transmitted until explicit submit. Tool results reflect actual acceptance.
- Normalised text/tool/outcome events do not discard required provider continuation data.

## Storage, optional sync and trust

```
local/         device-only data overrides
state.bee      optional validated/versioned server snapshot, never executable Lua
module.bee     verified signed Lua/assets/defaults
data.db        local SQLite; core-owned outbox/metadata when sync is enabled
```

Data lookup: `local/` → `state.bee` → `module.bee`. Local overrides visibly mask only the
device-effective value; expose provenance and leave canonical synced state untouched. Snapshots
cannot supply Lua or overwrite verified identity/compatibility metadata. Module APIs cannot
enable/write executable overrides.

SQLite stays for volume. Sync opt-in is separate from gateway/speech use and never silently
replaces local truth. At-least-once sync deduplicates `(device, module, origin, id)`. ACK and
versioned snapshot installation are separate. No client merge or stale snapshot replacement.

Admission is not durable completion: require a platform persistence barrier, not an in-memory
SQLite commit. Browser VFS/flush and exclusive writer ownership need tests. RAM mode is
explicitly volatile. Quotas fail visibly; retention never silently evicts unacknowledged outbox
work.

## UI, capabilities and audio

Flow UI via constrained `canlin.ui`, not arbitrary pixel positions. Below 40 em: one column; at
or above: permits two under core layout rules. Theme fonts/colors/logo/spacing live in
`theme.bee` with device overrides; DPI and minimum touch sizes are centrally enforced.

Query capabilities, not OS names; unsupported, denied and unavailable differ. Browser
Fetch/Streams/WebSocket uses browser TLS, not arbitrary native sockets. Speech is optional.
Audio uses bounded rings, 16 kHz mono 16-bit PCM at its protocol boundary and adapter-owned
rate conversion. Capture overflow fails the stream; underrun emits silence/status.

Tests use fake clocks and file/memory devices, the real Lua sandbox and the actual WASM for
browser integration. Report host, P2-QEMU, browser and physical-device evidence separately.

Related: `beepack-format` (libbee format), `canlin-coordination` (ownership, serialised handoff).
