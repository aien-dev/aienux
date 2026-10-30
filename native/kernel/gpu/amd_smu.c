/* native/kernel/gpu/amd_smu.c -- SMU (System Management Unit) state check for
 * the 2017 iMac's AMD Radeon Pro 570/575/580 (Polaris 10, PCI 1002:67df).
 *
 * Apple's EFI leaves the SMC running with its full power-management firmware.
 * We check: is it alive (its firmware header is readable), what version, and what
 * power state it reports.  We do NOT load or reset the SMC -- that would
 * require the polaris10 SMC firmware blob to be present and the full init
 * sequence (gfx_v8_0 + powerplay).  Those are Steps 4-6.
 *
 * If the SMC is alive, we can use it later for fan and power-limit control.
 * If it is not alive, we still proceed -- CP / SDMA do not need the SMC, and
 * display works without it.
 *
 * Sources: include/asic_reg/smu/smu_7_1_3_d.h MIT (C) 2016 AMD,
 *          pm/powerplay/inc/smu7.h, smu7_ppsmc.h MIT (C) 2016 AMD.
 * No floats.  All waits TSC-bounded. */
#include "amd_gpu.h"

/* SMC indirect access helper (reads 32 bits from SMC address space). */
static uint32_t smc_ind_rd(uint32_t addr)
{
    amd_wr(mmSMC_IND_INDEX_0, addr);
    __asm__ volatile("" ::: "memory");
    return amd_rd(mmSMC_IND_DATA_0);
}

int amd_smu_check(void)
{
    struct amd_gpu_state *g = &g_amd_gpu;

    if (!g->regs) {
        ck_puts("gpu: smu: no register aperture\n");
        return -1;
    }

    /* Read the SMC message registers directly to see if they look sane.  Nothing is sent:
     * Linux's only header-less message (PPSMC_MSG_Test) is an init command, not a ping. */
    uint32_t msg_reg  = amd_rd(mmSMC_MESSAGE_0);
    uint32_t resp_reg = amd_rd(mmSMC_RESP_0);
    ck_printf("gpu: smu: SMC_MESSAGE_0=0x%08x  SMC_RESP_0=0x%08x\n", msg_reg, resp_reg);

    if (msg_reg == 0xffffffffu && resp_reg == 0xffffffffu) {
        ck_puts("gpu: smu: registers read all-ones (SMU not accessible)\n");
        g->smu_alive = 0;
        g->steps_done |= AMD_STEP_SMU;
        return 0;  /* non-fatal: proceed without SMU */
    }

    /* The SMC firmware header sits at SMC address 0x20000 (smu7.h:166), reached through
     * SMC_IND_INDEX_0 / SMC_IND_DATA_0 (it is SMC address space, not VRAM).  Version is the
     * dword after Digest[5] (smu7.h:139-140).  All-ones or zero means the SMC is not running. */
    uint32_t fw_ver = smc_ind_rd(SMU7_FIRMWARE_HEADER_LOCATION +
                                 SMU7_FIRMWARE_HEADER_Version_offset);
    ck_printf("gpu: smu: SMC header at SMC+0x%x: Version=0x%08x\n",
              SMU7_FIRMWARE_HEADER_LOCATION + SMU7_FIRMWARE_HEADER_Version_offset, fw_ver);

    g->smu_fw_ver = fw_ver;
    g->smu_alive  = (fw_ver != 0u && fw_ver != 0xffffffffu);
    ck_printf("gpu: smu: smu_alive=%d fw_ver=0x%08x\n", g->smu_alive, g->smu_fw_ver);

    g->steps_done |= AMD_STEP_SMU;
    ck_puts("gpu: smu: done\n");
    return 0;
}
