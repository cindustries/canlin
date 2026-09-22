---
name: beepack-format
description: "Use when reading, writing, generating or validating a BeePack (.bee / .MP) file, an override directory (CONFIG/, hashed 8.3 filenames), or a layered key lookup — in any implementation: libbee (C), p5-beepack (Perl), sunriser8 firmware, gnx modules."
user-invocable: false
allowed-tools: Read, Grep, Glob
model: sonnet
---

# BeePack — the on-disk format

Implementation-independent facts about BeePack files. What any implementation must
agree on so that a file written by one is read by all. Implementation internals live
in the owning repo's skill (`libbee-core`, `beepack-core`).

## Two file kinds

| Extension | Content | Purpose |
|---|---|---|
| `.bee` | a CDB (djb constant database) whose values are MsgPack | a bundle: firmware, theme, module, server snapshot |
| `.MP` / `.mp` | raw MsgPack | one override value, a backup, a state dump |

A `.bee` is **read-only by construction** — CDB has no in-place update. Changing one
value means rebuilding the whole file (Perl `save`, libbee `bee_make`) and replacing it
with an atomic `rename`. That is a feature: a reader never sees a half-written bundle.

## Keys

- Byte strings, no length limit in the format; the firmware reader caps at 128.
- Hierarchy lives **in the key name**, `#`-separated: `pwm#1#min`,
  `sensors#sensor#28FF123456789012#offset`, `web#img/background.jpg#content`.
  Values are flat or one to two containers deep; the firmware copier refuses nesting
  beyond depth 8. Do not model hierarchy as nested maps.
- **Exactly one value per key.** CDB allows several; BeePack does not use that.
- Writers insert keys in sorted order so a rebuilt file is byte-reproducible.

## Values — the portable MsgPack subset

Everything must round-trip Linux x86 (Perl writer) ↔ STM32 (C reader). Writers emit
only:

| Type | Notes |
|---|---|
| nil | see "nil" below |
| bool | |
| uint ≤ 32 bit | positive fixint, uint8/16/32 |
| sint ≤ 32 bit | negative fixint, int8/16/32 |
| str, bin | the reader treats both as bytes |
| array, map | shallow, see Keys |

Excluded: float/double, 64-bit ints, ext types. The Perl writer is `canonical` + `utf8`
(deterministic bytes, decoded strings). The firmware reader logs and returns the
zero-value on an excluded type; a strict implementation rejects it as an error rather
than reinterpreting bit patterns.

Coercion the firmware readers apply and other implementations mirror: bool reads as
0/1 for integers and any integer reads as bool (non-zero = true); nil reads as
0 / false / empty array.

## nil

Perl default: a key whose value is nil **does not exist** (`exists` false, `get`
undef); `nil_exists => 1` flips that. The firmware treats a present nil as existing with
the zero-value. The difference matters only in a layered lookup: a nil in an upper layer
masks the layers below in the firmware model. Keep whichever an implementation chooses
explicit and tested; never silently "simplify" it.

## Layers — the override principle

A lookup walks a stack from most specific to least specific and stops at the first
hit:

```
CONFIG/           override directory — one .MP file per key, device-local
THEME.BEE         theme / customer layer
FIRMWARE.BEE      factory defaults
```

The stack is a **convention of the consumer**, not of the file format: gnx uses
`local/` → `state.bee` → `module.bee` with the same mechanics.

### Override directory entries

- Filename: `%08X.MP` of the **djb2 hash** of the key (`h = 5381; h = h*33 + c` over
  the key bytes, 32-bit wrap), uppercase hex. This satisfies FAT 8.3 naming, keeps the
  directory small, and makes collisions practically nonexistent.
- Content: MsgPack `str` of the key, immediately followed by the MsgPack value. The
  key is stored so a reader verifies the file really is that key — compare **both**
  length and bytes; the original firmware compares `len == || memcmp`, which is a bug
  the hash hides.
- Writing an override = write the file with create-always semantics. Deleting the
  override = unlink. Nothing else is needed.

### Replacing a layer

Ship the new file under a staging name and `rename` over the live one at a safe
moment (the firmware renames `INSTALL.BEE` → `FIRMWARE.BEE` on boot, parks an unreadable
file as `BROKEN.BEE`). Never write into a live layer file.

### Backup of an override directory

`BACKUP.MP`: one MsgPack map header with the entry count, followed by the
concatenated bodies of all override files (key, value, key, value…).

## Reference implementations

- Perl: `~/dev/p5-beepack` (`lib/BeePack.pm`, `bin/bee`) — the writer the fixtures come
  from. `bee file.bee key` prints, `bee file.bee key i 123` sets.
- C, firmware: `~/dev/sunriser8/src/bee.c` + `tinycdbff/` (tinycdb on FatFS).
- C, portable: `~/dev/libbee` (skill `libbee-core`).
