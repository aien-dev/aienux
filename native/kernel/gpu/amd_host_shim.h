/* native/kernel/gpu/amd_host_shim.h -- host test only (AMD_GPU_TEST_HOST).
 *
 * amd_gpu.h takes two x86 instructions from x86/x86.h (rdtsc and pause). The
 * host test runs on any build machine, so it gets portable stand-ins here
 * instead. The test sets tsc_hz to 1e9, so one "tick" is one nanosecond. */
#pragma once
#include <stdint.h>
#include <time.h>

static inline uint64_t ck_rdtsc(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static inline void ck_pause(void) { }
