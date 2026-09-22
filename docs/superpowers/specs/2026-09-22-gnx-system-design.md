# gnx — system design spec

Status: draft for review. This is the family-level design. Each sub-project gets its own
spec and plan; the first one is `libbee` (`~/dev/libbee/docs/superpowers/specs/`).

## 1. What gnx is

A thin-client runtime and the operating-system images that carry it. One static C binary
renders screens, records and plays audio, talks to a server and runs Lua modules. All
intelligence lives on the server: speech recognition, speech synthesis, the LLM, and the
source of truth for every module's data. The device shows, listens, speaks and caches.

The first hardware is two Pentium II laptops with 1 GB RAM. They are the design floor:
what runs there runs on the x86 tablets, the desktop, Android and iOS. One client source,
five hosts.

Principles that decide every later question:

- **Dumb core, smart modules, authoritative server.** The core is finished once it can
  render, store, sync, and stream audio. Features are Lua modules. Truth is on the server.
- **Fits everywhere before looks perfect.** Screens are described as flow and laid out
  per device. Branding is a theme file.
- **Own formats where they are small.** BeePack for bundles and configuration, MsgPack on
  the wire, SQLite where data has volume, Lua for logic.
- **The weakest host wins.** i686 without SSE, PUC Lua, no X11, no browser, no SIMD.

## 2. Targets

| Target | Display / input | Delivery | Owner |
|---|---|---|---|
| Pentium II laptop | rawfb + evdev | gnx-os image, initramfs in RAM, syslinux | gnx-os |
| x86 tablet (Bay Trail / Surface era) | rawfb + evdev, touch via i2c-hid | gnx-os image, UEFI | gnx-os |
| Desktop (development, kiosks) | SDL2 | binary | gnx |
| Android tablet, phone | SDL2 + NDK | sideloaded APK | gnx |
| iPad, iPhone | SDL2 + Xcode | signed app (TestFlight or 7-day dev signing) | gnx |

Compiler flags for every i686 build: `-march=i686 -mno-sse`. The i686 test suite runs
under `qemu-system-i386 -cpu pentium2 -m 1024` because the host silently executes SSE.

## 3. Decomposition and repos

| Part | Repo | Language |
|---|---|---|
| BeePack format and C library | `libbee` | C99 |
| Runtime core, Lua API, base modules, this spec | `gnx` | C99, Lua 5.4 |
| Images, kernels, boot media, QEMU harness | `gnx-os` | Buildroot |
| Fixture generation, `bee` CLI, later XS over libbee | `p5-beepack` | Perl |
| Reference IoT device, consumer of libbee | `sunriser8` | C, STM32 |
| Server: sync, audio, LLM, module distribution | `gnx-server` (not created yet) | Perl |

Order of work: libbee → core on the desktop with SDL2 → gnx-os QEMU image with rawfb →
audio and the server contract → mobile builds → IoT.

## 4. The core

Vendored, read-only under `third_party/`: Nuklear, Lua 5.4, cmp, stb_truetype, libbee,
SQLite amalgamation + sqlite-vec, mbedtls, monocypher (Ed25519 for module signatures;
tiny, C99, no SIMD, no allocation). MQTT is our own ~500-line 3.1.1 client on top of the
same socket layer, because every library found either allocates freely or assumes a
platform.

Subsystems, each one file or directory, each testable alone:

- **render**: Nuklear with a software rasterizer into an RGB buffer in RAM. Backends
  `rawfb` (copies only dirty rows to `/dev/fb0`) and `sdl2` (uploads the buffer as a
  texture). Redraw only on an event; idle blocks in `poll()`. One DPI scale factor from
  the theme or the platform.
- **input**: evdev (keyboard, mouse, touch) or SDL2 events, normalised to one event
  struct. On-screen keyboard is the platform's on mobile and a Nuklear widget on rawfb
  touch targets.
- **audio**: capture 16 kHz mono 16-bit PCM, playback the same; ALSA on images, SDL2
  audio elsewhere. Push-to-talk first (a key or a button on screen), voice activity
  detection later. Frames go to the server as binary WebSocket messages, raw PCM; a
  codec is a later ticket, Ethernet and WiFi carry 256 kbit/s without effort.
- **net**: sockets, TLS via mbedtls, HTTP/1.1 client, WebSocket client, the MQTT client.
  One event loop (`poll`), no threads in the core.
- **store**: libbee stacks per module and for the theme and the system; SQLite per
  module; the sync outbox.
- **modules**: locate, verify signature (Ed25519 over the `.bee` bytes with the image's
  public key), open the stack, load `lua#main` in a fresh sandbox, wire `gnx.*`.
- **sandbox**: Lua 5.4 state per module; `os`, `io`, `package`, `debug`, `load` with
  binary chunks removed; `require` resolves `lua#<name>` from the module's own stack;
  instruction-count hook to stop a runaway module; memory limit via the allocator.

## 5. Modules

A module is a BeePack. Reserved keys inside `module.bee`:

```
meta#name, meta#version, meta#title, meta#min-core      identity
lua#main                                                entry point, returns a table
lua#<name>                                              require-able chunks
font#<name>, icon#<name>, image#<name>                  blobs
<anything else>                                         the module's default values
```

The returned table implements callbacks the core calls: `init(ctx)`, `draw(ui)` (every
redraw), `event(name, payload)` (server push, MQTT message, timer, audio state),
`sync(rows)` (a new `state.bee` landed). Everything else is up to the module.

Screens are immediate mode, mirroring Nuklear one-to-one, with flow layout decided by
the core:

```lua
function M.draw(ui)
  ui:heading(M.title)
  ui:text(M.status)                      -- wraps
  for _, m in ipairs(M.messages) do ui:bubble(m.role, m.text) end
  M.input = ui:field("Nachricht", M.input)
  if ui:button("Senden") then M:send() end
  ui:meter("Mikrofon", gnx.audio.level())
end
```

Layout rules the core applies: content is one column below a width of 40 em and two
columns above; every interactive element is at least 2.5 em high; font size is the
theme's base times the DPI factor; nothing is positioned in pixels. A module cannot
break the layout, it can only choose what is on the screen.

## 6. Storage

Per module, on the writable data volume:

```
modules/<name>/module.bee     signed bundle, replaced by rename on update
modules/<name>/state.bee      server snapshot, replaced by rename on every sync
modules/<name>/local/         override dir, hashed .MP files, device-only
modules/<name>/data.db        SQLite: tables the module creates, plus `outbox`
theme/theme.bee, theme/local/
system/local/                 device id, server URL, network config, keys
```

Key lookup: `local/` → `state.bee` → `module.bee` (skill `beepack-format`). A hand edit
on the device is a file in `local/`; `lua#main` there overrides the module's code.

On the images the data volume is a small ext4 partition on the internal disk or a USB
stick, found by label `GNXDATA`; if none exists the runtime works from RAM and says so on
the status bar. On Android and iOS it is the app's data directory.

SQLite holds volume: sensor series (with a retention job per table the module declares),
chat history, embeddings the server delivered (sqlite-vec built without its optional
AVX/NEON paths, which are opt-in defines), and the outbox. The core opens each module's database with a per-module authorizer so a
module cannot attach another file.

## 7. Sync

The outbox is a table the core creates in every `data.db`:

```
outbox(id INTEGER PRIMARY KEY, ts INTEGER, kind TEXT, key TEXT, value BLOB)
```

`value` is MsgPack. A module writes rows through `gnx.sync.put(kind, key, value)`. When
online the core pushes unsent rows in order over the WebSocket, the server acknowledges
by id, the core deletes acknowledged rows. The server responds, at its own pace, with a
new `state.bee` for the module; the core writes it to a temp name and renames. The module
gets `sync(rows)` after the rename. The client never merges; conflicts are the server's
problem, and the server has every device's outbox in order.

Module updates travel the same way: the server offers `module.bee` with a version, the
core verifies the signature, renames, and restarts the module.

## 8. Audio and the AI loop

Push-to-talk: the module calls `gnx.audio.record(true)`; the core streams PCM frames to
the server on a dedicated WebSocket stream tagged with the module name. The server does
STT, runs the LLM, and streams back text events (shown by the module as they arrive) and
PCM for TTS, which the core plays. `gnx.audio.level()` exposes the input level for a
meter. Nothing about speech runs on the device.

## 9. IoT

Devices speak MQTT with MsgPack payloads; topics follow `gnx/<site>/<device>/<channel>`.
The core keeps one MQTT session per configured broker and dispatches messages to modules
by subscription. A device's configuration is a BeePack the device can be handed over
MQTT or HTTP (SunRiser already updates this way). `gnx.serial` opens a serial or USB
serial device for a module, so a laptop is a gateway for devices that have no network.

## 10. Theme

`theme.bee` keys: `theme#color#<role>` (background, surface, text, muted, accent,
danger, success), `theme#font#body`, `theme#font#heading` (TTF blobs), `theme#font#size`
(base in px at DPI 1.0), `theme#logo` (PNG), `theme#spacing` (em unit multiplier),
`theme#radius`. Loaded once at start into Nuklear's style table; a `theme/local/`
override allows per-device tweaks. Missing keys fall back to the built-in theme inside
the binary.

## 11. Server contract (for gnx-server, later)

- `GET /modules` → list of `{name, version, sha256}`; `GET /modules/<name>.bee`.
- WebSocket `/sync`: client sends `{device, module, rows[]}`, server acks ids and pushes
  `{module, state_bee_url}`.
- WebSocket `/audio`: binary PCM frames up, text events and PCM frames down.
- Auth: device key pair generated at first boot, public key registered once by the
  operator; every request signed. Details in the server's own spec.

## 12. Testing

- C unit tests per subsystem, an offscreen render backend for screen tests (render into
  memory, assert on pixels or on the Nuklear command list).
- Lua tests run through the real sandbox: a module fixture per test, the server replaced
  by a local fixture speaking the contract above.
- Every test suite runs on the host with SDL2 and on i686 under QEMU (harness from
  gnx-os).
- No test touches a real server, audio device or display.

## 13. Open points, deliberately

- Exact `gnx.*` namespaces beyond `ui`, `store`, `db`, `sync`, `net`, `mqtt`, `audio`,
  `serial`, `sys` are settled in the core spec, after libbee.
- The laptops' NIC, sound and graphics chips (gnx ticket #3) decide the first real kernel
  fragment; QEMU carries everything until then.
- 32-bit toolchain on the dev box: none today; gnx-os provides the musl cross toolchain
  that libbee's `test32` also uses.
- Licence for `gnx` and `gnx-os`: libbee is Perl (Artistic/GPL, like BeePack); the
  runtime and images are undecided.
