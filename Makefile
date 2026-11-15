# CanLin top-level build. Subsystems add themselves via mk/<name>.mk:
#   - their objects in a variable of their own (X_OBJ), appended to LIBOBJ;
#   - their test binaries to TESTS (each must exit 0 on success);
#   - the link line of their test binary, naming the objects it links.
# LIBOBJ is the runtime object set: the objects of every subsystem. A test binary
# without a link line of its own links LIBOBJ (the rule below), so the contract
# above is enough to get a linked binary. The subsystems in mk/ name their link
# objects and keep their own rule: the link then proves the "Needs" line of the
# mk file, because a test whose subsystem needs an object it did not name fails
# to link. Name LIBOBJ or single objects on a link line, never both: a duplicate
# object on a link line is an error. The runtime binary that consumes LIBOBJ as
# a whole does not exist yet.
TARGET ?= host
CC ?= cc
CFLAGS ?= -std=c99 -Wall -Wextra -Werror -O2 -g
CPPFLAGS += -Iinclude
# bionic has pthread in libc; the NDK ships no libpthread (android-armv7 empties this).
PTHREAD_LIBS := -lpthread

# The gate the suites run under (k143). RUNNER is the command in front of a
# suite binary: on the host it is empty and the binary runs directly, on i686 it
# is the user-mode gate (qemu-i386 -cpu pentium2). The host CPU has SSE and the
# target must not use it, so an i686 binary run on the host proves nothing.
# GATE names the gate for the result: the user-mode gate and the canlin-os
# full-system gate are two different gates and are never reported as one "QEMU"
# result (system design §2).
RUNNER ?=
GATE ?= host
# The timeout of one suite. It follows the target: the emulation is far slower
# than the host (t/ui/test_ui needs ~253 s under qemu-i386), so the host's 120 s
# would kill a run that is fine. A value on the command line wins over both.
TEST_TIMEOUT ?= 120

ifeq ($(TARGET),i686)
CC := i686-linux-musl-gcc
CFLAGS += -march=i686 -mno-sse -static
# The user-mode gate (system design §2): a Pentium II CPU model, which has no
# SSE. An instruction the target may not use dies here with signal 4.
RUNNER := qemu-i386 -cpu pentium2
GATE := i686 user-mode
# The emulation runs an order of magnitude slower than the host.
TEST_TIMEOUT := 600
endif
# Android armeabi-v7a, API 19, NDK r25c clang with its default flags (Thumb-2, softfp,
# NEON, PIE). Dynamic against the device's bionic. Run with 'make TARGET=android-armv7
# test-device' (tools/android-test.sh; ANDROID_SERIAL selects the device). The
# NativeActivity APK is 'make TARGET=android-armv7 apk' (mk/android.mk).
ifeq ($(TARGET),android-armv7)
NDK_HOME ?= $(HOME)/android-sdk/ndk/25.2.9519653
ANDROID_NDK ?= $(NDK_HOME)
ANDROID_API ?= 19
CC := $(ANDROID_NDK)/toolchains/llvm/prebuilt/linux-x86_64/bin/armv7a-linux-androideabi$(ANDROID_API)-clang
# Position-independent code everywhere: the same objects go into the test executables
# (PIE) and into libcanlin.so for the APK (mk/android.mk).
CFLAGS += -fPIC
PTHREAD_LIBS :=
endif
BUILD := build/$(TARGET)

# Sanitizer build (k87). 'make SAN=asan test' rebuilds the suites with the address
# and undefined-behaviour sanitizers and runs them; a report fails the run.
#   SAN=asan    -fsanitize=address,undefined -fno-sanitize-recover=undefined
#   SAN=ubsan   -fsanitize=undefined -fno-sanitize-recover=undefined
# Without SAN the build is exactly what it was: same flags, same objects, same run.
# The sanitized objects need a tree of their own -- the object names do not encode
# the flags, so the same file in the same place would be a stale host object.
# 'override' so a BUILD given on the command line is the base of that tree instead
# of being dropped, and so a CFLAGS given on the command line cannot drop the
# sanitizer flags (a suite built without them would pass without checking anything).
SAN ?=
ifneq ($(SAN),)
ifneq ($(TARGET),host)
$(error SAN=$(SAN) needs the host toolchain; TARGET=$(TARGET) ships no sanitizer runtime)
endif
ifeq ($(SAN),asan)
SAN_CFLAGS := -fsanitize=address,undefined -fno-sanitize-recover=undefined \
	-fno-omit-frame-pointer
else ifeq ($(SAN),ubsan)
SAN_CFLAGS := -fsanitize=undefined -fno-sanitize-recover=undefined \
	-fno-omit-frame-pointer
else
$(error unknown SAN='$(SAN)'; use asan or ubsan)
endif
override BUILD := $(BUILD)/san-$(SAN)
override CFLAGS += $(SAN_CFLAGS)
# The sanitizer runtime runs on the host, so this gate is the host plus the
# sanitizer -- not the i686 gate, and never one unnamed "QEMU" result.
GATE := host + sanitizer ($(SAN))
endif

# The one line that names the gate of a run: the gate, the runner and the
# timeout per suite. The result repeats the gate, so a report never leaves open
# which gate ran.
RUNNER_CMD = $(if $(strip $(RUNNER)),$(strip $(RUNNER)),direct)
GATE_LINE = gate: $(GATE) | runner: $(RUNNER_CMD) | timeout: $(TEST_TIMEOUT)s per suite

LIBOBJ :=
TESTS :=

all: tests

include $(sort $(wildcard mk/*.mk))

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

# A test binary without a link line of its own links the runtime objects (LIBOBJ):
# a new subsystem adds its objects to LIBOBJ and its binary to TESTS and gets a
# linked binary. The subsystems in mk/ name their link objects and keep their own
# rule; this one is only used where there is none.
.SECONDEXPANSION:
$(BUILD)/t/%: $(BUILD)/t/%.o $$(LIBOBJ)
	@echo "note: $@ links the runtime objects (LIBOBJ)"
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ -lm $(PTHREAD_LIBS)

tests: $(TESTS)

# The sanitizer run (k87): the same suites, run under the sanitizer runtime. A report
# is fatal -- the sanitizer ends the process (exitcode=1, halt_on_error=1) and the
# output is scanned for a report line as well, so a report cannot pass unnoticed.
# The binaries are checked for the sanitizer runtime first: an uninstrumented suite
# would pass without having checked anything.
SAN_ENV := ASAN_OPTIONS=detect_leaks=1:abort_on_error=0:exitcode=1:halt_on_error=1 \
	UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1
SAN_NM := $(shell $(CC) -print-prog-name=nm 2>/dev/null)
SAN_REPORT := ERROR: (Address|Leak)Sanitizer|SUMMARY: (Address|Leak|UndefinedBehavior)Sanitizer|runtime error:

ifeq ($(SAN),)
# Each test runs under timeout(1) when available, so a hang fails instead of blocking.
# The suites run under the runner of the gate: on i686 that is the user-mode gate
# (qemu-i386 -cpu pentium2), so the target binary never runs on the host CPU, which
# has SSE. The gate is one command -- 'make TARGET=i686 test' -- and the result
# names the gate that ran.
test: $(TESTS)
	@echo "$(GATE_LINE)"; \
	if [ -n "$(RUNNER)" ] && ! command -v "$(firstword $(RUNNER))" >/dev/null 2>&1; then \
	  echo "error: the runner '$(firstword $(RUNNER))' is not on PATH; the gate '$(GATE)' needs it" >&2; exit 1; fi; \
	to=; if command -v timeout >/dev/null 2>&1; then to="timeout $(TEST_TIMEOUT)"; fi; \
	for t in $(TESTS); do echo "== $$t"; \
	  if [ -n "$(RUNNER)" ]; then echo "   run: $$to $(RUNNER) ./$$t"; fi; \
	  rc=0; $$to $(RUNNER) ./$$t || rc=$$?; \
	  if [ -n "$$to" ] && [ $$rc -eq 124 ]; then \
	    echo "TIMEOUT: $$t (exceeded $(TEST_TIMEOUT)s, gate: $(GATE))"; exit 1; fi; \
	  if [ $$rc -ne 0 ]; then echo "FAIL: $$t (exit $$rc, gate: $(GATE))"; exit 1; fi; \
	done; echo "ALL TESTS PASSED (gate: $(GATE))"
else
# With a sanitizer build the run goes through the sanitizer runner below: a report
# has to fail the build, so 'make SAN=asan test' and 'make SAN=asan test-san' are
# the same run.
test: test-san
endif

ifeq ($(SAN),)
test-san:
	@echo "error: test-san needs SAN=asan or SAN=ubsan" >&2; exit 1
else
test-san: $(TESTS)
	@if [ -z "$(SAN_NM)" ] || ! command -v "$(SAN_NM)" >/dev/null 2>&1; then \
	  echo "error: nm not found; cannot prove the suites are instrumented" >&2; exit 1; fi; \
	echo "$(GATE_LINE)"; \
	echo "sanitizer build: $(BUILD) (SAN=$(SAN))"; \
	if [ -n "$(RUNNER)" ] && ! command -v "$(firstword $(RUNNER))" >/dev/null 2>&1; then \
	  echo "error: the runner '$(firstword $(RUNNER))' is not on PATH; the gate '$(GATE)' needs it" >&2; exit 1; fi; \
	to=; if command -v timeout >/dev/null 2>&1; then to="timeout $(TEST_TIMEOUT)"; fi; \
	for t in $(TESTS); do echo "== $$t"; \
	  if [ -n "$(RUNNER)" ]; then echo "   run: $$to $(RUNNER) ./$$t"; fi; \
	  if ! { $(SAN_NM) -D "$$t" 2>/dev/null || $(SAN_NM) "$$t" 2>/dev/null; } \
	     | grep -qE '__asan_init|__ubsan_'; then \
	    echo "FAIL: $$t is not instrumented (no sanitizer runtime in the binary)"; exit 1; fi; \
	  log=$$(mktemp); rc=0; $(SAN_ENV) $$to $(RUNNER) ./$$t >"$$log" 2>&1 || rc=$$?; cat "$$log"; \
	  if grep -qE '$(SAN_REPORT)' "$$log"; then \
	    echo "FAIL: $$t (sanitizer report)"; rm -f "$$log"; exit 1; fi; \
	  rm -f "$$log"; \
	  if [ -n "$$to" ] && [ $$rc -eq 124 ]; then \
	    echo "TIMEOUT: $$t (exceeded $(TEST_TIMEOUT)s, gate: $(GATE))"; exit 1; fi; \
	  if [ $$rc -ne 0 ]; then echo "FAIL: $$t (exit $$rc, gate: $(GATE))"; exit 1; fi; \
	done; echo "ALL SANITIZER TESTS PASSED (gate: $(GATE))"
endif

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

.PHONY: all tests test test-san test-device clean
