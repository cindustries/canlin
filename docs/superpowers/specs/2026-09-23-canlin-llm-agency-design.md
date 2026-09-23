# CanLin — LLM agency: tools, model-written modules and versions

Status: draft for review, from the owner's vision (2026-09-23): the user adds an LLM and,
if they want, gives it access to the system — to look at the screen, to build custom
modules that do something for the user, to manage modules and data within what the
granted capabilities allow. Data lives locally or in the cloud (accounts spec), modules
can be copied between instances, and every change is versioned. The user builds their
own world with the OS, steered by the LLM, and CanLin keeps it safe.

This **changes** a rule of the system design §5 / LLM/UI design §5 ("no execution from
model-generated code strings"): model-written Lua may run — but only as a module, in
the sandbox, with capabilities the user granted, and under the approval rule of §4.
Model-produced views stay declarative data (`set_view`).

## 1. The agent

An **agent** is an LLM profile (providers spec) plus a set of **agent grants**: what
this LLM may do on this instance. Without grants an LLM can only chat and use
`set_view`. Grants are set in trusted settings, per agent, and are stored per instance
(copyable like other configuration). Several agents can exist with different grants,
e.g. a cloud model with broad rights on the workshop PC and a local model with none on
the kitchen tablet.

The agent runs inside the module with the `assistant` role (module roles spec); the
tools below are core-provided, the assistant passes them to the model and executes the
calls through narrow host APIs. The model can never define its own tools.

## 2. Tools

| tool | needs agent grant | does |
|---|---|---|
| `set_view` | — | declarative view (UI spec §7) |
| `screenshot` | `see-screen` | image of one module view or the whole output **without** trusted chrome, settings screens or credential fields; sent as image input to vision-capable profiles |
| `module_list`, `module_source`, `module_log` | `read-modules` | list modules, read a module's source and its recent errors/faults (so the model can fix its own code) |
| `module_propose` | `write-modules` | create or change a module: Lua chunks, metadata, requested capabilities (§3) |
| `module_remove`, `module_rollback` | `manage-modules` | remove a model-written module, go back to an earlier version (§5) |
| `data_read`, `data_write` | `data:<module>` | read/write a module's local store or SQLite through the same bounded APIs the module uses |
| `device_list`, `device_read` | `device:<class>` | look at published devices (module roles spec §4) |
| `config_propose` | `manage-config` | propose configuration changes (layout, roles, theme values); never credentials, grants or accounts |

Every tool result is data for the model; every tool call is logged in the agent log
(§6). Screens, sources and data the model reads are untrusted input for it — they can
contain prompt-injection text, which is why §3 and §4 exist.

## 3. Capabilities of model-written modules

A module the agent writes requests capabilities like any module (`meta#requires`). The
**ceiling** is the agent's grants: a model-written module can never request more than
its agent was granted to hand out (`module-caps` list per agent), and never `settings`,
`llm`, `assistant-view`, account or credential access. So an agent allowed to write
modules with `net:api.weather.example` and `device:sensor.temperature` can build a
weather-and-sensor dashboard, and nothing that reaches other hosts or other devices.

## 4. Approval

Per agent, the user picks one mode:

- **ask** (default): every `module_propose`, `module_remove`, `module_rollback` and
  `config_propose` is shown in trusted UI — code diff, requested capabilities, data
  touched — and applies only after the user confirms.
- **workshop**: changes within the agent's grants apply immediately and are marked;
  the status bar shows "Agent darf ändern" while the mode is on, and a single action
  ("Agent stoppen") revokes it and cancels the agent's running operations. Anything
  beyond the grants still asks.

Reads (`screenshot`, `module_source`, `data_read`) follow the grants without a prompt;
the status bar shows when the agent is looking at the screen, like the microphone
indicator.

## 5. Versions

Every change to a module — by the model, by the user, by an update — creates a
**version**: the complete bundle (`module.bee`), its author (agent id or user), time,
message and parent. Versions are kept per module in the instance storage and synced
with the account like other configuration; a bounded number (`CANLIN_VERSIONS_MAX`) is
kept, older ones are pruned only after being synced.

- Activating a version uses the staged reload with rollback (core API §6): a broken
  version never replaces a working one.
- `module_rollback` / the user can go back to any kept version.
- Copying a module to another instance copies its current version and, if wanted, its
  history.
- **Git** as a module: a later `git` module can export a module's versions as commits
  and push them to a remote (HTTPS smart protocol, with a `net:` grant for that host).
  It is an optional mirror for people who want their world on GitHub/Gitea — the
  version store itself does not depend on git.

## 6. Safety

- **Sandbox and budgets unchanged:** model-written code runs with the same instruction,
  memory and native-work budgets as any module, and a fault only stops that module.
- **Provenance is visible:** model-written modules are marked in the switcher and on
  their pane; their data is theirs, not shared with other modules unless granted.
- **Agent log:** every tool call, approval and change, with the agent and the version it
  produced; viewable in settings, synced with the configuration.
- **Kill switch:** "Agent stoppen" revokes workshop mode and cancels the agent's work;
  "Zurück auf Stand von …" rolls all model-written modules back to versions at a point
  in time.
- **Prompt injection is assumed:** nothing the model reads is trusted; the grant ceiling
  and the approval mode are the defence, not the model's judgement.

## 7. WASM

The browser build is the same runtime compiled to WebAssembly (system design §2), so
the agent, tools, model-written Lua modules and versions work there too, within browser
limits (storage per accounts spec, no raw sockets). Running *WASM as a module format*
(for compute-heavy modules, e.g. with the wasm3 interpreter, MIT) is possible later as a
second sandbox kind with the same capabilities and budgets; it is not needed for
model-written modules, which stay Lua: small, readable in a diff, and fast enough on
the P2.

## 8. Order

1. Agent profiles and grants in trusted settings; agent log.
2. Read tools (`module_list/source/log`, `device_list/read`), then `module_propose` with
   **ask** mode, versions and rollback.
3. `screenshot` for vision-capable dialects (image input per dialect).
4. **workshop** mode, `config_propose`, data tools.
5. Optional `git` export module; WASM module sandbox if a real need appears.
