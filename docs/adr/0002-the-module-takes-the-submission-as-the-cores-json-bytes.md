# ADR 0002 — The module takes the submission as the core's JSON bytes

- Status: accepted — decided by the owner on 2026-11-14, recorded here on 2026-11-15
- Date: 2026-11-15
- Tags: assistant, security, json, view, contract

## Context

The `assistant-view` capability gives a module with the role `assistant` the `canlin.view`
binding, and `canlin.view.submission()` hands the pending form submission back (module
roles design §3, LLM/UI design §5). The submission is the contract: a submit sends
`{revision, action = <button id>, values = {<id>: <value>}}` for the input widgets of that
view — field/area text, toggle boolean, choice index, graph `{nodes, edges}` — and nothing
else (UI spec §7).

The core builds those bytes itself. `finish_submit` writes the submission with the
core's own bounded JSON writer (`src/assistant/view.c:1710-1757`; the writer is the
`wbuf` machinery at `src/assistant/view.c:1192-1267`), and the graph's value is written
by `graph_value` (`src/assistant/view.c:1605-1667`). The bytes are the contract, and the
core has them before any module sees them.

The module, however, was handed a **Lua table** — and the module then had to turn that
table back into JSON to give the values to the provider. The table was built by the core
and read back by the core, in a second reading of values the core already held, and the
module's own hand-written writer did the second serialisation. In the built-in assistant
module that writer was `jstr`/`jval` (`src/app/assistant.c:50-88` before commit
`0ba2432`): `jval` guarded itself with `if depth > 4 then return 'null' end`
(`src/app/assistant.c:67`), and the module called it as
`jval(sub.values, 1)` (`src/app/assistant.c:224`).

### What the second reading cost

The depth of a value in that table is not small. In the submission `values` holds one
entry per input widget, the graph's value sits under its widget id, its `nodes` array
holds the node objects, and a node object holds its fields (`id`, `title`, `x`, `y`) and
its port lists. Counted from `jval(sub.values, 1)`, a node's fields, the node's port
lists and an edge's `from`/`to` all sit at **depth 5** — one past the writer's own
limit. They came out as `null`, and the ports with them:

```
{"item":"Milk","plan":{"edges":[{"from":null,"to":null}],
 "nodes":[{"id":null,"inputs":null,"outputs":null,"title":null,"x":null,"y":null}]},
 "shop":2,"urgent":false}
```

The core's bytes had all of it. The module's writer dropped it on the way out.

The table itself was not at fault: the core's table reader (`canlin_assistant_view_take_lua`,
`src/assistant/view.c:1788-1959` before commit `0ba2432`) read the submission's JSON with
the house's bounded JSON reader, at a depth of 15, and the old test read the graph's
nodes and edges out of that table without trouble
(`t/assistant/test_binding.c:307` before `0ba2432`). The table was complete; it was the
module's second serialisation of it that lost the values. That is what made the shape of
the old path the problem: the values crossed three boundaries — the core's bytes, the
core's table, the module's JSON — and only the middle one was checked.

The measurement that carried the decision: **19 of the 130 checks of the binding suite
failed against the old binding**. The suite is `t/assistant/test_binding.c`; the
failing checks are the ones that read the submission — its type and its bytes
(`t/assistant/test_binding.c:433-437`), the values read back out of it (`revision`,
`action`, `item`, `urgent`, `shop`, the node's `id`, `title` and `x`, the ports' `id`s
and the edges' `from`/`to`, `t/assistant/test_binding.c:441-452`), and the comparison
of the module's bytes with the submission the core keeps
(`t/assistant/test_binding.c:464`). Against the old binding the module received a
table, so it had no JSON to hand on and nothing to read. The suite passes all 130
checks with the bytes.

## Decision

**`canlin.view.submission()` hands the module the submission's own JSON bytes — a Lua
string — not a Lua table.**

- The core builds the submission's bytes and keeps them as they are; the module receives
  those bytes (`src/assistant/binding.c:55-77` → `canlin_assistant_view_take_json`,
  `src/assistant/view.c:1796-1813`).
- The module is not given a table to re-serialise. What it hands back to the provider
  are the values the JSON carries, because it hands on the JSON itself.
- There is no JSON writer in the module's way: the core's writer stays in the core, and
  no writer in Lua is left over (`src/assistant/view.c:1788-1795`).

This is option (a) of the options weighed on k115 and k149, decided by the owner on
2026-11-14.

## Rationale

The bytes are the contract, and the core already builds them. The alternative — a
bounded JSON writer in the core that the module uses — would mean a new `canlin.*` API
across the security boundary: the core is the boundary (module roles design §1), modules
arrive from a server, and every new `canlin.*` API is reviewed for what a hostile module
could do with it before it ships. A writer is not needed for that: the thing the module
has to send on is already made, already bounded, and already the contract.

The second reading is where the values were lost, and it was lost silently. The core
wrote a node's `id`, `title`, `x` and `y` into its bytes, handed the module a table, and
the module's writer turned those fields into `null` on the way to the provider — the
submission looked complete and was not. A writer in the core would have had the same
shape of problem in the other direction: the module would still have had to describe the
values, and a value it describes wrongly is still a value the provider never receives.
With the bytes, the module cannot describe the values wrongly; it can only pass on the
ones the core wrote, or not pass them on at all.

The table also made the module's job harder than the contract requires. The submission
is one JSON document; the table was a second, differently shaped representation of it
that the module then had to walk and rebuild. Every value in the table had to be
understood twice — once by the module's reader and once by its writer — and the second
of the two had a depth limit the first did not.

The measurement is what turned the argument into a finding: nineteen checks of a
hundred and thirty failed against the old binding, and every one of them was a value
that the core's own bytes carried the whole time.

## Consequences

- The submission the module receives is the submission the core built, byte for byte.
  The test compares them: the module's bytes and the bytes the C side takes are the
  same string (`t/assistant/test_binding.c:462-464`).
- The module no longer needs a JSON writer to give the values to the provider. The
  built-in assistant module sends the bytes on as they are (`src/app/assistant.c:181-186`).
- A module that wants to *read* the values has no house reader for them yet: there is
  no `canlin.*` JSON reader in the module's surface. That is a separate question, and it
  is left open here — this decision settles what the module is handed, not every API
  the module may later ask for.
- The bytes are charged to the sandbox before they are handed over, one unit per byte
  (`src/assistant/view.c:1804-1807`); the size is bounded by `CANLIN_AV_SUBMIT_MAX`
  (`include/canlin/assistant_limits.h:40`).
- The contract on the C side is named in one place: `canlin_assistant_view_take_json`
  (`include/canlin/assistant_view.h:262-271`) with its three outcomes, and the binding
  maps them onto the module's codes (`src/assistant/binding.c:63-76`).
- The submission waits for one consumer only. Taking it is taking it (`src/assistant/view.c:1808`).

### The spec sentences this decision obliges

- UI §7: a submit sends `{revision, action, values}` — the bytes the module receives are
  exactly that document, not a table the core read back out of it.
- Module roles §3: the `assistant-view` capability grants the model-view binding. What
  the binding hands over is not widened by this decision; it is narrowed to the contract.

## Alternatives considered

- (a) The core's own JSON bytes. Chosen.
- (b) A bounded JSON writer in the core that the module uses. Rejected: it is a new
  `canlin.*` API across the security boundary, and it does not remove the second
  reading — it only moves who does it. The bytes the core already built are the better
  answer, and they cost no new API.
- (c) Keep the table and fix the module's writer. Rejected: it treats the symptom. The
  table is the reason a second serialiser existed at all; with the writer's depth limit
  raised, the next limit would have found the next value. The contract is the bytes.
