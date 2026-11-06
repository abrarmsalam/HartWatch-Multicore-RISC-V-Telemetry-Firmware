#!/usr/bin/env bash
# check_env.sh -- verify that everything HartWatch needs is installed.
# Usage: scripts/check_env.sh            (or: CROSS=riscv64-elf- scripts/check_env.sh)
CROSS="${CROSS:-riscv64-unknown-elf-}"
fail=0

need() {   # need <label> <command> <how to install>
    if command -v "$2" >/dev/null 2>&1; then
        printf "  ok       %-16s %s\n" "$1" "$("$2" --version 2>&1 | head -1)"
    else
        printf "  MISSING  %-16s -> %s\n" "$1" "$3"
        fail=1
    fi
}

echo "HartWatch environment check"
need "make"        make                    "sudo apt install make"
need "RISC-V gcc"  "${CROSS}gcc"           "sudo apt install gcc-riscv64-unknown-elf   (or set CROSS=...)"
need "QEMU"        qemu-system-riscv64     "sudo apt install qemu-system-misc"
need "GDB"         gdb-multiarch           "sudo apt install gdb-multiarch   (or use ${CROSS}gdb)"
need "python3"     python3                 "sudo apt install python3"
need "git"         git                     "sudo apt install git"
need "dtc (optional)" dtc                  "sudo apt install device-tree-compiler   (only for 'make dtb')"

if command -v qemu-system-riscv64 >/dev/null 2>&1; then
    if qemu-system-riscv64 -machine help | grep -q "^sifive_u "; then
        echo "  ok       sifive_u machine is available in this QEMU"
    else
        echo "  MISSING  this QEMU build has no 'sifive_u' machine"
        fail=1
    fi
fi

echo
if [ $fail -eq 0 ]; then echo "All required tools found. Next: make test"; else echo "Some tools are missing -- see SETUP.md"; fi
exit $fail
