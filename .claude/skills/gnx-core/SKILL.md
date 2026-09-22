---
name: gnx-core
description: "Load before implementing, reviewing or designing anything in the gnx runtime or a gnx module — the thin-client architecture, the hardware floor (Pentium II, no SSE), the C core / Lua module split, the storage stack (BeePack layers + SQLite), theme and responsive rules, sync and IoT principles."
user-invocable: false
allowed-tools: Read, Grep, Glob
model: sonnet
---

# gnx — architecture and invariants

gnx is a **thin client runtime**: one static C binary that renders screens, plays and
records audio, talks to a server, and runs Lua modules. All intelligence — speech
recognition, speech synthesis, the LLM, the source of truth for data — lives on the
server. The device shows, listens, speaks and caches.

The detailed design is the spec under `docs/superpowers/specs/`; where the spec is more
specific than this skill, the spec wins.

## The hardware floor decides the stack

First target: Pentium II laptops with 1 GB RAM. Everything that runs there runs on
every later target, so the floor is a design tool, not a limitation.

- **i686 without SSE.** Compile with `-march=i686 -mno-sse`. No LuaJIT (needs SSE2), no
  Firefox/Chromium (need SSE2), no library that assumes SIMD. The dev machine executes
  anything; only `qemu-system-i386 -cpu pentium2 -m 1024` catches an illegal
  instruction. Run the i686 build's tests under that QEMU, not on the host.
- **No X11, no browser.** The core draws with Nuklear into a software framebuffer.
  Backends: `rawfb` + evdev on gnx-os images, SDL2 everywhere else (desktop, Android
  via NDK, iOS via Xcode, x86 tablets could use either).
- **PUC Lua 5.4**, embedded, sandboxed. Same interpreter on every target.
- **Static musl binary** on the images; the same sources build as an SDL2 app.

## Targets — one client source, five hosts

| Target | Backend | Delivery |
|---|---|---|
| Pentium II laptop | rawfb + evdev | gnx-os image (Buildroot, initramfs in RAM) |
| x86 tablet (Bay Trail, Surface era) | rawfb + evdev, touch via i2c-hid | gnx-os image, UEFI |
| desktop (development) | SDL2 | binary |
| Android tablet / phone | SDL2 + NDK | sideloaded APK; never a custom Linux |
| iPad / iPhone | SDL2 + Xcode | signed app |

## Core vs modules

The C core is dumb and finished; modules are where features live.

**Core owns**: rendering (Nuklear, DPI scale, dirty-rect copy to the framebuffer,
redraw only on events — idle blocks in `poll()`), input, audio in/out, network (HTTP
and WebSocket over mbedtls, MQTT client), storage (libbee, SQLite + sqlite-vec), module
loading and signature check, the Lua sandbox and its `gnx.*` API.

**Modules are Lua**, shipped as BeePacks. A module declares screens through the
`gnx.ui` API, keeps its state in its own SQLite file and its own override directory,
and talks to the server through the core. Modules never see `os`, `io`, raw `require`
or another module's files.

Screens are described as **flow**, never as pixel positions: heading, then field,
then field, then button. The core lays that out for 800x600 landscape and for a phone
in portrait. Layout breakpoints are in font units, touch targets have a minimum size,
and the whole scale follows one DPI factor. "Fits everywhere" beats "looks perfect".

## Storage stack per module

```
local/         override directory (hashed .MP files) — device-only, never synced
state.bee      snapshot from the server — the synced truth for this module
module.bee     the module itself: Lua, fonts, icons, defaults — signed
data.db        SQLite: bulk data, time series, chat history, embeddings, outbox
```

Key lookups walk `local/` → `state.bee` → `module.bee` (skill `beepack-format`).
Local-only configuration and hand edits go into `local/`; putting `lua#main` there
overrides the module's code, which is how you tinker on the device.

**SQLite is for volume**, not for configuration: IoT sensor series, chat logs, vector
search over embeddings the server delivered (sqlite-vec, built without SIMD). Never
write sensor readings into the override directory.

**Sync**: a module writes changes as rows into an outbox table in `data.db`. The core
pushes them when online; the server answers with a new `state.bee` that lands by
atomic `rename`. The client never merges. Offline just grows the outbox.

## Theme

`theme.bee` is its own layer with `theme#…` keys: colors, TTF font blobs, logo, spacing.
It maps onto Nuklear's style table at startup; a `local/` override on top allows a
per-device tweak. Company branding is a theme file, never a code change.

## IoT is first-class

Devices speak MQTT with MsgPack payloads; a device's configuration is a BeePack. The
SunRiser controllers (sunriser8) are the reference device family — they are BeePack
native already. Serial/USB device access is a core API, so a laptop can act as a gateway.

## Related

- `beepack-format` — the file format the storage stack is built on.
- `gnx-coordination` — which repo owns what (libbee, gnx, gnx-os, p5-beepack, sunriser8).
