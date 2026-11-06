/*
 * boot.S -- the very first instructions each hart executes.
 *
 * WHAT HAPPENS AT POWER-ON (QEMU sifive_u, `-bios none`):
 *   All five harts start executing at 0x80000000 AT THE SAME TIME, in machine mode,
 *   with interrupts off, and with NO stack and NO C runtime. Nothing has set up
 *   memory for us. This file is "the C runtime": we must make C possible.
 *
 * OUR STRATEGY:
 *   1. Every hart reads its own ID from the mhartid CSR and builds its own stack.
 *   2. Hart 0 (the E51 monitor core) zeroes .bss and runs hart0_main().
 *   3. Harts 1-4 (the U54 app cores) go to sleep with `wfi` ("wait for interrupt")
 *      until hart 0 pokes their CLINT msip register. Then they run hart_main().
 */
#include "src/config.h"

    .section .text.init, "ax", @progbits
    .globl _start
_start:
    csrr    a0, mhartid                 # a0 = which hart am I?  (0..4)

    # --- 1. stack: hart N gets the N-th slice, counting down from _stack_top ----
    la      sp, _stack_top
    li      t0, STACK_SIZE
    mul     t0, t0, a0
    sub     sp, sp, t0                  # sp = _stack_top - hartid * STACK_SIZE

    # --- 2. traps + interrupts: known state -------------------------------------
    la      t0, trap_entry
    csrw    mtvec, t0                   # where the CPU jumps on an interrupt/exception
    csrw    mie, zero                   # no individual interrupt sources enabled
    csrci   mstatus, 8                  # MIE=0: no interrupts globally

    # --- 3. split by role ---------------------------------------------------------
    li      t0, NUM_HARTS
    bgeu    a0, t0, park                # unknown extra hart? just sleep forever
    bnez    a0, secondary_hart

primary_hart:                           # ===== hart 0: E51 monitor core =====
    la      t0, _bss_start              # zero .bss one doubleword at a time
    la      t1, _bss_end
1:  bgeu    t0, t1, 2f
    sd      zero, 0(t0)
    addi    t0, t0, 8
    j       1b
2:  li      a0, 0
    call    hart0_main                  # never returns
    j       park

secondary_hart:                         # ===== harts 1-4: U54 application cores =====
    # Allow ONLY the software interrupt (MSIE) to wake us. Because mstatus.MIE is 0,
    # the CPU will not take a trap -- `wfi` just returns when the interrupt goes pending.
    li      t0, 8                       # MIE.MSIE = bit 3
    csrw    mie, t0
1:  wfi
    csrr    t0, mip
    andi    t0, t0, 8                   # MIP.MSIP: did hart 0 poke us?
    beqz    t0, 1b                      # spurious wake-up -> sleep again

    li      t0, CLINT_BASE              # acknowledge: clear OUR msip word (CLINT + 4*hartid)
    slli    t1, a0, 2
    add     t0, t0, t1
    sw      zero, 0(t0)
    csrw    mie, zero
    call    hart_main                   # a0 still holds hartid
    j       park

park:                                   # dead end: sleep forever
    wfi
    j       park

/*
 * trap_entry -- every interrupt and exception on every hart lands here.
 *
 * Saves ONLY the caller-saved registers (ra, t0-t6, a0-a7 = 16 regs = 128 bytes).
 * The C compiler already guarantees s0-s11 are preserved by trap_handler(), so
 * saving them would be wasted work. This is deliberate: the sampling ISR runs
 * through this path 1000x/second and every instruction is overhead that
 * HartWatch exists to measure.
 */
    .section .text
    .balign 4                           # mtvec (direct mode) needs a 4-byte aligned target
    .globl trap_entry
trap_entry:
    addi    sp, sp, -128
    sd      ra,    0(sp)
    sd      t0,    8(sp)
    sd      t1,   16(sp)
    sd      t2,   24(sp)
    sd      a0,   32(sp)
    sd      a1,   40(sp)
    sd      a2,   48(sp)
    sd      a3,   56(sp)
    sd      a4,   64(sp)
    sd      a5,   72(sp)
    sd      a6,   80(sp)
    sd      a7,   88(sp)
    sd      t3,   96(sp)
    sd      t4,  104(sp)
    sd      t5,  112(sp)
    sd      t6,  120(sp)

    csrr    a0, mcause                  # why did we trap?
    csrr    a1, mepc                    # which instruction was interrupted?
    csrr    a2, mtval                   # extra info (bad address, etc.)
    call    trap_handler                # void trap_handler(mcause, mepc, mtval)

    ld      ra,    0(sp)
    ld      t0,    8(sp)
    ld      t1,   16(sp)
    ld      t2,   24(sp)
    ld      a0,   32(sp)
    ld      a1,   40(sp)
    ld      a2,   48(sp)
    ld      a3,   56(sp)
    ld      a4,   64(sp)
    ld      a5,   72(sp)
    ld      a6,   80(sp)
    ld      a7,   88(sp)
    ld      t3,   96(sp)
    ld      t4,  104(sp)
    ld      t5,  112(sp)
    ld      t6,  120(sp)
    addi    sp, sp, 128
    mret                                # return to the interrupted code (pc <- mepc)

/*
 * Stacks live in their own NOBITS section so they take no space in the ELF file and
 * sit outside the .bss range that hart 0 zeroes.
 */
    .section .stacks, "aw", @nobits
    .balign 16
    .space  NUM_HARTS * STACK_SIZE
    .globl _stack_top
_stack_top:
