# Assistant-owned view (canlin#20): set_view validator, model view and submit in
# src/assistant. Named to sort after mk/ui.mk, so every fragment it needs (ev, lua,
# module, net, render, sandbox, shell, ui) is read before it and their variables are set.
AV_HDR := include/canlin/assistant_view.h include/canlin/assistant_limits.h \
	include/canlin/json.h $(UI_HDR) $(SHELL_HDR)
AV_OBJ := $(BUILD)/src/assistant/view.o
LIBOBJ += $(AV_OBJ)

AV_TEST := $(BUILD)/t/assistant/test_view
TESTS += $(AV_TEST)

$(AV_OBJ) $(BUILD)/t/assistant/test_view.o: override CPPFLAGS += $(NK_CPPFLAGS) $(LUA_CPPFLAGS)
$(AV_OBJ) $(BUILD)/t/assistant/test_view.o: $(AV_HDR)

$(AV_TEST): $(BUILD)/t/assistant/test_view.o $(AV_OBJ) $(BUILD)/src/net/json.o $(SHELL_OBJ) \
		$(MODULE_OBJ) $(EV_OBJ) $(SANDBOX_OBJ) $(UI_OBJ) $(RENDER_OBJ) $(NK_OBJ) $(LUA_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ -lm
