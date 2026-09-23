# net: bounded incremental SSE and JSON primitives (canlin#5) and the HTTP/1.1 wire
# primitives of the native transport (canlin#41, HTTP transport spec §6).
NET_HDR := include/canlin/http_wire.h include/canlin/net_limits.h
NET_OBJ := $(BUILD)/src/net/sse.o $(BUILD)/src/net/json.o $(BUILD)/src/net/http_wire.o
LIBOBJ += $(NET_OBJ)
TESTS += $(BUILD)/t/net/sse_test $(BUILD)/t/net/json_test $(BUILD)/t/net/http_wire_test

$(BUILD)/src/net/http_wire.o $(BUILD)/t/net/http_wire_test.o: $(NET_HDR)

$(BUILD)/t/net/%_test: $(BUILD)/t/net/%_test.o $(NET_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $^ -o $@
