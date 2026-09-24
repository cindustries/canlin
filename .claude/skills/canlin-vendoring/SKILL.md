---
name: canlin-vendoring
description: Use when vendoring, updating or checking third-party code for CanLin — a C library under third_party/, a Lua lib under libs/, its VERSION/SHA256SUMS, mk/<lib>.mk, a patch under patches/, a THIRD-PARTY.md entry, or its platform gates (host, i686 QEMU, WASM).
---

# CanLin vendoring

Vendoring is mechanical: the ticket decides *what*, this skill fixes *how*. Every
decision that is not written in the ticket is a stop (§ Stops), never a guess.

A job needs, in the ticket: lib name, exact version (tag or commit), official source
URL, and for C the file subset and any configuration values. Missing → stop and ask.

## Two kinds of lib (libs design spec)

| kind | lives in | reaches the runtime by |
|---|---|---|
| C library | `third_party/<name>/` | compiled via `mk/<name>.mk` |
| Lua lib | `libs/<name>/` | packed into a lib `.bee` |

A Lua binding for a C library is **not** vendoring — it is `canlin-worker` work. Say so
in the report; do not write it.

## Provenance — the same for both kinds

1. Download the official release artifact (tarball, or the tagged commit when upstream
   publishes no tarball) into the scratchpad, not the repo.
2. Verify its sha256 against the checksum **upstream publishes** (checksum file, release
   notes, asset digest). A checksum you only computed yourself proves nothing — if
   upstream publishes none, stop. Exception, only when the ticket names tag and commit
   id: a tag without published checksum is verified by `git rev-parse <tag>^{commit}`
   equal to that id; `VERSION` records `commit:` and the `git archive` sha256 marked
   self-computed (see `third_party/cmp/VERSION`).
3. Licence must be permissive per `docs/THIRD-PARTY.md` (MIT, BSD, Apache-2.0, public
   domain/CC0, OFL for fonts). Dual licence with a permissive option: record the choice.
   Anything else → stop.
4. Copy the needed files **byte-identical**. Keep upstream paths. Drop only whole files
   (build scripts, tests, programs, docs) and list what was dropped.
5. Write `VERSION`, `SHA256SUMS` (`sha256sum` of every vendored file, paths relative to
   the lib dir) and the verbatim `LICENSE`. `sha256sum -c SHA256SUMS` must pass.
6. Add or update the row in `docs/THIRD-PARTY.md`.

`VERSION` follows `third_party/mbedtls/VERSION`:

```
<Name> <version> (<branch/LTS note if any>)
source: <exact URL of the artifact>
sha256: <hash> (<bytes> bytes)
verified: <date> against <where upstream publishes the checksum>

Contents, unmodified from the tarball:
- <paths>
Not vendored: <dropped paths>

Configuration: <how, never by editing vendored files>
Read-only. Any fix is a patch file plus ticket.
Check: `sha256sum -c SHA256SUMS` from this dir.
```

A Lua lib's `VERSION` adds one line `depends: <name> >= x.y.z, …` (empty if none);
its Lua files go under `libs/<name>/lua/` in upstream layout.

## C library: build integration

Copy the shape of `mk/mbedtls.mk` / `mk/lua.mk` into `mk/<name>.mk`:

- objects under `$(BUILD)`, appended to `LIBOBJ` (or a named `<NAME>_OBJ` a consumer
  pulls in);
- `$(<NAME>_OBJ): override CFLAGS += -w -Wno-error` — vendored code is not held to our
  warning set;
- configuration only through flags or a `<NAME>_CONFIG_FILE` header under `src/`; the
  header's content comes from the ticket. Editing a vendored config header is never it.

## Platform checklist

Before building, grep the vendored sources and report every hit with file:line:

| pattern | why |
|---|---|
| `__SSE`, `__AVX`, `immintrin`, `xmmintrin`, `emmintrin`, `__asm`, `asm(` | P2 has no SSE; i686 builds `-march=i686 -mno-sse` |
| `pthread`, `fork`, `dlopen`, `mmap` | WASM and static images lack them or differ |
| `socket`, `getaddrinfo`, `fopen`, `time(`, `getenv` | I/O and ambient authority; a native lib is pure computation |
| `long double`, `__int128`, `__BYTE_ORDER`, unaligned casts | i686 / WASM portability |

A hit is a finding, not an automatic stop — unless it is compiled in and cannot be
switched off by configuration.

Then run every gate that exists and name each one that does not:

1. host: `make` and `make test`
2. i686: `make TARGET=i686` (static musl); then the canlin-os harness under
   `qemu-system-i386 -cpu pentium2 -m 1024` if present
3. WASM / mobile builds, once they exist

`make`/`make test` only compile what something links. A lib without a consumer yet is
built by its own target: `make <name>` and `make TARGET=i686 <name>` — say which you ran.

A gate that does not exist yet is reported as **skipped: not available**, never as
passed. A host pass says nothing about i686.

## Lua lib: sandbox fit

The CanLin sandbox has no `os`, `io`, `package`, `debug`, `coroutine`, no `string.dump`
and no binary `load`. Grep for each and for `require` of names missing from `depends`.
Any hit → the lib is rejected as-is; report it. Adapting upstream code is never the fix.
Run the lib's own tests inside the sandbox when a sandbox runner exists; otherwise
report the gate as skipped.

## Updates

1. Vendor the new version by the steps above, replacing the old files.
2. `git diff --stat` old → new, plus the changed public symbols that our code uses:
   grep `src/` and `include/` for each changed or removed symbol and list the hits.
3. Every patch under `patches/<name>/` must still apply with `patch -F0`; do the manual
   re-checks its `README` names. A patch that no longer applies → stop.
4. Our code broken by the update → stop and report the call sites; fixing `src/` is not
   this job.

## Stops

Stop, change nothing further, and report when:

- the ticket leaves name, version, source, subset or configuration open;
- the upstream checksum is missing or does not match;
- the licence is not permissive or not clear;
- vendored code would need a change (a new patch is a proposal plus ticket);
- an update breaks code outside the lib's own dirs;
- a Lua lib needs a forbidden global;
- a gate fails and the cause is not your own mistake in the steps above.

## Report

Return: files changed; upstream URL, version and verified checksum source; licence;
platform-checklist hits; each gate with passed / failed (output) / skipped (reason);
for updates the symbol and call-site list; stops and ticket proposals; a proposed
commit message `chore(third_party): vendor <name> <version>` (or `update … to …`,
`chore(libs): …` for Lua libs) in the body style of the existing vendor commits.
