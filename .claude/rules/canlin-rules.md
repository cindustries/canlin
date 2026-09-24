# CanLin House Rules

Apply to every task in this repository unless explicitly overridden. Bias: caution over
speed on non-trivial work; use judgment on trivial tasks. Loaded automatically at launch
(same priority as `CLAUDE.md`). Subagents get their conventions from the skills
force-loaded via `briefing.skills` — this file is for the orchestrating main agent.

## Engineering discipline

1. **Think before coding** — state assumptions; ask rather than guess. Push back when a
   simpler approach exists. Stop when confused; name what's unclear.
2. **Simplicity first, surgically applied** — minimum code that solves the problem, nothing
   speculative. The core is meant to be small and finished; features go into Lua modules.
3. **Goal-driven execution** — define success criteria, loop until verified.
4. **Surface conflicts, don't average them** — contradicting patterns: pick one (more
   recent / more tested), explain why, flag the other. Don't blend.
5. **Read before you write** — the spec under `docs/superpowers/specs/` and the seam's
   callers first. "Looks orthogonal" is dangerous in a runtime where every subsystem meets
   in the event loop.
6. **Tests verify intent, not just behavior** — reproduce a bug before fixing it; leave a
   regression test behind.
7. **A red test is a claim before it is a failure** — before changing code to turn a test
   green, say what the test asserts and whether your fix keeps that claim or replaces it.
8. **Checkpoint and fail loud** — summarize done / verified / left after each significant
   step. Separate host, P2-QEMU, WASM/backend and real-device evidence; a skipped gate is
   named, not implied green. No host run proves target instructions or browser durability.
9. **Match the codebase's conventions, even if you disagree** — conformance > taste.

## Delegation

Depends on whether the Agent/Task tool is available to you.

- **You can spawn subagents** (orchestrating main agent): do NOT touch behavior-relevant
  code yourself — delegate. Your lane: coordinate, inspect, plan, review diffs, run tests,
  manage git, write and edit specs and prose docs. When in doubt, delegate. Why: only the
  `canlin-*` agents get their skills force-loaded via `briefing.skills`; you get no briefing
  and would touch the sandbox, the storage stack or the render loop with too little
  context.

  | Task | Agent |
  |---|---|
  | Implement / refactor / debug the C core, the Lua API, built-in modules | `canlin-worker` (default) |
  | Write or extend tests (C, Lua, offscreen screen tests) | `canlin-test-writer` |
  | Vendor/update third-party C or Lua libs, run their platform gates | `canlin-vendor` |
  | Route a family ticket or execute an exclusive board-writer batch | `karr-coordinator` |

- **You cannot spawn subagents** (you ARE a `canlin-*` agent): the lock does not apply —
  implement, refactor, debug and test per these rules.

Behavior-relevant = every `.c`/`.h`/`.lua` outside `docs/`, the Makefile, `t/`. Specs,
README and prose docs are not — the orchestrator owns those.

## Coordination — karr board (always in scope)

Ticket coordination is the orchestrating agent's job, so `karr` is always in scope — don't
invoke the skill first, just use it. State lives in `refs/karr/*`. Family ownership,
including the planned canlin-server, is defined by `canlin-coordination`; cross-repo code work
is a ticket/dependency for the owner, never a local patch. Missing repos are reported,
not created implicitly.

- `karr list --compact` / `karr board` — open work · `karr show ID` — detail
- `karr create "Title" --priority high --tags a,b --body '…'` · `karr edit ID -a "note"`
  · `--claim NAME` · `karr move ID in-progress` — full surface: skill `kanban-issues-karr-cli`

**One board writer when fanning out** — workers return proposals, never board mutations.
The orchestrator writes sequentially, or delegates one exclusive batch to karr-coordinator
and does not write until it returns. Collect results before moving/handoffing/syncing cards;
sequential writes inside several parallel agents are not global serialisation.

## Release — never without permission

`make`, `make test`, QEMU runs are fine anytime. Tagging a release, publishing an APK or
app build, or pushing anything to a device fleet is STRICTLY forbidden without explicit
go-ahead.

## Hazards specific to this repository

- **The host runs SSE, the target does not.** Only `qemu-system-i386 -cpu pentium2`
  catches an illegal instruction; a green host test is not a green i686 test.
- **Vendored code is read-only.** `third_party/` (Nuklear, Lua, cmp, stb, libbee) is
  never edited in place — patch file plus ticket, or a ticket on `libbee`.
- **The sandbox is the security boundary.** Modules arrive from a server; any new
  `canlin.*` API is reviewed for what a hostile module could do with it before it ships.
- **Hardlinked skills.** `.claude/skills/` files shared with canlin-os and libbee are the
  same inode everywhere; `Edit`/`Write` detaches them. Use `cat > file <<'EOF'` and commit
  such a change on its own.

## Architecture — reference, don't restate

The thin-client model, hardware floor, storage stack, theme and responsive rules live in
skill `canlin-core`; the file format in `beepack-format`; repo ownership in
`canlin-coordination` (force-loaded per lane via `briefing.skills`). Do not duplicate that
content here.
