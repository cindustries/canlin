---
name: canlin-coordination
description: "Use when identifying ownership, proposing or handing off cross-repo work, or coordinating karr mutations across libbee, canlin, canlin-os, p5-beepack, sunriser8 and the planned optional canlin-server."
user-invocable: false
allowed-tools: Read, Bash, Glob
model: sonnet
---

# CanLin Cross-Repo Coordination

The family consists of independent repos under `~/dev/`, each with its own git history
and karr board. Cross-repo implementation is handed off through tickets, never patched
from a sibling agent. CanLin works standalone with a direct LLM provider; canlin-server is an
optional service owner, not a prerequisite for the runtime or local data authority.

## Repo ownership

| Concern | Owning repo |
|---|---|
| BeePack format, portable C library, POSIX/FatFS backends and fixtures | `libbee` |
| Runtime/events, Lua API/sandbox, direct provider and GUI-tool adapters, credentials, storage/sync client, audio/MQTT, WASM/browser app; runtime specs and these skills | `canlin` |
| Buildroot, kernel/drivers, hardware configs, toolchain, QEMU harness, images/boot media; optional whole-image emulator demo | `canlin-os` |
| Perl `bee` CLI, fixture generation, future XS binding over libbee | `p5-beepack` |
| STM32 firmware consuming libbee; SunRiser reference IoT device | `sunriser8` |
| Optional provider gateway, STT/TTS, sync authority/deduplication, service auth/wire protocol, module distribution | `canlin-server` (planned, not yet created) |

Remotes: `libbee`, `canlin`, `canlin-os` and `p5-beepack` live public at
`github.com/cindustries/<repo>`; `sunriser8` at `src.ci:ledaquaristik/sunriser8`;
`canlin-server` gets `cindustries/canlin-server` once created. `canlin` pulls libbee as
submodule `../libbee`, so both stay on the same host. Boards travel with the repo:
`karr sync --push` / `--pull` for `refs/karr/*`.

Missing checkout: record the dependency on the originating board and report it. Do not
invent a remote, create the repo or implement its side locally without explicit assignment.
Client/service contract work needs both owners; a fixture does not silently decide the
service protocol. Direct-provider client work belongs to CanLin, not automatically the server.

`canlin-core` and `canlin-coordination` are owned by `canlin` and hardlinked into canlin-os.
`beepack-format` is owned by libbee and linked into consumers. In-place truncating writes
(`cat > … <<'EOF'`) preserve shared inodes; Edit/Write do not. Check pre/post inode/link
counts and keep any eventual shared-skill commit separate from other changes.

## One board writer

The orchestrator is the sole board writer by default. Workers/test-writers return findings,
routing/dependency proposals and verification evidence; they do not independently claim,
create, move or sync tickets while fanned out.

The orchestrator may explicitly hand one exclusive mutation batch to karr-coordinator.
While it runs nobody else in that fan-out mutates a board. The coordinator returns all
affected repos/card ids and hands writer ownership back. Sequential commands inside
several parallel agents are not global serialisation.

## Handoff protocol (for the active board writer)

1. Read the target board; annotate an existing matching card rather than duplicating it.
2. Create/annotate the owner's card with origin repo/id and completion criteria.
3. Record the downstream pointer/dependency on the origin. Keep it traceable while blocked;
   do not archive merely because it was forwarded. Release claims during cross-repo waits.
4. On downstream completion record the resolution on the origin and reassess its acceptance
   criteria. Forwarded work is not automatically completed local work.

```bash
karr --dir ~/dev/<owner> list --compact
karr --dir ~/dev/<owner> create "<title>" --priority high \
  --tags from:<origin-repo>,<origin-id> \
  --body "Origin: <origin-repo> kN. <need, contract, completion criteria>"
```

Use `kanban-issues-karr-cli` for exact dependency/claim commands. Needs in two repos become
two linked tickets, not one combined patch. Consumers request libbee API changes on its board.

## Boundaries

- Read sibling code as needed; implement only in the assigned repo.
- Do not treat `~/dev` as a workspace to rewrite or commit as one.
- Do not mirror/drain public trackers; public reads/writes require explicit direction.
- Board ownership grants no source pushes, releases, repo creation or deployments.
