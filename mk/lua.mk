# Vendored PUC Lua 5.4 (third_party/lua, read-only). Only the core plus the
# libraries the sandbox exposes are built: no lua.c/luac.c, loadlib, liolib,
# loslib, ldblib, lcorolib or linit. Configured by flags only; LUA_32BITS off.
LUA_DIR := third_party/lua/src
LUA_CORE := lapi lcode lctype ldebug ldo ldump lfunc lgc llex lmem lobject \
	lopcodes lparser lstate lstring ltable ltm lundump lvm lzio
LUA_LIBS := lauxlib lbaselib ltablib lmathlib lutf8lib
LUA_CPPFLAGS := -I$(LUA_DIR)

# lstrlib is compiled from a patched copy in $(BUILD) (patches/lua/README);
# third_party/lua stays byte-identical to the release tarball. -F0: any
# context drift fails the build instead of applying fuzzily.
LUA_PATCHED := $(BUILD)/patched/lua
LUA_LSTR_PATCH := patches/lua/0001-lstrlib-match-step-budget.patch
LUA_LSTR_C := $(LUA_PATCHED)/lstrlib.c
LUA_LSTR_OBJ := $(LUA_PATCHED)/lstrlib.o

LUA_OBJ := $(patsubst %,$(BUILD)/$(LUA_DIR)/%.o,$(LUA_CORE) $(LUA_LIBS)) $(LUA_LSTR_OBJ)

$(LUA_LSTR_C): $(LUA_DIR)/lstrlib.c $(LUA_LSTR_PATCH)
	@mkdir -p $(dir $@)
	@rm -f $@ $@.tmp $@.rej
	patch --batch --forward -F0 -s -r $@.rej -o $@.tmp $(LUA_DIR)/lstrlib.c < $(LUA_LSTR_PATCH) \
		|| { echo "error: $(LUA_LSTR_PATCH) does not apply to $(LUA_DIR)/lstrlib.c" >&2; exit 1; }
	mv $@.tmp $@

$(LUA_LSTR_OBJ): $(LUA_LSTR_C)
	$(CC) $(LUA_CPPFLAGS) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

# Vendored code is not held to our warning set. 'override' so this also
# applies when CFLAGS is given on the command line.
$(LUA_OBJ): override CFLAGS += -w -Wno-error

# bionic has no localeconv() before API 21, which luaconf.h's default
# lua_getlocaledecpoint() calls. CanLin never calls setlocale(), so the radix
# character is the C locale's '.' on every target; Android gets it as a constant.
ifeq ($(TARGET),android-armv7)
$(LUA_OBJ): override CPPFLAGS += "-Dlua_getlocaledecpoint()='.'"
endif
