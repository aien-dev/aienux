/* native/kernel/gpu/amd_mc.c -- GMC 8.x memory controller read-only probe for
 * the 2017 iMac's AMD Radeon Pro 570/575/580 (Polaris 10, PCI 1002:67df).
 *
 * Reads MC_VM_FB_LOCATION (VRAM start / end in MC address space), the system
 * aperture, and the VM context 0 page-table base address -- everything needed
 * to set up a GART mapping so CP / SDMA can read system RAM.  Nothing is
 * written in this step; we only report what the BIOS left.
 *
 * Sources: include/asic_reg/gmc/gmc_8_1_d.h MIT (C) 2016 AMD.
 * Sequence follows gmc_v8_0.c:gmc_v8_0_mc_resume / gmc_v8_0_gart_init.
 * No floats.  All waits TSC-bounded. */
#include "amd_gpu.h"

/* MC_VM_FB_LOCATION packing (gmc_8_1_sh_mask.h):
 *   bits [15:0]  = MC_FB_START  = VRAM start >> 24 (in 16-MiB units)
 *   bits [31:16] = MC_FB_TOP    = VRAM end   >> 24 (last occupied 16-MiB chunk)
 * VRAM start PA = MC_FB_START << 24
 * VRAM size     = ((MC_FB_TOP - MC_FB_START + 1) << 24) bytes */
#define MC_FB_START_MASK  0x0000ffffu
#define MC_FB_TOP_SHIFT   16u

/* MC_VM_SYSTEM_APERTURE_*_ADDR packing:
 *   bits [29:0] = address >> 12  (4-KiB pages) */
#define MC_SYS_ADDR_SHIFT 12u

int amd_mc_read(void)
{
    struct amd_gpu_state *g = &g_amd_gpu;

    if (!g->regs) {
        ck_puts("gpu: mc: no register aperture\n");
        return -1;
    }

    /* Read MC_VM_FB_LOCATION */
    uint32_t fb_loc = amd_rd(mmMC_VM_FB_LOCATION);
    uint64_t fb_start = (uint64_t)(fb_loc & MC_FB_START_MASK) << 24;
    uint64_t fb_top   = (uint64_t)((fb_loc >> MC_FB_TOP_SHIFT) & 0xffffu) << 24;
    uint64_t fb_sz    = (fb_top >= fb_start) ? (fb_top - fb_start + (1ull << 24)) : 0ull;

    g->mc_fb_loc   = fb_loc;
    g->mc_fb_start = fb_start;
    g->mc_fb_end   = fb_top + (1ull << 24) - 1ull;
    if (g->vram_sz == 0)
        g->vram_sz = fb_sz;

    ck_printf("gpu: mc: MC_VM_FB_LOCATION=0x%08x  VRAM start=0x%llx top=0x%llx size=%llu MiB\n",
              fb_loc,
              (unsigned long long)fb_start,
              (unsigned long long)fb_top,
              (unsigned long long)(fb_sz >> 20));

    /* Cross-check: VRAM start should match BAR0 physical address. */
    if (g->vram_pa != 0 && fb_start != g->vram_pa) {
        ck_printf("gpu: mc: WARNING: MC_VM_FB_START 0x%llx != BAR0 PA 0x%llx\n",
                  (unsigned long long)fb_start, (unsigned long long)g->vram_pa);
    }

    /* Read system aperture */
    uint32_t sys_lo = amd_rd(mmMC_VM_SYSTEM_APERTURE_LOW_ADDR);
    uint32_t sys_hi = amd_rd(mmMC_VM_SYSTEM_APERTURE_HIGH_ADDR);
    g->mc_sys_lo = (uint64_t)sys_lo << MC_SYS_ADDR_SHIFT;
    g->mc_sys_hi = (uint64_t)sys_hi << MC_SYS_ADDR_SHIFT;
    ck_printf("gpu: mc: system aperture lo=0x%llx hi=0x%llx\n",
              (unsigned long long)g->mc_sys_lo,
              (unsigned long long)g->mc_sys_hi);

    /* Read AGP aperture (the GART window).  BIOS programs this to the GART
     * range; for now just report.  We will program VM_CONTEXT0 after the
     * GART table is built. */
    uint32_t agp_top = amd_rd(mmMC_VM_AGP_TOP);
    uint32_t agp_bot = amd_rd(mmMC_VM_AGP_BOT);
    ck_printf("gpu: mc: AGP_BOT=0x%08x AGP_TOP=0x%08x\n", agp_bot, agp_top);

    /* Read VM context 0 page-table base (what BIOS set; we will override
     * once the GFX ring is up). */
    uint32_t ctx0 = amd_rd(mmVM_CONTEXT0_PAGE_TABLE_BASE_ADDR);
    ck_printf("gpu: mc: VM_CONTEXT0_PAGE_TABLE_BASE=0x%08x (PA=0x%llx)\n",
              ctx0, (unsigned long long)((uint64_t)ctx0 << 12));

    /* Sanity checks */
    if (fb_loc == 0xffffffffu) {
        ck_puts("gpu: mc: MC_VM_FB_LOCATION reads all-ones (chip not awake?)\n");
        return -1;
    }
    if (fb_sz == 0) {
        ck_puts("gpu: mc: VRAM size from FB_LOCATION is zero; not continuing\n");
        return -1;
    }

    g->steps_done |= AMD_STEP_MC;
    ck_printf("gpu: mc: done (VRAM %llu MiB, system aperture 0x%llx-0x%llx)\n",
              (unsigned long long)(g->vram_sz >> 20),
              (unsigned long long)g->mc_sys_lo,
              (unsigned long long)g->mc_sys_hi);
    return 0;
}
