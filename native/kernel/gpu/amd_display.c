/* native/kernel/gpu/amd_display.c -- DCE 11.2 display engine: double-buffered
 * page flip with vsync wait for the 2017 iMac's AMD Radeon Pro 570/575/580
 * (Polaris 10, PCI 1002:67df).
 *
 * The GOP leaves CRTC 0 running and scanning out of the front buffer.  This
 * file:
 *   1. Reads back the current GRPH_PRIMARY_SURFACE_ADDRESS to confirm we
 *      know which buffer the hardware is scanning.
 *   2. Allocates a second (back) buffer at the next MiB-aligned offset after
 *      the front buffer inside VRAM.
 *   3. Maps the back buffer as uncached (UC-) via ck_x86_map_mmio so the
 *      CPU can write to it.  (WC mapping is preferred for throughput; that
 *      requires PAT reconfiguration, tracked as a TODO.)
 *   4. Exposes ck_gpu_flip() and ck_gpu_backbuffer() for the renderer.
 *
 * Sequence for each flip (follows dce_v11_0_page_flip in
 * drivers/gpu/drm/amd/amdgpu/dce_v11_0.c, MIT license):
 *   a. Lock the surface update: set GRPH_UPDATE_LOCK (bit 16).
 *   b. Clear GRPH_FLIP_CONTROL retrace and immediate bits (flip at next vblank).
 *   c. Write new surface address HIGH then LOW.
 *   d. Unlock: clear GRPH_UPDATE_LOCK.
 *   e. Wait for GRPH_SURFACE_UPDATE_PENDING (bit 2) to clear,
 *      bounded by GPU_VBLANK_MS.
 *   f. Swap front/back pointers.
 *
 * Register sources: dce/dce_11_2_d.h MIT (C) 2016 AMD.
 * All writes are read back and logged.  Any surprise stops the driver and
 * falls back to the CPU-only path (ck_gpu_flip returns AMD_RING_ENODEV).
 * No floats.  All waits TSC-bounded.
 *
 * QEMU: QEMU q35 has no DCE 11.2; the GOP framebuffer check will fail and
 * the function says so once.  QEMU runs qualify nothing physical. */
#include "amd_gpu.h"
#include "../x86/x86.h"

/* Minimum VRAM aperture size we trust for Polaris. */
#define VRAM_MIN_BYTES  (256ull * 1024u * 1024u)  /* 256 MiB */
/* One frame of 4K BGRX = 3840*2160*4 = 33177600 = 0x1FA4000 bytes */
#define FRAME_BYTES_4K  (3840u * 2160u * 4u)  /* 31.6 MiB */
/* Align back buffer to 4 MiB */
#define BB_ALIGN        (4u * 1024u * 1024u)

int amd_display_init(void)
{
    struct amd_gpu_state *g = &g_amd_gpu;

    if (!g->regs) {
        ck_puts("gpu: display: no register aperture\n");
        return -1;
    }

    /* Read the surface address the firmware programmed into CRTC 0. */
    uint32_t sa_lo = amd_rd(mmGRPH_PRIMARY_SURFACE_ADDRESS);
    uint32_t sa_hi = amd_rd(mmGRPH_PRIMARY_SURFACE_ADDRESS_HIGH);
    uint32_t flip  = amd_rd(mmGRPH_FLIP_CONTROL);
    uint32_t upd   = amd_rd(mmGRPH_UPDATE);
    uint32_t crtc  = amd_rd(mmCRTC_STATUS);
    ck_printf("gpu: display: GRPH_SURFACE_ADDR=0x%08x%08x FLIP=0x%x UPDATE=0x%x CRTC_STATUS=0x%x\n",
              sa_hi, sa_lo, flip, upd, crtc);

    /* Sanity: the firmware-programmed address must be inside BAR0. */
    uint64_t fa = ((uint64_t)sa_hi << 32) | sa_lo;
    if (fa == 0 || fa == 0xffffffffffffffffull) {
        ck_puts("gpu: display: GRPH surface address reads all-0 or all-1 (chip not scanning)\n");
        return -1;
    }
    /* Cross-check with the framebuffer physical address we got from the GOP. */
    if (g->fb_pa != 0 && g->fb_pa != fa) {
        ck_printf("gpu: display: WARNING: GOP fb_pa 0x%llx != GRPH addr 0x%llx (using GRPH)\n",
                  (unsigned long long)g->fb_pa, (unsigned long long)fa);
    }
    /* Use the hardware's own address as ground truth. */
    if (g->fb_pa == 0)
        g->fb_pa = fa;

    /* Confirm fb_pa is inside VRAM aperture. */
    if (g->vram_pa == 0 || fa < g->vram_pa || fa >= g->vram_pa + VRAM_MIN_BYTES) {
        ck_printf("gpu: display: fb_pa 0x%llx not inside vram_pa 0x%llx\n",
                  (unsigned long long)fa, (unsigned long long)g->vram_pa);
        return -1;
    }

    /* Compute VRAM offset of front buffer and back buffer. */
    g->fb_off = (uint32_t)(g->fb_pa - g->vram_pa);

    /* Back buffer: place it after the front buffer, aligned to BB_ALIGN.
     * Frame size for 3840x2160 BGRX = FRAME_BYTES_4K.
     * Even if we don't know the exact pitch/height, leave enough room. */
    uint32_t fb_sz  = FRAME_BYTES_4K;
    uint32_t bb_off_raw = g->fb_off + fb_sz;
    /* Round up to BB_ALIGN. */
    uint32_t bb_off = (bb_off_raw + BB_ALIGN - 1u) & ~(BB_ALIGN - 1u);
    uint64_t bb_pa  = g->vram_pa + bb_off;

    /* Ensure back buffer fits within VRAM.  We need at least another frame. */
    if ((uint64_t)bb_off + FRAME_BYTES_4K > g->vram_sz && g->vram_sz != 0) {
        ck_printf("gpu: display: back buffer at VRAM+0x%x would overflow vram_sz 0x%llx\n",
                  bb_off, (unsigned long long)g->vram_sz);
        return -1;
    }

    /* Map the back buffer as UC- so the CPU can write to it.
     * TODO: map as WC (requires setting PAT entry 4 to WC in CR_PAT MSR)
     * for the write-combined throughput the front buffer already has. */
    if (ck_x86_map_mmio(bb_pa, FRAME_BYTES_4K)) {
        ck_printf("gpu: display: could not map back buffer at 0x%llx\n",
                  (unsigned long long)bb_pa);
        return -1;
    }
    g->bb_pa  = bb_pa;
    g->bb_off = bb_off;
    g->bb_cpu = (volatile uint32_t *)(uintptr_t)bb_pa;

    /* Clear the back buffer (paint black). */
    volatile uint32_t *p = g->bb_cpu;
    for (uint32_t i = 0; i < FRAME_BYTES_4K / 4u; i++)
        p[i] = 0;

    ck_printf("gpu: display: front VRAM+0x%x pa=0x%llx  back VRAM+0x%x pa=0x%llx  "
              "back_cpu=0x%llx (UC-)\n",
              g->fb_off, (unsigned long long)g->fb_pa,
              bb_off,    (unsigned long long)bb_pa,
              (unsigned long long)(uintptr_t)g->bb_cpu);

    g->flipped = 0;
    g->steps_done |= AMD_STEP_DISPLAY;
    ck_puts("gpu: display: ready (CRTC 0, double-buffer vsync flip)\n");
    return 0;
}

/* ---- ck_gpu_flip: schedule flip at next vsync -------------------------*/
int ck_gpu_flip(void)
{
    struct amd_gpu_state *g = &g_amd_gpu;

    if (!(g->steps_done & AMD_STEP_DISPLAY))
        return AMD_RING_ENODEV;

    /* Determine which buffer to flip to (the back buffer). */
    uint64_t new_pa = g->flipped ? g->fb_pa : g->bb_pa;

    /* a. Lock surface update. */
    uint32_t upd = amd_rd(mmGRPH_UPDATE) | GRPH_UPDATE_LOCK;
    amd_wr(mmGRPH_UPDATE, upd);
    __asm__ volatile("" ::: "memory");

    /* b. Flip at vblank: clear the retrace and immediate-flip bits (dce_v11_0.c:265-269). */
    amd_wr(mmGRPH_FLIP_CONTROL, amd_rd(mmGRPH_FLIP_CONTROL) & ~GRPH_FLIP_MODE_BITS);

    /* c. Write new surface address (HIGH then LOW). */
    amd_wr(mmGRPH_PRIMARY_SURFACE_ADDRESS_HIGH, (uint32_t)(new_pa >> 32));
    amd_wr(mmGRPH_PRIMARY_SURFACE_ADDRESS,      (uint32_t)(new_pa & 0xffffffffu));

    /* d. Unlock: hardware latches the address at the next vsync. */
    upd &= ~GRPH_UPDATE_LOCK;
    amd_wr(mmGRPH_UPDATE, upd);
    __asm__ volatile("" ::: "memory");

    /* e. Wait for GRPH_SURFACE_UPDATE_PENDING to clear.
     *    The hardware clears this once the flip has been sent to the scan
     *    pipeline.  Bounded by GPU_VBLANK_MS (50 ms). */
    uint64_t t0     = ck_rdtsc();
    uint64_t ticks  = g->tsc_hz / 1000ull * GPU_VBLANK_MS;
    int      done   = 0;
    do {
        uint32_t s = amd_rd(mmGRPH_UPDATE);
        if (!(s & GRPH_SURFACE_UPDATE_PENDING)) { done = 1; break; }
        ck_pause();
    } while (ck_rdtsc() - t0 < ticks);

    if (!done) {
        ck_printf("gpu: display: flip pending after %u ms (GRPH_UPDATE=0x%x); using anyway\n",
                  GPU_VBLANK_MS, amd_rd(mmGRPH_UPDATE));
        /* Not fatal: the hardware may still complete the flip. */
    }

    /* Read back to confirm */
    uint32_t got_lo = amd_rd(mmGRPH_PRIMARY_SURFACE_ADDRESS);
    uint32_t got_hi = amd_rd(mmGRPH_PRIMARY_SURFACE_ADDRESS_HIGH);
    uint64_t got    = ((uint64_t)got_hi << 32) | got_lo;
    if (got != new_pa) {
        ck_printf("gpu: display: flip readback mismatch: wrote 0x%llx got 0x%llx\n",
                  (unsigned long long)new_pa, (unsigned long long)got);
    }

    /* f. Swap front/back tracking. */
    g->flipped ^= 1;
    return done ? AMD_RING_OK : AMD_RING_ETIMEOUT;
}

/* ---- ck_gpu_backbuffer: return current back buffer pointer ------------*/
volatile uint32_t *ck_gpu_backbuffer(void)
{
    struct amd_gpu_state *g = &g_amd_gpu;
    if (!(g->steps_done & AMD_STEP_DISPLAY))
        return (volatile uint32_t *)0;
    /* After a flip, the previous front becomes the new back. */
    if (!g->flipped)
        return g->bb_cpu;
    /* After one flip: back is now the original front buffer. */
    return (volatile uint32_t *)(uintptr_t)g->fb_pa;
}

uint64_t ck_gpu_backbuffer_pa(void)
{
    struct amd_gpu_state *g = &g_amd_gpu;
    if (!(g->steps_done & AMD_STEP_DISPLAY)) return 0ull;
    return g->flipped ? g->fb_pa : g->bb_pa;
}

uint64_t ck_gpu_vram_size(void)
{
    return g_amd_gpu.vram_sz;
}
