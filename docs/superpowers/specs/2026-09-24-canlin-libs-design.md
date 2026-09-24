# CanLin — libs, dependencies and `require`

Status: draft for review, from the owner's direction (2026-09-24): third-party code and
extra Lua functionality are one thing — a **lib** that provides names a module can
`require` — packaged like a Debian package, with metadata naming what else it needs,
and delivered per platform. This spec **amends** the system design §4 ("`require`
resolves only this module's executable keys", §7 below).

## 1. Terms

| term | what | built by |
|---|---|---|
| **module** | an app as `module.bee` (`lua#main`, roles, data) — module roles spec | packing |
| **lib** | provides one or more `require` names; no `init`/`draw`, no roles, no data | packing (Lua lib) or compiling (native lib) |
| **Lua lib** | a lib written in Lua, shipped as a signed `lib.bee` | packing |
| **native lib** | a lib backed by C code, compiled into the runtime of each platform | `make` per target |
| **manifest** | the list of native and built-in Lua libs one runtime build provides | `make` per target |

A module never has to know whether a name it requires is Lua or C. That is the point
of the design: one concept, one resolver, one dependency list.

## 2. The lib package

A Lua lib is a BeePack with reserved keys:

```
meta#kind      "lib"                                  (absent = module)
meta#name      json                                   [a-z][a-z0-9_-]*
meta#version   1.4.2                                  numeric major.minor.patch
meta#depends   { "utf8x >= 1.0" }                     other libs (§3)
meta#license   MIT                                    SPDX id; permissive only (THIRD-PARTY.md policy)
meta#source    https://…  sha256:…                    upstream origin, verified at import
lua#json       main chunk of the lib
lua#json.util  further chunks
```

Every `lua#` key of a lib must be the lib's name or start with `<name>.`. A lib bee
contains only `meta#` and `lua#` keys — no fonts, images or data defaults; data a lib
needs is Lua tables in its chunks. Lua source only: binary chunks stay forbidden
(system design §4), so a Lua lib is identical on every platform.

Libs are signed like modules (Ed25519 over the `.bee` bytes, trusted key). There is no
local-code-override mode for libs in v1; tinkering happens in a module.

## 3. Dependencies

Modules and libs declare `meta#depends`: a list of `name` or `name <op> version`
constraints, several per entry joined by `,` — `json >= 1.2, < 2`. Operators are
`>=`, `<`, `=`; versions compare numerically per component. `meta#requires` keeps its
meaning from the module roles spec (capabilities) — the two lists are never mixed.

- **Explicit, also for native libs.** A module that calls `require "lpeg"` lists `lpeg`
  even though every current runtime has it compiled in. A name that is not in the
  module's dependency closure cannot be required, even if it is installed. The same
  module then fails the same way on every platform instead of working on one and
  breaking on another.
- **One version per name per instance** in v1. Resolution is against the running
  runtime's manifest plus the installed Lua libs. An install whose constraints cannot be
  met together with the installed set is refused and names the conflict; nothing is
  installed partially.
- No `recommends`, `conflicts`, virtual names or optional dependencies in v1.

## 4. `require` and the sandbox

`require "x"` inside a module resolves, in this order: the module's own `lua#x`, then
the libs of its dependency closure (Lua libs and native libs alike). Because names may
not collide (below), the order never decides anything; it only fixes the lookup.

- **No collisions.** An installed Lua lib may not use a name of a native or built-in lib.
  A module whose own `lua#` keys collide with a lib name in its closure is refused at
  install. Nothing masks anything silently.
- **Per-module instance.** A lib is loaded into the requiring module's own Lua state and
  cached there; modules never share lib tables. Its instructions and allocations are
  charged to that module's budgets. The memory cost of per-module copies is accepted.
- **A lib has no authority of its own.** It sees exactly the globals and the `canlin.*`
  API of the module that loaded it — no more. It follows that depending on a lib means
  trusting it with the module's grants; the grant screen (module roles §3) lists a
  module's dependency closure next to the requested capabilities.
- `canlin` and names starting with `canlin.` are reserved for the runtime.

## 5. Native code: per platform, inside the runtime, never downloaded

Native libs are C code compiled into the runtime for each target (`-march=i686
-mno-sse` for P2, arm64 for mobile, WASM for the browser …). The runtime per platform
*is* the per-platform delivery; a `lib.bee` never contains machine code. Reasons:

1. **The sandbox is the security boundary.** Native code runs outside it; a signature
   proves where code came from, not what it does. Every native entry point must charge
   its work to the budget (core API D8, canlin#8), never block and validate every value
   it hands to Lua — only reviewed core code can promise that.
2. **Native images are static binaries** (musl); there is no dynamic loader to load a
   library into.
3. **iOS forbids executing downloaded code**; Android's store policy does too, and both
   enforce W^X.
4. **The browser** would need WASM side modules and a second loader — a second native
   code path for one target.

What stays per platform:

- Each build has a **manifest** (`name`, `version`, `native`/`lua`) that the runtime
  reports and the module installer resolves against. A platform may lack a native lib
  (e.g. one that has no WASM port); modules depending on it do not install there, with
  a visible reason.
- A module source (a user's server, later a module store) may filter by manifest; the
  client's resolution stays authoritative.

**Native lib vs `canlin.*` API.** A native lib is pure computation: parsing, encoding,
hashing, pattern matching, maths. Anything with I/O, persistence, device access or other
authority is a capability-gated `canlin.*` API (module roles §3), never a lib. That line
keeps libs grant-free.

Adding a native lib is two separate pieces of work: vendoring the C library
(`third_party/`, VERSION/SHA256SUMS, THIRD-PARTY.md, all platform gates) and writing its
Lua binding (budget charging, value validation, D5 text rules, hostile-module review).
The first is mechanical, the second is not.

## 6. Where libs live and how they change

- **Built in:** native libs and the Lua libs the runtime ships (e.g. those the system
  modules need) are in the binary and listed in its manifest. Their sources live in the
  repo — native under `third_party/`, Lua under `libs/<name>/` with the same
  VERSION/SHA256SUMS discipline — and `make` packs the built-in Lua libs.
- **Installed:** `libs/<name>/lib.bee` on the data volume, through the same bounded,
  validated, atomic install as modules (system design §6).
- **Install** of a module installs missing Lua libs of its closure from the same source
  in one transaction: all or nothing.
- **Update** of a lib restarts the modules that depend on it with the staged reload and
  rollback of the core API §6; if a dependent fails, the previous lib version is kept.
- **Remove** of a lib that an installed module still depends on is refused. A lib no
  module depends on any more is removed with the last dependent.
- **Model-written modules** (LLM agency design) may depend on installed and built-in libs.
  Installing a new lib for them needs the same approval as installing a module.

## 7. Consequences for existing specs

- System design §4 (sandbox): `require` resolves the module's own keys and its declared
  dependency closure (§4 here), not only the module's own keys.
- Module roles §3: the grant screen shows the dependency closure; `meta#requires` stays
  capabilities, `meta#depends` is new.
- Core API §6: `canlin_module_source`'s require resolver gets the dependency closure;
  lib update uses the staged reload.
- THIRD-PARTY.md: Lua libs built into the runtime are listed like vendored C code.

## 8. Verification of a lib

- **Lua lib:** its own test suite runs inside the CanLin sandbox (no `os`, `io`,
  `package`, `debug`, `coroutine`, binary `load`) on host and on i686 under
  `qemu-system-i386 -cpu pentium2`; the WASM gate once the browser build exists.
  A lib that needs a forbidden global is not adapted in place — it is rejected or
  wrapped by a reviewed native lib.
- **Native lib:** the vendor gates (host, i686 QEMU, WASM, mobile when those builds
  exist) plus the binding's sandbox tests, including budget exhaustion and hostile
  inputs.
- **Resolver:** conflicting constraints, collisions, undeclared `require`, a missing
  native lib on one platform, update rollback and removal refusal each have a test.

## 9. Decisions and open questions

Confirmed by the owner (2026-09-24): native code only inside the runtime build (§5);
one version per name per instance in v1 (§3).

Open: do installed Lua libs come only from the module's own source, or also from a
shared lib store?
