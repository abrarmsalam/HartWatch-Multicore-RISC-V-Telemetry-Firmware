#ifndef HW_TIMER_H
#define HW_TIMER_H

#include <stdint.h>

/* Start a periodic machine-timer interrupt on the CALLING hart (interrupt-enable CSRs
 * are per-hart, so a hart can only arm its own timer).
 *   period_us : spacing between ticks (mtime runs at 1 MHz, so 1 unit = 1 microsecond)
 *   limit     : stop after this many ticks and set g_hart[h].done; 0 = run forever */
void timer_start(unsigned hart, uint64_t period_us, uint64_t limit);

void timer_stop(unsigned hart);

/* Called from trap_handler() when this hart's machine-timer interrupt fires. */
void timer_irq(unsigned hart);

#endif
