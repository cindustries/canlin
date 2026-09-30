#!/bin/sh
# Run test binaries on an attached Android device over adb: make TARGET=android-armv7
# test-device. Usage: android-test.sh TEST...  (paths relative to the checkout).
#
# Each binary is pushed to $DEVICE_DIR/<path> and run there with TMPDIR=$DEVICE_DIR/tmp
# (the tests write scratch files under $TMPDIR, default /tmp, which Android lacks).
# Android 4.4's adb shell does not return the remote exit code: the remote command ends
# with "rc=$?" and that line is parsed. The timeout is enforced host-side, like
# 'make test'; a hang is a failure and the remote process is killed. Output keeps the
# 'make test' shape; unlike it every test runs, and the exit is non-zero if any failed.
# ANDROID_SERIAL selects the device (read by adb itself).
set -u
ADB=${ADB:-adb}
DEVICE_DIR=${DEVICE_DIR:-/data/local/tmp/canlin}
TEST_TIMEOUT=${TEST_TIMEOUT:-120}

state=$($ADB get-state 2>/dev/null)
if [ "$state" != device ]; then
    echo "error: no adb device (state '${state:-none}'); set ANDROID_SERIAL if several" >&2
    exit 1
fi
to=
if command -v timeout >/dev/null 2>&1; then to="timeout $TEST_TIMEOUT"; fi

out=$(mktemp) || exit 1
trap 'rm -f "$out" "$out.rc"' EXIT
$ADB shell "rm -r $DEVICE_DIR/tmp 2>/dev/null; mkdir $DEVICE_DIR 2>/dev/null;
    mkdir $DEVICE_DIR/tmp" >/dev/null

failed=
for t in "$@"; do
    if ! $ADB push "$t" "$DEVICE_DIR/$t" >/dev/null 2>&1; then
        echo "error: adb push $t failed" >&2
        exit 1
    fi
done

for t in "$@"; do
    echo "== $t"
    # The test runs in the background so its pid is known for a kill after a timeout.
    { $to $ADB shell "cd $DEVICE_DIR && chmod 755 $t && TMPDIR=$DEVICE_DIR/tmp ./$t &
        echo \$! > $DEVICE_DIR/run.pid; wait \$!; echo rc=\$?"; echo $? > "$out.rc"; } |
        tr -d '\r' > "$out"
    grep -v '^rc=[0-9]*$' "$out"
    hrc=$(cat "$out.rc")
    rc=$(sed -n 's/^rc=\([0-9]*\)$/\1/p' "$out" | tail -n 1)
    if [ -n "$to" ] && [ "$hrc" -eq 124 ]; then
        echo "TIMEOUT: $t (exceeded ${TEST_TIMEOUT}s)"
        $ADB shell "kill -9 \$(cat $DEVICE_DIR/run.pid)" >/dev/null 2>&1
        failed="$failed $t"
    elif [ -z "$rc" ]; then
        echo "FAIL: $t (no exit code from the device, adb exit $hrc)"
        failed="$failed $t"
    elif [ "$rc" -ne 0 ]; then
        echo "FAIL: $t (exit $rc)"
        failed="$failed $t"
    fi
done

if [ -n "$failed" ]; then
    for t in $failed; do echo "FAIL: $t"; done
    exit 1
fi
echo "ALL TESTS PASSED"
