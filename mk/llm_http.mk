# LLM transport over the native HTTP/1.1 + TLS client (canlin#41): src/llm/http_transport.c
# and t/llm/test_llm_http (the openai-chat fixtures through the real transport against the
# loopback server). Separate from mk/llm.mk; the fragments it needs (lua, module, net,
# mbedtls) sort after this file, so their objects are named with second expansion.
LLM_HTTP_OBJ := $(BUILD)/src/llm/http_transport.o
LIBOBJ += $(LLM_HTTP_OBJ)

LLM_HTTP_TEST := $(BUILD)/t/llm/test_llm_http
TESTS += $(LLM_HTTP_TEST)

$(LLM_HTTP_OBJ) $(BUILD)/t/llm/test_llm_http.o: include/canlin/llm_http.h include/canlin/http.h \
	include/canlin/http_wire.h include/canlin/net_limits.h $(LLM_HDR)
$(BUILD)/t/llm/test_llm_http.o: override CPPFLAGS += $(LUA_CPPFLAGS)
$(BUILD)/t/llm/test_llm_http.o $(BUILD)/t/llm/test_llm.o: t/llm/openai_fixtures.h
$(BUILD)/t/llm/test_llm_http.o: t/net/tsrv.h

.SECONDEXPANSION:
$(LLM_HTTP_TEST): $(BUILD)/t/llm/test_llm_http.o $(LLM_HTTP_OBJ) $(LLM_OBJ) $$(TSRV_OBJ) \
		$$(HTTP_OBJ) $$(NET_OBJ) $$(MODULE_OBJ) $$(EV_OBJ) $$(MODULE_SB_OBJ) \
		$$(MODULE_UI_OBJ) $$(LUA_OBJ) $$(MBEDTLS_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ -lm $(HTTP_LIBS)
