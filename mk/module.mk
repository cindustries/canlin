# Module layer (src/module): one sandbox plus one scheduler generation (canlin#9).
# Needs mk/ev.mk, mk/lua.mk and mk/sandbox.mk. mk/sandbox.mk is included after this
# file (sorted), so the sandbox object is named by path, not by $(SANDBOX_OBJ).
MODULE_HDR := include/canlin/module.h include/canlin/module_limits.h \
	include/canlin/sandbox.h include/canlin/ev.h include/canlin/ev_limits.h
MODULE_OBJ := $(BUILD)/src/module/module.o
MODULE_SB_OBJ := $(BUILD)/src/sandbox/sandbox.o
LIBOBJ += $(MODULE_OBJ)

$(MODULE_OBJ) $(BUILD)/t/module/test_module.o: override CPPFLAGS += $(LUA_CPPFLAGS)
$(MODULE_OBJ) $(BUILD)/t/module/test_module.o $(MODULE_SB_OBJ): $(MODULE_HDR)

MODULE_TEST := $(BUILD)/t/module/test_module
TESTS += $(MODULE_TEST)

$(MODULE_TEST): $(BUILD)/t/module/test_module.o $(MODULE_OBJ) $(EV_OBJ) $(MODULE_SB_OBJ) \
		$(LUA_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ -lm
