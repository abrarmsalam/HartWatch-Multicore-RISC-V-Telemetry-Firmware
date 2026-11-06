# Day 1 -- Bring-up, explained from zero

## What you are building today

HartWatch will eventually measure what a multi-core chip is doing without slowing it down. Today you build the
floor everything else stands on: **code that boots on all 5 cores of a simulated RISC-V chip, and gives each
application core a steady 1 kHz clock tick.** That tick becomes the *sampling clock* on Day 2.

```
              QEMU "sifive_u" machine  (a simulated SiFive FU540 chip)
  +-------------------------------------------------------------------------+
  |  hart 0 (E51)     hart 1 (U54)   hart 2 (U54)   hart 3 (U54)   hart 4     |
  |  MONITOR core     <------------ APPLICATION cores ------------------>     |
  |  runs HartWatch   run the workload being measured                          |
  |      |                |              |              |            |        |
  |  ====+================+==============+==============+============+=====   |
  |     CLINT: shared clock (mtime), per-core timers, wake-up pokes           |
  |     UART0 (console)    UART1 (telemetry, Day 3)    RAM    L2-LIM          |
  +-------------------------------------------------------------------------+
```

## Words you will meet

| Word | Meaning |
| --- | --- |
| **hart** | "Hardware thread" = one CPU core, in RISC-V's vocabulary. This chip has 5. |
| **bare metal** | No operating system. Our program is the first and only thing that runs; nothing sets up memory, stacks or `printf` for us. |
| **machine mode** | RISC-V's most privileged mode. Our code runs here, so it may touch every register and device. |
| **CSR** | Control and Status Register: a special per-hart register (`mhartid`, `mcycle`, `mepc`, ...). The performance counters HartWatch samples are CSRs, and **a hart can only read its own** -- the central constraint of the project. |
| **MMIO** | Memory-mapped I/O: a device's registers appear at fixed memory addresses; you control the device with ordinary loads and stores. |
| **CLINT** | The chip's core-local interruptor. It holds `mtime` (one clock shared by all harts, ticking at 1 MHz), `mtimecmp` (a per-hart alarm) and `msip` (a per-hart doorbell). |
| **interrupt / exception** | Both "trap" the CPU into a handler. An interrupt comes from outside the running instruction (timer, doorbell); an exception is caused by it (bad address). |
| **linker script** | A file that says where in memory each part of the program goes. With no OS loader, it decides real addresses. |
| **QEMU** | The program that simulates the chip. We run our compiled firmware inside it. |

## The three steps

### Step 1 -- one core boots and prints

Files: `fw/boot.S`, `fw/linker.ld`, `fw/src/uart.c`, `fw/src/kprintf.c`

1. QEMU copies our ELF file into RAM at `0x80000000` (the address in `linker.ld`) and starts **every hart** there.
2. `boot.S` asks each hart "who are you?" (`mhartid`), gives it a private stack, and installs the trap handler.
3. Hart 0 clears the `.bss` section (C assumes global variables start at zero; nobody else will do it) and calls
   `hart0_main()`.
4. `hart0_main()` writes characters to UART0, a device at `0x10010000`. That is what you see on screen.
5. To end the run, firmware writes GPIO pin 10 (the board's reset line); QEMU is started with `-no-reboot`, so a reset
   request means "exit". Real hardware has no equivalent -- this exists purely so tests can finish.

### Step 2 -- wake the other four cores

Files: `fw/boot.S` (the `secondary_hart` part), `fw/src/main.c`, `fw/src/hart.h`

Harts 1-4 reach `boot.S` at the same moment as hart 0, but they must not run until hart 0 has cleared `.bss`. So they
sleep in a `wfi` loop. Hart 0 then writes `1` to each hart's `msip` register in the CLINT -- a doorbell -- and the
sleeping hart wakes, clears its doorbell and jumps to `hart_main()`.

Each woken hart reads its own `misa` CSR and sets an `online` flag; hart 0 waits for all four and prints what it
found. **Look at the ISA strings:** the E51 is `rv64acimu` and the U54s are `rv64acdfimsu`. The extra `d`, `f`
(floating point) and `s` (supervisor mode) are real hardware differences between the monitor and application core types.

Why `aligned(64)` in `hart.h`? So each hart's data sits on its own cache line. Otherwise cores writing *different*
variables in the *same* line would fight over it ("false sharing") and slow each other down.

### Step 3 -- a 1 kHz tick on every application core

Files: `fw/src/timer.c`, `fw/src/trap.c`, `trap_entry` in `fw/boot.S`

Each U54 sets its `mtimecmp` alarm 1000 microseconds ahead and enables the timer interrupt. When `mtime` reaches the
alarm, the hart is trapped into `trap_entry` (assembly: save registers) then `trap_handler` (C) then `timer_irq()`, which
records the time, sets the next alarm and returns. After 2000 ticks the hart stops its timer and raises a `done` flag.

Two details worth understanding:
- The next alarm is `previous_alarm + period`, not `now + period`. Latency then never accumulates as drift.
- `trap_entry` saves only 16 registers (the *caller-saved* ones), not all 31. The C compiler already guarantees the
  rest are preserved. On Day 2 this exact path becomes the sampling ISR, so every saved register is overhead you will
  later be measuring.

## Run it

```bash
make test
```

Output you should see (these are real results from this build):

```
hart 0: monitor core up, isa=rv64acimu
hart 0: all 5 harts are online
  hart 1  isa=rv64acdfimsu   (and harts 2-4)
 hart  ticks  mean_gap_us  min_gap_us  max_gap_us  late(>2x)
    1   2000         1000         999        1001          0
    ...
ALL TESTS PASSED
```

Each U54 took exactly 2000 interrupts with every gap 999-1001 us. That is only true because of `-icount`
(see below); without it you get millisecond-scale jitter.

## Things to try (20 minutes, worth it)

1. **Count the stores.** `make disasm`, open `build/hartwatch.lst`, find `<trap_entry>`. How many `sd` instructions run
   before `call trap_handler`? (Answer should match the comment in `boot.S`.)
2. **Predict, then break.** In `boot.S`, delete the line `csrw mie, t0` in `secondary_hart`. What should happen to
   the sleeping harts, and what message will hart 0 print? Run `make test` and see if you were right.
3. **Feel the jitter.** Run `make run QEMU_TIMING=` (no `-icount`) three times and compare the `late(>2x)` column
   between runs, then compare with the default.
4. **Change the rate.** Set `SAMPLE_HZ` in `main.c` to `10000`. What period does that give? Does the test still pass?
5. **Walk the boot in GDB.** `make debug`, then `make gdb` in a second terminal. Try `break hart0_main`, `continue`,
   `bt`, `info registers mhartid`, `next`.

## Known limitations (found today)

- **GDB sees only hart 0.** QEMU exposes the four U54s separately and GDB cannot attach to them in this setup
  (details in `ENGINEERING_LOG.md`). To inspect a U54 use the QEMU monitor instead. In one terminal:
  `make debug QEMU_EXTRA="-monitor unix:/tmp/mon.sock,server,nowait"`; in another: `nc -U /tmp/mon.sock`, then type
  `cpu 2` and `info registers` to see hart 2's registers (`mhartid`, `pc`, `mstatus`, ...).
- **QEMU timing is only deterministic with `-icount`.** In that mode time is *virtual*: 1 instruction = 1 ns. That makes
  runs reproducible, but it also means timing results measure instruction counts, not real hardware behaviour. This will
  matter on Day 2 when we look at what `mcycle` and `minstret` report.

## Tomorrow (Day 2)

Add an `hpm_backend` layer that reads `mcycle`, `minstret`, three `mhpmcounter`s and `mepc`; turn the timer ISR into the
sampling ISR; and write host-compiled unit tests. The first job: check what the counters actually do under `-icount`
before building anything on top of them.
