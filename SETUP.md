# Setup

You need four things: a RISC-V C compiler, QEMU (the simulated chip), GDB (debugger), and `make`.
Everything runs on a normal laptop; there is no hardware to buy.

## Ubuntu 24.04 / Debian (this is what the project was built and tested on)

```bash
sudo apt update
sudo apt install -y make git python3 \
    gcc-riscv64-unknown-elf binutils-riscv64-unknown-elf \
    qemu-system-misc gdb-multiarch device-tree-compiler
```

Tested versions: QEMU 8.2.2, GCC 13.2.0, GDB 15.1.

## Windows

Install **WSL2** with Ubuntu 24.04 (`wsl --install -d Ubuntu-24.04` in PowerShell), open the Ubuntu terminal,
and follow the Ubuntu steps above. Keep the project inside the Linux filesystem (`~/hartwatch`), not under `/mnt/c`.

## macOS

Use the container, so you get the pinned QEMU 8.2.2 and GCC 13.2.0. Homebrew's QEMU is a much newer version, and the
counter results depend on the version. `scripts/run_test.sh` also needs `timeout`, which macOS lacks. Tested on an arm64
Mac with Docker Desktop.

```bash
docker build -f Dockerfile.dev -t hartwatch-dev .                  # once
docker run --rm -v "$PWD":/work hartwatch-dev make test
```

## Older distros

If the compiler complains about an unsupported `-march` string, edit `ARCH` in the `Makefile` and drop the
`_zicsr_zifencei` suffix (`-march=rv64imac`).

## Check it

```bash
cd hartwatch
git init && git add -A && git commit -m "Day 1: bare-metal bring-up"   # your first commit
scripts/check_env.sh      # lists anything missing
make test                 # builds, boots 5 harts in QEMU, prints PASS
```

A good `make test` ends with:

```
ALL TESTS PASSED
-----------------------------------------------------------
PASS
```

## Everyday commands

| Command | What it does |
| --- | --- |
| `make` | Build `build/hartwatch.elf` |
| `make run` | Boot it in QEMU and watch the console |
| `make test` | Run the host unit tests, then boot it with a timeout and print PASS/FAIL |
| `make host-test` | Run only the host unit tests |
| `make probe` | Run the counter probe (add `QEMU_TIMING=` to run without `-icount`) |
| `make debug` then `make gdb` (second terminal) | Boot paused; attach GDB to hart 0 |
| `make disasm` | Write `build/hartwatch.lst`, the machine code interleaved with your C |
| `make dtb` | Dump QEMU's hardware description (addresses of every device) |
| `make clean` | Delete `build/` |

## If something goes wrong

- **`riscv64-unknown-elf-gcc: command not found`** -- install the compiler or pass `CROSS=<prefix>-`.
- **`make test` says "timed out"** -- the firmware hung. Run `make run` and see how far the output gets; the last
  line printed is the clue.
- **Output shows `PANIC`** -- the firmware caught an unexpected trap and printed the faulting address (`pc=`). Look
  it up with `riscv64-unknown-elf-addr2line -e build/hartwatch.elf 0x<pc>`.
- **To quit QEMU manually** -- press `Ctrl-A` then `x`.
