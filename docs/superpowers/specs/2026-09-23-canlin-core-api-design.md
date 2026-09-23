# CanLin — core API design (v1 slice)

Status: draft for review. Fixes the API shapes, numeric budgets and open semantics that
the [event-runtime design](2026-09-22-canlin-event-runtime-design.md) and the
[system design](2026-09-22-canlin-system-design.md) §13 defer to "the core API spec", for
the first runtime slice (system design §3, step 2). It is written against the code that
exists on `main` (`src/ev`, `src/net`, `src/sandbox`) and records where that code must
change. Subsystems not needed by the slice are listed in §9 with their own follow-ups.

## 1. Scope

In scope: module lifecycle and callbacks, the C seam between scheduler and sandbox,
error and handle conventions, timers and cancellation, `canlin.sys`, numeric budgets and
the semantic decisions collected on canlin#10. Out of scope here: the `canlin.ui` widget
set, the GUI tool schema, storage/SQLite, provider dialects, sync, MQTT, audio and serial.
Those get their own specs; they must use the conventions of §3 and the budgets of §7.

## 2. Naming

- C: every public symbol, type and macro starts with `canlin_` / `CANLIN_`, followed by
  the subsystem (`canlin_ev_`, `canlin_sse_`, `canlin_json_`, `canlin_sandbox_`). Static
  functions are free. Headers live in `include/canlin/<subsystem>.h`; numeric limits in
  `include/canlin/<subsystem>_limits.h`, the only place a number from §7 may appear.
- Lua: everything a module sees lives under the global table `canlin`. No other new
  globals. Sub-tables are named after the subsystem (`canlin.timer`, `canlin.sys`, …).

## 3. Conventions

### 3.1 Results and errors

A Lua API function either succeeds with its documented return values, or returns
`nil, code, message`. `code` is one of a fixed set of strings:

| code | meaning |
|---|---|
| `full` | a count/byte bound refuses admission now; retry later may succeed |
| `invalid` | argument outside the documented domain (size, range, enum) |
| `unsupported` | this host does not provide the capability at all |
| `denied` | the capability exists but permission is not granted |
| `unavailable` | the capability exists and is permitted but is not usable now |
| `stale` | handle unknown, foreign or from a dead generation |

`message` is diagnostic text for logs, never parsed. A wrong Lua *type* (a table where a
number is required, a missing required argument) is a programming error and raises a
Lua error instead. Faults (budget, memory) are not results: they end the scope (§5.3).

`unsupported`, `denied` and `unavailable` are never collapsed into each other and never
replaced by a simulated success (system design §5).

### 3.2 Handles

A handle identifies a timer, an operation or a stream owned by one module generation.
In Lua it is an integer (Lua 5.4 integers are 64-bit on every target, `LUA_32BITS` off).
Value: generation in the high 32 bits, a per-generation serial in the low 32 bits; 0 is
never valid; neither part is ever reused. A module can forge any integer, so every API
that takes a handle checks that it belongs to the calling generation and answers
`stale` otherwise. Knowing another module's handle grants nothing.

### 3.3 Admission and completion

A function that starts a finite asynchronous operation returns `handle` after reserving
its terminal slot and result budget, or `nil, code, message`. It never calls back into
Lua before returning. Its outcome arrives later as exactly one `event("done", …)` (§5.2)
on a later scheduler turn than the one in which the producer completed (decision D3).

## 4. Module shape

`lua#main` is loaded as a text chunk in a fresh sandbox and must return a table `M`.
The core calls these fields when present; absent fields are skipped:

| callback | when | arguments |
|---|---|---|
| `M.init(ctx)` | once, first callback of a generation | `ctx = {name=, version=, generation=}` (read-only copies) |
| `M.event(name, payload)` | queued events, completions, timers | see §5.2 |
| `M.sync(info)` | a new snapshot was installed | `info = {version=}` |
| `M.draw(ui)` | the module is dirty and the scheduler grants a draw | `ui` = the flow API object |

There is no `stop`/`unload` callback: stop is observable to nobody inside the stopped
generation, and correctness never depends on a final flush (event runtime §7, §8).

A callback that raises an ordinary Lua error is logged with the module's name and the
callback name; the scope stays alive and the next callback runs normally. After
`CANLIN_MODULE_MAX_ERRORS` consecutive erroring callbacks the scope is faulted like a
budget fault, so a module whose `draw` always fails cannot occupy its draw slot forever.
`init` raising is fatal for the generation.

## 5. Events, timers and cancellation

### 5.1 Lua surface

```lua
local h, code, msg = canlin.timer.after(ms)   -- one-shot, ms >= 0 integer
local r = canlin.cancel(h)                    -- "cancelled" | "suppressed" | "already" | nil,"stale"
canlin.invalidate()                           -- request a draw; coalesced
local t = canlin.sys.now()                    -- monotonic ms, integer
```

`canlin.timer.after` is bounded by `CANLIN_EV_MOD_MAX_TIMERS`; a module rearms after its
timer fires. `ms` above `CANLIN_TIMER_MAX_MS` is `invalid`. `canlin.cancel` works on any
handle kind: `cancelled` means the operation or timer will produce no delivery and
its producer was asked to stop; `suppressed` means the outcome already existed and its
queued delivery is dropped; `already` means nothing is left to prevent (repeat call).

### 5.2 Event names and payloads

| name | payload |
|---|---|
| `"timer"` | `{handle=h}` |
| `"done"` | `{handle=h, ok=true, result=…}` or `{handle=h, ok=false, code=, message=}` |
| subsystem streams | named and shaped by their own spec (e.g. `"llm"` in the LLM/UI slice) |

Payload values are fresh Lua values built by the core from owned C data; a module can
never obtain a pointer, a borrowed buffer or another module's value. Marshalling limits
are §7 `CANLIN_MARSHAL_*`; a result exceeding them is converted into `ok=false,
code="full"` at completion time, never truncated silently (this is today's
`CANLIN_EV_DETAIL_OVERSIZE`).

Input does not arrive as `event`. The scheduler routes normalised input to the owner
thread's UI state; the module observes it through return values of `ui` widgets in the
next `draw`. Control latches (stop, fault, input overflow, connection loss) are consumed
by the core, not delivered to Lua; a subsystem that needs to tell a module about a
lost connection does so with its own stream event.

### 5.3 Faults

A scope is faulted by: budget exhaustion, memory exhaustion (decision D6), repeated
callback errors (§4), or `init` failing. A faulted generation is stopped exactly like
`canlin_ev_module_stop` (mark inactive, cancel, retire) and its Lua state is closed. The
shell shows the module as faulted; restart is an explicit user/operator action and gets a
new generation. Other modules are unaffected.

## 6. C seam between scheduler and sandbox

The existing `canlin_sandbox_run` only executes a chunk. The slice adds one module
object that owns a sandbox and a scheduler generation, so no other code has to keep them
in step:

```c
typedef struct canlin_module canlin_module;

canlin_module *canlin_module_load(canlin_ev *ev, const canlin_module_source *src,
                                  const canlin_sandbox_config *cfg, int *err);
/* Called from the scheduler's deliver callback for this module's slot. */
canlin_sandbox_status canlin_module_deliver(canlin_module *m, const canlin_ev_delivery *d);
int  canlin_module_reload(canlin_module *m, const canlin_module_source *src);
void canlin_module_stop(canlin_module *m);
```

Inside the sandbox this needs `canlin_sandbox_call(sb, field, nargs, push_fn, ud)`: look up
`field` in the stored module table, let the host push at most `CANLIN_MARSHAL_MAX_ARGS`
arguments through `push_fn` under the same memory limit, and call it as one budgeted
callback. The budget is reset per callback, not per turn. `canlin_module_source` supplies
`lua#main` and the require resolver; where the source comes from (verified bundle or
operator-enabled local override) is decided by the loader, not by this seam.

`canlin_module_deliver` is the only path into Lua, and it runs only from
`canlin_ev_turn`, so the no-nesting rule of the event runtime holds by construction.

Recorded from the canlin#9 implementation (`include/canlin/module.h`):

- **Payloads** are a core-owned value tree `canlin_value` (nil, bool, integer, byte
  string, table with integer or string keys). An operation result is
  `canlin_module_result {value | code, message}`; a module event is
  `canlin_module_event {name, value}`. `CANLIN_MARSHAL_*` apply to the result value,
  not to the `{handle, ok, …}` wrapper; `canlin_value_check` lets a producer check
  before completing. A failure with an unknown or missing code arrives as `unavailable`.
- **Marshalling is charged** to the receiving callback: the budget is reset, the payload
  cost (`CANLIN_SB_COST_ELEM` per entry, bytes / `CANLIN_SB_COST_BYTES_PER_UNIT` per
  string) is charged, then values are built and the callback runs. Host functions use
  `canlin_sandbox_charge` for this; Lua cannot reach it.
- **Reserved event names:** `"timer"` and `"done"` only come from the core; posting a
  module event with either name is refused.
- **First draw:** a successful `init` marks the module dirty. `canlin.*` is usable at
  the top level of `lua#main`, because the generation exists before the chunk runs.
- **Control latches:** `FAULT` faults and `STOP` stops the module; `INPUT_OVERFLOW`
  and `CONN_LOST` are consumed without effect until the UI and transport specs use them.
- **Stop from inside the module's own callback** is deferred until that callback
  returns; reload from inside it fails with `CANLIN_MODULE_EBUSY`.
- **Reload failure:** if the new sandbox or generation cannot be created, the old
  generation keeps running. Once the old one is retired, a failing `lua#main` or `init`
  leaves the module faulted without a generation — there is no rollback in v1. A staged
  reload (start new, then retire old) is an open decision (canlin#16).

## 7. Numeric budgets

Initial values for the v1 slice, sized for the 1 GB P2 floor and to be re-measured on
real hardware (canlin#3). Each has a compile-time maximum; configuration can lower, never
raise, a value beyond its maximum.

| constant | v1 value | max | notes |
|---|---|---|---|
| `CANLIN_EV_MAX_MODULES` | 16 | 32 | live + retiring generations share a slot |
| `CANLIN_EV_MOD_MAX_ITEMS` | 64 | 256 | queued items + reserved terminal slots |
| `CANLIN_EV_MOD_MAX_BYTES` | 256 KiB | 1 MiB | queued payload + reserved result bytes |
| `CANLIN_EV_PROC_MAX_ITEMS` | 512 | 2048 | |
| `CANLIN_EV_PROC_MAX_BYTES` | 4 MiB | 16 MiB | |
| `CANLIN_EV_MOD_MAX_OPS` | 32 | 128 | timers included |
| `CANLIN_EV_MOD_MAX_TIMERS` | 16 | 64 | |
| `CANLIN_EV_PROC_MAX_OPS` | 256 | — | half the sum of module op limits |
| `CANLIN_EV_PROC_MAX_TIMERS` | 128 | — | |
| `CANLIN_EV_TURN_MAX_DISPATCH` | 16 | — | callbacks per turn |
| `CANLIN_EV_TURN_MAX_PER_MODULE` | 4 | — | queued items per module per turn |
| `CANLIN_TIMER_MAX_MS` | 24 h | — | longer waits rearm |
| `CANLIN_SB_MEM_DEFAULT` | 16 MiB | 64 MiB | Lua heap per module |
| `CANLIN_SB_BUDGET_DEFAULT` | 2 000 000 | 50 000 000 | instructions + charged units per callback (≈100–200 ms on a P2, to be measured) |
| `CANLIN_SB_BUDGET_DRAW` | 500 000 | — | `draw` gets a smaller budget: it runs every dirty frame |
| `CANLIN_MODULE_MAX_ERRORS` | 8 | — | consecutive erroring callbacks before fault |
| `CANLIN_MARSHAL_MAX_ARGS` | 4 | — | |
| `CANLIN_MARSHAL_MAX_DEPTH` | 8 | — | nested tables in a payload |
| `CANLIN_MARSHAL_MAX_ENTRIES` | 1024 | — | table entries across one payload |
| `CANLIN_MARSHAL_MAX_BYTES` | 64 KiB | 1 MiB | strings across one payload |

Parser limits (`canlin_sse_*`, `canlin_json_*`) are per call and chosen by the adapter that
owns the stream; the LLM/UI implementation spec fixes them per dialect. Process-wide
memory accounting beyond the Lua heap and the scheduler (transport buffers, SQLite, fonts)
is a follow-up (§9); until then this table is not the module's whole resource boundary.

## 8. Decisions on the open questions of canlin#4–#6

- **D1 — CANCELLED is never delivered to Lua.** The canceller learns the result from
  `canlin.cancel`'s return value; a stopped generation gets nothing. When the *core*
  aborts a live module's operation (capability revoked, transport closed), that is
  `ok=false` with a code, not "cancelled".
- **D2 — Retiring work counts against its slot** until its producer completes. A reloaded
  module can briefly see `full`; the bound is `CANLIN_EV_MOD_MAX_ITEMS`/`_BYTES`, so the
  shortfall is finite. Accepted as the price of §7 step 4 of the event runtime.
- **D3 — Next-turn delivery.** A completion or event queued during turn N is eligible from
  turn N+1. Stricter than "after the callback boundary", and it makes ordering testable.
- **D4 — Suspension clock.** The canonical adapter clock keeps counting across suspension;
  on resume each due timer fires once. An adapter whose host clock stops documents it
  and its test states the resulting delay. Wall time is never a timer source.
- **D5 — Text encoding.** SSE is byte-transparent; JSON validates UTF-8. Every string a
  provider adapter hands to Lua or the UI as *text* is valid UTF-8, validated by that
  adapter. JSON `\u0000` is accepted by the tokenizer; C code never uses `strlen` on
  decoded strings; the GUI tool validator rejects control characters in ids and enums.
- **D6 — Memory exhaustion faults the scope.** A Lua allocation failure can leave a
  module's own tables half-updated; continuing would hide that. Same handling as budget.
- **D7 — No coroutines in v1.** `coroutine` stays absent until tests show every
  hooks-off path (thread reset/close, `__close` in dead threads) is bounded.
- **D8 — Lua patterns are budgeted** through a step counter in a build-time patch of
  `lstrlib` (canlin#7); proportional C-library work is charged up front (canlin#8).

## 9. Follow-up specs

In the order the slice needs them:

1. **UI flow API and GUI tool schema** — widget set, ids, value types, layout rules of
   system design §5, the view-model limits and the `submit` action (LLM/UI §5).
2. **LLM slice** — first dialect, its parser limits, `"llm"` stream events, credential
   handle API, fixture endpoint.
3. **Store and db** — BeePack lookup over libbee (after canlin#2), SQLite authorizer and
   per-module quotas, persistence barrier and its completion codes.
4. **Process-wide memory accounting** across transport, SQLite and render buffers.
5. Sync, MQTT, audio and serial as in system design §13.
