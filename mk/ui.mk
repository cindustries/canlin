# UI: theme resolution (canlin#17) and the flow UI / draw(ui) object (canlin#18) in
# src/ui. Needs mk/render.mk (Nuklear object and include flags), mk/lua.mk, mk/ev.mk,
# mk/module.mk and mk/sandbox.mk (all sorted before this file).
UI_HDR := include/canlin/theme.h include/canlin/ui_limits.h include/canlin/ui.h $(NK_HDR)
UI_OBJ := $(BUILD)/src/ui/theme.o $(BUILD)/src/ui/ui.o $(BUILD)/src/ui/graph.o
LIBOBJ += $(UI_OBJ)

THEME_TEST := $(BUILD)/t/ui/test_theme
UI_TEST := $(BUILD)/t/ui/test_ui
TESTS += $(THEME_TEST) $(UI_TEST)

$(UI_OBJ) $(BUILD)/t/ui/test_theme.o $(BUILD)/t/ui/test_ui.o: override CPPFLAGS += $(NK_CPPFLAGS)
$(BUILD)/src/ui/ui.o $(BUILD)/src/ui/graph.o $(BUILD)/t/ui/test_ui.o: override CPPFLAGS += $(LUA_CPPFLAGS)
$(UI_OBJ) $(BUILD)/t/ui/test_theme.o $(BUILD)/t/ui/test_ui.o: $(UI_HDR) \
	include/canlin/render.h include/canlin/sandbox.h include/canlin/module.h

$(THEME_TEST): $(BUILD)/t/ui/test_theme.o $(BUILD)/src/ui/theme.o $(RENDER_OBJ) $(NK_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ -lm

$(UI_TEST): $(BUILD)/t/ui/test_ui.o $(UI_OBJ) $(MODULE_OBJ) $(EV_OBJ) $(SANDBOX_OBJ) \
		$(RENDER_OBJ) $(NK_OBJ) $(LUA_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ -lm

# Graph editor widget (canlin#19): src/ui/graph.c is part of UI_OBJ; its own test.
GRAPH_TEST := $(BUILD)/t/ui/test_graph
TESTS += $(GRAPH_TEST)
$(BUILD)/t/ui/test_graph.o: override CPPFLAGS += $(NK_CPPFLAGS) $(LUA_CPPFLAGS)
$(BUILD)/src/ui/ui.o $(BUILD)/src/ui/graph.o $(BUILD)/t/ui/test_graph.o: src/ui/graph.h
$(BUILD)/t/ui/test_graph.o: $(UI_HDR) include/canlin/module.h include/canlin/sandbox.h
$(GRAPH_TEST): $(BUILD)/t/ui/test_graph.o $(UI_OBJ) $(MODULE_OBJ) $(EV_OBJ) $(SANDBOX_OBJ) \
		$(RENDER_OBJ) $(NK_OBJ) $(LUA_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ -lm

# The UI budgets (canlin#96): a tool, not a test - it prints what the font atlas
# and the graph at its limits cost, and its numbers are read from its output, so
# it does not belong in TESTS. Run it on the host and under the emulated target:
# 'make ui-budgets BUILD=build/host' and
# 'qemu-i386 -cpu pentium2 ./build/i686/tools/ui_budget'.
UI_BUDGETS := $(BUILD)/tools/ui_budget

ui-budgets: $(UI_BUDGETS)

$(BUILD)/tools/ui_budget.o: override CPPFLAGS += $(NK_CPPFLAGS) -Isrc/render
$(UI_BUDGETS): $(BUILD)/tools/ui_budget.o $(UI_OBJ) $(MODULE_OBJ) $(EV_OBJ) $(SANDBOX_OBJ) \
		$(RENDER_OBJ) $(NK_OBJ) $(LUA_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ -lm

# On-screen keyboard (canlin#45): part of ui.c; its own test.
KBD_TEST := $(BUILD)/t/ui/test_kbd
TESTS += $(KBD_TEST)
$(BUILD)/t/ui/test_kbd.o: override CPPFLAGS += $(NK_CPPFLAGS) $(LUA_CPPFLAGS)
$(BUILD)/t/ui/test_kbd.o: $(UI_HDR) include/canlin/module.h include/canlin/sandbox.h
$(KBD_TEST): $(BUILD)/t/ui/test_kbd.o $(UI_OBJ) $(MODULE_OBJ) $(EV_OBJ) $(SANDBOX_OBJ) \
		$(RENDER_OBJ) $(NK_OBJ) $(LUA_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ -lm
