# Module sandbox (src/sandbox) and its tests (t/sandbox). Needs mk/lua.mk.
SANDBOX_OBJ := $(BUILD)/src/sandbox/sandbox.o
LIBOBJ += $(SANDBOX_OBJ) $(LUA_OBJ)

# The test also uses a plain Lua state to produce a genuine binary chunk.
$(SANDBOX_OBJ) $(BUILD)/t/sandbox/test_sandbox.o: override CPPFLAGS += $(LUA_CPPFLAGS)

SANDBOX_TEST := $(BUILD)/t/sandbox/test_sandbox
TESTS += $(SANDBOX_TEST)

$(SANDBOX_TEST): $(BUILD)/t/sandbox/test_sandbox.o $(SANDBOX_OBJ) $(LUA_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ -lm
