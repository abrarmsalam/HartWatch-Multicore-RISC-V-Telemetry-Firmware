/*
 * hart.h -- per-hart shared state.
 *
 * Each hart owns ONE slot of g_hart[]. The owner writes its slot; hart 0 reads all of them.
 *
 * WHY aligned(64)?  A cache moves data in 64-byte "lines". If two harts wrote to two
 * variables that happen to share a line, the line would ping-pong between their caches
 * even though they never touch each other's data ("false sharing"). Giving every hart a
 * slot that starts on a 64-byte boundary and fills whole lines prevents it. The same
 * trick (cacheline-padded indices) is used for the sample rings on Day 3.
 *
 * The `online`/`done` flags are written with RELEASE and read with ACQUIRE:
 *   "if you can see my flag, you can also see everything I wrote before the flag."
 */
#ifndef HW_HART_H
#define HW_HART_H

#include <stdint.h>
#include "config.h"

typedef struct {
    volatile uint32_t online;       /* 1 once the hart is running C code               */
    volatile uint32_t done;         /* 1 once the hart finished its self-test          */
    uint64_t misa;                  /* the hart's own `misa` CSR: which ISA it has     */

    /* periodic timer state + statistics (step 3) */
    uint64_t period;                /* tick spacing in mtime units (1 unit = 1 us)     */
    uint64_t limit;                 /* stop after this many ticks (0 = run forever)    */
    volatile uint64_t ticks;        /* timer interrupts taken so far                   */
    uint64_t next_deadline;         /* absolute mtime of the next scheduled tick       */
    uint64_t last_tick_time;        /* mtime at the previous tick                      */
    uint64_t first_tick_time;
    uint64_t min_gap, max_gap;      /* smallest/largest spacing between ticks, in us   */
    uint64_t late_ticks;            /* ticks that arrived > 2x the period late         */
} __attribute__((aligned(64))) hart_state_t;

extern hart_state_t g_hart[NUM_HARTS];

#endif
