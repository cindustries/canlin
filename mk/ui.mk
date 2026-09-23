# UI: theme resolution (src/ui, canlin#17). Needs mk/render.mk (Nuklear object and
# include flags; sorted before this file).
UI_HDR := include/canlin/theme.h include/canlin/ui_limits.h $(NK_HDR)
UI_OBJ := $(BUILD)/src/ui/theme.o
LIBOBJ += $(UI_OBJ)

THEME_TEST := $(BUILD)/t/ui/test_theme
TESTS += $(THEME_TEST)

$(UI_OBJ) $(BUILD)/t/ui/test_theme.o: override CPPFLAGS += $(NK_CPPFLAGS)
$(UI_OBJ) $(BUILD)/t/ui/test_theme.o: $(UI_HDR) include/canlin/render.h

$(THEME_TEST): $(BUILD)/t/ui/test_theme.o $(UI_OBJ) $(RENDER_OBJ) $(NK_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ -lm
