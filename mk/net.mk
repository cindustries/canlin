# net: bounded incremental SSE and JSON primitives (canlin#5), the HTTP/1.1 wire
# primitives and the native HTTP/1.1 + TLS transport (canlin#41, HTTP transport spec).
# Needs mk/mbedtls.mk (sorted before this file) and mk/ev.mk.
NET_HDR := include/canlin/http_wire.h include/canlin/net_limits.h
NET_OBJ := $(BUILD)/src/net/sse.o $(BUILD)/src/net/json.o $(BUILD)/src/net/http_wire.o
LIBOBJ += $(NET_OBJ)
TESTS += $(BUILD)/t/net/sse_test $(BUILD)/t/net/json_test $(BUILD)/t/net/http_wire_test

$(BUILD)/src/net/http_wire.o $(BUILD)/t/net/http_wire_test.o: $(NET_HDR)

$(BUILD)/t/net/%_test: $(BUILD)/t/net/%_test.o $(NET_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $^ -o $@

# Native transport: sockets, resolver worker (pthread), TLS over Mbed TLS, canlin_http.
HTTP_HDR := include/canlin/http.h $(NET_HDR) include/canlin/ev.h src/net/net_int.h \
	src/net/canlin_mbedtls_config.h
HTTP_OBJ := $(BUILD)/src/net/http.o $(BUILD)/src/net/sock.o $(BUILD)/src/net/dns.o \
	$(BUILD)/src/net/tls.o
HTTP_LIBS := -lpthread
LIBOBJ += $(HTTP_OBJ)

$(HTTP_OBJ): $(HTTP_HDR)
$(BUILD)/src/net/tls.o: override CPPFLAGS += $(MBEDTLS_CPPFLAGS)

