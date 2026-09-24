# CLAUDE.md — CanLin

Thin-client runtime: portable C (Nuklear, Lua 5.4, libbee, SQLite + sqlite-vec) running
Lua modules on P2 laptops, x86 tablets, desktop, Android, iOS and browser/WASM. Native
images carry a static binary; browser I/O uses platform adapters. Standalone setup is
provider/compatible URL + API key + model; a built-in assistant renders validated GUI
tool calls. Inference is remote, while a self-hosted server is optional for gateway,
speech or sync. No runtime code yet — system, event-runtime and LLM/UI specs come first
under `docs/superpowers/specs/`.

Planned build/test interface: `make`, `make test` (host); `make TARGET=i686` and the
canlin-os harness under `qemu-system-i386 -cpu pentium2 -m 1024`. WASM/backend checks are
additional gates, not substitutes. These commands are targets for implementation, not
an existing Makefile.

## Delegation

Delegate behavior-relevant code to the right agent instead of touching it yourself —
principle and lane are in `.claude/rules/canlin-rules.md`.

| Task | Agent |
|---|---|
| Implement / refactor / debug the C core, event/platform adapters, Lua API and modules | `canlin-worker` (default) |
| Write/extend C, sandboxed Lua, offscreen and WASM contract tests; propose coverage gaps | `canlin-test-writer` |
| Vendor/update a C library or Lua lib, run its platform gates (ticket fixes lib, version, config) | `canlin-vendor` (sonnet, own worktree) |
| Propose family ticket routing; execute an explicitly delegated exclusive board batch | `karr-coordinator` |

The agents carry their skills via `briefing.skills` (see `.claude/agents/`); the main
agent delegates rather than loading them. Skill sources live under `.claude/skills/` —
`canlin-core` (architecture and invariants) and `canlin-coordination` (family ownership) are
owned here and hardlinked into `canlin-os`; `canlin-vendoring` (vendoring procedure) is
owned here, canlin only; `beepack-format` is owned by `libbee`. Work is
tracked on the local `karr` board.
