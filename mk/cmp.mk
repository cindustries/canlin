# Vendored cmp (MessagePack, third_party/cmp, read-only), canlin#67. No configuration:
# no defines, no CMP_CONFIG_FILE. No consumer in src/ yet; a Lua binding is separate
# canlin-worker work.
CMP_DIR := third_party/cmp
CMP_CPPFLAGS := -I$(CMP_DIR)
CMP_SRC := $(CMP_DIR)/cmp.c

# cmp is compiled from a patched copy in $(BUILD) (patches/cmp/README);
# third_party/cmp stays byte-identical to the upstream tag. -F0: any context drift
# fails the build instead of applying fuzzily.
CMP_PATCHED := $(BUILD)/patched/cmp
CMP_PATCH := patches/cmp/0001-skip-bytes-error-propagation.patch
CMP_C := $(CMP_PATCHED)/cmp.c
CMP_OBJ := $(CMP_PATCHED)/cmp.o
LIBOBJ += $(CMP_OBJ)

$(CMP_C): $(CMP_SRC) $(CMP_PATCH)
	@mkdir -p $(dir $@)
	@rm -f $@ $@.tmp $@.rej
	patch --batch --forward -F0 -s -r $@.rej -o $@.tmp $(CMP_SRC) < $(CMP_PATCH) \
		|| { echo "error: $(CMP_PATCH) does not apply to $(CMP_SRC)" >&2; exit 1; }
	mv $@.tmp $@

$(CMP_OBJ): $(CMP_C)
	$(CC) $(CMP_CPPFLAGS) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

# Vendored code is not held to our warning set ('override': also with CFLAGS on the
# command line), like mk/mbedtls.mk.
$(CMP_OBJ): override CFLAGS += -w -Wno-error

cmp: $(CMP_OBJ)
.PHONY: cmp
