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

- **em** = theme base font size (px at DPI 1.0) × platform DPI factor. All sizes below
  are in em. Interactive elements are at least 2.5 em high (system design §5); touch
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

## 3. Widget API (Lua)

`ui` is a per-draw object; using it after `draw` returned is a Lua error. Stateful
widgets are identified by `opts.id` if given, else by label plus ordinal within the
draw; two stateful widgets with the same identity in one draw are a Lua error.

| call | returns | notes |
|---|---|---|
| `ui:heading(text [, level])` | — | level 1–3 |
| `ui:text(text [, opts])` | — | `opts.style = "normal"\|"muted"\|"danger"\|"success"` |
| `ui:bubble(role, text)` | — | chat bubble; role `"user"\|"assistant"\|"system"` |
| `ui:field(label, value [, opts])` | new value | single line; `opts.placeholder`, `opts.max` bytes |
| `ui:area(label, value [, opts])` | new value | multi-line; `opts.rows` 2–20 |
| `ui:button(label [, opts])` | `true` once when activated | |
| `ui:toggle(label, on)` | new boolean | |
| `ui:choice(label, options, index)` | new index | options: array of strings |
| `ui:meter(label, value)` | — | value clamped to 0.0–1.0 |
| `ui:icon(name)` | — | `icon#<name>` of the module bundle only |
| `ui:separator()` | — | |
| `ui:columns(left_fn, right_fn)` | — | see §2; the functions run immediately, in order |
| `ui:section(title, fn [, opts])` | — | collapsible group; `opts.open` default |
| `ui:graph(graph [, opts])` | array of edits | §5 |

There is no secret/password field for modules: credential entry exists only in trusted
settings (LLM/UI design §3). Limits per draw are §8. Calls beyond a limit are refused
with a Lua error, so the bug is visible instead of a silently truncated view.

Input (pointer, touch, keys, text) reaches widgets through the core's UI state; the
scheduler never delivers raw input as `event` (core API §5.2). Focus moves with Tab /
Shift-Tab in draw order on keyboard hosts; the rawfb touch path shows the Nuklear
on-screen keyboard for a focused field (system design §4).

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

The core rejects locally, without producing an edit: connecting output→output or
input→input, a port to its own node, and an edge that already exists. If both ports
carry a `type` and the strings differ, the port shows as incompatible during the drag
and no `connect` edit is produced. Everything else — cycles, fan-in rules, node
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
| `theme#font#size` | integer px at DPI 1.0 | 10–40 |
| `theme#logo` | PNG blob | ≤ `CANLIN_THEME_LOGO_MAX` bytes and ≤ 1024×1024 decoded |
| `theme#spacing` | decimal string | 0.25–4.0 (em multiplier) |
| `theme#radius` | decimal string | 0.0–2.0 em |

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
function; widgets read only the resolved theme. Text/background contrast below 4.5:1
is logged as a warning, not refused.

### 6.3 Fonts

stb_truetype via Nuklear's font baking. Bodies and headings are baked once per theme
load at base size × DPI; heading levels use scale factors of the heading font. The
built-in theme carries one small Latin font subset; full Unicode coverage needs a theme
font. Missing glyphs render as a replacement box, never crash.

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
  and every button. Control characters are rejected in every string (core API D5).
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
