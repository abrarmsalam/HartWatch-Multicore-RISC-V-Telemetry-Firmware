/*
 * linker.ld -- tells the linker WHERE in memory each part of the program lives.
 *
 * There is no operating system to load us. QEMU reads our ELF file, copies each
 * section to the address written here, and starts every hart at the first
 * instruction of the program. So the address in this file IS the address in RAM.
 *
 * On the sifive_u machine, DRAM starts at 0x80000000. (QEMU is started with
 * `-m 256M`, matching LENGTH below.)
 */
OUTPUT_ARCH(riscv)
ENTRY(_start)

MEMORY
{
    RAM (rwx) : ORIGIN = 0x80000000, LENGTH = 256M
}

SECTIONS
{
    /* .text.init (boot.S) must come FIRST: _start has to sit at 0x80000000. */
    .text : ALIGN(4)
    {
        KEEP(*(.text.init))
        *(.text .text.*)
    } > RAM

    .rodata : ALIGN(8)
    {
        *(.rodata .rodata.* .srodata .srodata.*)
    } > RAM

    .data : ALIGN(8)
    {
        *(.data .data.* .sdata .sdata.*)
    } > RAM

    /* Zero-initialised globals. Hart 0 clears [_bss_start, _bss_end) at boot. */
    .bss (NOLOAD) : ALIGN(8)
    {
        _bss_start = .;
        *(.sbss .sbss.* .bss .bss.* COMMON)
        . = ALIGN(8);
        _bss_end = .;
    } > RAM

    /* Per-hart stacks. Deliberately OUTSIDE .bss: hart 0 is running on its stack
     * while it zeroes .bss, so the stacks must not be inside the cleared range. */
    .stacks (NOLOAD) : ALIGN(16)
    {
        *(.stacks)
    } > RAM

    /DISCARD/ : { *(.eh_frame*) *(.comment) }
}
