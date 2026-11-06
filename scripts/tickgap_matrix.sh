#!/usr/bin/env bash
# tickgap_matrix.sh -- run the tick-gap experiment, changing one factor at a time.
# Run inside the toolchain container. Results go to stdout; see docs/ENGINEERING_LOG.md.
run() {     # run <label> <GAP flags>
    out=$(make -s tickgap GAP="$2" 2>&1 | grep '^h[0-9] gap')
    printf '%-34s %s\n' "$1" "${out:-FAILED}"
}
echo "--- number of busy harts (alu everywhere, all 4 tick)"
run "0 busy"              "-DGAP_BUSY=0x00 -DGAP_WL=0"
run "1 busy (h1)"         "-DGAP_BUSY=0x02 -DGAP_WL=0"
run "1 busy (h4)"         "-DGAP_BUSY=0x10 -DGAP_WL=0"
run "2 busy (h1,h2)"      "-DGAP_BUSY=0x06 -DGAP_WL=0"
run "3 busy (h1-h3)"      "-DGAP_BUSY=0x0E -DGAP_WL=0"
run "4 busy"              "-DGAP_BUSY=0x1E -DGAP_WL=0"
echo "--- workload (4 busy)"
run "all branch"          "-DGAP_WL=1"
run "all load"            "-DGAP_WL=2"
run "all store"           "-DGAP_WL=3"
run "mixed"               "-DGAP_WL=4"
run "mixed, rotated"      "-DGAP_WL=5"
echo "--- ISR length (4 busy, mixed)"
run "no sampling"         "-DGAP_SAMPLE=0"
run "sampling"            "-DGAP_SAMPLE=1"
run "sampling + pad 1000" "-DGAP_PAD=1000"
run "sampling + pad 10000" "-DGAP_PAD=10000"
echo "--- E51 tick (4 busy, mixed)"
run "E51 100 Hz tick"     "-DGAP_E51_TICK=1"
run "E51 asleep"          "-DGAP_E51_TICK=0"
echo "--- only one hart ticks (others busy, no timer)"
run "h1 ticks, 4 busy"    "-DGAP_TICKS=0x02 -DGAP_WL=0"
run "h4 ticks, 4 busy"    "-DGAP_TICKS=0x10 -DGAP_WL=0"
echo "--- period (4 busy, alu)"
run "period 500 us"       "-DGAP_PERIOD=500 -DGAP_WL=0"
run "period 2000 us"      "-DGAP_PERIOD=2000 -DGAP_WL=0"
run "period 4000 us"      "-DGAP_PERIOD=4000 -DGAP_WL=0"
echo "--- CPU count with the same 2 busy, ticking harts (h1,h2, alu): the budget divides by this"
runsmp() {
    out=$(make -s tickgap SMP="$2" GAP="-DGAP_BUSY=0x06 -DGAP_TICKS=0x06 -DGAP_WL=0" 2>&1 | grep '^h[0-9] gap')
    printf '%-34s %s\n' "$1" "${out:-FAILED}"
}
runsmp "-smp 3" 3
runsmp "-smp 4" 4
runsmp "-smp 5" 5
