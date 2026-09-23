# LLM providers (canlin#38): presets, profiles, session credentials, the openai-chat
# dialect, the fixture transport and canlin.llm, in src/llm. Needs mk/ev.mk, mk/lua.mk,
# mk/module.mk (and what it needs) and mk/net.mk; headers are named by path.
LLM_HDR := include/canlin/llm.h include/canlin/llm_limits.h src/llm/llm_int.h \
	include/canlin/sse.h include/canlin/json.h include/canlin/module.h \
	include/canlin/module_limits.h include/canlin/sandbox.h include/canlin/ev.h
LLM_OBJ := $(BUILD)/src/llm/llm.o $(BUILD)/src/llm/openai_chat.o $(BUILD)/src/llm/jsonw.o \
	$(BUILD)/src/llm/fixture.o
LIBOBJ += $(LLM_OBJ)

LLM_TEST := $(BUILD)/t/llm/test_llm
TESTS += $(LLM_TEST)

$(LLM_OBJ) $(BUILD)/t/llm/test_llm.o: override CPPFLAGS += $(LUA_CPPFLAGS)
$(LLM_OBJ) $(BUILD)/t/llm/test_llm.o: $(LLM_HDR)

# mk/lua.mk and mk/module.mk sort after this file: their object lists are expanded in the
# second phase (the $$ references below).
.SECONDEXPANSION:
$(LLM_TEST): $(BUILD)/t/llm/test_llm.o $(LLM_OBJ) $$(NET_OBJ) $$(MODULE_OBJ) $$(EV_OBJ) \
		$$(MODULE_SB_OBJ) $$(MODULE_UI_OBJ) $$(LUA_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ -lm
