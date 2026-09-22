# CLAUDE.md — gnx

Thin-client runtime: one static C binary (Nuklear, Lua 5.4, libbee, SQLite + sqlite-vec,
ALSA/SDL2 audio, mbedtls, MQTT) that runs Lua modules on a Pentium II laptop, x86
tablets, desktop, Android and iOS. All intelligence lives on the server. No code yet —
the design spec comes first, under `docs/superpowers/specs/`.

Build and test: `make`, `make test` (SDL2 backend on the host); `make TARGET=i686` and
run under `qemu-system-i386 -cpu pentium2 -m 1024` (harness in `gnx-os`).

## Delegation

Delegate behavior-relevant code to the right agent instead of touching it yourself —
principle and lane are in `.claude/rules/gnx-rules.md`.

| Task | Agent |
|---|---|
| Implement / refactor / debug the C core, the `gnx.*` Lua API, built-in modules | `gnx-worker` (default) |
| Write or extend tests (C, Lua through the sandbox, offscreen screens) | `gnx-test-writer` |
| Route a ticket to libbee, gnx-os, p5-beepack or sunriser8 | `karr-coordinator` |

The agents carry their skills via `briefing.skills` (see `.claude/agents/`); the main
agent delegates rather than loading them. Skill sources live under `.claude/skills/` —
`gnx-core` (architecture and invariants) and `gnx-coordination` (family ownership) are
owned here and hardlinked into `gnx-os`; `beepack-format` is owned by `libbee`. Work is
tracked on the local `karr` board.
