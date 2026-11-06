/*
 * spinlock.h -- the simplest possible lock, built on RISC-V atomic instructions.
 *
 * Four harts printing at once would interleave their characters into garbage, so
 * printing takes a lock. __atomic_exchange_n compiles to a single `amoswap.w.aq`
 * instruction: "swap in a 1 and tell me the old value, atomically".
 *   - old value 0  -> the lock was free, and we now own it
 *   - old value 1  -> someone else holds it, try again
 * ACQUIRE / RELEASE are memory-ordering labels: everything we do while holding the
 * lock stays between the lock and the unlock, as seen by other harts. Understanding
 * this pair is the key to the lock-free rings on Day 3.
 *
 * RULE: never take this lock inside an interrupt handler that can interrupt code
 * which already holds it -- the hart would wait forever for itself (deadlock).
 */
#ifndef HW_SPINLOCK_H
#define HW_SPINLOCK_H

#include <stdint.h>

typedef struct {
    volatile uint32_t locked;
} spinlock_t;

static inline void spin_lock(spinlock_t *l)
{
    while (__atomic_exchange_n(&l->locked, 1, __ATOMIC_ACQUIRE)) {
        /* busy-wait */
    }
}

static inline void spin_unlock(spinlock_t *l)
{
    __atomic_store_n(&l->locked, 0, __ATOMIC_RELEASE);
}

#endif
