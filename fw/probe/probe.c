/*
 * probe.c -- what do the performance counters REALLY report under QEMU?
 *
 * This is a separate firmware image (`make probe`), not part of HartWatch itself. It exists
 * to answer one question before anything is built on the counters (CLAUDE.md rule 2).
 * It prints raw numbers and draws no conclusions; those belong in the engineering log.
 *
 * Hart 0 scans its CSRs and wakes hart 1. Hart 1 runs every test while harts 0 and 2-4 sleep,
 * so nothing else is executing, until phase G wakes harts 2-4 on purpose.
 */
#include <stdint.h>
#include "config.h"
#include "csr.h"
#include "kprintf.h"
#include "platform.h"
#include "uart.h"

void wl_alu(uint64_t iters);
void wl_branch(uint64_t iters);
void wl_stride(uint64_t iters, uintptr_t base, uint64_t stride, uint64_t size_mask);

#define ALU_ITERS     5000000UL
#define BRANCH_ITERS  2000000UL
#define STRIDE_ITERS  5000000UL
#define ARRAY_BASE    0x84000000UL      /* free DRAM, far above the firmware image */
#define ARRAY_MASK    0x01FFFFFFUL      /* 32 MiB array */
#define REPS          5

/* Set by trap_handler when a CSR access was illegal. One slot per hart, on its own line. */
static volatile struct { uint32_t illegal; } __attribute__((aligned(64))) g_trap[NUM_HARTS];

/* The probe's own trap handler: an illegal instruction means "this CSR does not exist",
 * which is an answer, not a crash. Record it and skip the 4-byte CSR instruction. */
void trap_handler(uint64_t mcause, uint64_t mepc, uint64_t mtval)
{
    if (mcause == 2) {
        g_trap[csr_read(mhartid)].illegal++;
        csr_write(mepc, mepc + 4);
        return;
    }
    kpanic("unexpected trap, mcause=0x%lx, pc=0x%lx, mtval=0x%lx", mcause, mepc, mtval);
}

/* CSR names must be literal in the instruction, so indexed access needs a switch. */
#define FOR_HPM(X)                                                                   \
    X(3)  X(4)  X(5)  X(6)  X(7)  X(8)  X(9)  X(10) X(11) X(12) X(13) X(14) X(15)    \
    X(16) X(17) X(18) X(19) X(20) X(21) X(22) X(23) X(24) X(25) X(26) X(27) X(28)    \
    X(29) X(30) X(31)

static uint64_t hpm_counter_read(unsigned n)
{
    uint64_t v = 0;
    switch (n) {
#define X(i) case i: __asm__ volatile("csrr %0, mhpmcounter" #i : "=r"(v)); break;
    FOR_HPM(X)
#undef X
    }
    return v;
}

static void hpm_counter_write(unsigned n, uint64_t v)
{
    switch (n) {
#define X(i) case i: __asm__ volatile("csrw mhpmcounter" #i ", %0" ::"r"(v)); break;
    FOR_HPM(X)
#undef X
    }
}

static void hpm_event_write(unsigned n, uint64_t v)
{
    switch (n) {
#define X(i) case i: __asm__ volatile("csrw mhpmevent" #i ", %0" ::"r"(v)); break;
    FOR_HPM(X)
#undef X
    }
}

static uint64_t hpm_event_read(unsigned n)
{
    uint64_t v = 0;
    switch (n) {
#define X(i) case i: __asm__ volatile("csrr %0, mhpmevent" #i : "=r"(v)); break;
    FOR_HPM(X)
#undef X
    }
    return v;
}

/* Read one CSR and say whether it trapped. A trapped read leaves garbage in the
 * destination register, so the value is only printed when the CSR really exists. */
#define SCAN_ONE(hart, reg)                                                          \
    do {                                                                             \
        uint32_t before = g_trap[hart].illegal;                                      \
        uint64_t v = csr_read(reg);                                                  \
        if (g_trap[hart].illegal == before)                                          \
            kprintf("[A] hart %u: %14s = %lu (0x%lx)\n", hart, #reg, v, v);          \
        else                                                                         \
            kprintf("[A] hart %u: %14s TRAPS (illegal instruction)\n", hart, #reg);  \
    } while (0)

static void scan_csrs(unsigned hart)
{
    uint32_t before;

    SCAN_ONE(hart, mcycle);
    SCAN_ONE(hart, minstret);
    SCAN_ONE(hart, mcountinhibit);
    SCAN_ONE(hart, mcounteren);

    kprintf("[A] hart %u: mhpmcounter readable:", hart);
    for (unsigned n = 3; n <= 31; n++) {
        before = g_trap[hart].illegal;
        (void)hpm_counter_read(n);
        if (g_trap[hart].illegal == before)
            kprintf(" %u", n);
    }
    kprintf("\n[A] hart %u: mhpmevent readable:  ", hart);
    for (unsigned n = 3; n <= 31; n++) {
        before = g_trap[hart].illegal;
        (void)hpm_event_read(n);
        if (g_trap[hart].illegal == before)
            kprintf(" %u", n);
    }
    kprintf("\n");
}

/* ---- workloads ------------------------------------------------------------------ */
static void run_alu(void)      { wl_alu(ALU_ITERS); }
static void run_branch(void)   { wl_branch(BRANCH_ITERS); }
static void run_stride64(void) { wl_stride(STRIDE_ITERS, ARRAY_BASE, 64, ARRAY_MASK); }
static void run_stride4k(void) { wl_stride(STRIDE_ITERS, ARRAY_BASE, 4096 + 64, ARRAY_MASK); }

static const struct workload {
    const char *name;
    void (*run)(void);
    uint64_t expected_instret;      /* iterations x instructions per iteration */
} workloads[] = {
    { "alu",       run_alu,      ALU_ITERS * 8 },
    { "branch",    run_branch,   BRANCH_ITERS * 24 },
    { "stride64",  run_stride64, STRIDE_ITERS * 7 },
    { "stride4k",  run_stride4k, STRIDE_ITERS * 7 },
};
#define NUM_WORKLOADS (sizeof(workloads) / sizeof(workloads[0]))

/* mhpmcounter5 is programmed with the DTLB read-miss selector (see selectors[]), so each
 * measurement also shows whether that per-hart event moves. IPC is in thousandths:
 * kprintf has no floating point. */
static void measure(const struct workload *w, int rep)
{
    uint64_t t0 = CLINT_MTIME;
    uint64_t h0 = hpm_counter_read(5);
    uint64_t c0 = csr_read(mcycle);
    uint64_t i0 = csr_read(minstret);
    w->run();
    uint64_t i1 = csr_read(minstret);
    uint64_t c1 = csr_read(mcycle);
    uint64_t h1 = hpm_counter_read(5);
    uint64_t t1 = CLINT_MTIME;

    uint64_t dc = c1 - c0, di = i1 - i0, dt = t1 - t0;
    kprintf("%9s %d %12lu %12lu %9lu %9lu %11ld %10lu\n", w->name, rep, dc, di, dt,
            dc ? di * 1000 / dc : 0, (long)(di - w->expected_instret), h1 - h0);
}

static void measure_all(const char *tag)
{
    kprintf("[%s]  workload rep   d_mcycle   d_minstret   d_mtime ipc_x1000 instret_err d_hpm5\n", tag);
    for (unsigned w = 0; w < NUM_WORKLOADS; w++)
        for (int r = 0; r < REPS; r++)
            measure(&workloads[w], r);
}

/* ---- individual tests ----------------------------------------------------------- */
static void test_back_to_back(void)
{
    kprintf("[B] back-to-back reads (delta between two consecutive csrr):\n");
    for (int i = 0; i < 5; i++) {
        uint64_t c0 = csr_read(mcycle), c1 = csr_read(mcycle);
        uint64_t i0 = csr_read(minstret), i1 = csr_read(minstret);
        kprintf("[B]   mcycle +%lu   minstret +%lu\n", c1 - c0, i1 - i0);
    }
}

/* Does a sleeping hart count? Wake from wfi on a timer that is pending but not trapped
 * (mstatus.MIE stays 0, so wfi just returns). */
static void test_wfi(unsigned hart)
{
    kprintf("[D] counters across a 10000 us wfi sleep:\n");
    for (int i = 0; i < 3; i++) {
        uint64_t t0 = CLINT_MTIME;
        CLINT_MTIMECMP(hart) = t0 + 10000;
        csr_write(mie, MIE_MTIE);
        uint64_t c0 = csr_read(mcycle), i0 = csr_read(minstret);
        while (CLINT_MTIME - t0 < 10000)
            __asm__ volatile("wfi");
        uint64_t i1 = csr_read(minstret), c1 = csr_read(mcycle);
        uint64_t t1 = CLINT_MTIME;
        csr_write(mie, 0);
        CLINT_MTIMECMP(hart) = ~0UL;
        kprintf("[D]   d_mtime=%lu d_mcycle=%lu d_minstret=%lu\n", t1 - t0, c1 - c0, i1 - i0);
    }
}

static void test_controls(unsigned hart)
{
    const struct workload *alu = &workloads[0];

    csr_write(mcycle, 1000);
    uint64_t c = csr_read(mcycle);
    csr_write(minstret, 1000);
    uint64_t i = csr_read(minstret);
    kprintf("[E] wrote 1000: mcycle reads %lu, minstret reads %lu\n", c, i);

    uint32_t before = g_trap[hart].illegal;
    csr_write(mcountinhibit, 0x5);      /* bit 0 = mcycle, bit 2 = minstret */
    kprintf("[E] write mcountinhibit=0x5: %u trap(s); one alu run after it:\n",
            g_trap[hart].illegal - before);
    uint64_t c0 = csr_read(mcycle), i0 = csr_read(minstret);
    alu->run();
    uint64_t i1 = csr_read(minstret), c1 = csr_read(mcycle);
    csr_write(mcountinhibit, 0);
    kprintf("[E]   d_mcycle=%lu d_minstret=%lu\n", c1 - c0, i1 - i0);
}

/* Event selectors to try. Each gets its OWN counter, because on the first run reusing one
 * counter let an old selector keep counting after a new one was written. The first group
 * are the ids in QEMU's PMU code and the rest are SiFive-style (class in bits 7:0, event mask
 * above). The SiFive values are from memory and unverified: whether a selector does anything
 * is exactly what is being tested. Selector 0 on the first counter is the control. */
#define FIRST_SEL_COUNTER 3
static const struct { const char *name; uint64_t sel; } selectors[] = {
    { "control:event0",     0x0 },
    { "qemu:cycles",        0x1 },
    { "qemu:dtlb_rd_miss",  0x10019 },      /* mhpmcounter5: the d_hpm5 column in measure() */
    { "qemu:instructions",  0x2 },
    { "qemu:dtlb_wr_miss",  0x1001B },
    { "qemu:itlb_miss",     0x10021 },
    { "sifive:int_load",    (1UL << 9)  | 0 },
    { "sifive:cond_branch", (1UL << 14) | 0 },
    { "sifive:br_mispred",  (1UL << 13) | 1 },
    { "sifive:icache_miss", (1UL << 8)  | 2 },
    { "sifive:dcache_miss", (1UL << 9)  | 2 },
};
#define NUM_SELECTORS (sizeof(selectors) / sizeof(selectors[0]))

static void setup_selectors(unsigned hart)
{
    uint32_t before = g_trap[hart].illegal;
    for (unsigned s = 0; s < NUM_SELECTORS; s++) {
        hpm_event_write(FIRST_SEL_COUNTER + s, selectors[s].sel);
        hpm_counter_write(FIRST_SEL_COUNTER + s, 0);
    }
    kprintf("[F] programmed %u selectors, %u trap(s)\n",
            (unsigned)NUM_SELECTORS, g_trap[hart].illegal - before);
}

static void test_selectors(void)
{
    uint64_t start[NUM_SELECTORS], delta[NUM_WORKLOADS][NUM_SELECTORS];

    for (unsigned w = 0; w < NUM_WORKLOADS; w++) {
        for (unsigned s = 0; s < NUM_SELECTORS; s++)
            start[s] = hpm_counter_read(FIRST_SEL_COUNTER + s);
        workloads[w].run();
        for (unsigned s = 0; s < NUM_SELECTORS; s++)
            delta[w][s] = hpm_counter_read(FIRST_SEL_COUNTER + s) - start[s];
    }

    kprintf("[F] ctr             selector  readback");
    for (unsigned w = 0; w < NUM_WORKLOADS; w++)
        kprintf(" %12s", workloads[w].name);
    kprintf("\n");
    for (unsigned s = 0; s < NUM_SELECTORS; s++) {
        kprintf("[F] %3u %20s %9lx", FIRST_SEL_COUNTER + s, selectors[s].name,
                hpm_event_read(FIRST_SEL_COUNTER + s));
        for (unsigned w = 0; w < NUM_WORKLOADS; w++)
            kprintf(" %12lu", delta[w][s]);
        kprintf("\n");
    }
}

void hart0_main(uint64_t hartid)
{
    uart_init(UART0_BASE);
    kprintf("\n=== HartWatch counter probe ===\n");
    scan_csrs((unsigned)hartid);
    __asm__ volatile("fence" ::: "memory");
    CLINT_MSIP(1) = 1;
    for (;;)
        __asm__ volatile("wfi");
}

void hart_main(uint64_t hartid)
{
    if (hartid != 1) {
        /* Phase G background load: keep this hart busy until the machine exits. */
        for (;;)
            wl_alu(ALU_ITERS);
    }

    scan_csrs(1);
    setup_selectors(1);
    test_back_to_back();
    measure_all("C");
    test_wfi(1);
    test_controls(1);
    test_selectors();

    kprintf("[G] harts 2-4 now running the alu loop; hart 1 repeats phase C:\n");
    for (unsigned h = 2; h < NUM_HARTS; h++)
        CLINT_MSIP(h) = 1;
    measure_all("G");

    kprintf("PROBE DONE\n");
    platform_exit();
}
