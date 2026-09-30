# CanLin top-level build. Subsystems add themselves via mk/<name>.mk:
#   append objects to LIBOBJ, test binaries to TESTS (each must exit 0 on success).
TARGET ?= host
TEST_TIMEOUT ?= 120
CC ?= cc
CFLAGS ?= -std=c99 -Wall -Wextra -Werror -O2 -g
CPPFLAGS += -Iinclude
# bionic has pthread in libc; the NDK ships no libpthread (android-armv7 empties this).
PTHREAD_LIBS := -lpthread
ifeq ($(TARGET),i686)
CC := i686-linux-musl-gcc
CFLAGS += -march=i686 -mno-sse -static
endif
# Android armeabi-v7a, API 19, NDK r25c clang with its default flags (Thumb-2, softfp,
# NEON, PIE). Dynamic against the device's bionic. Run with 'make TARGET=android-armv7
# test-device' (tools/android-test.sh; ANDROID_SERIAL selects the device).
ifeq ($(TARGET),android-armv7)
NDK_HOME ?= $(HOME)/android-sdk/ndk/25.2.9519653
ANDROID_NDK ?= $(NDK_HOME)
ANDROID_API ?= 19
CC := $(ANDROID_NDK)/toolchains/llvm/prebuilt/linux-x86_64/bin/armv7a-linux-androideabi$(ANDROID_API)-clang
PTHREAD_LIBS :=
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

# Push the test binaries to an attached Android device and run them there (same output
# shape; every test runs, the target fails if any failed). Only for TARGET=android-armv7.
ADB ?= adb
DEVICE_DIR ?= /data/local/tmp/canlin
ifeq ($(TARGET),android-armv7)
test-device: $(TESTS)
	@ADB='$(ADB)' DEVICE_DIR='$(DEVICE_DIR)' TEST_TIMEOUT='$(TEST_TIMEOUT)' \
	  sh tools/android-test.sh $(TESTS)
else
test-device:
	@echo "error: test-device needs TARGET=android-armv7" >&2; exit 1
endif

clean:
	rm -rf build

.PHONY: all tests test test-device clean
