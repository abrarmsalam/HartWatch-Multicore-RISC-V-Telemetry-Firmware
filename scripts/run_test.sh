#!/usr/bin/env bash
# run_test.sh <elf> "<full qemu command>" [build dir]
# Boots the firmware with a hard timeout, saves the console to <build dir>/last_run.log,
# and fails if QEMU hangs, the firmware panics, or the expected "ALL TESTS PASSED" line is missing.
set -u
elf="$1"; cmd="$2"
dir="${3:-build}"
mkdir -p "$dir"
log="$dir/last_run.log"

timeout --signal=KILL "${TEST_TIMEOUT:-120}" bash -c "$cmd" > "$log" 2>&1
rc=$?

cat "$log"
echo "-----------------------------------------------------------"
if [ $rc -eq 137 ]; then echo "FAIL: timed out (QEMU never exited -- firmware hung?)"; exit 1; fi
if grep -q "PANIC" "$log"; then echo "FAIL: firmware panicked"; exit 1; fi
if ! grep -q "${EXPECT:-ALL TESTS PASSED}" "$log"; then echo "FAIL: success marker not found"; exit 1; fi
echo "PASS"
