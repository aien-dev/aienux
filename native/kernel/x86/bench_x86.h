/* bench_x86.h -- GPU bench console declarations (kernel-internal). */
#ifndef AIENOS_CK_BENCH_X86_H
#define AIENOS_CK_BENCH_X86_H
#include <stdint.h>
/* Init: store GPU BAR pointers. Pass NULL when no GPU present (QEMU). */
void ck_x86_bench_init(volatile uint8_t *gpu_cfg, volatile uint32_t *gpu_bar5,
                        volatile uint32_t *gpu_bar0);
/* Step: poll for bench requests. Called from hold_step. */
void ck_x86_bench_step(uint64_t tsc_hz);
#endif
