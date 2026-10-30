/* native/kernel/tests/test_amd_gpu_ring.c -- host test of the real GPU ring code.
 *
 * Compiles the driver's own gpu/amd_ucode.c (amd_gfx_ring_init, ck_gpu_ring_submit,
 * ck_gpu_fence_wait, amd_gfx_ring_test) and the DMA pool (amd_dma_pool_take in
 * gpu/amd_gpu.h). Replaced for the host: the x86 rdtsc/pause (amd_host_shim.h),
 * the register file (a plain array) and the GPU itself.
 *
 * The GPU is a model thread. It watches CP_RB0_WPTR, decodes the PM4 packets in
 * the ring (type 2 fillers, type 3 headers with a count of body dwords), executes
 * WRITE_DATA by writing to the physical address, and writes its read pointer back.
 * It checks the packets against the layout gfx_v8_0.c emits; the test asserts on
 * what the model executed. Host run only: nothing physical is qualified here. */
#define AMD_GPU_TEST_HOST 1
#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "amd_gpu.h"

#define POOL_PAGES  1024u /* same size as the driver's DMA pool */

/* ---- stand-ins for the kernel's console (ck.h) -------------------------*/
void ck_puts(const char *s) { fputs(s, stdout); }
void ck_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

/* ---- the driver's global state and register file -----------------------*/
struct amd_gpu_state g_amd_gpu; /* gpu_x86.c is not linked into this test */
static volatile uint32_t fake_regs[1u << 16];

/* ---- physical pool: the host buffer that stands in for its frames -------*/
static uint8_t *pool_host;
static uint64_t pool_pa, pool_bytes;

static int in_pool(uint64_t pa, uint64_t n)
{
    return pa >= pool_pa && pa + n <= pool_pa + pool_bytes;
}

static volatile uint32_t *pa_to_host(uint64_t pa)
{
    return in_pool(pa, 4) ? (volatile uint32_t *)(pool_host + (pa - pool_pa)) : NULL;
}

/* ---- the GPU model (CP) ------------------------------------------------*/
struct cp_log {
    unsigned fillers;    /* type-2 filler dwords executed */
    unsigned writes;     /* WRITE_DATA packets executed */
    unsigned unknown;    /* anything else: must stay 0 */
    unsigned faults;     /* WRITE_DATA to an address outside the pool */
    uint32_t hdr;        /* first type-3 header seen */
    uint32_t info;       /* WRITE_DATA info dword of the last one */
    uint64_t addr;       /* its address */
    uint32_t data;       /* its data */
    uint32_t rptr;       /* read pointer the CP last reported */
};
static struct cp_log cpl;
static uint32_t cp_rptr;      /* the GPU's read pointer, kept across runs */
static volatile int cp_stop;
static pthread_t cp_thread;

static void *cp_main(void *arg)
{
    (void)arg;
    struct amd_one_ring *r = &g_amd_gpu.ring_gfx;
    uint32_t mask = r->ring_dw - 1u, rptr = cp_rptr;
    while (!cp_stop) {
        uint32_t wptr = fake_regs[mmCP_RB0_WPTR] & mask;
        while (rptr != wptr) {
            uint32_t dw = r->ring[rptr];
            if ((dw >> 30) == 2u) { /* type 2: one dword, no body */
                cpl.fillers++;
                rptr = (rptr + 1u) & mask;
                continue;
            }
            if ((dw >> 30) != 3u) { /* not a packet this model knows */
                cpl.unknown++;
                rptr = wptr;
                break;
            }
            uint32_t count = ((dw >> 16) & 0x3fffu) + 1u; /* body dwords */
            uint32_t op = (dw >> 8) & 0xffu;
            if (!cpl.hdr)
                cpl.hdr = dw;
            if (op == 0x37u && count == 4u) { /* WRITE_DATA */
                uint32_t info = r->ring[(rptr + 1u) & mask];
                uint32_t lo   = r->ring[(rptr + 2u) & mask];
                uint32_t hi   = r->ring[(rptr + 3u) & mask];
                uint32_t data = r->ring[(rptr + 4u) & mask];
                uint64_t addr = ((uint64_t)hi << 32) | lo;
                cpl.info = info;
                cpl.addr = addr;
                cpl.data = data;
                cpl.writes++;
                if (((info >> 8) & 0xfu) == 5u) { /* DST_SEL 5: memory */
                    volatile uint32_t *p = pa_to_host(addr);
                    __atomic_thread_fence(__ATOMIC_SEQ_CST);
                    if (p) *p = data; else cpl.faults++;
                }
            } else {
                cpl.unknown++;
                rptr = wptr;
                break;
            }
            rptr = (rptr + 1u + count) & mask;
        }
        cpl.rptr = rptr;
        cp_rptr = rptr;
        *r->rptr_va = rptr;
        sched_yield();
    }
    return NULL;
}

static void cp_start(void)
{
    memset(&cpl, 0, sizeof cpl);
    cp_stop = 0;
    if (pthread_create(&cp_thread, NULL, cp_main, NULL)) {
        printf("cannot start the GPU model\n");
        exit(2);
    }
}

static void cp_stop_join(void)
{
    cp_stop = 1;
    pthread_join(cp_thread, NULL);
}

/* ---- test helpers ------------------------------------------------------*/
static int pass_count, fail_count;
#define CHECK(cond, label)                                  \
    do {                                                    \
        if (cond) { printf("  PASS  %s\n", (label)); pass_count++; } \
        else      { printf("  FAIL  %s\n", (label)); fail_count++; } \
    } while (0)

/* A pool at 2 GiB: frames 2 GiB .. 4 GiB + 1 MiB, the pool is the first 4 MiB. */
static void setup_pool(void)
{
    struct ck_frames fr;
    ck_frames_init(&fr);
    ck_frames_add(&fr, 0x80000000ull, 0x100100000ull);
    memset(&g_amd_gpu, 0, sizeof g_amd_gpu);
    if (amd_dma_pool_take(&g_amd_gpu, &fr, POOL_PAGES, AMD_DMA_LIMIT)) {
        printf("setup: no pool\n");
        exit(2);
    }
    pool_host  = aligned_alloc(4096, (size_t)POOL_PAGES * 4096u);
    if (!pool_host) { printf("setup: no host memory\n"); exit(2); }
    memset(pool_host, 0, (size_t)POOL_PAGES * 4096u);
    pool_pa    = g_amd_gpu.dma_pa;
    pool_bytes = g_amd_gpu.dma_total;
    g_amd_gpu.dma_va   = pool_host;
    g_amd_gpu.regs     = fake_regs;
    g_amd_gpu.tsc_hz   = 1000000000ull; /* one tick per nanosecond (amd_host_shim.h) */
    /* The firmware load is not under test here: mark the ucode step as done. */
    g_amd_gpu.steps_done = AMD_STEP_UCODE;
    memset((void *)fake_regs, 0, sizeof fake_regs);
}

/* ---- R1-R3: the pool comes from frames, below 4 GiB --------------------*/
static void test_pool_from_frames(void)
{
    printf("R1: the DMA pool is taken from frames below 4 GiB\n");
    struct ck_frames fr;
    ck_frames_init(&fr);
    ck_frames_add(&fr, 0x80000000ull, 0x100100000ull);
    memset(&g_amd_gpu, 0, sizeof g_amd_gpu);
    int rc = amd_dma_pool_take(&g_amd_gpu, &fr, POOL_PAGES, AMD_DMA_LIMIT);
    CHECK(rc == 0 && g_amd_gpu.dma_pa == 0x80000000ull, "pool starts at the first free frame");
    CHECK(g_amd_gpu.dma_total == POOL_PAGES * 4096u && g_amd_gpu.dma_off == 0,
          "pool size is 4 MiB with the cursor at zero");
    CHECK(g_amd_gpu.dma_pa + g_amd_gpu.dma_total <= AMD_DMA_LIMIT, "pool ends at or below 4 GiB");

    printf("R2: frames only above 4 GiB give no pool, and nothing is taken\n");
    ck_frames_init(&fr);
    ck_frames_add(&fr, 0x100000000ull, 0x110000000ull);
    uint64_t before = ck_frames_free_bytes(&fr);
    memset(&g_amd_gpu, 0, sizeof g_amd_gpu);
    rc = amd_dma_pool_take(&g_amd_gpu, &fr, POOL_PAGES, AMD_DMA_LIMIT);
    CHECK(rc == -1 && g_amd_gpu.dma_total == 0, "take refuses: no frame below the limit");
    CHECK(ck_frames_free_bytes(&fr) == before, "a refused take leaves the frames free");

    printf("R3: no frame allocator (fr == NULL) gives no pool and no crash\n");
    memset(&g_amd_gpu, 0, sizeof g_amd_gpu);
    CHECK(amd_dma_pool_take(&g_amd_gpu, NULL, POOL_PAGES, AMD_DMA_LIMIT) == -1 &&
          g_amd_gpu.dma_total == 0, "NULL frames: take returns -1");
}

/* ---- R4: the gfx ring allocates from the pool -------------------------*/
static void test_ring_init(void)
{
    printf("R4: amd_gfx_ring_init allocates ring, rptr and fence from the pool\n");
    int rc = amd_gfx_ring_init();
    struct amd_one_ring *r = &g_amd_gpu.ring_gfx;
    CHECK(rc == 0 && r->ready, "gfx ring init returns 0 and marks the ring ready");
    CHECK(in_pool(r->ring_pa, AMD_RING_BYTES), "ring buffer lies inside the pool");
    CHECK(in_pool(r->rptr_pa, 4) && in_pool(r->fence_pa, 4), "rptr and fence slots lie inside the pool");
    CHECK(r->ring_pa != r->rptr_pa && r->rptr_pa != r->fence_pa && r->ring_pa != r->fence_pa,
          "ring, rptr and fence are three different allocations");
    CHECK(((r->ring_pa - pool_pa) % 64u) == 0 && ((r->fence_pa - pool_pa) % 64u) == 0,
          "allocations are 64-byte aligned (amd_dma_alloc)");
    CHECK((volatile uint8_t *)r->ring == pool_host + (r->ring_pa - pool_pa),
          "host pointer and physical address of the ring agree");
    CHECK(fake_regs[mmCP_RB0_BASE] == (uint32_t)(r->ring_pa >> 8) &&
          fake_regs[mmCP_RB0_BASE_HI] == (uint32_t)(r->ring_pa >> 40),
          "CP_RB0_BASE and _HI carry ring_pa >> 8 (gfx_v8_0.c:4278-4280)");
}

/* ---- R5-R6: NOP + WRITE_DATA fence round trip --------------------------*/
static void test_fence_round_trip(void)
{
    struct amd_one_ring *r = &g_amd_gpu.ring_gfx;

    printf("R5: NOP + WRITE_DATA fence round-trips through the GPU model\n");
    cp_start();
    int rc = amd_gfx_ring_test();
    cp_stop_join();
    CHECK(rc == 0, "amd_gfx_ring_test returns 0");
    CHECK(*r->fence_va == 1, "fence slot reads 1 (the GPU wrote it)");
    CHECK(cpl.fillers == 1 && cpl.writes == 1 && cpl.unknown == 0 && cpl.faults == 0,
          "the GPU executed one filler and one WRITE_DATA, nothing else");
    CHECK(cpl.hdr == 0xc0033700u, "WRITE_DATA header is PACKET3(0x37, 3) (gfx_v8_0.c:891)");
    CHECK(cpl.info == 0x00100500u, "WRITE_DATA info is DST_SEL 5 (memory) with WR_CONFIRM (gfx_v8_0.c:892)");
    CHECK(cpl.addr == r->fence_pa && cpl.data == 1, "WRITE_DATA targets the fence PA with seq 1");
    CHECK(cpl.rptr == 6 && r->wptr == 6, "the GPU consumed all six dwords (rptr == wptr == 6)");

    printf("R6: a second round trip advances the ring and the fence\n");
    cp_start();
    rc = amd_gfx_ring_test();
    cp_stop_join();
    CHECK(rc == 0 && *r->fence_va == 2, "second fence lands with seq 2");
    CHECK(cpl.writes == 1 && cpl.rptr == 12 && r->wptr == 12,
          "the second packet pair starts at dword 6 and ends at dword 12");
}

/* ---- R7: a fence nobody writes times out -----------------------------*/
static void test_fence_timeout(void)
{
    printf("R7: ck_gpu_fence_wait times out when nothing writes the fence\n");
    volatile uint32_t slot = 0;
    uint64_t t0 = ck_rdtsc();
    int rc = ck_gpu_fence_wait(&slot, 5u, 20u);
    uint64_t dt = ck_rdtsc() - t0;
    CHECK(rc == AMD_RING_ETIMEOUT, "returns AMD_RING_ETIMEOUT");
    CHECK(dt < 2000ull * 1000000ull, "gives up within the TSC bound (about 20 ms, under 2 s)");
}

int main(void)
{
    printf("AMD GPU ring host test\n");
    printf("======================\n");
    test_pool_from_frames();
    setup_pool();
    test_ring_init();
    test_fence_round_trip();
    test_fence_timeout();
    printf("\n======================\n");
    printf("PASS: %d  FAIL: %d\n", pass_count, fail_count);
    if (fail_count > 0) {
        printf("AMD_GPU_RING_TEST: FAIL\n");
        return 1;
    }
    printf("AMD_GPU_RING_TEST: PASS\n");
    return 0;
}
