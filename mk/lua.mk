# Vendored PUC Lua 5.4 (third_party/lua, read-only). Only the core plus the
# libraries the sandbox exposes are built: no lua.c/luac.c, loadlib, liolib,
# loslib, ldblib, lcorolib or linit. Configured by flags only; LUA_32BITS off.
LUA_DIR := third_party/lua/src
LUA_CORE := lapi lcode lctype ldebug ldo ldump lfunc lgc llex lmem lobject \
	lopcodes lparser lstate lstring ltable ltm lundump lvm lzio
LUA_LIBS := lauxlib lbaselib lstrlib ltablib lmathlib lutf8lib
LUA_OBJ := $(patsubst %,$(BUILD)/$(LUA_DIR)/%.o,$(LUA_CORE) $(LUA_LIBS))
LUA_CPPFLAGS := -I$(LUA_DIR)

# Vendored code is not held to our warning set. 'override' so this also
# applies when CFLAGS is given on the command line.
$(LUA_OBJ): override CFLAGS += -w -Wno-error
