# Module layer (src/module): one sandbox plus one scheduler generation (canlin#9), with
# its UI view (canlin#18). Needs mk/ev.mk, mk/lua.mk, mk/render.mk, mk/sandbox.mk and
# mk/ui.mk. Fragments sorted after this one are named by path, not by variable (a
# prerequisite list is expanded when the rule is read).
MODULE_HDR := include/canlin/module.h include/canlin/module_limits.h \
	include/canlin/sandbox.h include/canlin/ev.h include/canlin/ev_limits.h \
	include/canlin/ui.h include/canlin/ui_limits.h include/canlin/theme.h \
	include/canlin/render.h include/canlin/render_limits.h include/canlin/nk.h
MODULE_OBJ := $(BUILD)/src/module/module.o
MODULE_SB_OBJ := $(BUILD)/src/sandbox/sandbox.o
MODULE_UI_OBJ := $(BUILD)/src/ui/ui.o $(BUILD)/src/ui/graph.o $(BUILD)/src/ui/theme.o $(BUILD)/src/render/render.o \
	$(BUILD)/src/render/font_builtin.o $(BUILD)/src/render/nk_impl.o
LIBOBJ += $(MODULE_OBJ)

$(MODULE_OBJ) $(BUILD)/t/module/test_module.o: \
	override CPPFLAGS += $(LUA_CPPFLAGS) -isystem third_party/nuklear
$(MODULE_OBJ) $(BUILD)/t/module/test_module.o $(MODULE_SB_OBJ): $(MODULE_HDR)

MODULE_TEST := $(BUILD)/t/module/test_module
TESTS += $(MODULE_TEST)

$(MODULE_TEST): $(BUILD)/t/module/test_module.o $(MODULE_OBJ) $(EV_OBJ) $(MODULE_SB_OBJ) \
		$(MODULE_UI_OBJ) $(LUA_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ -lm
