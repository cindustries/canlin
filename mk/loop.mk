# Native main loop (canlin#61, native loop spec): the core in src/loop/loop.c needs only
# mk/ev.mk; the adapters (canlin_http source, canlin_llm pump, shell display) are separate
# objects. t/loop/test_loop covers the core (fake clock/wait, a wake thread);
# t/loop/test_loop_native drives canlin_http and canlin_llm against the loopback server
# with the real poll() wait, and the shell display. Fragments sorting after this file
# (lua, mbedtls, module, net, render, sandbox, shell, ui) are named with second expansion.
LOOP_HDR := include/canlin/loop.h include/canlin/loop_limits.h include/canlin/ev.h \
	include/canlin/ev_limits.h
LOOP_OBJ := $(BUILD)/src/loop/loop.o
LOOP_HTTP_OBJ := $(BUILD)/src/loop/loop_http.o
LOOP_LLM_OBJ := $(BUILD)/src/loop/loop_llm.o
LOOP_SHELL_OBJ := $(BUILD)/src/loop/loop_shell.o
LOOP_LIBS := $(PTHREAD_LIBS)
LIBOBJ += $(LOOP_OBJ) $(LOOP_HTTP_OBJ) $(LOOP_LLM_OBJ) $(LOOP_SHELL_OBJ)

LOOP_TEST := $(BUILD)/t/loop/test_loop
LOOP_NATIVE_TEST := $(BUILD)/t/loop/test_loop_native
TESTS += $(LOOP_TEST) $(LOOP_NATIVE_TEST)

$(LOOP_OBJ) $(LOOP_HTTP_OBJ) $(LOOP_LLM_OBJ) $(LOOP_SHELL_OBJ) $(BUILD)/t/loop/test_loop.o \
	$(BUILD)/t/loop/test_loop_native.o: $(LOOP_HDR)
$(LOOP_HTTP_OBJ) $(BUILD)/t/loop/test_loop_native.o: include/canlin/http.h \
	include/canlin/http_wire.h include/canlin/net_limits.h
$(LOOP_LLM_OBJ) $(BUILD)/t/loop/test_loop_native.o: include/canlin/llm.h \
	include/canlin/llm_limits.h include/canlin/llm_http.h
$(LOOP_SHELL_OBJ) $(BUILD)/t/loop/test_loop_native.o: include/canlin/shell.h \
	include/canlin/shell_limits.h include/canlin/render.h include/canlin/ui.h \
	include/canlin/module.h
$(LOOP_SHELL_OBJ) $(BUILD)/t/loop/test_loop_native.o: override CPPFLAGS += \
	-isystem third_party/nuklear -Ithird_party/lua/src
$(BUILD)/t/loop/test_loop_native.o: t/net/tsrv.h t/llm/openai_fixtures.h

$(LOOP_TEST): $(BUILD)/t/loop/test_loop.o $(LOOP_OBJ) $(EV_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ $(LOOP_LIBS)

.SECONDEXPANSION:
$(LOOP_NATIVE_TEST): $(BUILD)/t/loop/test_loop_native.o $(LOOP_OBJ) $(LOOP_HTTP_OBJ) \
		$(LOOP_LLM_OBJ) $(LOOP_SHELL_OBJ) $(LLM_HTTP_OBJ) $(LLM_OBJ) $$(TSRV_OBJ) \
		$$(HTTP_OBJ) $$(NET_OBJ) $$(SHELL_OBJ) $$(MODULE_OBJ) $(EV_OBJ) $$(MODULE_SB_OBJ) \
		$$(MODULE_UI_OBJ) $$(LUA_OBJ) $$(MBEDTLS_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ -lm $(LOOP_LIBS)
