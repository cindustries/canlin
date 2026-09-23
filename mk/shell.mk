# Shell (canlin#35): arrangement, chrome and focus in src/shell. Needs mk/ev.mk,
# mk/lua.mk, mk/module.mk, mk/render.mk and mk/sandbox.mk (sorted before this file);
# mk/ui.mk sorts after it, so its objects are named by path (MODULE_UI_OBJ), not UI_OBJ.
SHELL_HDR := include/canlin/shell.h include/canlin/shell_limits.h $(MODULE_HDR)
SHELL_OBJ := $(BUILD)/src/shell/shell.o
LIBOBJ += $(SHELL_OBJ)

SHELL_TEST := $(BUILD)/t/shell/test_shell
TESTS += $(SHELL_TEST)

$(SHELL_OBJ) $(BUILD)/t/shell/test_shell.o: override CPPFLAGS += $(NK_CPPFLAGS) $(LUA_CPPFLAGS)
$(SHELL_OBJ) $(BUILD)/t/shell/test_shell.o: $(SHELL_HDR)

$(SHELL_TEST): $(BUILD)/t/shell/test_shell.o $(SHELL_OBJ) $(MODULE_OBJ) $(EV_OBJ) \
		$(SANDBOX_OBJ) $(MODULE_UI_OBJ) $(LUA_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ -lm
