# Vendored cmp (MessagePack, third_party/cmp, read-only), canlin#67. No configuration:
# no defines, no CMP_CONFIG_FILE. No consumer in src/ yet; a Lua binding is separate
# canlin-worker work.
CMP_DIR := third_party/cmp
CMP_CPPFLAGS := -I$(CMP_DIR)
CMP_SRC := $(CMP_DIR)/cmp.c
CMP_OBJ := $(patsubst %.c,$(BUILD)/%.o,$(CMP_SRC))
LIBOBJ += $(CMP_OBJ)

# Vendored code is not held to our warning set ('override': also with CFLAGS on the
# command line), like mk/mbedtls.mk.
$(CMP_OBJ): override CFLAGS += -w -Wno-error
$(CMP_OBJ): override CPPFLAGS += $(CMP_CPPFLAGS)

cmp: $(CMP_OBJ)
.PHONY: cmp
