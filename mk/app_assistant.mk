# The assistant session of the app (canlin#40, k109): the built-in assistant module,
# the model view, the response gate and the routing between them (src/app/assistant.c).
# Needs mk/app.mk, mk/llm.mk, mk/module.mk and mk/ui_assistant.mk; the objects are
# named in the second phase (the $$ references below), so this fragment may sort before
# them. The headers are named by path: a prerequisite list is expanded when the rule is
# read.
APP_ASSIST_HDR := include/canlin/app.h src/app/app_int.h include/canlin/assistant_binding.h \
	include/canlin/assistant_response.h include/canlin/assistant_view.h \
	include/canlin/assistant_limits.h include/canlin/llm.h include/canlin/llm_limits.h \
	include/canlin/module.h include/canlin/ui.h
APP_ASSIST_OBJ := $(BUILD)/src/app/assistant.o
LIBOBJ += $(APP_ASSIST_OBJ)

APP_ASSIST_TEST := $(BUILD)/t/app/test_assistant
TESTS += $(APP_ASSIST_TEST)

$(APP_ASSIST_OBJ) $(BUILD)/t/app/test_assistant.o: override CPPFLAGS += \
	-isystem third_party/nuklear -Ithird_party/lua/src
$(APP_ASSIST_OBJ) $(BUILD)/t/app/test_assistant.o: $(APP_ASSIST_HDR)

.SECONDEXPANSION:
$(APP_ASSIST_TEST): $(BUILD)/t/app/test_assistant.o $$(APP_LINK_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ -lm
