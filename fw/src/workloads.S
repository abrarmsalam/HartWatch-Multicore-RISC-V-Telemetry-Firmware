/*
 * workloads.S -- loops with very different behaviour, for the probe and the firmware self-test.
 *
 * They are in assembly so the compiler cannot change them and we know EXACTLY how many
 * instructions each executes. That gives a ground truth to check minstret against.
 * Every path through a loop body has the same length, so the count does not depend on data:
 *
 *   wl_alu     8 instructions/iteration   no memory, one predictable branch
 *   wl_branch 24 instructions/iteration   4 data-dependent (pseudo-random) branches
 *   wl_stride  7 instructions/iteration   one load per iteration, walking a large array
 *   wl_store   7 instructions/iteration   one store per iteration, walking a large array
 */
    .section .text
    .balign 4

/* void wl_alu(uint64_t iters) */
    .globl wl_alu
    .type wl_alu, @function
wl_alu:
    li      t0, 1
    li      t1, 3
1:  add     t0, t0, t1
    xor     t1, t1, t0
    add     t0, t0, t1
    xor     t1, t1, t0
    add     t0, t0, t1
    xor     t1, t1, t0
    addi    a0, a0, -1
    bnez    a0, 1b
    ret
    .size wl_alu, . - wl_alu
    .globl wl_alu_end
wl_alu_end:

/* One data-dependent branch. Taken and not-taken paths are both 4 instructions. */
.macro RANDOM_BRANCH mask
    andi    t1, t0, \mask
    beqz    t1, 10f
    addi    t2, t2, 1
    j       11f
10: addi    t2, t2, -1
    nop
11:
.endm

/* void wl_branch(uint64_t iters) -- xorshift64 picks each branch's direction. */
    .globl wl_branch
    .type wl_branch, @function
wl_branch:
    li      t0, 0x2545F491
    li      t2, 0
1:  slli    t1, t0, 13
    xor     t0, t0, t1
    srli    t1, t0, 7
    xor     t0, t0, t1
    slli    t1, t0, 17
    xor     t0, t0, t1
    RANDOM_BRANCH 1
    RANDOM_BRANCH 2
    RANDOM_BRANCH 4
    RANDOM_BRANCH 8
    addi    a0, a0, -1
    bnez    a0, 1b
    ret
    .size wl_branch, . - wl_branch
    .globl wl_branch_end
wl_branch_end:

/* void wl_stride(uint64_t iters, uintptr_t base, uint64_t stride, uint64_t size_mask) */
    .globl wl_stride
    .type wl_stride, @function
wl_stride:
    li      t0, 0
    li      t3, 0
1:  add     t1, a1, t0
    ld      t2, 0(t1)
    add     t3, t3, t2
    add     t0, t0, a2
    and     t0, t0, a3
    addi    a0, a0, -1
    bnez    a0, 1b
    ret
    .size wl_stride, . - wl_stride
    .globl wl_stride_end
wl_stride_end:

/* void wl_store(uint64_t iters, uintptr_t base, uint64_t stride, uint64_t size_mask) */
    .globl wl_store
    .type wl_store, @function
wl_store:
    li      t0, 0
    li      t3, 0
1:  add     t1, a1, t0
    sd      t3, 0(t1)
    addi    t3, t3, 1
    add     t0, t0, a2
    and     t0, t0, a3
    addi    a0, a0, -1
    bnez    a0, 1b
    ret
    .size wl_store, . - wl_store
    .globl wl_store_end
wl_store_end:
