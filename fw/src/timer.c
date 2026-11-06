/*
 * timer.c -- periodic machine-timer interrupts.
 *
 * HOW THE CLINT TIMER WORKS
 *   mtime      one 64-bit counter, +1 every microsecond, shared by all harts.
 *   mtimecmp[h] a per-hart 64-bit compare value.
 *   Hart h's timer interrupt is PENDING whenever  mtime >= mtimecmp[h].
 *   It stays pending until software writes a new, larger mtimecmp.
 *
 * To get a tick every P microseconds we do, inside the handler:
 *       next_deadline += P;   mtimecmp = next_deadline;
 * Note we add P to the previous DEADLINE, not to "now". Adding to "now" would let
 * every bit of interrupt latency pile up as drift; this way the long-run rate is
 * exact and a late tick just shows up as one short gap afterwards.
 *
 * The timer interrupt IS the sampling clock: each U54 takes a sample on every tick.
 * The E51 also has a tick, used only to wake from wfi, and it does not sample.
 */
#include "timer.h"
#include "csr.h"
#include "hart.h"
#include "platform.h"
#include "sampler.h"

void timer_start(unsigned hart, uint64_t period_us, uint64_t limit)
{
    hart_state_t *s = &g_hart[hart];

    s->period = period_us;
    s->limit = limit;
    s->ticks = 0;
    s->done = 0;
    s->next_deadline = CLINT_MTIME + period_us;
    CLINT_MTIMECMP(hart) = s->next_deadline;

    csr_set(mie, MIE_MTIE);          /* enable the timer interrupt source...       */
    csr_set(mstatus, MSTATUS_MIE);   /* ...and interrupts globally on this hart    */
}

void timer_stop(unsigned hart)
{
    csr_clear(mie, MIE_MTIE);
    CLINT_MTIMECMP(hart) = ~0UL;     /* "never": also drops the pending bit */
}

void timer_irq(unsigned hart, uint64_t mepc)
{
    hart_state_t *s = &g_hart[hart];
    uint64_t now = CLINT_MTIME;

    if (hart != MONITOR_HART)
        sampler_tick(hart, now, mepc);      /* first, so the snapshot is close to the tick */

    if (s->ticks == 0) {
        s->first_tick_time = now;
        s->min_gap = ~0UL;
        s->max_gap = 0;
        s->late_ticks = 0;
    } else {
        uint64_t gap = now - s->last_tick_time;
        if (gap < s->min_gap) s->min_gap = gap;
        if (gap > s->max_gap) s->max_gap = gap;
        if (gap > 2 * s->period) s->late_ticks++;
    }
    s->last_tick_time = now;
    s->ticks++;

    if (s->limit && s->ticks >= s->limit) {
        timer_stop(hart);
        __atomic_store_n(&s->done, 1, __ATOMIC_RELEASE);
        return;
    }

    s->next_deadline += s->period;
    CLINT_MTIMECMP(hart) = s->next_deadline;
}
