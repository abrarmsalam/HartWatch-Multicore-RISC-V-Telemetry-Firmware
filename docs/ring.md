# The sample ring

Written before the ring code. The design has to stand on this argument, because tests cannot prove it (see the end).

## What it does

Each U54 has one ring in L2-LIM. The U54's timer ISR puts samples in, and the E51 takes them out. The two sides run at
the same time on different cores and share no lock. A ring is a fixed array used in a circle: when the writer reaches
the end, it carries on from the start, as long as the reader has finished with those slots.

## Why this design

- **No lock.** A lock could make the ISR wait, and the core being measured must never wait for telemetry.
- **One producer, one consumer.** With exactly one writer per index, no atomic read-modify-write is needed. Plain
  loads and stores with the right ordering are enough.
- **Drop when full.** If the E51 falls behind, the U54 throws the new sample away and counts it. Losing telemetry is
  acceptable. Slowing the workload, or overwriting a record the E51 may be reading, is not.
- **Power-of-two size.** `index & (capacity - 1)` replaces a slow division, and it stays correct as the counters grow.

## Layout

```
cache line 0   head, dropped      written only by the producer
cache line 1   tail               written only by the consumer
cache line 2   mask               written once at init, then read-only
then           slots[capacity]    64 bytes each, one cache line per slot
```

`head` and `tail` sit on separate 64-byte lines so a write to one does not take the other's line away from the other
core (false sharing).

## Who writes what

| Field | Written by | Read by |
| --- | --- | --- |
| `head` | Producer only | Both |
| `dropped` | Producer only | Both |
| `tail` | Consumer only | Both |
| `slots[i]` | Producer, only while `i` is free | Consumer, only while `i` is published |
| `mask` | Hart 0 at init, before U54s wake | Both |

**There really is only one producer.** Only one hart's ISR touches its ring. A trap clears `mstatus.MIE`, and our handler
never sets it again, so an ISR cannot interrupt itself. The workload never touches the ring. **There is one consumer**:
only the E51 drains.

## Invariants

`head` and `tail` are 64-bit counters that only ever go up. They are never wrapped to the array size; only the array
index is. At 20 kHz they would take far longer than any run to overflow.

1. `tail <= head`: the consumer never passes the producer.
2. `head - tail <= capacity`: the producer never laps the consumer.
3. Slots in `[tail, head)` are **published**: fully written, owned by the consumer.
4. All other slots are **free**: owned by the producer.
5. Empty means `head == tail`. Full means `head - tail == capacity`. These can't be confused, because the counters are not
   wrapped. That is why every slot is usable, with no wasted slot.
6. Every sample attempt takes a `seq` number. At rest, `attempts == head + dropped`.

## The acquire/release argument

A CPU and a compiler may both reorder memory operations, as long as a single thread cannot tell. RISC-V's memory
model is weak: another core can see one core's writes in a different order from the one the program wrote them in.
Without care, the E51 could see `head` go up before the slot's contents arrive, and read a half-written record.

**Publishing a sample (producer to consumer).**

1. The producer load-acquires `tail` and sees there is room.
2. It writes all 64 bytes of the slot with ordinary stores.
3. It store-releases `head = head + 1`. Release means no earlier read or write may be seen after this store.
4. The consumer load-acquires `head`. Acquire means no later read may happen before this load.
5. So if the consumer's load sees the new `head`, the release/acquire pair guarantees it also sees everything written
   before that store. The slot is complete.

**Returning a slot (consumer to producer).** This direction is easy to forget, and it matters just as much.

1. The consumer reads the slot.
2. It store-releases `tail = tail + 1`, so its reads of the slot complete before the store can be seen.
3. The producer load-acquires `tail`. If it sees the new value, the consumer has finished, and its writes to that slot
   come after those reads. Without this, a write could overtake the consumer's read and corrupt a record in flight.

**What needs no ordering.** Each side reads its own index with a relaxed load, since it is the only writer. `dropped` is
relaxed too: it is a statistic and guards no data, so a frame may report a slightly old value. It only ever grows, so
the final value is exact once the producer stops.

**Tearing.** `head` and `tail` are naturally aligned 64-bit values, and each is accessed by a single load or store, which
RV64 performs as one access. A reader never sees half of a new value.

## What could go wrong

- **Forgetting the second direction.** Producer overwrites a slot being read. Rare, and only under load.
- **Wrong order in the producer.** Publishing `head` before filling the slot.
- **Two producers.** Anything else that calls `ring_reserve` breaks invariant 2. Nothing checks this at runtime.
- **Starting without init.** L2-LIM is not cleared at boot. Hart 0 must initialise every ring before waking the U54s.
- **Caches on real hardware.** This argument assumes coherent memory. QEMU has no caches. Whether L2-LIM is coherent
  and cached in the way this needs on the FU540 must be checked against the manual. Not verified.

## The compiled barriers

Checked by reading the disassembly (GCC 13.2.0, `-O2`). This shows the barriers exist. It does not prove they are enough:
that is the argument above.

| Source | RISC-V code in the firmware |
| --- | --- |
| Load-acquire (`tail`, `head`) | `ld`, then a full `fence` |
| Store-release of `head` in `sampler_tick` | `fence iorw,ow`, then `amoswap.d zero, ...` |

`sampler_tick` and `send_samples` were read. GCC used an atomic swap, not a plain store, for that release: this document
says "store", and it is at least as strong.

## What the tests can and cannot show

Raw results: `docs/data/ring_mutations.txt`, `docs/data/stress_mutations.txt`, `docs/data/stress_*.txt`.

| Planted bug | ThreadSanitizer (host) | RISC-V stress under QEMU, both regimes |
| --- | --- | --- |
| None (control) | clean | passed |
| Commit before filling the slot | not tried | **caught** (253 bad records near empty, 1 when full) |
| `ring_commit` release to relaxed | **caught** | **not caught** |
| `ring_peek` acquire to relaxed | **caught** | **not caught** |
| `ring_consume` release to relaxed | **caught** | **not caught** |
| `ring_reserve` acquire to relaxed | **caught** | **not caught** |
| Full check off by one | **caught** (also a unit test) | not tried |

**ThreadSanitizer shows** that the C code has no unordered access pair under the C11 model, on the paths the run reached.
It does not look at RISC-V machine code at all.

**The RISC-V stress test shows** that the logic works: 75,000,915 records verified with no bad record, `seq` gaps equal to
the drop counter, across 32-bit `seq` wraps, on actual firmware with real host parallelism. **It cannot show that the
barriers are sufficient.** Removing any one of them still passed every run. QEMU did not produce the reordering a RISC-V
core is allowed to, so a pass says nothing about ordering on silicon.

**Neither test proves correctness.** The ordering claim rests on the written argument above, checked against the
disassembly. Running this on real RISC-V hardware, or checking it with a tool that models the RISC-V memory model, has not
been done.
