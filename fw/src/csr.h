/*
 * csr.h -- tiny wrappers around the RISC-V CSR instructions.
 *
 * A CSR ("control and status register") is a special register that is not part of
 * normal memory or the 32 general registers. You reach it with csrr/csrw/csrs/csrc.
 * The performance counters this project samples (mcycle, minstret, mhpmcounterN) are
 * CSRs, and each hart has its own copy -- that is the whole reason HartWatch exists.
 */
#ifndef HW_CSR_H
#define HW_CSR_H

#include <stdint.h>

#define csr_read(reg)                                                         \
    ({                                                                        \
        uint64_t __v;                                                         \
        __asm__ volatile("csrr %0, " #reg : "=r"(__v));                       \
        __v;                                                                  \
    })

#define csr_write(reg, val)                                                   \
    do {                                                                      \
        uint64_t __v = (uint64_t)(val);                                       \
        __asm__ volatile("csrw " #reg ", %0" ::"r"(__v) : "memory");          \
    } while (0)

/* csrs = "set bits": reg |= mask      csrc = "clear bits": reg &= ~mask */
#define csr_set(reg, mask)                                                    \
    do {                                                                      \
        uint64_t __v = (uint64_t)(mask);                                      \
        __asm__ volatile("csrs " #reg ", %0" ::"r"(__v) : "memory");          \
    } while (0)

#define csr_clear(reg, mask)                                                  \
    do {                                                                      \
        uint64_t __v = (uint64_t)(mask);                                      \
        __asm__ volatile("csrc " #reg ", %0" ::"r"(__v) : "memory");          \
    } while (0)

/* Bit positions we use. */
#define MSTATUS_MIE   (1UL << 3)    /* global machine interrupt enable          */
#define MIE_MSIE      (1UL << 3)    /* enable software interrupt  (wake-up IPI) */
#define MIE_MTIE      (1UL << 7)    /* enable timer interrupt                   */
#define MCAUSE_INTR   (1UL << 63)   /* top bit of mcause: 1 = interrupt, 0 = exception */

#endif
