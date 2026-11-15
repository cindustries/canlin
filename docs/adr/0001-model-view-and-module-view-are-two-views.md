# ADR 0001 — The model's view and the module's view are two views

- Status: accepted — decided by the owner on 2026-11-14, recorded here on 2026-11-15
- Date: 2026-11-15
- Tags: ui, assistant, security, view, identity

## Context

The `assistant-view` capability gives a module with the role `assistant` the `canlin.view`
binding: `canlin.view.draw()` draws the model-produced widgets, `canlin.view.submission()`
hands the pending form submission back (module-roles design §3, LLM/UI design §5). The
binding was written against the module's own view: `canlin.view.draw()` pushed the module's
view and drew the model's widgets into it (`src/assistant/binding.c:36-55`).

One `canlin_ui_view` is one state container. It holds the slot table of the widgets'
identities, the keyboard focus, the focus order, the graph states and the frame counter
(`src/ui/ui.c:67-141`). The identity of a stateful widget is `opts.id`, or, lacking that,
the label plus its ordinal among equal labels in the draw (`src/ui/ui.c:1672-1697`). The
model's widgets carry the revision-scoped identity `r<rev>:<id>`
(`src/assistant/view.c:1831-1840`); the revision is public — `canlin.view.draw()` returns it
(`src/assistant/binding.c:51`) — and the widget ids are public through the submission
payload, which is keyed by them (`src/assistant/view.c:1670-1700`). A module that draws with
an explicit `opts.id` can therefore name the identity of a model widget.

The slot table carries an identity across a frame boundary. At the end of a frame
`slots_evict` keeps only the slots that were used in that frame
(`src/ui/ui.c:1197-1208`, called at the frame end `src/ui/ui.c:1322`), so a slot written in
frame N is still there in frame N+1, where `identity()` finds it and does not raise the
duplicate check.

### The measurement of 2026-11-14

A measurement in the real sandbox with the real assistant-view binding (canlin-test-writer,
by execution; recorded on k112) found that a module could prime all four kinds of the model
view's identity-keyed state silently across a frame boundary:

| state | where it lives | file |
|---|---|---|
| section open (latched by `slot.init`) | the slot of the section widget | `src/ui/ui.c:2329-2337` |
| choice expanded | `slot.expanded` of the choice widget | `src/ui/ui.c:2101`, `:2116` |
| keyboard focus | the per-view `v->focus`, restored for the model's field by `edit_focus_sync` | `src/ui/ui.c:1873`, `:1945`, frame end `:1329-1341` |
| graph pan | the per-view graph state table, keyed by the graph identity | `src/ui/ui.c:2242-2246` → `src/ui/graph.c:480-509`, pan `:1189-1200`, `:1217-1218` |

The field *text* was not inherited: the text of a field is not in the slot table. It comes
from the caller's own value at draw time (`src/ui/ui.c:1943`) and, in the model view, from
the assistant's own per-widget state (`src/assistant/view.c:1374-1378`, drawn at `:2158`).
What a module can
reach is the identity-keyed state, and the revision and the widget ids are public, so the
module can aim.

In the same frame the collision is loud: `identity()` raises "ui: two stateful widgets with
identity '%s' in one draw" (`src/ui/ui.c:1693`) and the whole draw is aborted. Across a
frame boundary it is silent. The priming survives exactly one frame boundary and stays only
as long as the model's widget keeps the identity alive.

## Decision

**The model view gets its own `canlin_ui_view`. The module's view and the model's view are
two views: two state containers, two identity spaces, two rectangles.**

- The assistant view is attached to a view of its own; it is not the module's view.
- `canlin.view.draw()` draws into the model's view.
- The shell holds the model's view as a second view of the module's pane and frames it;
  the module's own widgets live outside that frame, in their own view.
- The model's view has its own slot table, identity space, focus, focus order, graph state,
  scroll position and rectangle. Nothing a module draws can reach into it.

This is option (c) of the options weighed on k112 and k114, decided by the owner on
2026-11-14.

## Rationale

The model view is the area in which the model speaks. A module that can write into that
area can change what the model's statement looks like: the frame that labels the area as
model-generated would then also label the module's words, and the module's words could stand
in for the model's. With one shared view the boundary between "what the model said" and
"what the module claims" is not provable — and LLM/UI design §5 asks for exactly that
boundary to be recognisable: the model-produced area and the submission boundary must stay
visible, and module text must not be presentable as a request from a trusted surface. The
separation is what makes the frame's label true.

The state that carries this risk is keyed by identity, and the identity space is the
view's. The module's widgets and the model's widgets shared one slot table, one focus and
one graph state, so a module could reach into the state of a view it does not own; the
measurement above shows how far that reach goes. The other half of the same root cause
(k114) is the frame: the shell knows one bit per module — "this pane is a model pane" — and
framed the whole pane, so the module's own question field sat inside the frame labelled
model-generated. A second view with its own rectangle settles both halves at once.

A separate view is also the cheapest of the options that work. The alternatives that
separate only the identities are inert or partial here: both populations share the one view
and would share a per-view nonce, and an owner-scoped namespace separates the identity but
not the appearance, so the frame half stays open. A region concept inside one view would
have to invent a frame that encloses a band of a scroll region — a rectangle the shell does
not own.

The separation needs no new guard in the ui layer. `canlin_ui_view` is already the unit of
state; the view boundary is the boundary, and the ui layer never has to ask whose widget
this is.

## Consequences

- The model's identity space is its own. A module's `opts.id` can no longer name a model
  widget's identity, and the four primed state kinds are unreachable across the boundary.
- The shell frames the model's rectangle, and the module's own widgets — its question field
  among them — sit outside the frame. UI §4 ("a view built from the GUI tool sits in a
  frame labelled as model-generated") becomes true as written.
- The model's view is driven by its own frame, `canlin_assistant_view_frame`
  (`src/assistant/view.c:2319-2351`), on a private Lua state: the model can be drawn
  without the module's `draw()` running.
- A model pane holds two views. The shell routes the pointer to the view under the point.
  Two follow-on questions are open, and they are consequences of this decision rather than
  of any implementation: the keys and the text reach the focused module, and the shell
  cannot know which of the two views holds the keyboard focus (k131); the events the
  model's view receives carry the backend's `t = 0`, so a long press and a double tap
  inside the model view measure nothing (k132).
- The two views scroll independently, so a model pane is split into two rectangles.
- The separation is held by whoever attaches the model's view, not by the ui layer. The
  shell still keeps the legacy arrangement — a model pane without a model view of its own
  shows the module's view inside the frame (`src/shell/shell.c:397-425`) — so a path that
  forgets to attach the model's view silently gets the old shared view back.

### The spec sentences this decision obliges

- UI §2: a companion to "View = the module's content area, one vertical scroll region
  owned by the core" — the model's view is a view of its own.
- UI §4: the model view sits in the frame; the module's own widgets do not.
- Shell §1: a companion to "One view per module instance".
- Shell §3: what the model-view frame frames, and where the submit boundary is drawn.

## Alternatives considered

- (a) A per-view nonce in the identity. Inert for this problem: both populations share the
  one view and would share the nonce. It changes the identity scheme, so it needs a
  decision, not a patch.
- (b) An owner-scoped namespace: the ui layer learns an owner and mixes it into the
  identity. It separates the identity but not the appearance, so the frame half (k114)
  stays open. Six `identity()` call sites, a public header change.
- (c) A separate view for the model view. Chosen.
- (d) A region concept in the ui layer: a frame around a sub-band of a scroll region. The
  largest new concept in the question, and it buys the least — one view is one vertical
  scroll region, so a region is not a rectangle the shell owns.
- (e) Draw the model's input as trusted chrome. Inverts the module-roles model and
  contradicts the assistant-view capability and LLM/UI design §5's requirement that the
  submission boundary remain visible.
