# CLAUDE.md — CanLin

Thin-client runtime: portable C (Nuklear, Lua 5.4, libbee, SQLite + sqlite-vec) running
Lua modules on P2 laptops, x86 tablets, desktop, Android, iOS and browser/WASM. Native
images carry a static binary; browser I/O uses platform adapters. Standalone setup is
provider/compatible URL + API key + model; a built-in assistant renders validated GUI
tool calls. Inference is remote, while a self-hosted server is optional for gateway,
speech or sync. The specs come first, under `docs/superpowers/specs/`; the runtime they
describe now stands in `src/` with its suites in `t/`.

Build and test: `make`, `make test` (host); `make TARGET=i686` plus the canlin-os harness
under `qemu-system-i386 -cpu pentium2 -m 1024`; `make TARGET=android-armv7 test-device` on a
device. WASM/backend checks are additional gates, not substitutes. The host runs SSE, the
target does not — a green host test is not a green i686 test.

## The agent team

Start Claude in this folder. The session you talk to hands the work to the right agent.

- `canlin-scout`: reads and reports, changes nothing.
- `canlin-worker-runtime`: the event scheduler, the module seam and the sandbox — `src/ev/`, `src/module/`, `src/sandbox/`.
- `canlin-worker-netllm`: transport, dialects and the assistant — `src/net/`, `src/llm/`, `src/assistant/`.
- `canlin-worker-ui`: widgets, renderer and shell — `src/ui/`, `src/render/`, `src/shell/`.
- `canlin-worker-platform`: main loop, Android adapter, bootstrap — `src/loop/`, `src/android/`, `src/app/`.
- `canlin-test-writer`: writes the tests.
- `canlin-vendor`: brings in third-party code and runs its gates (sonnet, own worktree).
- `canlin-doc-writer`: writes the specs under `docs/superpowers/specs/`.
- `canlin-adr-auditor`: records the decisions in `docs/adr/`.
- `canlin-reviewer`: checks finished work and runs the tests, changes nothing.
- `canlin-release-manager`: checks a release; never commits, never publishes.
- `karr-coordinator`: routes family tickets, writes no code.

The agents carry their skills via `briefing.skills` (see `.claude/agents/`); the main agent
delegates rather than loading them. Skill sources live under `.claude/skills/` —
`canlin-core` (architecture and invariants) and `canlin-coordination` (family ownership) are
owned here, and `canlin-os` installs them with skilletor (source `canlin`);
`canlin-vendoring` (vendoring procedure) is owned here, canlin only; `beepack-format` comes
from `libbee` through skilletor. Work is tracked on the local `karr` board.

Nothing is pushed, released or posted without your word.
