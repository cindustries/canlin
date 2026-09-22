# CanLin — event runtime design

Status: draft for review. This specifies the event and platform boundary introduced by
[the system design](2026-09-22-canlin-system-design.md). It is a runtime contract, not an
implementation plan or a new OS process. CanLin owns this code; canlin-os supplies the native
kernel/devices and test image. The same contract covers rawfb, SDL2 and browser/WASM.

## 1. Scope and non-goals

The event runtime connects input, timers, networking, storage completion, audio status,
module lifecycle and redraw without making Lua aware of OS event facilities. It must
stay responsive on the P2 floor and must yield to the browser when compiled to WASM.
Direct-provider and optional gateway streams use this same machinery; neither the
scheduler nor ordinary local state requires a canlin-server connection. Model-driven view
updates follow the [LLM/UI contract](2026-09-22-canlin-llm-ui-design.md), not privileged
callbacks from provider text.

This is not a general-purpose message broker, a distributed event log, a thread per
module, or a promise of lossless delivery from an arbitrarily fast device. v1 has no
unrestricted module-to-module event bus. Backend/library selection is separate from the
contract: use the smallest implementation that passes the same tests on all targets.

## 2. Ownership and execution

- One runtime owner thread owns the Lua states, module registry, UI state, Nuklear,
  scheduling and delivery decisions. Only that thread invokes a module callback.
- Backend callbacks enqueue bounded owned data or update a defined latch. They do not
  call Lua, render, retain borrowed Lua memory or mutate module state.
- Native audio callback threads and explicitly isolated workers are allowed. Each
  worker owns its job resources; shared database connections or other mutable objects
  are not accessed concurrently without an explicit owner/transfer protocol.
- `init`, `event`, `sync` and `draw` run serially, with no callback nested inside another.
  A backend that completes synchronously still queues completion for a later dispatch.
  Rendering happens only when dirty; events/state transitions request invalidation.

Run-to-completion means return **or enforced budget termination**, not unlimited runtime.
Every path into Lua, including any enabled coroutine path, shares the callback budget.
A runaway module is faulted and unscheduled; it cannot evade termination by repeatedly
catching hook errors with `pcall`. Whether restricted coroutines are exposed and how the
host enforces an uncatchable scheduling boundary must be demonstrated by sandbox tests
before that API is enabled. A fault must not stop other modules or kill the runtime.

Native calls have their own input, result, allocation and work limits. SQLite progress
handlers help with interruptible query work but do not bound every C extension. Work
without a provable small bound must be split, rejected, or isolated in an appropriate
worker. The worker's existence does not waive memory limits or allow unsafe forced
termination. Whole-process CPU/memory accounting and backend buffers are budgeted too;
a Lua allocator limit alone is not the module's resource boundary.

## 3. Platform drive contract

A backend feeds normalised events and drives bounded scheduler turns. The owner returns
after a finite callback/work budget, exposing whether runnable work remains and the next
monotonic deadline. It never waits synchronously for work whose completion needs that
same owner or browser event loop.

| Backend | Waiting, wakeup and delivery |
|---|---|
| rawfb / native POSIX | Wait on descriptors plus a wakeup source and nearest deadline; `poll()` is suitable here. Drain readiness only within a bounded turn. |
| SDL2 / native | Integrate SDL events, transport readiness and wakeups through a platform adapter. Do not assume SDL supplies a portable pollable event descriptor. |
| Browser / WASM | Schedule work through browser callbacks/tasks; return control after each turn. Request animation frames when dirty. No blocking `poll()`, busy-wait or simulated synchronous network I/O on the browser thread. |
| Offscreen / test | Fake clock and controllable event sources; stepping a turn and injecting a completion need no real device or sleep. |

Idle with no pending work or deadline does not repeatedly dispatch or redraw. An audio
stream or active deadline may legitimately require wakeups; this is not idle. A wakeup
must not be lost between observing an empty queue and entering the backend's wait.

`draw` is scheduled fairly with other runnable work. A self-invalidating module cannot
force unbounded redraws or prevent network, lifecycle or other modules from progressing.
Dirty state is coalesced; rendered frames are not a reliable event queue.

## 4. Event identity, ownership and order

Internally an event identifies its kind, source, target module instance/generation and,
when applicable, operation handle. Payloads are owned values or bounded handles with
explicit lifetime, not borrowed backend pointers. Source and target are assigned and
validated by the core, not trusted from a network payload or module argument.

Every module restart allocates a new runtime generation. A delayed completion from an
old generation cannot call the new module, access its Lua state, or reuse one of its
handles. Generation identifiers must not be recycled while old producers still exist.
Persisted outbox origin/sequence identities are separate: a runtime reload must not
change the identity of operations already accepted into the outbox. Assistant requests add a
conversation/turn scope, and generated-view input carries its view revision. Reusing a
widget id after replacement must not make an old queued click target the new view.

Ordering is FIFO for non-coalesced events in a single `(module, generation, source)`
stream. There is no global order between unrelated sources or modules. Code needing a
cross-source dependency establishes it explicitly, for example by starting a second
operation only after the first has completed. All dispatch happens on the owner thread.

Module callbacks receive the established `event(name, payload)` API. Normalised input
feeds the owner-thread UI; `init`, snapshot `sync(info)` and `draw` are distinct scheduler
entry points with the same budgets and lifecycle checks. Exact event names, payloads and
API signatures are defined by the core API spec, not guessed independently by adapters.

## 5. Finite capacity and overload

Queues and operations have declared finite limits for both item count and total bytes.
There are per-module bounds and a process-wide bound, including backend buffers, pending
payloads and results not yet consumed. The core spec must give numeric limits and tests
before implementation is accepted; a configurable limit still has a bounded maximum.

| Event/work class | Overload behaviour |
|---|---|
| Invalidation, audio level, other explicitly latest-state values | Coalesce to current state; do not allocate one item per update. |
| Pointer motion / resize | Coalesce only compatible adjacent updates without crossing an ordered input boundary. `motion A → button-down → motion B` cannot become `motion B → button-down`. |
| Finite operation completion | Reserve a bounded terminal-message slot and result capacity at admission; otherwise refuse to start. |
| Ordered input / unsolicited stream data | Pause reading/production where possible; otherwise report source overflow and reset/close the affected interaction or stream. No silent success or unbounded buffering. |
| Stop, fault, connection-loss status needed to terminate work | Use bounded control state/latches outside the data queue; repeated identical state coalesces. They cannot be blocked behind a full data queue. |

One reservation covers **one terminal outcome**, not arbitrary stream messages. Large
results must fit a separate admitted result budget or use bounded chunks/handles with
backpressure. Backend buffering counts too: repeatedly calling browser WebSocket send
while its outbound buffer grows is not bounded transport.

Dropped pointer/button edges require input-state reconciliation, so overflow cannot leave
a permanently pressed key. A broken stream reports the break rather than silently
continuing as though every byte/message arrived. For application protocols, transport
flow control is separate from durable ACK: do not acknowledge retained work that was not
actually accepted under that protocol's contract.

Audio capture uses a bounded ring. Overflow terminates/reports the recording stream;
playback underrun emits silence and a coalesced status. Audio callbacks do not allocate
unboundedly, log synchronously or enter Lua. Raw PCM travels in the audio pipeline, not
as thousands of queued Lua events.

Dispatch uses fair module rotation and bounded callback/native work per turn. Lifecycle
control can pre-empt ordinary queued work at a callback boundary. It is not an unlimited
high-priority event class a module can use to starve everything else.

## 6. Operations and cancellation

Starting a finite asynchronous operation either fails admission immediately or returns
a module-scoped request handle after reserving its resources. Completion is never an
inline Lua call, even for cache hits or immediately available results.

Every admitted operation reaches exactly one **core terminal state**: success, failure or
cancelled. These outcomes share the same reserved completion capacity. A live module is
notified at most once; a dead generation receives no Lua notification. Admission, local
commit, transport send and server ACK are different milestones, not interchangeable
meanings of success.

Cancellation is idempotent. It prevents future Lua delivery once the owner processes it,
but cannot retract a callback already running or undo an external side effect. For a
storage append, the store owner must report whether commit occurred; an accepted outbox
row remains eligible for sync even if the originating callback has disappeared. Cancelling
a UI request is not a protocol-level undo operation.

Resources stay alive until their actual owner has finished. Late completion from a dead
scope still releases its payload, reservation and worker references; only Lua delivery
is discarded. If a native job cannot be interrupted, it runs within its admitted bound
and its result is retired safely. Do not free a database, pack mapping or buffer still
referenced by that job.

Unsolicited streams are separate scoped resources: opening a stream may complete once,
while subsequent bounded messages obey that stream's flow-control contract. Closing it
stops future delivery and eventually releases all producer resources.

## 7. Timers and lifecycle

Timers use a monotonic clock, not wall time or timestamps received from the server.
v1 needs one-shot timers plus cancellation; a module can rearm after handling a timer.
Timer count is bounded and each timer has a generation-scoped handle. Expiration queues
work for a later scheduler boundary rather than invoking Lua inside the clock adapter.

Timer cancellation suppresses an expiration not yet dispatched, including one already
queued in the same generation. It does not interrupt a running callback. A suspended
browser/mobile app does not accumulate an unbounded catch-up queue: on resume, each
still-live due one-shot timer is eligible once and the scheduler remains budgeted.
The clock adapter must specify suspension behaviour; unavailable or stalled host time
must not be mistaken for successful periodic real-time execution.

Module stop/reload proceeds in this order:

1. Mark the old scope inactive: no new admissions or Lua callbacks.
2. Cancel timers/subscriptions, close scoped streams and request operation cancellation.
3. Retire or drain work through its owners; suppress old-scope Lua delivery but perform
   all completion accounting and cleanup.
4. Release resources only after their owners no longer reference them. A new generation
   cannot reopen mutable storage still exclusively owned by a retiring operation.
5. Start the new sandbox when the required resources are available; retain persistent
   outbox identities and accepted work. A failed new module does not erase that work.

Backgrounding/suspension stops capture, invalidates unusable transport state and preserves
already durable pending work. Resume rechecks capabilities/permissions, reconnects as
needed and redraws. Neither mobile nor browser targets promise continuous execution in
the background or a final chance to flush storage when the app/page is killed.

## 8. Capabilities and persistence

The core exposes runtime capabilities and current availability through `canlin.sys`; modules
do not branch on platform-name guesses. Persistent storage, microphone, serial/USB,
network transport and background execution each have their own availability/permission
state. Capability discovery does not pre-authorise an operation: permission can change,
and every request validates its scope and current access.

An unsupported operation fails explicitly; it never succeeds against a pretend device.
The same Lua module can degrade its UI while using the same API on all supported hosts.
Web transport uses Fetch/WebSocket with browser TLS and a browser-compatible auth flow.
No arbitrary TCP/UDP, framebuffer device file or kernel driver is promised inside WASM.

Storage completions distinguish accepted work from restart-durable work. The platform
adapter owns its persistence barrier. Native file replacement requires the appropriate
flush/rename/directory-sync sequence; a browser filesystem may require asynchronous
synchronisation. A SQLite transaction against memory does not by itself satisfy a durable
browser completion. Explicit volatile mode is visible and has its own completion result.

No correctness depends on `unload`/tab-close callbacks successfully flushing. Browser
storage eviction, quota failure and denied permission are observable recovery cases.
Only one runtime may own a device identity/outbox storage namespace as a writer at a
time; another tab must acquire exclusive ownership or fail visibly. The browser storage
spec must test crash recovery and release of writer ownership, not merely a happy reload.

## 9. Required evidence before implementation is accepted

Use fake clocks and injected producers rather than sleeps in portable contract tests.
The test writer may propose missing adversarial cases; the dispatcher owns acceptance.

| Scenario | Required observation |
|---|---|
| Inline backend/cache-hit completion during `draw` | Completion delivered only after the callback boundary; no re-entry. |
| A busy module beside an interactive module | Both make progress within the declared work bounds; input/redraw is not starved. |
| Endless Lua, caught hook errors, enabled coroutine path | Runtime regains control, faults the offending scope and keeps others responsive. |
| Expensive SQLite/native extension/result | Declared work and memory bounds hold or the request fails explicitly. |
| Saturated queue and result-byte budget | Further admission fails; reserved terminal outcomes/control state still retire safely. |
| Pointer updates around button edges | Coalescing preserves the defined input ordering; overflow clears stuck interaction state. |
| Timer expires, then cancel before dispatch | No timer callback; repeat cancel is harmless. |
| Reload with pending timer, worker and snapshot download | No old-generation callback or stale handle reuse; all resources are eventually retired. |
| Cancel races with storage commit | Exactly one core outcome; committed outbox work is not silently lost or treated as undone. |
| Browser pause/resume, missing permission, quota failure | Bounded recovery, explicit status, no hidden durable-success claim or timer/audio backlog. |
| Idle without deadlines or active I/O | No periodic dispatch/redraw spin; newly arriving work reliably wakes the native/browser adapter. |

Run portable tests on host and P2-QEMU, then backend integration against the actual
WASM build in a browser and against native adapters. Fixture tests do not prove physical
device performance, mobile background policy or P2 timing; qualify those separately.

## 10. Implementation boundaries

The core/API spec supplies numeric budgets, C interfaces, operation/event payloads and
Lua timer/cancellation signatures. The browser slice chooses the VFS and writer-exclusion
mechanism; the audio/IoT specs choose framing and protocol subsets. These choices must
preserve the ownership, finite-capacity, ordering and lifecycle rules above.

Do not create a new framework or a family-wide library solely to host this scheduler.
Start inside CanLin with narrow platform adapters and a deterministic offscreen harness.
Extract shared code only when a real second consumer demonstrates the same contract.
