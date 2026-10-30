/* native/kernel/gpu/amd_gpu.h -- internal state shared across the AMD GPU driver.
 *
 * This header is NOT part of the public interface; only files in gpu/ include it.
 * The public interface is amd_ring.h and x86/x86.h (ck_x86_gpu_start).
 * No floats. All waits are TSC-bounded. */
#pragma once
#include "../core/ck_internal.h"
#include "../mm/frames.h"
#ifdef AMD_GPU_TEST_HOST
#include "amd_host_shim.h" /* host test: rdtsc and pause without x86 asm */
#else
#include "../x86/x86.h"
#endif
#include "amd_ring.h"
#include "amd_gpu_regs.h"

/* ---- geometry -----------------------------------------------------------*/
#define AMD_RING_DWORDS    4096u    /* entries in each ring; must be power of 2 */
#define AMD_RING_BYTES     (AMD_RING_DWORDS * 4u)
#define AMD_FENCE_SLOTS    4u       /* fence value slots per ring */
#define AMD_SDMA_RING_DW   2048u    /* SDMA ring dword count */
#define AMD_MQD_SIZE       256u     /* MQD is 64 dwords (256 bytes) */
#define AMD_EOP_SIZE_DW    4096u    /* end-of-pipeline buffer for MEC */

/* ---- timing constants ---------------------------------------------------*/
#define GPU_VBLANK_MS      50u      /* max wait for one vblank at 60 Hz panel (16.7 ms) */
#define GPU_RING_WAIT_MS   5000u    /* max for ring idle / fence */
#define GPU_SMU_MSG_MS     50u      /* SMC message round-trip */
#define GPU_RESET_MS       100u     /* soft reset settle time */
#define GPU_UCODE_MS       200u     /* microcode load settle */

/* ---- one ring's bookkeeping --------------------------------------------*/
struct amd_one_ring {
    volatile uint32_t *ring;  /* ring buffer (physical, mapped uncached-minus or WC) */
    uint64_t           ring_pa;
    uint32_t           wptr;  /* next dword to write (in dwords, mod ring_dw) */
    uint32_t           ring_dw;  /* ring size in dwords */
    /* read pointer written back by hardware */
    volatile uint32_t *rptr_va;
    uint64_t           rptr_pa;
    /* fence: hardware writes seq here when CP reaches PM4_WRITE_DATA */
    volatile uint32_t *fence_va;
    uint64_t           fence_pa;
    uint32_t           fence_seq;
    int                ready;
};

/* ---- global GPU state --------------------------------------------------*/
struct amd_gpu_state {
    /* BAR5 register aperture */
    volatile uint32_t *regs;

    /* BAR0 VRAM aperture (as seen by CPU) */
    volatile uint8_t  *vram_cpu;  /* mapped UC-, or WC for front+back */
    uint64_t           vram_pa;   /* BAR0 physical address */
    uint64_t           vram_sz;   /* size in bytes (from MC_VM_FB_LOCATION) */

    /* GOP front buffer (CRTC 0 is scanning out of here) */
    uint64_t           fb_pa;     /* physical address */
    uint32_t           fb_w, fb_h, fb_pitch;  /* pixels */
    uint32_t           fb_off;    /* offset within VRAM = fb_pa - vram_pa */

    /* Back buffer (allocated in VRAM after front buffer) */
    uint64_t           bb_pa;
    uint32_t           bb_off;    /* offset within VRAM */
    volatile uint32_t *bb_cpu;    /* CPU pointer (mapped UC-) */

    /* Current front/back swap state: 0 = GOP layout active, 1 = swapped */
    int                flipped;

    /* TSC Hz (from kmain_x86 measurement) */
    uint64_t           tsc_hz;

    /* MC state */
    uint32_t           mc_fb_loc;   /* raw MC_VM_FB_LOCATION */
    uint64_t           mc_fb_start; /* VRAM MC start */
    uint64_t           mc_fb_end;   /* VRAM MC end */
    uint64_t           mc_sys_lo, mc_sys_hi; /* system aperture */

    /* SMU state as-found */
    int                smu_alive;   /* SMC firmware header readable (Version not 0 or all-ones) */
    uint32_t           smu_fw_ver;

    /* Ring state */
    struct amd_one_ring ring_gfx;
    struct amd_one_ring ring_sdma;
    struct amd_one_ring ring_mec;

    /* DMA memory pool (allocated from frames) */
    uint64_t           dma_pa;    /* base PA of contiguous DMA block */
    uint8_t           *dma_va;    /* CPU-side pointer */
    uint32_t           dma_off;   /* allocation cursor */
    uint32_t           dma_total; /* total bytes in block */

    /* Overall init state (bitmask of completed steps) */
    unsigned           steps_done; /* AMD_STEP_* flags */
    int                ready;
};

#define AMD_STEP_DISPLAY   (1u << 0)
#define AMD_STEP_MC        (1u << 1)
#define AMD_STEP_SMU       (1u << 2)
#define AMD_STEP_UCODE     (1u << 3)
#define AMD_STEP_GFX_RING  (1u << 4)
#define AMD_STEP_SDMA      (1u << 5)
#define AMD_STEP_COMPUTE   (1u << 6)

/* Singleton (defined in gpu_x86.c). */
extern struct amd_gpu_state g_amd_gpu;

/* ---- helpers -----------------------------------------------------------*/
static inline uint32_t amd_rd(unsigned idx) {
    return g_amd_gpu.regs[idx];
}
static inline void amd_wr(unsigned idx, uint32_t v) {
    g_amd_gpu.regs[idx] = v;
}
/* wait_us via TSC */
static inline void amd_wait_us(uint32_t us) {
    uint64_t t0 = ck_rdtsc();
    uint64_t ticks = g_amd_gpu.tsc_hz / 1000000ull * us;
    while (ck_rdtsc() - t0 < ticks) ck_pause();
}
static inline void amd_wait_ms(uint32_t ms) {
    amd_wait_us((uint32_t)(ms * 1000u));
}
/* Allocate contiguous bytes from DMA pool; returns PA and zero-initialises. */
static inline int amd_dma_alloc(uint32_t bytes, uint64_t *pa_out, void **va_out) {
    uint32_t aligned = (bytes + 63u) & ~63u;
    if (g_amd_gpu.dma_off + aligned > g_amd_gpu.dma_total) return -1;
    *pa_out = g_amd_gpu.dma_pa + g_amd_gpu.dma_off;
    *va_out = g_amd_gpu.dma_va + g_amd_gpu.dma_off;
    g_amd_gpu.dma_off += aligned;
    return 0;
}

/* Takes npages of contiguous frames from fr, all ending at or below limit, and
 * records them as the DMA pool (dma_pa, dma_total, dma_off). The caller maps
 * dma_va and zeroes the pool. Returns 0, or -1 if fr is NULL or nothing fits.
 * Limit: every ring address the CP and SDMA take is a 40-bit field (gfx_v8_0.c
 * gfx_v8_0_cp_gfx_resume: CP_RB0_BASE = gpu_addr >> 8, CP_RB0_BASE_HI = the rest;
 * sdma_v3_0.c sdma_v3_0_gfx_resume: SDMA0_GFX_RB_BASE_HI = gpu_addr >> 40). The
 * pool sits below 4 GiB, which is stricter than that field. This driver does not
 * program the GART, so the GPU reaches a physical address only if the firmware
 * left the mapping in place: unverified on the iMac. */
#define AMD_DMA_LIMIT 0x100000000ull

static inline int amd_dma_pool_take(struct amd_gpu_state *g, struct ck_frames *fr,
                                    uint64_t npages, uint64_t limit)
{
    uint64_t pa;
    if (!fr || ck_frames_alloc_below(fr, npages, CK_PAGE, limit, &pa))
        return -1;
    g->dma_pa    = pa;
    g->dma_total = (uint32_t)(npages * CK_PAGE);
    g->dma_off   = 0;
    return 0;
}
/* Read BAR5 register with fence */
static inline uint32_t amd_rd_fence(unsigned idx) {
    uint32_t v = amd_rd(idx);
    __asm__ volatile("" ::: "memory");
    return v;
}

/* ---- step init functions (each implemented in its own .c file) ----------*/
int amd_display_init(void);    /* amd_display.c */
int amd_mc_read(void);         /* amd_mc.c */
int amd_smu_check(void);       /* amd_smu.c */
int amd_ucode_load(void);      /* amd_ucode.c */
int amd_gfx_ring_init(void);   /* amd_ucode.c */
int amd_gfx_ring_test(void);    /* amd_ucode.c */
void amd_sdma_bench(void);      /* amd_sdma.c */
int amd_sdma_init(void);       /* amd_sdma.c */
int amd_compute_init(void);    /* amd_compute.c */
