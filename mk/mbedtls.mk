# Vendored Mbed TLS 3.6 (third_party/mbedtls, read-only), canlin#41. Configured only by
# src/net/canlin_mbedtls_config.h through MBEDTLS_CONFIG_FILE; the same configuration
# for the host and TARGET=i686 (HTTP transport spec §5). Every library source is
# compiled; what the configuration disables compiles to (almost) nothing.
MBEDTLS_DIR := third_party/mbedtls
MBEDTLS_CPPFLAGS := -I$(MBEDTLS_DIR)/include -Isrc/net \
	-DMBEDTLS_CONFIG_FILE='"canlin_mbedtls_config.h"'
MBEDTLS_SRC := $(sort $(wildcard $(MBEDTLS_DIR)/library/*.c))
MBEDTLS_OBJ := $(patsubst %.c,$(BUILD)/%.o,$(MBEDTLS_SRC))
LIBOBJ += $(MBEDTLS_OBJ)

# Vendored code is not held to our warning set ('override': also with CFLAGS on the
# command line), like mk/lua.mk.
$(MBEDTLS_OBJ): override CFLAGS += -w -Wno-error
$(MBEDTLS_OBJ): override CPPFLAGS += $(MBEDTLS_CPPFLAGS) -I$(MBEDTLS_DIR)/library
$(MBEDTLS_OBJ): src/net/canlin_mbedtls_config.h

mbedtls: $(MBEDTLS_OBJ)
.PHONY: mbedtls
