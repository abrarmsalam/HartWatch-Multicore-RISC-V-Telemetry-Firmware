#!/usr/bin/env bash
# wire_rates.sh -- measure bytes on UART1 at several sample rates, in both timing modes.
# Run inside the toolchain container. Bytes are counted from the capture file. Rates are per
# VIRTUAL second under -icount and per host second without it. QEMU's UART has no baud limit.
run() {     # run <label> <timing> <FW_DEFS> <attempts>
    dir=build/wr; rm -rf $dir
    make test-fw BUILD=$dir QEMU_TIMING="$2" FW_DEFS="$3" CHECK_FLAGS="--attempts $4" >$dir.out 2>&1
    rc=$?
    echo "=== $1  [rc=$rc]"
    grep -E '^ +[1-4] +[0-9]+ +[0-9]+ +[0-9]+ +[0-9]+ +[0-9.]+ |^ all |DECODER|PROBLEM|PANIC|FAIL' $dir.out | head -12
    grep -E '^ +[1-4] +2?[0-9]+ +[0-9]+ +[0-9]+ +[0-9]+ +[0-9]+$' $dir.out | awk '{printf "   tick gaps h%s %s..%s late %s\n",$1,$4,$5,$6}'
}
IC="-icount shift=0,sleep=off"
run "100 Hz, -icount"    "$IC" "-DSAMPLE_HZ=100 -DTEST_TICKS=200" 200
run "1 kHz, -icount"     "$IC" "" 2000
run "20 kHz, -icount"    "$IC" "-DSAMPLE_HZ=20000 -DTEST_TICKS=2000" 2000
run "100 Hz, no icount"  ""    "-DSAMPLE_HZ=100 -DTEST_TICKS=200" 200
run "1 kHz, no icount"   ""    "" 2000
run "20 kHz, no icount"  ""    "-DSAMPLE_HZ=20000 -DTEST_TICKS=2000" 2000
run "1 kHz, -icount, 4 samples/frame"  "$IC" "-DFRAME_SAMPLES=4" 2000
run "1 kHz, -icount, 64 samples/frame" "$IC" "-DFRAME_SAMPLES=64" 2000
