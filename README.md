# gnx

A thin-client runtime: one static C binary with Nuklear, Lua 5.4, libbee and SQLite that
runs Lua modules on anything from a Pentium II laptop with a framebuffer to an Android
or iOS device via SDL2. Screens, audio in and out, network and storage are the core;
features are Lua modules shipped as BeePacks; all intelligence (speech, LLM, the source
of truth for data) lives on the server.

Architecture: `.claude/skills/gnx-core/SKILL.md`. Design specs:
`docs/superpowers/specs/`. Family: `libbee` (storage format and library), `gnx-os`
(bootable images), `p5-beepack` (Perl tooling), `sunriser8` (reference IoT device).
