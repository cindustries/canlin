# Third-party components and licences

Policy: everything linked into the CanLin runtime is under a permissive licence (MIT,
BSD, Apache-2.0, public domain / CC0, or an equally permissive font licence). No GPL,
LGPL or AGPL code is linked into the runtime. Each vendored component keeps its original
licence text next to it, and every build or package ships these texts (canlin#43).

## Linked into the runtime today

| component | where | licence |
|---|---|---|
| Lua 5.4.9 | `third_party/lua` | MIT |
| Nuklear (incl. embedded stb_truetype, stb_rect_pack) | `third_party/nuklear` | MIT or public domain (Unlicense), our choice: MIT |
| DejaVu Sans 2.37 (Latin subset) | `third_party/fonts/dejavu-sans` | Bitstream Vera / Arev font licence (permissive, embedding and subsetting allowed); DejaVu changes public domain |
| Adapted code from Nuklear's rawfb demo | `src/render/render.c` | MIT (© Patrick Rudolph), attribution kept in the file |

## Planned (system design §4)

| component | licence | note |
|---|---|---|
| SQLite | public domain | |
| sqlite-vec | MIT or Apache-2.0 | |
| mbedtls | Apache-2.0 or GPL-2.0-or-later | we use it under **Apache-2.0** |
| monocypher | CC0 or BSD-2-Clause | |
| cmp (MessagePack) | MIT | |
| libbee | same terms as Perl (Artistic-1.0 or GPL-1.0-or-later) | own library; see below |

**libbee** follows the Perl licence of BeePack, which is dual: it can be used under the
Artistic License instead of the GPL, so linking it does not make CanLin GPL. If a plain
permissive licence is preferred for the runtime, relicensing libbee (its own author) to
MIT/BSD is the simple fix — a decision for its owner.

## Build and test tools (not shipped)

gcc/binutils (GPL, with the GCC runtime library exception for libgcc), musl (MIT, the
C library of static i686 images), fontTools (MIT, used by `tools/mkfont.py`), QEMU (GPL,
test harness only). Using them does not affect the licence of what they build.

## OS images (canlin-os)

The images necessarily contain the Linux kernel (GPL-2.0). It runs CanLin as a separate
program, so the runtime keeps its own licence, but distributing an image requires
offering the kernel's corresponding source. For userspace, prefer permissive tools
(toybox 0BSD over BusyBox GPL-2.0) where the canlin-os owner agrees.
