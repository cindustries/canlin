# Minimal runtime assembly (canlin#72): scheduler + theme/ui + shell + one output with its
# frame + the built-in demo module, wired for the native loop (src/app/app.c,
# include/canlin/app.h). Backend-neutral: the Android library links it now, rawfb/SDL2
# later. Sorts before every fragment it needs, so their objects are named with second
# expansion (APP_LINK_OBJ is expanded only there).
APP_HDR := include/canlin/app.h include/canlin/loop.h include/canlin/loop_limits.h \
	include/canlin/shell.h include/canlin/shell_limits.h include/canlin/module.h \
	include/canlin/ui.h include/canlin/render.h include/canlin/theme.h include/canlin/ev.h
APP_OBJ := $(BUILD)/src/app/app.o
LIBOBJ += $(APP_OBJ)

APP_TEST := $(BUILD)/t/app/test_app
TESTS += $(APP_TEST)

$(APP_OBJ) $(BUILD)/t/app/test_app.o: override CPPFLAGS += -isystem third_party/nuklear \
	-Ithird_party/lua/src
$(APP_OBJ) $(BUILD)/t/app/test_app.o: $(APP_HDR)
# The test taps through the pure Android touch mapping (mk/android.mk).
$(BUILD)/t/app/test_app.o: $(ANDROID_HDR)

# Every object of one assembly (the Android library links the same list).
APP_LINK_OBJ = $(APP_OBJ) $(LOOP_OBJ) $(LOOP_SHELL_OBJ) $(SHELL_OBJ) $(MODULE_OBJ) $(EV_OBJ) \
	$(MODULE_SB_OBJ) $(MODULE_UI_OBJ) $(LUA_OBJ)

.SECONDEXPANSION:
$(APP_TEST): $(BUILD)/t/app/test_app.o $$(APP_LINK_OBJ) $$(ANDROID_PURE_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ -lm
