# net: bounded incremental SSE and JSON primitives (canlin#5).
NET_OBJ := $(BUILD)/src/net/sse.o $(BUILD)/src/net/json.o
LIBOBJ += $(NET_OBJ)
TESTS += $(BUILD)/t/net/sse_test $(BUILD)/t/net/json_test

$(BUILD)/t/net/%_test: $(BUILD)/t/net/%_test.o $(NET_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $^ -o $@
