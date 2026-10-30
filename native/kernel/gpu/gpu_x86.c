/* native/kernel/gpu/gpu_x86.c -- x86 glue: GPU driver entry point for the
 * 2017 iMac (AIENOS-KERNEL-0 / AIENOS-IMAC-GPU-CORE-1).
 *
 * Called from x86/pci_x86.c:ck_x86_display() when a Polaris display chip is
 * found whose VRAM aperture (BAR0) holds the GOP framebuffer.  The PCI BAR5
 * (register aperture) is expected to already be mapped by panel_x86.c.
 *
 * Build: CK_X86_GPU=1 (the default of `make imac-kernel`).  Without it, or
 * when the chip is absent (QEMU), everything here says so once and returns.
 *
 * CK_X86_GPU_UCODE=1 additionally embeds the polaris11 CP/RLC/SDMA firmware
 * blobs (fetched by scripts/fetch_gpu_ucode.sh; never committed).  Without
 * that flag, Steps 4-6 are skipped.
 *
 * Steps and log prefixes:
 *   gpu: display: ...    -- DCE 11.2 double-buffer flip (amd_display.c)
 *   gpu: mc:      ...    -- GMC 8 VRAM size + aperture (amd_mc.c)
 *   gpu: smu:     ...    -- SMU state as Apple left it (amd_smu.c)
 *   gpu: ucode:   ...    -- RLC + CP firmware load (amd_ucode.c)
 *   gpu: gfx_ring:...    -- GFX ring + NOP/fence test (amd_ucode.c)
 *   gpu: sdma:    ...    -- SDMA0 fill + timed copy (amd_sdma.c)
 *   gpu: compute: ...    -- MEC HQD compute queue (amd_compute.c)
 *
 * No floats.  All waits TSC-bounded.  A surprise at any step leaves the step
 * alone; subsequent steps check their prerequisites and skip gracefully. */
#include "amd_gpu.h"
#include "../mm/frames.h"
#include "../x86/x86.h"

#ifndef CK_X86_GPU
#define CK_X86_GPU 0
#endif

/* Global GPU state (all driver files share this via extern in amd_gpu.h). */
struct amd_gpu_state g_amd_gpu;

/* DMA pool: 4 MiB physically contiguous, below 4 GiB for ring pointers. */
#define GPU_DMA_PAGES  1024u  /* 4 MiB */

/* Polaris device ID range (same as polaris() in panel_x86.c) */
static int polaris_id(uint16_t device)
{
    return (device >= 0x67c0u && device <= 0x67ffu) ||
           (device >= 0x6980u && device <= 0x699fu);
}

#if CK_X86_GPU

void ck_x86_gpu_start(volatile uint8_t *cfg, unsigned bus, unsigned dev, unsigned fn,
                      uint16_t vendor, uint16_t device,
                      uint64_t bar0_pa, uint64_t bar5_pa,
                      uint64_t fb_pa, uint32_t fb_w, uint32_t fb_h, uint32_t fb_pitch,
                      struct ck_frames *fr, uint64_t tsc_hz)
{
    static int seen;
    if (seen++) return;

    (void)cfg; (void)bus; (void)dev; (void)fn;

    ck_printf("gpu: start: %04x:%04x bar0=0x%llx bar5=0x%llx fb=0x%llx %ux%u pitch=%u tsc_hz=%llu\n",
              vendor, device,
              (unsigned long long)bar0_pa, (unsigned long long)bar5_pa,
              (unsigned long long)fb_pa, fb_w, fb_h, fb_pitch,
              (unsigned long long)tsc_hz);

    if (vendor != 0x1002u || !polaris_id(device)) {
        ck_puts("gpu: start: not a Polaris GPU, skipped\n");
        return;
    }
    if (!bar5_pa) {
        ck_puts("gpu: start: BAR5 not mapped, skipped\n");
        return;
    }

    struct amd_gpu_state *g = &g_amd_gpu;
    g->tsc_hz    = tsc_hz;
    g->regs      = (volatile uint32_t *)(uintptr_t)bar5_pa;
    g->vram_pa   = bar0_pa;
    g->fb_pa     = fb_pa;
    g->fb_w      = fb_w;
    g->fb_h      = fb_h;
    g->fb_pitch  = fb_pitch;

    /* Map BAR0 for back buffer (panels_x86 only maps BAR5). */
    if (bar0_pa) {
        /* The GOP framebuffer range is already mapped WC by the EFI stub.
         * Map the rest of the first 256 MiB of BAR0 as UC- (enough for back
         * buffer + GART scratch).  ck_x86_map_mmio skips if already mapped. */
        (void)ck_x86_map_mmio(bar0_pa, 256ull * 1024u * 1024u);
        g->vram_cpu  = (volatile uint8_t *)(uintptr_t)bar0_pa;
        g->vram_sz   = 4ull * 1024u * 1024u * 1024u; /* UNMEASURED: assumes 4 GiB (Pro 570/575); a Pro 580 has 8 GiB. amd_mc.c does not override it. */
    }

    /* DMA pool for the ring buffers, all below AMD_DMA_LIMIT (amd_gpu.h). fr is
     * NULL when the caller passes no frame allocator; the ring steps then skip. */
    if (amd_dma_pool_take(g, fr, GPU_DMA_PAGES, AMD_DMA_LIMIT)) {
        ck_puts("gpu: start: no DMA frames below 4 GiB, ring steps skipped\n");
    } else {
        g->dma_va = (uint8_t *)(uintptr_t)g->dma_pa;
        __builtin_memset(g->dma_va, 0, g->dma_total);
        ck_printf("gpu: dma_pool: 0x%llx..0x%llx (%u pages)\n",
                  (unsigned long long)g->dma_pa,
                  (unsigned long long)(g->dma_pa + g->dma_total),
                  GPU_DMA_PAGES);
    }

    /* Step 1: Display (double-buffer page flip). */
    ck_x86_step("gpu_display");
    if (amd_display_init() != 0)
        ck_puts("gpu: display: init failed; CPU-only rendering continues\n");

    /* Step 2: GMC read. */
    ck_x86_step("gpu_mc");
    (void)amd_mc_read();

    /* Step 3: SMU state check. */
    ck_x86_step("gpu_smu");
    (void)amd_smu_check();

    /* Step 4: Microcode load (if CK_X86_GPU_UCODE=1). */
    ck_x86_step("gpu_ucode");
    (void)amd_ucode_load();

    /* Step 5: GFX ring + fence self-test. */
    ck_x86_step("gpu_gfx");
    if (amd_gfx_ring_init() == 0)
        (void)amd_gfx_ring_test();

    /* Step 6: SDMA0. */
    ck_x86_step("gpu_sdma");
    if (amd_sdma_init() == 0)
        amd_sdma_bench();

    /* Step 7: Compute queue. */
    ck_x86_step("gpu_compute");
    (void)amd_compute_init();

    g->ready = (g->steps_done & AMD_STEP_DISPLAY) ? 1 : 0;
    ck_printf("gpu: done: steps=0x%02x (%s%s%s%s%s%s%s) ready=%d\n",
              g->steps_done,
              (g->steps_done & AMD_STEP_DISPLAY) ? "display " : "",
              (g->steps_done & AMD_STEP_MC)       ? "mc "      : "",
              (g->steps_done & AMD_STEP_SMU)      ? "smu "     : "",
              (g->steps_done & AMD_STEP_UCODE)    ? "ucode "   : "",
              (g->steps_done & AMD_STEP_GFX_RING) ? "gfx "     : "",
              (g->steps_done & AMD_STEP_SDMA)     ? "sdma "    : "",
              (g->steps_done & AMD_STEP_COMPUTE)  ? "compute"  : "",
              g->ready);
}

#else /* !CK_X86_GPU */

void ck_x86_gpu_start(volatile uint8_t *cfg, unsigned bus, unsigned dev, unsigned fn,
                      uint16_t vendor, uint16_t device,
                      uint64_t bar0_pa, uint64_t bar5_pa,
                      uint64_t fb_pa, uint32_t fb_w, uint32_t fb_h, uint32_t fb_pitch,
                      struct ck_frames *fr, uint64_t tsc_hz)
{
    static int said;
    (void)cfg;(void)bus;(void)dev;(void)fn;(void)bar0_pa;(void)bar5_pa;
    (void)fb_pa;(void)fb_w;(void)fb_h;(void)fb_pitch;(void)fr;(void)tsc_hz;
    if (said++) return;
    if (vendor == 0x1002u && polaris_id(device))
        ck_printf("gpu: %04x:%04x Polaris found; this build (CK_X86_GPU=0) "
                  "does not drive the GPU\n", vendor, device);
}

/* Stub implementations of the public API for non-GPU builds */
int  ck_gpu_flip(void)                       { return AMD_RING_ENODEV; }
volatile uint32_t *ck_gpu_backbuffer(void)   { return (volatile uint32_t *)0; }
uint64_t ck_gpu_backbuffer_pa(void)          { return 0; }
uint64_t ck_gpu_vram_size(void)              { return 0; }
int  ck_gpu_ring_submit(amd_ring_id_t id, const uint32_t *dw, unsigned count) {
    (void)id;(void)dw;(void)count; return AMD_RING_ENODEV;
}
int  ck_gpu_fence_wait(const volatile uint32_t *fence_va, uint32_t seq, unsigned timeout_ms) {
    (void)fence_va;(void)seq;(void)timeout_ms; return AMD_RING_ENODEV;
}

#endif /* CK_X86_GPU */
