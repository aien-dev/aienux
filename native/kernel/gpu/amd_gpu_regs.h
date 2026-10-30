/* native/kernel/gpu/amd_gpu_regs.h -- AMD GCN register indices for Polaris 10
 * (Radeon Pro 570/575/580, PCI 1002:67df rev c5 on the real iMac, DCE 11.2 / GMC 8.x / GFX 8.0 / OSS 3.0 / SMU 7.1.3).
 *
 * MMIO byte offset = index * 4.  All indices refer to BAR5 (256 KiB register aperture).
 * Indices sourced from MIT-licensed AMD Linux kernel headers; see THIRD_PARTY_NOTICES.
 *
 * Sources (all MIT, Copyright (C) 2014-2016 Advanced Micro Devices, Inc.):
 *   include/asic_reg/dce/dce_11_2_d.h      -- display engine
 *   include/asic_reg/gmc/gmc_8_1_d.h       -- memory controller
 *   include/asic_reg/gca/gfx_8_0_d.h       -- GFX (CP, RLC)
 *   include/asic_reg/oss/oss_3_0_d.h       -- SDMA
 *   include/asic_reg/smu/smu_7_1_3_d.h    -- SMU
 * Repository: https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git
 *             drivers/gpu/drm/amd/include/asic_reg/ */
#pragma once
#include <stdint.h>

/* -------------------------------------------------------------------------
 * Display engine -- DCE 11.2 (dce_11_2_d.h)
 * CRTC 0 / GRPH 0 registers; the other controllers add a per-controller offset (dce_v11_0.c crtc_offsets).
 * Byte offset within BAR5 = index * 4.
 * ------------------------------------------------------------------------ */

/* Panel power sequencer (used by panel_x86.c; repeated here for reference) */
#define mmLVTMA_PWRSEQ_CNTL                      0x481bu  /* dce_11_2_d.h:1473 */

/* GRPH (display pipe) surface address -- for scan-out page flip */
#define mmGRPH_PRIMARY_SURFACE_ADDRESS           0x1a04u  /* dce_11_2_d.h:3537 */
#define mmGRPH_PRIMARY_SURFACE_ADDRESS_HIGH      0x1a07u  /* dce_11_2_d.h:3558 */
#define mmGRPH_FLIP_CONTROL                      0x1a12u  /* dce_11_2_d.h:3628 */
#define mmGRPH_UPDATE                            0x1a11u  /* dce_11_2_d.h:3621 */

/* GRPH_UPDATE bits (dce_11_2_sh_mask.h) */
#define GRPH_UPDATE__GRPH_SURFACE_UPDATE_PENDING_MASK 0x00000004u  /* dce_11_2_sh_mask.h:6345 */
#define GRPH_UPDATE__GRPH_UPDATE_LOCK_MASK            0x00010000u  /* dce_11_2_sh_mask.h:6351 */
#define GRPH_UPDATE_LOCK                     GRPH_UPDATE__GRPH_UPDATE_LOCK_MASK
#define GRPH_SURFACE_UPDATE_PENDING          GRPH_UPDATE__GRPH_SURFACE_UPDATE_PENDING_MASK

/* GRPH_FLIP_CONTROL bits.  Both clear = flip at vblank (dce_v11_0.c:265, "default is vsync");
 * IMMEDIATE_EN set = flip at once (tears). */
#define GRPH_FLIP_CONTROL__GRPH_SURFACE_UPDATE_H_RETRACE_EN_MASK   0x00000001u  /* dce_11_2_sh_mask.h:6359 */
#define GRPH_FLIP_CONTROL__GRPH_SURFACE_UPDATE_IMMEDIATE_EN_MASK   0x00000010u  /* dce_11_2_sh_mask.h:6363 */
#define GRPH_FLIP_MODE_BITS  (GRPH_FLIP_CONTROL__GRPH_SURFACE_UPDATE_H_RETRACE_EN_MASK | \
                              GRPH_FLIP_CONTROL__GRPH_SURFACE_UPDATE_IMMEDIATE_EN_MASK)

/* CRTC status -- for vblank detection */
#define mmCRTC_STATUS                            0x1ba3u  /* dce_11_2_d.h:371 */
#define CRTC_STATUS__CRTC_V_BLANK_MASK                0x00000001u  /* dce_11_2_sh_mask.h:723 */
#define CRTC_V_BLANK                         CRTC_STATUS__CRTC_V_BLANK_MASK

/* CRTC master update lock (CRTC 0) */
#define mmCRTC_MASTER_UPDATE_LOCK                0x1bbdu  /* dce_11_2_d.h:539 */
#define CRTC_MASTER_UPDATE_LOCK__MASTER_UPDATE_LOCK_MASK 0x00000001u  /* dce_11_2_sh_mask.h:881 */
#define CRTC_MASTER_UPDATE_LOCK_BIT          CRTC_MASTER_UPDATE_LOCK__MASTER_UPDATE_LOCK_MASK

/* -------------------------------------------------------------------------
 * Memory Controller -- GMC 8.1 (gmc_8_1_d.h)
 * ------------------------------------------------------------------------ */
#define mmMC_VM_FB_LOCATION                      0x0809u  /* gmc_8_1_d.h:281 */
#define mmMC_VM_FB_OFFSET                        0x081au  /* gmc_8_1_d.h:298 */
#define mmMC_VM_AGP_TOP                          0x080au  /* gmc_8_1_d.h:282 */
#define mmMC_VM_AGP_BOT                          0x080bu  /* gmc_8_1_d.h:283 */
#define mmMC_VM_SYSTEM_APERTURE_LOW_ADDR         0x080du  /* gmc_8_1_d.h:285 */
#define mmMC_VM_SYSTEM_APERTURE_HIGH_ADDR        0x080eu  /* gmc_8_1_d.h:286 */
#define mmMC_VM_MX_L1_TLB_CNTL                   0x0819u  /* gmc_8_1_d.h:297 */

/* CONFIG_MEMSIZE: VRAM size in MiB */
#define mmCONFIG_MEMSIZE                         0x150au  /* bif_5_1_d.h:33 */

/* HDP for coherency flush */
#define mmHDP_MEM_COHERENCY_FLUSH_CNTL           0x1520u  /* bif_5_1_d.h:51 */
#define mmHDP_REG_COHERENCY_FLUSH_CNTL           0x1528u  /* bif_5_1_d.h:50 */

/* VM context 0 page table base address */
#define mmVM_CONTEXT0_PAGE_TABLE_BASE_ADDR       0x054fu  /* gmc_8_1_d.h:636 */
#define mmVM_CONTEXT0_CNTL                       0x0504u  /* gmc_8_1_d.h:601 */
#define mmVM_L2_CNTL                             0x0500u  /* gmc_8_1_d.h:597 */
#define mmVM_L2_CNTL2                            0x0501u  /* gmc_8_1_d.h:598 */

/* -------------------------------------------------------------------------
 * GFX 8 / CP (gfx_8_0_d.h)
 * ------------------------------------------------------------------------ */
/* GRBM -- Graphics Register Bus Manager */
#define mmGRBM_SOFT_RESET                        0x2008u  /* gfx_8_0_d.h:868 */
#define GRBM_SOFT_RESET__SOFT_RESET_CP_MASK    0x00000001u  /* gfx_8_0_sh_mask.h:5667 */
#define GRBM_SOFT_RESET__SOFT_RESET_RLC_MASK   0x00000004u  /* gfx_8_0_sh_mask.h:5669 */
#define GRBM_SOFT_RESET__SOFT_RESET_GFX_MASK   0x00010000u  /* gfx_8_0_sh_mask.h:5671 */
#define GRBM_SOFT_RESET_CP                   GRBM_SOFT_RESET__SOFT_RESET_CP_MASK
#define GRBM_SOFT_RESET_GFX                  GRBM_SOFT_RESET__SOFT_RESET_GFX_MASK
#define GRBM_SOFT_RESET_RLC                  GRBM_SOFT_RESET__SOFT_RESET_RLC_MASK

#define mmGRBM_STATUS                            0x2004u  /* gfx_8_0_d.h:862 */
#define GRBM_STATUS__GUI_ACTIVE_MASK           0x80000000u  /* gfx_8_0_sh_mask.h:5541 */
#define GRBM_STATUS_GUI_ACTIVE               GRBM_STATUS__GUI_ACTIVE_MASK

/* CP PFP microcode upload */
#define mmCP_PFP_UCODE_ADDR                      0xf814u  /* gfx_8_0_d.h:271 */
#define mmCP_PFP_UCODE_DATA                      0xf815u  /* gfx_8_0_d.h:272 */

/* CP ME microcode upload */
#define mmCP_ME_RAM_WADDR                        0xf816u  /* gfx_8_0_d.h:274 */
#define mmCP_ME_RAM_RADDR                        0xf816u  /* gfx_8_0_d.h:273 */
#define mmCP_ME_RAM_DATA                         0xf817u  /* gfx_8_0_d.h:275 */

/* CP CE microcode upload */
#define mmCP_CE_UCODE_ADDR                       0xf818u  /* gfx_8_0_d.h:279 */
#define mmCP_CE_UCODE_DATA                       0xf819u  /* gfx_8_0_d.h:280 */

/* CP MEC (Micro Engine Compute) microcode upload */
#define mmCP_MEC_ME1_UCODE_ADDR                  0xf81au  /* gfx_8_0_d.h:281 */
#define mmCP_MEC_ME1_UCODE_DATA                  0xf81bu  /* gfx_8_0_d.h:282 */
#define mmCP_MEC_ME2_UCODE_ADDR                  0xf81cu  /* gfx_8_0_d.h:283 */
#define mmCP_MEC_ME2_UCODE_DATA                  0xf81du  /* gfx_8_0_d.h:284 */

/* GFX ring 0 control */
#define mmCP_RB0_BASE                            0x3040u  /* gfx_8_0_d.h:219 */
#define mmCP_RB0_BASE_HI                         0x30b1u  /* gfx_8_0_d.h:220 */
#define mmCP_RB0_CNTL                            0x3041u  /* gfx_8_0_d.h:225 */
#define mmCP_RB0_WPTR                            0x3045u  /* gfx_8_0_d.h:238 */
#define mmCP_RB0_RPTR_ADDR                       0x3043u  /* gfx_8_0_d.h:230 */
#define mmCP_RB0_RPTR_ADDR_HI                    0x3044u  /* gfx_8_0_d.h:234 */
#define mmCP_RB_VMID                             0x3051u  /* gfx_8_0_d.h:263 */

/* CP status */
#define mmCP_STAT                                0x21a0u  /* gfx_8_0_d.h:564 */
#define CP_STAT__PFP_BUSY_MASK               0x00008000u  /* gfx_8_0_sh_mask.h:3595 */
#define CP_STAT_PFP_BUSY                     CP_STAT__PFP_BUSY_MASK

#define CP_RB0_CNTL__RB_BUFSZ_MASK           0x0000003fu  /* gfx_8_0_sh_mask.h:1357 */
#define CP_RB0_CNTL__RB_BLKSZ_MASK           0x00003f00u  /* gfx_8_0_sh_mask.h:1359 */
#define CP_RB0_CNTL__RB_BLKSZ__SHIFT         0x00000008u  /* gfx_8_0_sh_mask.h:1360 */
#define CP_RB0_CNTL__RB_NO_UPDATE_MASK       0x08000000u  /* gfx_8_0_sh_mask.h:1371 */
/* RB_BUFSZ = log2(ring bytes / 8), RB_BLKSZ = RB_BUFSZ - 2 (gfx_v8_0.c:4248-4250). */
#define CP_RB_BUFSZ(n)           ((n) & CP_RB0_CNTL__RB_BUFSZ_MASK)
#define CP_RB_BLKSZ(n)           (((n) << CP_RB0_CNTL__RB_BLKSZ__SHIFT) & CP_RB0_CNTL__RB_BLKSZ_MASK)
#define CP_RB_NO_UPDATE          CP_RB0_CNTL__RB_NO_UPDATE_MASK  /* 1 = CP does not write RPTR back */

/* RLC (RunList Controller) */
#define mmRLC_CNTL                               0xec00u  /* gfx_8_0_d.h:1342 */
#define RLC_CNTL__RLC_ENABLE_F32_MASK        0x00000001u  /* gfx_8_0_sh_mask.h:8477 */
#define RLC_ENABLE                           RLC_CNTL__RLC_ENABLE_F32_MASK
#define mmRLC_SRM_CNTL                           0xec80u  /* gfx_8_0_d.h:1452 */
#define mmRLC_CGTT_MGCG_OVERRIDE                 0xec48u  /* gfx_8_0_d.h:1391 */
#define mmRLC_SRM_INDEX_CNTL_ADDR_0              0xec8bu  /* gfx_8_0_d.h:1463 */
#define mmRLC_SRM_INDEX_CNTL_DATA_0              0xec93u  /* gfx_8_0_d.h:1471 */
#define mmRLC_GPM_UCODE_ADDR                     0xf83cu  /* gfx_8_0_d.h:1374 */
#define mmRLC_GPM_UCODE_DATA                     0xf83du  /* gfx_8_0_d.h:1376 */

/* -------------------------------------------------------------------------
 * SDMA (oss_3_0_d.h)
 * SDMA0 registers; SDMA1 adds an offset of 0x200.
 * ------------------------------------------------------------------------ */
#define mmSDMA0_GFX_RB_BASE                      0x3481u  /* oss_3_0_d.h:340 */
#define mmSDMA0_GFX_RB_BASE_HI                   0x3482u  /* oss_3_0_d.h:341 */
#define mmSDMA0_GFX_RB_RPTR                      0x3483u  /* oss_3_0_d.h:342 */
#define mmSDMA0_GFX_RB_WPTR                      0x3484u  /* oss_3_0_d.h:343 */
#define mmSDMA0_GFX_RB_RPTR_ADDR_LO              0x3489u  /* oss_3_0_d.h:348 */
#define mmSDMA0_GFX_RB_RPTR_ADDR_HI              0x3488u  /* oss_3_0_d.h:347 */
#define mmSDMA0_GFX_RB_CNTL                      0x3480u  /* oss_3_0_d.h:339 */
#define mmSDMA0_GFX_IB_CNTL                      0x348au  /* oss_3_0_d.h:349 */
#define mmSDMA0_CNTL                             0x3404u  /* oss_3_0_d.h:295 */
#define mmSDMA0_F32_CNTL                         0x3412u  /* oss_3_0_d.h:309 */
#define mmSDMA0_STATUS_REG                       0x340du  /* oss_3_0_d.h:303 */
#define SDMA0_STATUS_REG__IDLE_MASK          0x00000001u  /* oss_3_0_sh_mask.h:1513 */
#define SDMA0_STATUS_IDLE                    SDMA0_STATUS_REG__IDLE_MASK

/* SDMA0_GFX_RB_CNTL bits */
#define SDMA0_GFX_RB_CNTL__RB_ENABLE_MASK    0x00000001u  /* oss_3_0_sh_mask.h:1901 */
#define SDMA0_GFX_RB_CNTL__RB_SIZE_MASK      0x0000003eu  /* oss_3_0_sh_mask.h:1903 */
#define SDMA0_GFX_RB_CNTL__RB_SIZE__SHIFT    0x00000001u  /* oss_3_0_sh_mask.h:1904 */
#define SDMA_RB_ENABLE                       SDMA0_GFX_RB_CNTL__RB_ENABLE_MASK
#define SDMA_RB_SIZE(n)                      (((n) << SDMA0_GFX_RB_CNTL__RB_SIZE__SHIFT) & SDMA0_GFX_RB_CNTL__RB_SIZE_MASK)  /* log2(ring dwords) */

/* -------------------------------------------------------------------------
 * SMU 7.1.3 (smu_7_1_3_d.h) -- checking firmware state
 * ------------------------------------------------------------------------ */
#define mmSMC_MESSAGE_0                          0x0094u  /* smu_7_1_3_d.h:97 */
#define mmSMC_MSG_ARG_0                          0x00a4u  /* smu_7_1_3_d.h:113 */
#define mmSMC_RESP_0                             0x0095u  /* smu_7_1_3_d.h:98 */
#define mmSMC_IND_INDEX_0                        0x0080u  /* smu_7_1_3_d.h:78 */
#define mmSMC_IND_DATA_0                         0x0081u  /* smu_7_1_3_d.h:79 */

/* SMC firmware header: lives in SMC address space (read through SMC_IND_INDEX_0/DATA_0),
 * not in VRAM.  Layout is struct SMU7_Firmware_Header (smu7.h:138-140): Digest[5], then Version. */
#define SMU7_FIRMWARE_HEADER_LOCATION        0x20000u  /* smu7.h:166 */
#define SMU7_FIRMWARE_HEADER_Version_offset  (5u * 4u)  /* smu7.h:139-140: Version follows Digest[5] */
/* SMC response codes (smu7_ppsmc.h:83).  No message is sent by amd_smu_check: PPSMC_MSG_Test
 * is Linux's "tell the SMC the header offset" init message (smu7_smumgr.c:213), not a ping. */
#define PPSMC_Result_OK                      0x0001u  /* smu7_ppsmc.h:83 */

/* RSMU interrupt / power state */
#define mmSMU_IND_INDEX_1                        0x01a8u  /* smu_7_1_3_d.h:161 */
#define mmSMU_IND_DATA_1                         0x01a9u  /* smu_7_1_3_d.h:162 */

/* -------------------------------------------------------------------------
 * Compute HQD / MQD (gfx_8_0_d.h)
 * ------------------------------------------------------------------------ */
#define mmCP_HQD_ACTIVE                          0x3247u  /* gfx_8_0_d.h:632 */
#define mmCP_HQD_PQ_BASE                         0x324du  /* gfx_8_0_d.h:638 */
#define mmCP_HQD_PQ_BASE_HI                      0x324eu  /* gfx_8_0_d.h:639 */
#define mmCP_HQD_PQ_RPTR                         0x324fu  /* gfx_8_0_d.h:640 */
#define mmCP_HQD_PQ_RPTR_REPORT_ADDR             0x3250u  /* gfx_8_0_d.h:641 */
#define mmCP_HQD_PQ_RPTR_REPORT_ADDR_HI          0x3251u  /* gfx_8_0_d.h:642 */
#define mmCP_HQD_PQ_WPTR_POLL_ADDR               0x3252u  /* gfx_8_0_d.h:643 */
#define mmCP_HQD_PQ_WPTR_POLL_ADDR_HI            0x3253u  /* gfx_8_0_d.h:644 */
#define mmCP_HQD_PQ_CONTROL                      0x3256u  /* gfx_8_0_d.h:647 */
#define mmCP_HQD_PQ_WPTR                         0x3255u  /* gfx_8_0_d.h:646 */
#define mmCP_HQD_PIPE_PRIORITY                   0x324au  /* gfx_8_0_d.h:635 */
#define mmCP_HQD_QUEUE_PRIORITY                  0x324bu  /* gfx_8_0_d.h:636 */
#define mmCP_HQD_QUANTUM                         0x324cu  /* gfx_8_0_d.h:637 */
#define mmCP_HQD_IB_BASE_ADDR                    0x3257u  /* gfx_8_0_d.h:648 */
#define mmCP_HQD_IB_BASE_ADDR_HI                 0x3258u  /* gfx_8_0_d.h:649 */
#define mmCP_HQD_IB_CONTROL                      0x325au  /* gfx_8_0_d.h:651 */
#define mmCP_HQD_DEQUEUE_REQUEST                 0x325du  /* gfx_8_0_d.h:654 */
#define mmCP_HQD_EOP_BASE_ADDR                   0x326au  /* gfx_8_0_d.h:670 */
#define mmCP_HQD_EOP_BASE_ADDR_HI                0x326bu  /* gfx_8_0_d.h:671 */
#define mmCP_HQD_EOP_CONTROL                     0x326cu  /* gfx_8_0_d.h:672 */

/* GRBM_GFX_INDEX -- selects shader engine / array / instance for register writes.  It does NOT select a
 * compute queue: Linux selects the MEC pipe/queue through SRBM_GFX_CNTL (oss_3_0_d.h:88; vi_srbm_select). */
#define mmGRBM_GFX_INDEX                         0xc200u  /* gfx_8_0_d.h:871 */
#define GRBM_GFX_INDEX__INSTANCE_INDEX_MASK            0x000000ffu  /* gfx_8_0_sh_mask.h:5685 */
#define GRBM_GFX_INDEX__SH_INDEX_MASK                  0x0000ff00u  /* gfx_8_0_sh_mask.h:5687 */
#define GRBM_GFX_INDEX__SH_INDEX__SHIFT                0x00000008u  /* gfx_8_0_sh_mask.h:5688 */
#define GRBM_GFX_INDEX__SE_INDEX_MASK                  0x00ff0000u  /* gfx_8_0_sh_mask.h:5689 */
#define GRBM_GFX_INDEX__SE_INDEX__SHIFT                0x00000010u  /* gfx_8_0_sh_mask.h:5690 */
#define GRBM_GFX_INDEX__SH_BROADCAST_WRITES_MASK       0x20000000u  /* gfx_8_0_sh_mask.h:5691 */
#define GRBM_GFX_INDEX__INSTANCE_BROADCAST_WRITES_MASK 0x40000000u  /* gfx_8_0_sh_mask.h:5693 */
#define GRBM_GFX_INDEX__SE_BROADCAST_WRITES_MASK       0x80000000u  /* gfx_8_0_sh_mask.h:5695 */
#define GRBM_GFX_INDEX_INSTANCE_INDEX(n)     ((n) & GRBM_GFX_INDEX__INSTANCE_INDEX_MASK)
#define GRBM_GFX_INDEX_SH_INDEX(n)           (((n) << GRBM_GFX_INDEX__SH_INDEX__SHIFT) & GRBM_GFX_INDEX__SH_INDEX_MASK)
#define GRBM_GFX_INDEX_SE_INDEX(n)           (((n) << GRBM_GFX_INDEX__SE_INDEX__SHIFT) & GRBM_GFX_INDEX__SE_INDEX_MASK)
#define GRBM_GFX_INDEX_SH_BROADCAST          GRBM_GFX_INDEX__SH_BROADCAST_WRITES_MASK
#define GRBM_GFX_INDEX_INSTANCE_BROADCAST    GRBM_GFX_INDEX__INSTANCE_BROADCAST_WRITES_MASK
#define GRBM_GFX_INDEX_SE_BROADCAST          GRBM_GFX_INDEX__SE_BROADCAST_WRITES_MASK
/* PM4 packet headers live in amd_ucode.c (PM4_PACKET3, checked against gfx_v8_0.c:891-892 by
 * tests/test_amd_gpu_ring.c).  The unused NOP/WRITE_DATA macros that were here were wrong
 * (count field in the wrong bits, DST_SEL 2 instead of 5) and are removed. */
