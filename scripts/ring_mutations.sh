#!/usr/bin/env bash
# ring_mutations.sh -- plant ordering bugs in a throwaway copy of ring.h and see which test
# notices. Run inside the toolchain container. A test that cannot fail proves nothing.
set -u
tsan() {    # tsan <label> <python expression that edits s>
    rm -rf /tmp/rm; mkdir /tmp/rm; cp -r fw /tmp/rm/
    python3 - "$2" <<'PY'
import sys
p='/tmp/rm/fw/src/ring.h'; s=open(p).read(); o=s
exec(sys.argv[1])
assert s!=o, 'mutation changed nothing'
open(p,'w').write(s)
PY
    gcc -std=gnu11 -O1 -g -fsanitize=thread -pthread -I/tmp/rm/fw/src /tmp/rm/fw/tests/test_ring_tsan.c \
        /tmp/rm/fw/src/sample.c -o /tmp/rm/t 2>/dev/null || { echo "$1: BUILD FAILED"; return; }
    /tmp/rm/t 2000000 64 >/tmp/rm/out 2>&1; rc=$?
    races=$(grep -c 'WARNING: ThreadSanitizer: data race' /tmp/rm/out)
    printf '%-40s rc=%-3s tsan_races=%-6s %s\n' "$1" "$rc" "$races" "$(grep '^attempts' /tmp/rm/out)"
}
tsan "control (no mutation)"              "s=s+' '"
tsan "ring_commit: release -> relaxed"    "s=s.replace('__atomic_store_n(&r->head, head + 1, __ATOMIC_RELEASE)','__atomic_store_n(&r->head, head + 1, __ATOMIC_RELAXED)')"
tsan "ring_peek: acquire -> relaxed"      "s=s.replace('uint64_t head = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);\n\n    return head == tail','uint64_t head = __atomic_load_n(&r->head, __ATOMIC_RELAXED);\n\n    return head == tail')"
tsan "ring_consume: release -> relaxed"   "s=s.replace('__atomic_store_n(&r->tail, tail + 1, __ATOMIC_RELEASE)','__atomic_store_n(&r->tail, tail + 1, __ATOMIC_RELAXED)')"
tsan "ring_reserve: acquire -> relaxed"   "s=s.replace('uint64_t tail = __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);','uint64_t tail = __atomic_load_n(&r->tail, __ATOMIC_RELAXED);')"
tsan "full check off by one"              "s=s.replace('if (head - tail > r->mask) {','if (head - tail > r->mask + 1) {')"
