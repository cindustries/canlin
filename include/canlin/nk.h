/* CanLin — the one way to include Nuklear.
 *
 * Every translation unit that sees Nuklear types includes this header, never
 * nuklear.h directly: the NK_INCLUDE_* set changes struct layouts, so it must be
 * identical everywhere. src/render/nk_impl.c is the only TU with NK_IMPLEMENTATION.
 * Nuklear types stay inside src/render and src/ui (UI spec §1); they never reach Lua.
 *
 * Flag set (fixed; changing it is a reviewed decision, not a local tweak):
 *   NK_INCLUDE_FIXED_TYPES       <stdint.h> types; nk_size/nk_ptr are uintptr_t, so no
 *                                compiler/arch guessing (i686, x86_64, arm, wasm32).
 *   NK_INCLUDE_DEFAULT_ALLOCATOR malloc/free for nk_init_default and the font atlas.
 *   NK_INCLUDE_FONT_BAKING       embedded stb_truetype + stb_rect_pack (UI spec §6.3).
 * Deliberately absent: NK_INCLUDE_DEFAULT_FONT (ProggyClean; the built-in font is the
 * Inter subset in src/render/font_builtin.c, canlin#22), NK_INCLUDE_STANDARD_IO (no file access from the UI library),
 * NK_INCLUDE_STANDARD_VARARGS (no printf-style widgets; labels are formatted by the
 * caller), NK_INCLUDE_VERTEX_BUFFER_OUTPUT (the software rasterizer consumes the
 * command list directly), NK_INCLUDE_STANDARD_BOOL, NK_INCLUDE_COMMAND_USERDATA.
 */
#ifndef CANLIN_NK_H
#define CANLIN_NK_H

#if defined(NK_NUKLEAR_H_) && !defined(CANLIN_NK_CONFIGURED)
#error "nuklear.h was included before canlin/nk.h; include canlin/nk.h only"
#endif
#define CANLIN_NK_CONFIGURED 1

#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_FONT_BAKING
#include "nuklear.h"

#endif
