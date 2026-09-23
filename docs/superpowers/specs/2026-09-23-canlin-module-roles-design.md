# CanLin — module roles, capabilities and device modules

Status: draft for review, from the owner's direction (2026-09-23): the LLM assistant and
the default behaviour of a device are **modules**, removable and replaceable — even both
at once by a single module — and hardware needs "driver" modules. This spec fixes what
stays core, what becomes a module, and how a module gets the powers such a role needs.
It changes the LLM providers spec §4 ("only the built-in assistant gets `canlin.llm`")
into a capability grant (§3).

## 1. Three layers

| layer | what | replaceable? |
|---|---|---|
| **core** (C, in the binary) | scheduler, sandbox, render, input, shell chrome (status bar, switcher, model-view frame, permission prompts), transports, credential vault, LLM dialects, platform adapters for display/input/audio/network | no — it is the security boundary and the trusted chrome |
| **system modules** (Lua, shipped in the binary) | `home` (what the device does after start: launcher, onboarding "Wer bist du?"), `assistant` (LLM chat + `set_view`), `settings` (trusted settings UI) | yes — removable and replaceable by other modules with the same role |
| **modules** (Lua, installed) | apps, dashboards, flows, device modules (§4) | yes |

The rule: anything that must protect the user from a module (credentials, permission
prompts, indicators, the model-view frame) stays in the core. Everything that is only
*behaviour* — what starts, how a chat looks, what a kiosk shows — is a module.

## 2. Roles

A module declares roles in its bundle: `meta#roles` = list of `home`, `assistant`,
`settings`, `device`, `app` (default `app`). Roles are about *placement*; powers come
from capabilities (§3).

- **home** — started first on every output that has no other assignment; exactly one
  active home module per instance. It may be the only module on a kiosk.
- **assistant** — receives the `set_view` model view and the submission pickup
  (UI spec §7) and is the module the shell opens for "ask the assistant".
- **settings** — the trusted settings surface. Because it edits provider profiles,
  credentials, accounts and grants, only a module **shipped in the binary or signed by
  the operator's key** can hold this role; a module installed from a service cannot.
- A single module may hold several roles: a kiosk module can be `home` + `assistant`
  and replace both built-ins. Removing the built-in `assistant` just means no module
  holds that role — the device works without any LLM.

Which modules hold which role on an instance is instance configuration (accounts spec:
stored per instance, copyable).

## 3. Capabilities

A module gets powers only through capabilities granted in trusted configuration, never
by its own declaration. The bundle declares what it *requests* (`meta#requires`); the
settings surface shows the request and the user/operator grants or denies it; the
grant is stored per instance. Examples:

| capability | gives | default for |
|---|---|---|
| `llm` | `canlin.llm` with the profiles the grant names | built-in `assistant` |
| `assistant-view` | the model-view binding (`set_view` rendering, submissions) | modules with role `assistant` |
| `settings` | the privileged settings API (profiles, credentials by handle only, grants, accounts) | built-in `settings`; binary/operator-signed only |
| `net:<origin>` | bounded HTTP to that origin | none |
| `serial:<port>`, `mqtt:<topic filter>`, `hid:<device>` | raw device channels (§4) | none |
| `device:<class>` | publish or read a device class (§4) | none |

A capability that is not granted makes the API absent (`nil`) or answers `denied` —
never a pretend success (system design §5). Grants are revocable; revocation cancels
the module's affected operations like a stop of that capability.

## 4. Drivers and device modules

"Driver" means three different things here:

1. **Kernel drivers** — NIC, sound chip, graphics, touch controller, USB serial. They
   belong to the kernel and **canlin-os** (hardware inventory canlin#3). No module can
   add one.
2. **Platform adapters** — the core's rawfb/evdev/ALSA/SDL/browser adapters. Core C,
   not modules.
3. **Device modules** — Lua modules that speak a *device protocol* over a raw channel
   the core provides and grants: a sensor on a serial port, an MQTT device, a Modbus
   meter, a HID barcode scanner, the SunRiser8 over MQTT. They are the "hardware
   modules". They never touch `/dev` directly; they get a bounded channel handle.

A device module publishes what it read as a **device**: a typed, bounded record other
modules can read without talking to the hardware themselves.

```lua
-- device module (grants: serial:/dev/ttyUSB0, device:sensor.temperature)
local d = canlin.device.publish("sensor.temperature", { id = "werkstatt", unit = "°C" })
d:update({ value = 21.4 })                -- latest-state, coalesced

-- any module (grant: device:sensor.temperature)
for _, dev in ipairs(canlin.device.list("sensor.temperature")) do ... end
local h = canlin.device.watch("sensor.temperature", "werkstatt")   -- "device" events
```

This is the one controlled path between modules that v1 allows; it is not a general
message bus (event runtime §1): classes are named and granted, values are bounded
latest-state records (plus optional bounded command calls in a later version), and the
publisher is identified by the core. Device classes also become natural nodes in the
graph editor (UI spec §5) for flow modules.

## 5. Start-up

1. Core starts, shell chrome appears.
2. The instance configuration lists modules, roles and grants. On a fresh instance the
   defaults are the built-in `home`, `assistant` and `settings` with their default grants.
3. The `home` module is started and assigned to each output without an assignment.
   With no `home` module the shell shows the switcher, never a blank screen.
4. A faulted `home` or `settings` module falls back to the built-in one, so a broken
   replacement can never lock the user out of the settings.

## 6. Consequences for existing specs

- LLM providers §4: `canlin.llm` is granted by the `llm` capability, not reserved to the
  built-in assistant. (Implementation in canlin#38 keeps a trusted flag for now; it
  becomes the grant check once grants exist.)
- UI spec §7 / canlin#20: the model-view binding is the `assistant-view` capability.
- Shell spec §3: the settings screen is the `settings`-role module; its privileged API
  is the `settings` capability, binary- or operator-signed only.

## 7. Open questions for the owner

1. **Replacing `settings`:** is "only shipped or operator-signed modules" the right
   boundary, or should settings never be replaceable?
2. **Device commands:** v1 devices are read-only records (sensors). Do you need
   commands to devices early (switch a relay, set a lamp), e.g. for SunRiser8?
3. **Grant UI:** ask on first use of a capability (like phones), or only at install
   time?
