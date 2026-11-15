# CanLin — the browser adapter (the third trace)

Status: draft for review (2026-11-15). It carries out
[ADR 0003](../../adr/0003-the-browser-slice-has-one-display-backend-one-wait-model-and-a-size-budget.md)
(accepted 2026-11-15): one display backend (the canvas over the existing present hook),
one wait model (an animation frame when dirty, a timer for the deadline) and one size
budget (at most 1 MiB compressed). It adds the browser row to the
[native loop](2026-09-24-canlin-native-loop-design.md) §4 and states the contract of the
four adapters of the third trace — clock, wait, present, input — on the seams that exist
today. **None of the browser slice is built yet**: the build (k156), the adapter trace
(k158), the transports (k159, k160) and the browser gate (k161) are open. The seams this
spec names exist and carry the native and the Android trace; their browser bindings do not
exist yet. Every section says which part is which.

## 1. Scope

In scope: the four platform adapters of the browser slice — the clock, the wait, the
present path and the input path — and the rules that hold them together: the order of
the work per callback, the behaviour at a suspension, the size budget of the module and
the evidence that will prove it.

Out of scope: the storage backing (OPFS or IndexedDB), whether SQLite comes into the
browser, the transports (Fetch/Streams and WebSocket), the toolchain and the build, the
loader and its delivery, and the credential path. Each is named in §8 with the card that
owns it.

The browser slice is the third trace, next to the native one (rawfb/SDL2,
[native loop](2026-09-24-canlin-native-loop-design.md)) and the Android one
([Android adapter](2026-09-30-canlin-android-adapter-design.md)). It brings no new
runtime concept: the core is portable — the scheduler takes its clock injected
(include/canlin/ev.h:35-38), the renderer is a software rasterizer without OS calls
(include/canlin/render.h:7), the module layer knows no filesystem. What the slice adds
is a binding of four seams to browser facilities.

## 2. The shape of the target

**Decision — one output, one canvas.** The browser target stays one canvas
([UI design](2026-09-23-canlin-ui-design.md) §2.1). The slice registers one output with
the shell, with the identity `browser:main` — the pattern of `android:main`
(Android adapter §5).

**Decision — one display backend: the canvas, over the existing present hook.** ADR 0003
§1 decides it: the browser slice implements the present hook of `canlin_app_config`
(include/canlin/app.h:51-54) and writes the dirty rows of the frame into one 2D canvas.
SDL2 is not compiled into the browser module — no SDL2 dependency, no SDL2 event
handling, no SDL2 window. The frame in RAM is what the canvas shows; the slice does not
draw into the canvas itself.

**Decision — no native TLS stack in the module.** The browser transport uses the
browser's HTTP, WebSocket and TLS facilities rather than tunnelling the native
socket/TLS stack ([system design](2026-09-22-canlin-system-design.md) §4,
system-design:118). The module therefore carries no mbedtls; the build (k156) has to keep
the socket and DNS objects out of the link as well.

*Seam to mark for the maintainer:* the [system design](2026-09-22-canlin-system-design.md)
§2 target table still names the browser as "Emscripten + SDL2/Canvas"
(system-design:62), and §4 render still says that "`sdl2` uploads the buffer as a texture,
including through Emscripten in the browser" (system-design:133-134). ADR 0003 takes the
Canvas half of the first line and drops SDL2 from the browser build. The system design
text is not updated by this spec — the maintainer decides whether to amend it there or to
leave the decision to live in ADR 0003 and here. The two texts disagree today; the ADR
is the newer one and says so.

## 3. The clock (Takt)

The scheduler's time source is injected: `canlin_ev_clock`
(include/canlin/ev.h:35-38). The browser adapter binds it to `performance.now()`,
converted to whole milliseconds — the runtime's clock unit is `canlin_ev_time`,
milliseconds, unsigned (ev.h:28). `performance.now()` is the browser's monotonic clock
since the page's time origin; it does not run backwards, and if a host clock ever stepped
backwards the scheduler clamps to the last observed value (ev.h:30-32).

**Decision — the clock counts across a hidden page.** The browser throttles timers and
stops the animation frames of a hidden page; it does not stop the clock. The runtime
relies on the rule the event runtime fixes for a clock that keeps counting (event runtime
§7, ev.h:30-34): on resume, each still-live due one-shot timer is eligible **once**. A
timer that fell due while the page was hidden fires once, late by the hidden duration —
there is no backlog of missed expiries and no queue of them.

What the runtime relies on holds either way: whether the browser kept the clock running
(the timer is late by the hidden time) or froze the page with the clock (the timer fires
when the clock reaches its deadline), each still-live due one-shot timer is eligible
exactly once on resume.

**Decision — what a suspension means for the UI.** Three things, and no fourth:

1. **Nothing is drawn.** A hidden page gets no animation frame, so the runtime presents
   nothing. There is no offscreen redraw either: the render step runs only inside a
   callback, and no callback runs.
2. **The state stays.** Module state, conversation and view survive a hidden page; a
   hidden page is not a reset. On resume the shell redraws the current state.
3. **There is no catch-up.** Pending invalidations coalesce into one redraw, the due
   timers fire once each, and an input queue that overflowed while the page was hidden is
   reported as an overflow (event runtime §5), not silently dropped.

A browser may freeze, throttle or discard a background page. CanLin promises no
continuous background execution in either case (event runtime §7: "Neither mobile nor
browser targets promise continuous execution in the background or a final chance to
flush storage when the app/page is killed").

## 4. The wait (Wait)

The browser has no descriptor to poll and may not block. The event runtime names the
wait model in its own words (event runtime §3, the Browser/WASM row: "Schedule work
through browser callbacks/tasks; return control after each turn. Request animation frames
when dirty. No blocking `poll()`, busy-wait or simulated synchronous network I/O on the
browser thread"), and the system design repeats it (system-design:129-130).

Two facts of the native loop do not survive the move:

- **The wake pipe does not exist in the browser.** `canlin_loop_new()` creates it
  unconditionally (src/loop/loop.c:54). It exists so that a producer on another thread
  can end the wait; the browser has no second thread, so there is nobody to wake it.
- **`canlin_loop_run()` does not run in the browser.** It iterates until quit
  (src/loop/loop.c:267-282), and the browser's wait returns at once — the loop would
  spin the browser's thread instead of waiting (ADR 0003 §2).

**Decision — the blocking loop is not on the browser path; the slice runs the same
steps from the browser callback.** `canlin_loop_new()` cannot be used unchanged either,
because of the wake pipe above. The order, the bounds and the reasons are the native
loop's (§3):

```
browser callback:
  1. ev turns: canlin_ev_turn() until !more or CANLIN_LOOP_TURNS_MAX (4) turns
  2. pump hook (the transport's pump)
  3. step every source once
  4. render: for each dirty output one frame (begin / shell_render / end, present)
  5. arm: an animation frame if the runtime is dirty, a timer for the nearest deadline,
     nothing at all if there is nothing to do
  return control
```

The timeout is computed from `canlin_ev_peek()` after the work, so nothing enqueued
during the steps is missed; the turns are bounded; a frame is drawn at most once per
output per iteration.

**Decision — three ways to be woken, one of them at a time.**

| Situation | Arm | Who runs the next iteration |
|---|---|---|
| The runtime is dirty (a frame is owed) | `requestAnimationFrame(cb)` | the browser, at the next paint |
| A deadline exists (nearest timer, source deadline or display tick) | `setTimeout(cb, delay)` | the browser, at the deadline |
| Nothing pending | none | the next browser event: an input event, a transport completion, a visibility change, a message from the page |

The third row is the idle rule (event runtime §3, native loop §3): an idle runtime arms
no timer and redraws nothing. Any event that arrives re-enters the iteration, and the
iteration arms the next callback by itself. The animation frame is requested when the
runtime is dirty, not on a fixed cadence — the browser decides when it fires.

**Decision — no simulated synchronous network I/O on the browser thread.** The Fetch
transport (k159) delivers its chunks through the scheduler; it never blocks the callback
to "read" a response (event runtime §3). The rule is what keeps the page responsive, and
the event runtime forbids it by name.

**The wait seam.** The native wait seam is `canlin_loop_wait_fn`
(include/canlin/loop.h:78-81): like `poll()`, it waits on descriptors with a timeout.
That contract cannot be met in the browser — there is no descriptor, and a wait that
returns at once is exactly what turns `canlin_loop_run()` into a spin. The browser's wait
is therefore not a wait function but an *arming*: the adapter computes the next deadline
and arms a browser callback. `canlin_loop_wait_fn` stays what it is, a native seam.

**Decision — how this is factored is the platform lane's.** ADR 0003 leaves open whether
the browser slice gets its own iteration function or `canlin_loop` grows a mode without a
pipe; the ADR calls that an implementation choice for the platform lane. This spec fixes
the observable behaviour — the order of the five steps, the seams they use and the arming
rules — and not the factoring.

*Seam in flight:* the platform lane is adding a backend seam to the tree
(include/canlin/backend.h, k167): a backend hands the runtime its metrics, its present
and its input, and — for a native backend — its descriptors and its wait. The browser
fills the first three and has no use for the last two: `pollfds` has no descriptor to
report, and `wait` is the native blocking wait that this section replaces with an
arming. If that seam becomes the plug-in point for every backend, the browser backend
is the one without a wait hook. The spec does not decide that; it names it, so the two
lanes do not build two truths.

## 5. The present path (Present)

The seam is the present hook of `canlin_app_config` (include/canlin/app.h:51-54): after
`canlin_render_end()` reports the dirty row count, the hook is called with the frame and
the row count, and it "copies pixels out, nothing else" (app.h:51-53). The hook runs
inside the loop's frame, so it may not call back into the runtime (app.h:13-14).

The Android backend is the worked example of the contract: its hook finds the dirty span
in `r->dirty` and copies that span into the window, then returns and touches nothing else
(src/android/activity.c:139-150, with `canlin_android_dirty_span()` and
`canlin_android_blit()` in src/android/adapter.c). The browser hook is the same shape
with the canvas as the target.

**Decision — the hook writes the dirty rows into the canvas and returns.** The runtime's
frame is RGB: three bytes per pixel, byte order R, G, B, rows `pitch` bytes apart
(include/canlin/render.h:7). The canvas wants RGBA. The adapter converts the dirty span
row by row — R, G, B from the frame, alpha 255, since CanLin paints opaque. A frame that
changes nothing reports no dirty rows (render.h:73-75) and costs no canvas traffic.

**Decision — the conversion is a pure function, tested on the host.** The browser slice
follows the Android pattern (src/android/adapter.c:1-3: "No Android headers: built and
tested on every target"): the dirty-span search and the row conversion are plain C99
without any browser header, so the host suite tests them on every target; the browser
glue hands the adapter the canvas and the pixels and adds nothing else. That is what
makes the present path testable without a browser.

**Decision — the update is synchronous inside the hook.** The hook copies the pixels out
and returns. It does not schedule an asynchronous canvas update — a deferred canvas
update is the browser glue's job, not the hook's (ADR 0003, consequences). The hook may
not draw into the runtime and may not call back into it.

**Decision — one canvas, sized to the output.** The canvas' backing store has the
output's size in pixels. A change of the output size (a window resize) reallocates the
frame (canlin_app_resize, app.h:83); the next frame is drawn and presented whole. A CSS
size that differs from the backing store (a scaled canvas) is the glue's business — and
the input coordinates of §6 are mapped through the same ratio.

## 6. The input path (Eingabe)

The seam is `canlin_ui_input` (include/canlin/ui.h:213-224) and the app's route into the
shell (canlin_app_input, include/canlin/app.h:87). The normalised events are the
runtime's five: motion, button, key, text, scroll (ui.h:205-211). The browser delivers
them from pointer events and from the keyboard; the mapping into the runtime's normalised
form is a pure function, tested on the host, exactly as the Android input mapping is
(src/android/adapter.c, the touch state machine `canlin_android_touch_map()`).

**Decision — the pointer state machine follows the Android one.** `pointerdown`,
`pointermove` and `pointerup` on the canvas. The first pointer is the primary one; a
`pointerdown` while a press is held continues the held press instead of producing a
second edge (the Android rule: a lost `UP` must not leave a stuck press). A press is
`CANLIN_UI_IN_BUTTON` with `down` 1 or 0 and the primary button; a move is
`CANLIN_UI_IN_MOTION`; a release without a preceding press produces nothing. The
coordinates are the pointer's position relative to the canvas' rect, multiplied by the
ratio of the backing store to the CSS size — output pixels, not CSS pixels.

**Decision — the other three events.**

- `wheel` → `CANLIN_UI_IN_SCROLL` with `dy` in output pixels, positive scrolling the
  content towards the end (ui.h:211).
- `keydown` / `keyup` → `CANLIN_UI_IN_KEY` with a key code from a mapping table of the
  browser's `KeyboardEvent.code`, in the shape of the Android key table `key_of()`:
  letters, digits, Enter, Tab, Backspace, Delete, the arrows, Home, End, Shift, Ctrl,
  Escape. The browser's reserved combinations (F5, Ctrl+W, the browser's zoom) are not
  consumed: the adapter calls `preventDefault()` only for a key it maps and CanLin uses.
- A printable key → `CANLIN_UI_IN_TEXT` with the Unicode scalar value (ui.h:210).
  Composition (IME) and dead keys are not in v1 — a browser delivers composed text, and
  CanLin's own on-screen keyboard is the v1 path (Android adapter §6.1). Open point, §8.

**Decision — the queue rules are the runtime's.** `canlin_ui_input_valid()` rejects a
malformed event (ui.h:237). A full input queue is an overflow (event runtime §5): the
queue is reset and the stuck interaction state is reconciled, so a held pointer cannot
stay pressed.

## 7. The size budget of the module

**Decision — the WASM module the browser downloads is at most 1,048,576 bytes (1 MiB)
compressed.** The number is the size of the transferred bytes: the content-encoded body
over HTTPS, as the delivery of the browser target has it (system-design:62, "WASM +
loader + assets over HTTPS").

The number's origin (ADR 0003, "The size of the WASM module"): the measurement was made
before the browser build existed — the runtime's loadable C content was measured on the
host build and compressed as a proxy. The measurement is the sum of the loadable
sections (.text, .rodata*, .data*) over a partial link of the objects the browser module
would carry: CanLin's own C, Nuklear, Lua 5.4 with its libraries and the embedded font —
mbedtls is not in that set, because the browser transport uses the browser's own
facilities.

| part | loadable bytes |
|---|---|
| Nuklear, the rasterizer | 271,696 |
| CanLin's own C | 262,947 |
| Lua 5.4 core and its libraries | 172,250 |
| the embedded Inter Latin subset | 23,972 |
| **sum over the partial link** | **732,738** |

gzip -9 takes that to **333,319 bytes** (45.5 %). The real wasm module will be larger:
the wasm encoding carries its own per-function overhead, and the module adds the wasm C
library and the toolchain's runtime glue. The estimate is roughly 1.0–1.3 MB
uncompressed and 0.45–0.65 MB compressed, which leaves room inside the bound.

The budget covers the WASM module only. The loader and the browser glue are a separate
download and get their own budget when the browser slice has one (§8).

The budget is checkable: it is the size of the transferred bytes of the module, and the
browser gate (k161) is what proves it.

## 8. Open points

What this spec does not decide, with the card that owns it.

1. **The storage backing.** OPFS or IndexedDB (system-design:261-262). Its flush, quota,
   multi-tab writer ownership and recovery behaviour are a decision of their own (event
   runtime §8) — and they decide whether SQLite comes into the browser at all. The
   storage layer does not exist for any platform yet; k157 owns it.
2. **SQLite in the browser.** SQLite is not in the tree: docs/THIRD-PARTY.md:19-26 still
   lists SQLite and sqlite-vec under "Planned". The size budget constrains the answer —
   a wasm build of the SQLite amalgamation is of the order of a megabyte before
   compression, which does not fit inside the 1 MiB bound (ADR 0003, rationale).
3. **The credential path.** The core hands a transport a credential as a header bound to
   an origin (include/canlin/llm.h:89-96); the browser forbids many headers and the
   mapping is undecided (llm-ui:72-77: browser-direct credentials share the page's and
   the origin's trust boundary, session-only by default). k159 owns the Fetch transport;
   where the credential is stored follows from the storage choice (point 1).
4. **The WebSocket constructor.** It cannot set arbitrary auth headers
   (system-design:405-406): a short-lived connection ticket or an authenticated
   first-message exchange is needed, and the server spec picks the flow. k160 owns the
   transport.
5. **Text input beyond the printable key.** IME composition, dead keys and the browser's
   own text field: v1 is the printable key and CanLin's on-screen keyboard (§6).
6. **Multi-touch.** v1 is one primary pointer, like Android (Android adapter §6.1); a
   pinch in the graph editor is open.
7. **The loader's own size.** The budget of §7 covers the module; the loader and the
   glue get theirs when they exist.
8. **The clock of the glue.** §3 fixes the runtime's clock; the glue's own timing — the
   clamp the browser puts on the timers of a throttled tab — is a browser fact, not a
   CanLin promise.

## 9. What must still be built

The trace this spec describes does not exist yet.

| Card | What it builds |
|---|---|
| k156 | the WASM build of the core — no network, no storage — and the portable suites under node; the build keeps mbedtls and the socket/DNS objects out of the link (§2) |
| k158 | the adapter trace `src/browser` — clock, wait, present, input — in the Android pattern: pure C in the port, no browser header |
| k159 | the Fetch/Streams transport over the `canlin_llm_transport` seam |
| k160 | the WebSocket transport — new in the core, not only an adapter |
| k161 | the browser gate: a headless browser and a fixture server, running the nine cases of system-design:453-455 |

The evidence follows the house rule: the host suite proves the portable parts (the
conversion, the input mapping, the arming logic), the wasm build proves the module, the
browser gate proves the runtime in a browser. A green host run proves nothing about the
browser, and the browser gate proves nothing about the P2.
