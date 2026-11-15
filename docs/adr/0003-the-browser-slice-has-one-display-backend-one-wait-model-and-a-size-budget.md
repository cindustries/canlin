# ADR 0003 — The browser slice has one display backend, one wait model and a size budget

- Status: accepted — decided by the owner on 2026-11-15, recorded here on 2026-11-15
- Date: 2026-11-15
- Tags: browser, wasm, display, wait, size, budget

## Context

Three questions of the browser slice were open. Which display backend the browser target
uses, how the browser target waits, and how large the WASM module may be.

The system design names the browser in the target table as "Emscripten + SDL2/Canvas,
browser I/O adapters" and delivers it as "WASM + loader + assets over HTTPS"
(`docs/superpowers/specs/2026-09-22-canlin-system-design.md:62`). That line offers two
display backends and decides neither. The UI spec says "The browser target stays one
canvas" (`docs/superpowers/specs/2026-09-23-canlin-ui-design.md:75`) — that is the shape
of the target, not a build decision. No spec carries a number for the WASM module.

### The display backend

The runtime already has the seam a display backend needs. `canlin_app_config` carries a
present hook: a function pointer that takes the frame and the number of dirty rows
(`include/canlin/app.h:51-54`), which the app hands to the loop's display step
(`src/app/app.c:291-292`). The hook copies pixels out and nothing else; it runs inside
the loop's frame, so it may not call back into the runtime (`include/canlin/app.h:13-14`).

What it copies is a plain RGB buffer in RAM — three bytes per pixel, byte order R, G, B,
rows `pitch` bytes apart (`include/canlin/render.h:7`) — and `canlin_render_end()`
reports how many rows changed (`include/canlin/render.h:73-75`). The Android backend is
the worked example of the contract: its hook finds the dirty span in `r->dirty` and
copies that span into the window, and returns without touching anything else
(`src/android/activity.c:139-150`).

The alternative was SDL2 compiled through Emscripten. SDL2 is already a target on desktop
and on iOS (system design §2), so it is a known quantity. In the browser it would be a
second display backend standing beside the hook that already exists, and it would bring
its own input stack with it — while the browser target has no SDL2 window, no SDL2 event
queue, and no reason to have either.

### The wait model

The event runtime names the browser wait directly: "Browser / WASM | Schedule work through
browser callbacks/tasks; return control after each turn. Request animation frames when
dirty. No blocking `poll()`, busy-wait or simulated synchronous network I/O on the browser
thread" (`docs/superpowers/specs/2026-09-22-canlin-event-runtime-design.md:61`). The
system design repeats it — "the browser schedules callbacks and always returns control"
(`system-design.md:128-130`).

The runtime offers two seams for this. The clock is one: `canlin_ev_clock` is the
scheduler's time source (`include/canlin/ev.h:35-38`), and the clock's suspension
behaviour is the platform adapter's to state (`include/canlin/ev.h:30-34`). The wait is
the other: `canlin_loop_wait_fn` is the hook the loop calls instead of blocking
(`include/canlin/loop.h:78-84`), and the loop's config takes it as a field
(`include/canlin/loop.h:86-93`).

Two facts of the native loop do not survive the move to the browser. The loop owns a wake
pipe, created unconditionally in `canlin_loop_new` (`src/loop/loop.c:54`): it exists so
that a producer on another thread can end the wait. The browser has no second thread, so
there is nobody to wake it. And `canlin_loop_run()` iterates until quit
(`src/loop/loop.c:267-282`); in the browser the wait returns at once, so `run()` would
spin the browser's thread instead of waiting.

### The size of the WASM module

No spec has a number. The system design says the browser target is delivered as "WASM +
loader + assets over HTTPS" (`system-design.md:62`): the transfer is compressed, so the
number that matters is the compressed one.

The base is measurable now that the toolchain is there. The runtime — its own C,
Nuklear, Lua 5.4 with its libraries, the embedded font and the provider dialects — was
compiled for wasm32 with emcc 7.0.0 at `-Os` and linked with every symbol the runtime
defines exported, so that the linker keeps the whole runtime instead of dropping the
parts the browser glue does not call yet. The result is a **604,772 byte** wasm module;
compressed with gzip -9 it is **286,727 bytes**, with zstd -19 **254,293 bytes**. The
loader Emscripten emits beside it is a separate download and not part of this number
(144,342 bytes here, uncompressed).

Two variants bound the number from both sides. At `-O2` the module is 632,275 bytes and
280,469 bytes compressed — the optimisation level moves the uncompressed size, not the
compressed one. Without the native socket stack, which the browser's own transport
replaces, the module is 587,791 bytes and 278,291 bytes compressed. The compressed size
sits near 280 KB whatever the flags and whatever the transport.

Where the weight is, measured on the host objects as the loadable sections (`.text`,
`.rodata*`, `.data*`) of the object files the module carries:

| part | where | loadable bytes |
|---|---|---|
| Nuklear, the rasterizer | `src/render/nk_impl.o` (2,640,568 bytes on disk with debug info) | 271,696 |
| CanLin's own C | `src/`, without Nuklear and the font | 262,947 |
| Lua 5.4 core and its libraries (26 objects) | `third_party/lua/src`, `patched/lua/lstrlib.o` | 172,250 |
| the embedded Inter Latin subset | `src/render/font_builtin.c` | 23,972 |
| **sum over the partial link** (the rows above plus alignment) | | **732,738** |

Mbed TLS is in neither the table nor the module: a browser transport uses the browser's
own HTTP, WebSocket and TLS facilities rather than tunnelling the native socket/TLS
stack (`system-design.md:118-119`). For the measurement above the native TLS entry
points were stubbed out at the link, because the module carries no TLS stack.

The measurement can be repeated: the runtime's sources are compiled with
`emcc -Os -Iinclude -Ithird_party/lua/src -isystem third_party/nuklear`, and the link
passes `--export=<symbol>` for each of the 1,233 symbols the objects define, so that
nothing is collected away. The compressed figures are the sizes of the `.wasm` file
after gzip -9 and after zstd -19.

## Decision

**The browser slice shows through the canvas, waits through the browser, and ships a WASM
module of at most 512 KiB compressed.**

### 1. The display backend is the canvas, over the existing present hook

- The browser slice implements the present hook of `canlin_app_config`
  (`include/canlin/app.h:51-54`); it writes the dirty rows of the frame into one 2D
  canvas.
- There is no second display path. The frame in RAM is what the canvas shows; the
  browser slice does not draw into the canvas itself.
- SDL2 is not compiled into the browser module. No SDL2 dependency, no SDL2 event
  handling, no SDL2 window.
- The hook keeps its contract: it copies pixels out and returns; it does not call into
  the runtime.

### 2. The wait model: an animation frame when dirty, a timer for the deadline

- The browser slice never blocks. It requests an animation frame when the runtime is
  dirty, and arms a `setTimeout` for the nearest deadline when there is one
  (`event-runtime-design.md:61`, `system-design.md:128-130`).
- The runtime's tick comes from the browser's monotonic clock, bound as a
  `canlin_ev_clock` (`include/canlin/ev.h:35-38`).
- The loop is driven from the browser callback: the callback runs the scheduler's turn,
  the pump, the source steps and the display step, and then returns control. The
  browser's callback is the wait.
- `canlin_loop_run()` does not run in the browser. It would spin, because the wait
  returns at once.
- The wake pipe does not exist in the browser: `canlin_loop_new()` creates it
  unconditionally (`src/loop/loop.c:54`), and in the browser there is no second thread
  that would ever wake it.

### 3. The size budget: at most 512 KiB compressed

- The WASM module that the browser downloads must not exceed **524,288 bytes**
  (512 KiB) in its compressed form. The number is the size of the transferred bytes —
  the content-encoded body over HTTPS, as the system design has it for the browser
  delivery (`system-design.md:62`).
- The number is measured with gzip, the more conservative of the two encodings a server
  may use: the module measured today is 286,727 bytes with gzip -9 and 254,293 bytes
  with zstd -19, so a server that serves brotli or zstd has more room, not less.
- The budget covers the WASM module. The loader and the browser glue are a separate
  download and get their own budget when the browser slice has one.
- The budget is a bound, not a target. The measured module is 286,727 bytes compressed
  today; the bound leaves room for the browser adapter, the fetch transport and the
  growth the slice still needs, and it is tight enough that a second runtime or a large
  vendored dependency cannot be added unnoticed.

## Rationale

The canvas is the right display backend because the seam for it already exists. The
present hook is the runtime's one way of getting pixels to a screen, the Android backend
already implements it, and the contract it offers — copy the dirty rows out of an RGB
buffer, return — is exactly what a 2D canvas wants. The browser slice inherits a
display backend that the rest of the runtime already knows how to drive, and the
browser's own drawing API is reached through one function pointer.

SDL2 in Emscripten would deliver the same picture by a longer road: a full input and
render stack compiled into the module, a second display backend next to a hook that
already does the work, and a vendored dependency of a size that has to be carried in the
module the browser downloads — against a budget this ADR sets. The UI spec's "the
browser target stays one canvas" is the same answer in the shape of the target: one
canvas, one backend.

The browser cannot block, and the runtime's own wait seam says so without needing to be
told twice: the event runtime forbids a blocking `poll()` on the browser thread, and the
system design says the browser always returns control. The two seams the runtime offers
— the clock and the wait function — fit that: the browser's clock becomes the
scheduler's tick, and the browser's callbacks become the wait. What does not fit is the
native loop's own machinery: the wake pipe serves a second thread that the browser does
not have, and `canlin_loop_run()` assumes a wait that blocks, which the browser's does
not. Naming both here keeps the implementation from discovering them as a spin on a
tab or a frozen page.

The size budget exists because the specs have none and the browser slice is the first
target where the runtime's own weight is felt by someone else's network. The number is
measured, not guessed: the runtime compiled to wasm is 604,772 bytes, 286,727 of them
after gzip, and neither the optimisation level nor the transport moves that figure by
more than a few per cent. The 512 KiB bound sits about 1.8 times above the measured
module — room for the browser adapter, the fetch transport and the growth the slice
still has to do, and no more.

It also has a second purpose: it is a constraint on the open questions that follow.
SQLite is not in the tree today, so it is not in the measurement — and a wasm build of
the SQLite amalgamation is of the order of a megabyte before compression, an estimate
and not a measurement made here. Half a megabyte of compressed room does not hold it,
so the question of SQLite in the browser has to be answered against this number rather
than alongside it.

## Consequences

- The browser slice has one display path: the present hook. The canvas backend writes
  the dirty rows the runtime reports and nothing else, and a frame that changes nothing
  reports no dirty rows and costs no canvas traffic
  (`include/canlin/render.h:73-75`).
- The present hook runs inside the loop's frame, so the canvas backend may only copy
  pixels out (`include/canlin/app.h:13-14`). It may not draw into the runtime, and it
  may not call the browser's drawing API asynchronously from the hook; a deferred canvas
  update is the browser glue's job, not the hook's.
- The browser module carries no SDL2 and no TLS stack. The transport is the browser's
  own (`system-design.md:118-119`).
- `canlin_loop_new()` cannot be used unchanged in the browser: it creates the wake pipe
  unconditionally (`src/loop/loop.c:54`), and the browser has neither a second thread
  nor a use for it. The browser slice drives the scheduler's turn, the pump, the source
  steps and the display step itself, from its callback. Which of the two ways this is
  built — a browser adapter that does not use `canlin_loop`, or a loop mode without a
  pipe — is an implementation choice for the platform lane, not a decision here.
- `canlin_loop_run()` is not on the browser path. A browser slice that called it would
  spin the thread.
- The clock the browser slice binds decides the suspension behaviour, and the event
  runtime's rule for it is fixed: a suspended browser app does not accumulate an
  unbounded catch-up queue, and on resume each still-live due one-shot timer is
  eligible once (`event-runtime-design.md:168-172`). Which clock the browser adapter
  provides — one that keeps counting across a suspension or one that stops — is the
  adapter's to document (`include/canlin/ev.h:30-34`), and it is left open here.
- The size budget is checkable: it is the size of the transferred bytes of the module.
  The wasm build measures it, and the budget is the line the build must not cross.
- Four questions of the browser slice follow from this decision and are **not** decided
  by it:
  - **The storage backing.** The system design names two: "a virtual filesystem/SQLite
    VFS backed by suitable browser storage such as OPFS or IndexedDB"
    (`system-design.md:260-262`). Which one, and what its flush, quota, multi-tab
    ownership and recovery behaviour are, is a decision of its own — and it decides
    whether SQLite comes into the browser at all, which the size budget constrains.
  - **SQLite in the browser.** SQLite is not in the repo today: `docs/THIRD-PARTY.md:19-26`
    still lists it and sqlite-vec under "Planned (system design §4)". The system design
    is explicit that "merely compiling SQLite to WASM does not settle those choices"
    (`system-design.md:470-471`).
  - **The browser clock across a suspension.** See above; the event runtime's rule is
    fixed, the adapter's clock is not.
  - **The credential path.** "Browser-direct credentials necessarily share the browser
    page/origin's trust boundary… Default to session-only storage there"
    (`llm-ui-design.md:72-77`). The credential path follows from the storage choice,
    which is still open.

### The spec sentences this decision obliges

The line numbers below point at the committed HEAD (c8176c2). The spec is amended to
carry this decision afterwards — that is the doc trace — which moves them; the
sentences are named by section as well, so they stay findable.

- UI §2 (`ui-design.md:75`): "The browser target stays one canvas" — the browser slice
  has exactly one canvas and one display backend, and it is the present hook.
- System design §2 (`system-design.md:62`): "Emscripten + SDL2/Canvas, browser I/O
  adapters" — this decision takes the Canvas half of the line and drops SDL2 from the
  browser build; and "WASM + loader + assets over HTTPS" — the module is delivered
  compressed, so the size bound is a bound on the compressed bytes.
- System design §2 (`system-design.md:118-119`): "A browser transport uses the browser's
  HTTP, WebSocket and TLS facilities rather than tunnelling the native socket/TLS
  stack" — the browser module carries no TLS stack, which is what the size measurement
  assumes.
- System design §4, render (`system-design.md:132-134`): "Nuklear with a software
  rasterizer into an RGB buffer in RAM… Redraw on invalidation, not an unconditional
  frame loop" — the present hook copies the dirty rows out of that buffer, and the
  redraw-on-invalidation rule is what bounds the canvas traffic.
- System design §4, events (`system-design.md:128-130`): "the browser schedules
  callbacks and always returns control" — the wait model.
- Event runtime §4 (`event-runtime-design.md:61`): "Schedule work through browser
  callbacks/tasks; return control after each turn. Request animation frames when dirty.
  No blocking `poll()`, busy-wait or simulated synchronous network I/O on the browser
  thread" — the wait model in the spec's own words: the animation frame when dirty, and
  no blocking wait.
- Event runtime §7 (`event-runtime-design.md:168-172`): the suspension rule for timers —
  named here as an open question this decision does not answer.
- System design §8 (`system-design.md:260-262`): the browser storage adapter and its two
  named backings — named here as an open question this decision does not answer.
- LLM/UI §3 (`llm-ui-design.md:72-77`): the browser credential path — named here as an
  open question this decision does not answer.

## Alternatives considered

### The display backend

- (a) The canvas, over the existing present hook. Chosen.
- (b) SDL2 in Emscripten. Rejected: a heavy dependency and a second display backend
  standing beside a hook that already does the work, for the same picture. It would also
  bring an input stack the browser target has no use for, and it would have to be
  carried in the module this ADR bounds.
- (c) A browser-only drawing path beside the hook — the browser slice writes into the
  canvas itself, outside the present hook. Rejected: two display paths, and the hook's
  contract (copy pixels out, call nothing back) would hold for one backend and not for
  the other.

### The wait model

- (a) An animation frame when dirty, a timer for the deadline, the loop driven from the
  browser callback. Chosen.
- (b) A blocking `poll()` in the browser. Rejected: forbidden by the event runtime, and
  it would freeze the page.
- (c) A timer for everything, no animation frame. Rejected: the animation frame is the
  browser's own paint cadence, and the spec asks for it when the runtime is dirty.
- (d) A `MessageChannel`/`postMessage` timer for the deadline arm. Not chosen now:
  `setTimeout` is what the spec names and what every browser provides. Its known cost —
  background tabs clamp timers — belongs with the open question about the browser clock
  and suspension, and not with this decision.

### The size budget

- (a) A hard upper bound on the compressed module: 512 KiB. Chosen.
- (b) No number, measured at release time. Rejected: it leaves the budget invisible
  until it has been broken, and there is no spec number to fall back on.
- (c) A bound on the uncompressed module. Rejected: the browser pays the compressed
  size, so the number has to be the one the browser pays. The uncompressed figure is
  recorded above as the base the bound was derived from.
