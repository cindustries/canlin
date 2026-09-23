# CanLin — UI flow API, graph editor, theme and GUI tool schema

Status: draft for review. Fixes the `canlin.ui` surface that `draw(ui)` receives, the
node-graph editor widget, the theme format and loading, and the assistant's GUI tool
schema. It is follow-up §9.1 of the [core API design](2026-09-23-canlin-core-api-design.md)
and implements system design §5 and §10 and LLM/UI design §5. Conventions (results,
handles, `canlin_` naming, limits headers) are those of the core API design.

## 1. Principles

- **Modules describe, the core lays out.** A module emits widgets in order; the core
  decides geometry from the available width, the theme and the DPI factor. No module
  API takes pixels. The one widget with free positioning, the graph editor (§5), takes
  positions in its own graph space in em, and the core maps them to the screen.
- **One draw, one frame.** `draw(ui)` builds the whole view each time it runs
  (immediate mode). Interaction results appear as return values in the *next* draw;
  a widget never calls back into Lua.
- **Nuklear is the engine, not the API.** Widgets are implemented with Nuklear inside
  `src/ui`; Nuklear types, handles and pixel primitives never reach Lua. Nuklear demos
  (the node editor, the rawfb rasterizer) are references, adapted as owned code in
  `src/ui` and `src/render` — never edited inside `third_party/`.
- **Theme first.** Every colour, font, size, spacing and radius comes from the theme
  (§6). Widgets have no style arguments; a module cannot draw outside the theme.

## 2. Units and layout

- **em** = theme base font size (px at DPI 1.0) × the output's DPI factor × the
  output's UI scale (§2.1). All sizes below are in em. Interactive elements are at least 2.5 em high (system design §5); touch
  targets of graph ports are at least 2.5 em square even when the drawn dot is smaller.
- **View** = the module's content area, one vertical scroll region owned by the core.
  The module cannot scroll it programmatically in v1.
- **Columns:** below 40 em of content width everything is one column. `ui:columns()`
  (§3) splits into two equal columns at ≥ 40 em and stacks them below. There is no
  automatic multi-column placement of ordinary widgets.
- **Text:** `text`, `bubble` and headings wrap at word boundaries, breaking inside a
  word only when it is wider than the column. Single-line labels (buttons, choices,
  field labels, node titles) end with an ellipsis when too long. Field contents scroll
  horizontally. All text is UTF-8; invalid UTF-8 from a module is a Lua error.
- **Rounding:** layout computes in float em, rounds each widget rectangle to whole
  pixels once, and never lets rounding shrink an interactive element below 2.5 em.
- **Spacing unit** = `theme#spacing` × 0.25 em. View padding and the gap between
  widgets are 2 units; the gap between columns is 4 units.

### 2.1 Outputs and UI scale

An **output** is one display surface: pixel size, DPI factor, UI scale and a stable
identity supplied by the platform (SDL display, framebuffer device, browser window).
Every view is laid out for exactly one output; nothing in `src/ui` or `src/render`
keeps display metrics as process-wide or theme-wide state.

- **UI scale** is a device setting per output identity in `system/local/`, changed only
  in trusted settings, never by a module or model view. Steps: 75, 100, 125, 150, 200 %;
  default 100 %. It multiplies em, so the whole view — nested sections, columns, graph
  nodes and touch targets — grows or shrinks together. Because columns switch at 40 em,
  a scaled-up view on a small output falls back to one column instead of clipping.
- **Changing scale or DPI** (setting changed, SDL window moved to a display with other
  DPI) re-lays out and invalidates every view on that output. Fonts are baked once per
  distinct em pixel size (one atlas per size, holding body and both heading sizes);
  at most `CANLIN_UI_FONT_SIZES_MAX` sizes are held, least recently used unpinned ones
  are released. A view pins its size while its last command list may still be
  rendered. If every slot is pinned and a view needs a new size, that draw is skipped
  with a log line (`EFONT`), the previous frame stays, Lua is not called and no module
  error is counted. No continuous scaling, and no per-module or per-section scale:
  modules must look consistent and cannot shrink touch targets below 2.5 em.
- **v1 drives one output.** The API is shaped for several; running them is follow-up
  canlin#25:
  - The shell assigns module views to outputs (for example a dashboard or flow view on
    a second monitor, a kiosk wall). A module does not know which output it is on; it
    just gets `draw(ui)` with that view's metrics. One view is shown on one output at a
    time; hardware/OS mirroring counts as a single output.
  - Pointer and touch coordinates are per output; keyboard focus belongs to one view
    across all outputs, and the shell shows which one.
  - Backends: SDL2 opens one window per output. rawfb uses `/dev/fb0` in v1; several
    framebuffers or KMS/DRM multi-head are a canlin-os kernel/driver question first.
    The browser target stays one canvas; mobile stays one display.

## 3. Widget API (Lua)

`ui` is a per-draw object (a table with a protected metatable, methods called as
`ui:x()`); using it after `draw` returned is a Lua error. Stateful widgets are identified
by `opts.id` if given, else by label plus its ordinal among widgets with the same label
in this draw. Explicit ids and automatic identities are separate namespaces; two widgets
with the same explicit id in one draw are a Lua error. Field focus follows identity, not
draw position. A module without a view still gets `draw(ui)`, but every method raises.

| call | returns | notes |
|---|---|---|
| `ui:heading(text [, level])` | — | level 1–3 |
| `ui:text(text [, opts])` | — | `opts.style = "normal"\|"muted"\|"danger"\|"success"` |
| `ui:bubble(role, text)` | — | chat bubble; role `"user"\|"assistant"\|"system"` |
| `ui:field(label, value [, opts])` | new value | single line; `opts.placeholder`, `opts.max` bytes |
| `ui:area(label, value [, opts])` | new value | multi-line; `opts.rows` 2–20 |
| `ui:button(label [, opts])` | `true` once when activated | |
| `ui:toggle(label, on)` | new boolean | 1.25 em framed box, label after one widget gap; whole row is the hit target |
| `ui:choice(label, options, index)` | new index | options: array of strings; expands inline with touch-height rows |
| `ui:meter(label, value)` | — | value clamped to 0.0–1.0 |
| `ui:icon(name)` | — | `icon#<name>` of the module bundle only; raises `unsupported` until the PNG decoder (canlin#23) |
| `ui:separator()` | — | |
| `ui:columns(left_fn, right_fn)` | — | see §2; the functions run immediately, in order |
| `ui:section(title, fn [, opts])` | — | collapsible group, open unless `opts.open == false`; heading-style row with disclosure arrow left (▶/▼), no box, muted bottom rule; content indented 1 em |
| `ui:graph(graph [, opts])` | array of edits | §5 |

There is no secret/password field for modules: credential entry exists only in trusted
settings (LLM/UI design §3). Limits per draw are §8. Calls beyond a limit are refused
with a Lua error, so the bug is visible instead of a silently truncated view.

Input (pointer, touch, keys, text) reaches widgets through the core's UI state; the
scheduler never delivers raw input as `event` (core API §5.2). Pointer input carries a
button (primary, secondary, middle); ordinary widgets act on primary only.

**Keyboard focus (canlin#26):** field, area, button, toggle, choice, section header and
graph take focus in the draw order of the previous finished frame; Tab / Shift-Tab move
and wrap. The focused widget shows an accent mark when focus came from the keyboard.
Enter or Space activates a focused button, toggle or section header and opens/closes a
choice; Up/Down step a focused choice. A focused graph keeps Tab for node cycling and Del
for delete; Esc hands Tab back to the view order. A pointer press on button, toggle,
choice or section header clears keyboard focus; fields and graphs take focus on press.
An output has an input class (pointer, touch, keyboard; set by the shell through
`canlin_ui_output_set_input`). On a touch output without a keyboard, a focused field or
area shows the on-screen keyboard (canlin#45): German QWERTZ with umlauts and ß, a
symbol layer with digits, € and @, one-shot shift / double-tap caps lock, backspace,
space, Enter (newline in an area, "next" = Tab in a field) and hide (keeps focus and
value). Keys are 2.5 em square, shrinking evenly to 1.5 em in narrow views; below one
free 2.5 em content row the panel is not shown. Keys act on release inside the pressed
key; panel input never reaches Lua. Moving keyboard focus (Tab, "next", panel shown or
typing) scrolls the focused widget into view by the least amount. Tab is navigation and
never inserts a tab character.

## 4. Model area and trusted chrome

The shell draws the status bar, module switcher, settings entry and permission/error
chrome itself, outside every module view. A view built from the GUI tool (§7) sits in a
frame labelled as model-generated, with its submit boundary visible. Module and model
views cannot draw over, restyle or imitate that chrome: the theme applies to them, but
their widgets have no access to chrome roles or positions.

## 5. Graph editor

A node-graph editor for visual flow building in the style of n8n or Node-RED: nodes
with typed input and output ports, connected by edges, on a pannable canvas.

### 5.1 Data model (module-owned)

```lua
local g = {
  nodes = {
    { id = "src", title = "HTTP", x = 2, y = 3,           -- graph space, em
      inputs  = {},
      outputs = { { id = "out", label = "body", type = "bytes" } } },
    { id = "parse", title = "JSON", x = 16, y = 3,
      inputs  = { { id = "in", label = "text", type = "bytes" } },
      outputs = { { id = "obj", label = "value", type = "json" } },
      body = { { kind = "text", text = "strict" } } },   -- optional, see 5.3
  },
  edges = { { from = { "src", "out" }, to = { "parse", "in" } } },
  selected = "parse",                                     -- optional
}
local edits = ui:graph(g, { id = "flow", height = 24 })
```

The graph is a **controlled** widget: the module owns the data and passes it in on every
draw; the core never mutates it. User actions come back as edits, and the module decides
whether to apply them — the same way `field` returns a new value.

### 5.2 Edits returned

| edit | fields | produced by |
|---|---|---|
| `move` | `node, x, y` | drag a node (x/y in em, rounded to 0.25) |
| `connect` | `from = {node, port}, to = {node, port}` | drag output port → input port |
| `disconnect` | `edge` (index into `g.edges`) | drag an edge off its input, or delete a selected edge |
| `select` | `node` or `edge`, or neither to clear | tap/click |
| `delete` | `node` | delete key / context action on a selected node |
| `add` | `x, y` | context action on empty canvas: the module shows its own node picker |
| `open` | `node` | double-click / long-press: the module shows its own node detail view |

Edits are tables `{kind = "<edit>", …}`; `connect.from`/`to` use the edge shape
`{node, port}`; `edge` is a 1-based index into `g.edges`. `g.selected` is a node id, an
edge index or nil. A `move` is produced once on release (the core draws the drag preview
without touching `g`); a move that rounds back to the start produces nothing. `select`
is produced only when the selection changes. Long press (≥ `CANLIN_UI_LONG_PRESS_MS`,
movement ≤ 0.5 em) and double tap (within `CANLIN_UI_DOUBLE_TAP_MS`) are measured on the
scheduler's monotonic clock stamped into each input event, and recognised at release.
A secondary press on empty canvas produces `add` at that point (on the press edge);
it does nothing on nodes, ports or edges. While a press is held within the slop, the
core arms one core-owned timer (never delivered to Lua, counted against the module's
timer limit) so an accent mark appears at the long-press deadline; recognition itself
stays at release. `opts.fit` is edge-triggered: it re-centres when it becomes true, so a later pan
survives. A press inside the graph gives it keyboard focus, so Del in a field never
deletes a node. Dragging from an input that has an edge lifts that edge: drop on another
input → disconnect + connect, on the same input → nothing, elsewhere → disconnect.
Hit priority: ports, nodes (topmost first), edges (within 0.5 em), empty canvas.

The core rejects locally, without producing an edit: connecting output→output or
input→input, a port to its own node, and an edge that already exists. If both ports
carry a `type` and the strings differ, the port shows as incompatible during the drag
and no `connect` edit is produced. During a drag every port the drop would be rejected
on is shown incompatible: type clashes, the dragged node's own ports and ports that
already carry the identical edge. Everything else — cycles, fan-in rules, node
semantics — is the module's decision.

### 5.3 Rendering and interaction

- Nodes are drawn as themed panels: title row, one row per port (inputs left, outputs
  right), then an optional `body` of up to 4 read-only rows (`text` or `meter`), so a
  node can show status without the module drawing inside it. Editing node settings
  happens in the module's own view after an `open` edit, with ordinary widgets.
- Edges are cubic Bézier curves from output to input port, subdivided into a bounded
  number of segments (`CANLIN_UI_GRAPH_CURVE_SEGMENTS`). Off-screen nodes and edges are
  culled before building Nuklear commands.
- Canvas: drag on empty space pans; the pan offset is core UI state kept per graph `id`.
  `opts.fit = true` on a draw re-centres once. An optional background grid uses the
  theme's `muted` role.
- **No zoom in v1.** Nuklear has no transform stack; zoom means rendering node text at
  other font sizes. It becomes a follow-up with a small set of pre-baked font sizes,
  not continuous scaling.
- Narrow views (< 40 em) keep the graph usable by panning; the port touch targets of §2
  still apply. Keyboard hosts can cycle selection with Tab and delete with Del.
- Reference implementation: Nuklear's `demo/common/node_editor.c` (layout space, groups,
  `nk_stroke_curve`). It is a demo, not library API; `src/ui/graph.c` adapts its approach
  under the constraints above.

### 5.4 Scope

The widget edits and shows graphs; it does not run them. Executing a flow — scheduling
nodes, passing data along edges, retries — is module logic in Lua, using the ordinary
`canlin.*` APIs and budgets. A node type never grants a capability the module does not
already have. A shared flow-execution module is a separate design once a first real
flow module exists.

## 6. Theme

### 6.1 Keys

The keys of system design §10, with value formats fixed:

| key | value | valid range |
|---|---|---|
| `theme#color#<role>` | `"#RRGGBB"` string | roles: background, surface, text, muted, accent, danger, success |
| `theme#font#body`, `theme#font#heading` | TTF blob | ≤ `CANLIN_THEME_FONT_MAX` bytes |
| `theme#font#size` | decimal string, px at DPI 1.0 | 10–40 |
| `theme#logo` | PNG blob | ≤ `CANLIN_THEME_LOGO_MAX` bytes and ≤ 1024×1024 decoded |
| `theme#spacing` | decimal string | 0.25–4.0 (em multiplier) |
| `theme#radius` | decimal string | 0.0–2.0 em |

All scalar values are ASCII text in the pack (like `spacing` and `radius`), so the
key/value source hands bytes to the theme without type conversion.

Graph colours are derived, not extra keys: node panel = `surface`, edges and grid =
`muted`, selection and dragging = `accent`, incompatible port = `danger`.

### 6.2 Loading

At startup and on explicit reload the core resolves each key through a key/value source
interface: `theme/local/` override, then `theme/theme.bee`, then the built-in theme
compiled into the binary. Until libbee is consumed (canlin#2), the source is an
in-memory table in tests and the built-in theme on devices. Each key is validated on its
own: an invalid or oversized value falls back to the next layer for that key only and
logs one line naming the key. Sizes are checked before decoding or allocation. A theme
never fails startup.

The resolved theme maps onto Nuklear's style table (`struct nk_style`) in one owned
function; widgets read only the resolved theme. Contrast of `text` against both
`background` and `surface` below 4.5:1 is logged as a warning, not refused.

Resolved font and logo blobs point into the source's memory (libbee mmap/CDB), so a
key/value source must outlive the theme resolved from it. Strokes are drawn inside
their rectangle, `[x, x+w)`, so a bordered widget never paints outside its layout box. The view background covers its whole rectangle (square corners).

### 6.3 Fonts

stb_truetype via Nuklear's font baking. Bodies and headings are baked once per theme
load at base size × DPI; heading levels use scale factors of the heading font. The
built-in theme carries a DejaVu Sans 2.37 subset (345 codepoints: ASCII, Latin-1, Latin
Extended-A, common punctuation incl. …, €, arrows and ▲▶▼◀), embedded as a C array
generated by `tools/mkfont.py` from the vendored original and baked 1:1 per size.
Missing glyphs render as a hollow box (U+25A1) and never crash. Full Unicode coverage
needs a theme font. Nuklear's default font (ProggyClean) is not linked. The DejaVu
licence text must ship with every build (canlin#43).

**Untrusted fonts:** stb_truetype states it is not safe on untrusted font files, and a
`theme.bee` can come from a server. Theme TTF blobs are size- and magic-checked but not
baked until an untrusted-font policy exists (isolation, fuzzing or a hardened
rasterizer, canlin#21). Until then the built-in font is used.

## 7. GUI tool schema (assistant-owned view)

The provider-visible tool `set_view` replaces the assistant view (LLM/UI design §5).
Its argument is JSON mapped onto the same widget set:

```json
{ "title": "Einkaufsliste",
  "widgets": [
    { "type": "heading", "text": "Was fehlt?" },
    { "type": "field", "id": "item", "label": "Artikel" },
    { "type": "choice", "id": "shop", "label": "Laden", "options": ["Markt", "Drogerie"] },
    { "type": "graph", "id": "plan", "nodes": [], "edges": [] },
    { "type": "button", "id": "ok", "label": "Hinzufügen", "action": "submit" } ] }
```

- Allowed `type`: heading, text, bubble, field, area, toggle, choice, meter, separator,
  section (with nested `widgets`), columns (`left`/`right` arrays), graph, button.
  `icon` references built-in assistant icons only.
- `id`: `[a-z0-9_-]{1,32}`, unique within the view; required for every input widget
  and every button. Control characters are rejected in ids, labels, titles, options and
  enum values (core API D5); multi-line content (`text`, `bubble`, `area` values) may
  contain `\n` and `\t` but no other control character.
- Bubble roles in model views are `user` and `assistant` only; `system` is reserved for
  trusted content and rejected in `set_view`.
- Field names (canlin#20): `value` is the initial value of field, area and toggle and
  the value of a meter; `choice` uses a 1-based `index` (as `ui:choice`), also in the
  submission; edges are `{from: [node, port], to: [node, port]}`; unknown keys, duplicate
  JSON keys and non-integer numbers where integers are expected are rejected. A view has
  at most 8 graphs. Recorded results per turn: at most 16; a repeated call id returns the
  recorded result, the same id with different arguments is `invalid`.
- The only `action` in v1 is `submit`. A submit sends
  `{revision, action = <button id>, values = {<id>: <value>}}` for the input widgets of
  that view — field/area text, toggle boolean, choice index, graph `{nodes, edges}` —
  and nothing else. Typing, dragging and graph edits are local until submit.
- In a model view the graph applies its own edits (the assistant owns the data), so a
  user can build or rearrange a flow and submit it; the model gets the result only then.
- Limits: §8. Validation is complete before the view changes; on any error the last
  valid view stays and the tool result is a structured error naming the first failing
  JSON path. Success returns `{revision}`.

## 8. Limits

Initial values, in `include/canlin/ui_limits.h`, to be checked on P2 hardware:

| constant | value | applies to |
|---|---|---|
| `CANLIN_UI_MAX_WIDGETS` | 512 | widgets per draw |
| `CANLIN_UI_MAX_DEPTH` | 4 | nesting of section/columns |
| `CANLIN_UI_MAX_TEXT` | 16 KiB | one text/bubble/area string |
| `CANLIN_UI_MAX_LABEL` | 256 B | labels, titles, options |
| `CANLIN_UI_MAX_OPTIONS` | 64 | choice options |
| `CANLIN_UI_GRAPH_MAX_NODES` | 256 | |
| `CANLIN_UI_GRAPH_MAX_PORTS` | 16 | per node, inputs + outputs |
| `CANLIN_UI_GRAPH_MAX_EDGES` | 1024 | |
| `CANLIN_UI_GRAPH_CURVE_SEGMENTS` | 16 | per edge |
| `CANLIN_UI_TOOL_MAX_BYTES` | 64 KiB | `set_view` JSON argument |
| `CANLIN_UI_TOOL_MAX_WIDGETS` | 128 | widgets in one model view (graph nodes counted separately) |
| `CANLIN_THEME_FONT_MAX` | 2 MiB | per font blob |
| `CANLIN_THEME_LOGO_MAX` | 256 KiB | |
| `CANLIN_UI_FONT_SIZES_MAX` | 4 | baked font pixel sizes held at once |
| `CANLIN_UI_DPI_MIN` / `_MAX` | 0.5 / 4.0 | output DPI factor; outside is rejected |
| `CANLIN_UI_OUTPUT_ID_MAX` | 63 B | stable output identity |

## 9. Rendering path

Nuklear draws into a command list; `src/render` rasterizes it into an RGB buffer in
RAM (system design §4), adapted from Nuklear's `demo/rawfb/nuklear_rawfb.h`, integer
and float code only, no SIMD. Backends copy dirty rows: rawfb to `/dev/fb0`, SDL2 as a
texture, offscreen into a test buffer. Redraw only on invalidation or input.

## 10. Tests

- Offscreen with a fixed font and theme: widget geometry at 39.9 and 40 em, wrapping,
  ellipsis, 2.5 em minimum height, rounding at DPI 1.0/1.5/2.0.
- Controlled widgets: field/choice/toggle return values across draws, identity
  collisions raise, `ui` used after `draw` raises, limits raise.
- Graph: every edit kind from injected input; rejected connections produce no edit;
  type mismatch; pan state per id; culling of off-screen nodes; port hit area ≥ 2.5 em;
  256 nodes / 1024 edges within the draw budget.
- Theme: every key valid/invalid/oversized per layer, fallback per key, contrast warning,
  missing glyph.
- GUI tool: valid view, every invalid case of §7 leaves the old view, duplicate ids,
  stale-revision submit, submit carries only declared inputs, graph round trip.
- Pixel checks only for the rasterizer (lines, curves, rounded rectangles, text).
- Host plus P2-QEMU for everything portable; SDL2 and browser backends separately.

## 11. Implementation order

1. Vendor Nuklear (pinned commit) and stb_truetype; offscreen rasterizer; theme
   resolution with the built-in theme and in-memory source.
2. Flow widgets and layout, wired to `canlin_module` so `draw(ui)` gets the real object.
3. Graph editor.
4. GUI tool validator and assistant view (with the LLM slice).
