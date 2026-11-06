/*
 * workloads.h -- assembly loops with known instruction counts (workloads.S).
 * The *_end symbols mark where each loop stops, so a sampled mepc can be checked against it.
 */
#ifndef HW_WORKLOADS_H
#define HW_WORKLOADS_H

#include <stdint.h>

void wl_alu(uint64_t iters);
void wl_branch(uint64_t iters);
void wl_stride(uint64_t iters, uintptr_t base, uint64_t stride, uint64_t size_mask);
void wl_store(uint64_t iters, uintptr_t base, uint64_t stride, uint64_t size_mask);

void wl_alu_end(void);
void wl_branch_end(void);
void wl_stride_end(void);
void wl_store_end(void);

#endif
