# CanLin top-level build. Subsystems add themselves via mk/<name>.mk:
#   append objects to LIBOBJ, test binaries to TESTS (each must exit 0 on success).
TARGET ?= host
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

test: $(TESTS)
	@set -e; for t in $(TESTS); do echo "== $$t"; ./$$t; done; echo "ALL TESTS PASSED"

clean:
	rm -rf build

.PHONY: all tests test clean
