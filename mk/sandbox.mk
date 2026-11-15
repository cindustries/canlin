# Module sandbox (src/sandbox) and its tests (t/sandbox). Needs mk/lua.mk.
SANDBOX_OBJ := $(BUILD)/src/sandbox/sandbox.o
SANDBOX_HDR := include/canlin/sandbox.h include/canlin/sandbox_limits.h
LIBOBJ += $(SANDBOX_OBJ) $(LUA_OBJ)

# The test also uses a plain Lua state to produce a genuine binary chunk.
$(SANDBOX_OBJ) $(BUILD)/t/sandbox/test_sandbox.o: override CPPFLAGS += $(LUA_CPPFLAGS)

# The charge factors live in sandbox_limits.h: a change of a factor must rebuild
# every object that charges with it, or the build keeps the old factors.
$(SANDBOX_OBJ) $(BUILD)/t/sandbox/test_sandbox.o $(BUILD)/tools/sb_costs.o: $(SANDBOX_HDR)

SANDBOX_TEST := $(BUILD)/t/sandbox/test_sandbox
TESTS += $(SANDBOX_TEST)

$(SANDBOX_TEST): $(BUILD)/t/sandbox/test_sandbox.o $(SANDBOX_OBJ) $(LUA_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ -lm

# The cost calibration (core API spec §7, canlin#13): a tool, not a test - it prints
# what the charged native work costs in VM instructions, and its numbers are read
# from its output, so it does not belong in TESTS. Run it on the host and under the
# emulated target: 'make sandbox-costs BUILD=build/host' and
# 'qemu-i386 -cpu pentium2 ./build/i686/tools/sb_costs'.
SB_COSTS := $(BUILD)/tools/sb_costs

sandbox-costs: $(SB_COSTS)

$(SB_COSTS): $(BUILD)/tools/sb_costs.o $(SANDBOX_OBJ) $(LUA_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ -lm
