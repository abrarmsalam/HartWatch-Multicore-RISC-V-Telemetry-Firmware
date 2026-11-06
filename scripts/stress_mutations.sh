#!/usr/bin/env bash
# stress_mutations.sh -- plant ring bugs and see whether `make stress` notices, in both
# regimes. Run inside the toolchain container. A pass here means the bug was NOT detected.
set -u
one() {     # one <label> <file> <python edit> <FW_DEFS>
    rm -rf /tmp/sm; mkdir /tmp/sm; cp -r Makefile fw scripts /tmp/sm/
    python3 - "$2" "$3" <<'PY'
import sys
p='/tmp/sm/'+sys.argv[1]; s=open(p).read(); o=s
exec(sys.argv[2])
assert s!=o or sys.argv[2]=='pass', 'mutation changed nothing'
open(p,'w').write(s)
PY
    (cd /tmp/sm && make stress FW_DEFS="$4" STRESS_TIMEOUT=200 >out 2>&1); rc=$?
    printf '%-34s %-10s rc=%s  %s\n' "$1" "$5" "$rc" "$(grep -E 'records verified' /tmp/sm/out)"
    grep -E '^ +[1-4] ' /tmp/sm/out | awk '{b+=$7; k+=$8} END {printf "      bad=%d backwards=%d\n", b, k}'
}
both() {
    one "$1" "$2" "$3" "-DSTRESS_TARGET=5000000UL" "full"
    one "$1" "$2" "$3" "-DSTRESS_PACE=60000 -DSTRESS_TARGET=2000000UL" "near-empty"
}
R=fw/src/ring.h
both "control (no mutation)" $R "pass"
both "commit before fill (logic bug)" fw/probe/ring_stress.c "s=s.replace('''            fill(s, (uint32_t)i, (uint8_t)hartid);
            ring_commit(ring);''','''            ring_commit(ring);
            fill(s, (uint32_t)i, (uint8_t)hartid);''')"
both "ring_commit: release -> relaxed" $R "s=s.replace('__atomic_store_n(&r->head, head + 1, __ATOMIC_RELEASE)','__atomic_store_n(&r->head, head + 1, __ATOMIC_RELAXED)')"
both "ring_peek: acquire -> relaxed" $R "s=s.replace('uint64_t head = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);\n\n    return head == tail','uint64_t head = __atomic_load_n(&r->head, __ATOMIC_RELAXED);\n\n    return head == tail')"
both "ring_consume: release -> relaxed" $R "s=s.replace('__atomic_store_n(&r->tail, tail + 1, __ATOMIC_RELEASE)','__atomic_store_n(&r->tail, tail + 1, __ATOMIC_RELAXED)')"
both "ring_reserve: acquire -> relaxed" $R "s=s.replace('uint64_t tail = __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);','uint64_t tail = __atomic_load_n(&r->tail, __ATOMIC_RELAXED);')"
