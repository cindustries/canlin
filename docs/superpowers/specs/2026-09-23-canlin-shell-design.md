# CanLin — shell: arrangement, chrome and focus

Status: draft for review. The shell is the trusted part of the screen: it arranges module
views on each output, draws the chrome (status bar, switcher, dividers, model-view
frames) and owns focus. It builds on the [UI design](2026-09-23-canlin-ui-design.md)
(§2.1 outputs and scale, §4 trusted chrome) and the
[core API design](2026-09-23-canlin-core-api-design.md). Modules never see the shell:
a module gets `draw(ui)` for its view and nothing about where that view sits.

## 1. Decisions

- **Arrangement follows the output, not the platform.** The shell picks a mode from the
  output's width in em (after DPI and UI scale) and its input class (touch, pointer,
  keyboard-only). It never branches on "Android" or "desktop".
- **Two modes in v1: full-screen and tiled.** Every module must work full-screen; tiling
  only arranges several such views side by side. **Free floating windows are not in
  v1** (decision 2026-09-23): no overlap, no stacking order, no partial-occlusion
  redraw. They are a later, opt-in mode for pointer outputs with enough space
  (canlin#34); nothing in v1 may assume they will never exist, e.g. views already get
  arbitrary rectangles.
- **One view per module instance.** A module is shown in at most one place at a time.
  Showing it elsewhere moves the view (`canlin_ui_view_bind`), it does not clone it.

## 2. Modes

| mode | available | default when | layout |
|---|---|---|---|
| full-screen | always | output < 80 em wide | one view fills the content area; the switcher changes which |
| tiled | output ≥ 60 em wide | output ≥ 80 em wide | 2 or 3 panes in one row or one column, divided by draggable dividers |

The user can choose full-screen on any output and tiled where available; the choice is
a device setting per output identity in `system/local/`, like UI scale (canlin#31).

**Tiling rules.** Panes are at least `CANLIN_SHELL_PANE_MIN` (30 em) in the split
direction; a module in a pane narrower than 40 em simply gets the one-column layout of
UI spec §2. Dividers have a 2.5 em hit area and work with touch and pointer. When an
output shrinks below what its tiles need (scale raised, window resized, rotated), the
shell falls back to full-screen showing the focused view and remembers the tiling, so
it comes back when space returns. No pane is ever squeezed below its minimum.

## 3. Chrome

The shell draws, in C with the same theme and widgets internals, outside every view:

- **Status bar** (one per output, top): time, network state, storage state (volatile
  mode is always visible), an active-microphone indicator whenever capture runs, and
  a notice for faulted modules. Modules cannot draw into it or hide it.
- **Switcher**: the list of running modules with their state (running, faulted,
  offline/volatile). Opened from the status bar, by a swipe from the bar on touch
  outputs, and by a key on keyboard outputs. In tiled mode it assigns a module to the
  focused pane.
- **Dividers** between tiles, and the **focus mark** (accent border) on the focused view.
- **Model-view frame** around assistant views built by the GUI tool, labelled as
  model-generated with the submit boundary visible (UI spec §4, LLM/UI design §5).
- **Faulted view**: a faulted module's pane shows the fault and a restart action;
  restart is a user action and creates a new generation (core API §5.3).

Trusted settings (provider profiles, credentials, UI scale, arrangement) are a built-in
screen reached from the status bar. How it is implemented — C, or a built-in Lua module
with a narrow privileged API that the loader grants only to modules compiled into the
binary — is decided in the settings spec; either way no downloaded module gets that API.

## 4. Focus and input routing

- Exactly one view is focused across all outputs; it gets keyboard and text input.
- Pointer and touch go to the view under the point (per output, UI spec §2.1). A press
  in an unfocused view focuses it and is delivered to it in the same frame, so one tap
  on a button in another pane works.
- Chrome gets input before views where they touch (a divider drag never reaches a view).
- Keys reserved for the shell (switcher, focus next pane) are fixed and documented;
  modules cannot bind them. On keyboard-only outputs focus can reach every view and the
  chrome without a pointer.
- Input is routed through the scheduler to the target view's module (canlin#27); the
  shell decides the target before posting.

## 5. Drawing

Chrome and views are separate dirty regions. A view redraws only when its module is
dirty or its rectangle changed; the status bar redraws on its own state changes (a clock
at most once per minute, not per frame). Moving a divider re-lays out the two adjacent
views. The render backend copies only dirty rows (UI spec §9), so on rawfb a clock tick
touches the status bar rows only.

## 6. Limits

In `include/canlin/shell_limits.h`:

| constant | value |
|---|---|
| `CANLIN_SHELL_PANES_MAX` | 3 per output |
| `CANLIN_SHELL_PANE_MIN` | 30 em |
| `CANLIN_SHELL_TILED_MIN` | 60 em output width |
| `CANLIN_SHELL_TILED_DEFAULT` | 80 em output width |
| `CANLIN_SHELL_BAR_HEIGHT` | 2.5 em |

## 7. Tests

Offscreen with fixed theme/font and fake clock: mode choice at 59.9/60/79.9/80 em and
after scale changes; fallback to full-screen and restore of tiling; divider drag keeps
both panes ≥ minimum; focus changes and tap-through into an unfocused pane; reserved
keys never reach modules; status bar indicators (volatile, microphone, faulted) cannot
be covered by a view; a faulted module's pane shows restart and restart creates a new
generation; idle shell with a clock redraws only the bar rows once per minute.

## 8. Follow-ups

- canlin#34 free floating windows (opt-in, pointer outputs only).
- canlin#25 several outputs: this spec already assigns modes and focus per output.
- Settings spec: trusted settings screen and its privileges.
