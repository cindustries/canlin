# CanLin top-level build. Subsystems add themselves via mk/<name>.mk:
#   append objects to LIBOBJ, test binaries to TESTS (each must exit 0 on success).
TARGET ?= host
TEST_TIMEOUT ?= 120
CC ?= cc
CFLAGS ?= -std=c99 -Wall -Wextra -Werror -O2 -g
CPPFLAGS += -Iinclude
ifeq ($(TARGET),i686)
CC := i686-linux-musl-gcc
CFLAGS += -march=i686 -mno-sse -static
endif
BUILD := build/$(TARGET)
LIBOBJ :=
TESTS :=

all: tests

include $(sort $(wildcard mk/*.mk))

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

tests: $(TESTS)

# Each test runs under timeout(1) when available, so a hang fails instead of blocking.
test: $(TESTS)
	@to=; if command -v timeout >/dev/null 2>&1; then to="timeout $(TEST_TIMEOUT)"; fi; \
	for t in $(TESTS); do echo "== $$t"; rc=0; $$to ./$$t || rc=$$?; \
	  if [ -n "$$to" ] && [ $$rc -eq 124 ]; then \
	    echo "TIMEOUT: $$t (exceeded $(TEST_TIMEOUT)s)"; exit 1; fi; \
	  if [ $$rc -ne 0 ]; then echo "FAIL: $$t (exit $$rc)"; exit 1; fi; \
	done; echo "ALL TESTS PASSED"

clean:
	rm -rf build

.PHONY: all tests test clean
