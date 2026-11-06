/*
 * trap.c -- C half of the trap handler (the assembly half is trap_entry in boot.S).
 *
 * "Trap" = the CPU is forced to stop what it is doing and jump to the handler.
 *   - an INTERRUPT is asynchronous (timer fired, another hart poked us)
 *   - an EXCEPTION is caused by the current instruction (bad address, illegal opcode)
 * mcause tells them apart: its top bit is 1 for interrupts, and the low bits are the
 * reason code (7 = machine timer, 3 = machine software, ...).
 *
 * RULE: the timer path must never print (kprintf takes a lock; if the interrupted code
 * already holds it, we would deadlock) and must stay as short as possible.
 */
#include <stdint.h>
#include "csr.h"
#include "kprintf.h"
#include "timer.h"

#define IRQ_MACHINE_TIMER 7

void trap_handler(uint64_t mcause, uint64_t mepc, uint64_t mtval)
{
    if (mcause & MCAUSE_INTR) {
        uint64_t code = mcause & ~MCAUSE_INTR;
        if (code == IRQ_MACHINE_TIMER) {
            timer_irq((unsigned)csr_read(mhartid), mepc);
            return;
        }
        kpanic("unexpected interrupt, code %lu, at pc=0x%lx", code, mepc);
    }

    kpanic("exception, mcause=%lu, pc=0x%lx, mtval=0x%lx", mcause, mepc, mtval);
}
