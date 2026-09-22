---
name: gnx-coordination
description: "Cross-repo workflow for the gnx family (libbee, gnx, gnx-os, p5-beepack, sunriser8). Which repo owns what. How to hand off work via karr tickets on the other repo's board."
user-invocable: false
allowed-tools: Read, Bash, Glob
model: sonnet
---

# gnx Cross-Repo Coordination

The gnx family is **independent repos** under `~/dev/`, each its own git repo with its
own karr board. There is no central workspace. Coordination happens via `karr` tickets
created on the owning repo's board.

## Repo ownership

| Concern | Owning repo |
|---|---|
| BeePack on-disk format (skill `beepack-format`), the portable C library, its POSIX/FatFS backends, fixtures | `libbee` |
| The runtime: C core, Nuklear backends, Lua sandbox and `gnx.*` API, storage stack, sync, audio, network, MQTT; the design spec; skills `gnx-core`, `gnx-coordination` | `gnx` |
| Buildroot external tree, kernel configs per hardware target, QEMU test harness, image and boot media | `gnx-os` |
| Perl tooling: `bee` CLI, fixture generation, future XS binding over libbee | `p5-beepack` |
| STM32 firmware consuming libbee; the SunRiser reference IoT device | `sunriser8` |

Skills `gnx-core` and `gnx-coordination` are owned by `gnx` and hardlinked into
`gnx-os`; `beepack-format` is owned by `libbee` and hardlinked into `gnx` (and into
`p5-beepack` once it links). Edit hardlinked skills only with an in-place truncating
write (`cat > … <<'EOF'`), never `Edit`/`Write`.

## Decision rule when a ticket arrives

```
about the file format or reading/writing .bee/.MP in C   → libbee
about how gnx uses storage, the Lua API, screens, sync    → gnx
about the kernel, drivers, Buildroot, boot media, QEMU    → gnx-os
about generating fixtures or the bee CLI                  → p5-beepack
about the firmware's use of the library                   → sunriser8
needs change in two repos                                 → two tickets, tagged with
                                                            from:<repo> and the origin id
```

The libbee API is a contract: gnx and sunriser8 file a ticket in `libbee` for a
change they need, they do not patch the library locally.

## Handoff

```bash
( cd ~/dev/<other-repo> \
  && karr create "<title>" --priority high --tags from:<this-repo>,<origin-id> \
       --body "Originated from <this-repo> ticket #N. <why it matters there>" )
```

Read the target board first (`karr list`); comment on an existing ticket with
`karr edit ID -a "…"` instead of opening a duplicate. When downstream is done, note the
resolution on the originating ticket. Do not hold a claim across a cross-repo wait.

## What NOT to do

- Edit code in another repo from this repo's agent. Cross-repo work = ticket.
- Treat `~/dev` as a workspace root. Each repo is independent.
- Mirror karr tickets into a public tracker or drain one into karr.
