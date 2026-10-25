# CanLin House Rules

The shared house rules come from skilletor — bundle `getty-house@getty`: engineering
discipline, the delegation lock, the publish gate, karr coordination. They load as
`.claude/rules/.local.*.md`; if they are missing, run `skilletor sync` and publish nothing
until they load. This file adds what is true only here, and wins where it differs.

## What this repo is

A thin-client runtime: portable C with Nuklear, Lua 5.4, libbee and SQLite. The core is meant
to be small and finished; features go into Lua modules. Read first: the spec under
`docs/superpowers/specs/` and the seam's callers — every subsystem meets in the event loop.

## Agent team

| Task | Agent |
|---|---|
| Implement / refactor / debug the C core, the Lua API, built-in modules | `canlin-worker` (default) |
| Write or extend tests (C, Lua, offscreen screen tests) | `canlin-test-writer` |
| Vendor/update third-party C or Lua libs, run their platform gates | `canlin-vendor` |
| Route a family ticket or execute an exclusive board-writer batch | `karr-coordinator` |
| Read-only research, audit, gap analysis | `canlin-worker` ("read-only, change nothing") |

Behavior-relevant = every `.c`/`.h`/`.lua` outside `docs/`, the Makefile, `t/`. Specs, README
and prose docs are not — the orchestrator owns those.

Family ownership, including the planned canlin-server: skill `canlin-coordination`.
Cross-repo code work is a ticket/dependency for the owner, never a local patch. Missing repos
are reported, not created implicitly.

## Release

`make`, `make test` and QEMU runs are fine anytime. Tagging a release, publishing an APK or app
build, or pushing anything to a device fleet needs the maintainer's explicit go-ahead.

## Deviations from the house rules

- **No release-manager.** The team has no release-manager agent: the dispatching main agent
  owns commits; workers leave a commit-ready tree plus a proposed subject and `Changes` entry.
- **One board writer when fanning out** — workers return proposals, never board mutations. The
  orchestrator writes sequentially, or delegates one exclusive batch to `karr-coordinator` and
  does not write until it returns. Sequential writes inside several parallel agents are not
  global serialisation.

## Hazards

- **Evidence per platform.** Separate host, P2-QEMU, WASM/backend and real-device evidence — a
  skipped gate is named, not implied green. No host run proves target instructions or browser
  durability.
- **The host runs SSE, the target does not.** Only `qemu-system-i386 -cpu pentium2` catches an
  illegal instruction; a green host test is not a green i686 test.
- **Vendored code is read-only.** `third_party/` (Nuklear, Lua, cmp, stb, libbee) is never
  edited in place — patch file plus ticket, or a ticket on `libbee`.
- **The sandbox is the security boundary.** Modules arrive from a server; any new `canlin.*`
  API is reviewed for what a hostile module could do with it before it ships.

## Architecture — reference, don't restate

The thin-client model, hardware floor, storage stack, theme and responsive rules live in skill
`canlin-core`; the file format in `beepack-format`; repo ownership in `canlin-coordination`
(force-loaded per lane via `briefing.skills`).
