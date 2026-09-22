# CanLin

*Linux that can. Straight out of a can.*

A thin-client runtime: portable C with Nuklear, Lua 5.4, libbee and SQLite, running Lua
modules on anything from a Pentium II laptop to mobile devices and a browser/WASM build.
Native images carry one static binary; apps and the browser use platform adapters around
the same bounded, event-driven runtime. Features are Lua modules shipped as BeePacks.
Choose an LLM provider or compatible URL, API key and model; the built-in assistant can
populate its GUI through validated declarative tool calls. Inference stays remote, but
local data and interaction work without deploying a canlin-server. A user's own server can
optionally add a provider gateway, speech, sync or other services.

## Design first

There is no runtime implementation or Makefile yet.

- [System design](docs/superpowers/specs/2026-09-22-canlin-system-design.md): targets, storage,
  sync, trust boundaries, server contracts and verification gates.
- [Event runtime](docs/superpowers/specs/2026-09-22-canlin-event-runtime-design.md): owner-thread
  dispatch, platform wait/yield, bounded queues, cancellation, lifecycle and capabilities.
- [LLM connections and GUI](docs/superpowers/specs/2026-09-22-canlin-llm-ui-design.md): direct
  providers, optional gateways, credentials, streaming and the model's constrained GUI tool.
- `.claude/skills/canlin-core/SKILL.md`: the implementers' briefing summary, subordinate to
  those specs.

The browser target compiles CanLin itself to WASM; it is not a second web frontend or a
whole-PC emulator. Host, P2-QEMU and browser tests prove different things.

Family: `libbee` (format/library), `canlin-os` (images/toolchain/QEMU), `p5-beepack` (Perl
tooling), `sunriser8` (reference IoT device) and the planned optional `canlin-server`
(gateway, speech and explicitly enabled sync/hosted services).
