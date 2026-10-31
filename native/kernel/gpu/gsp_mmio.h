/* gsp_mmio.h -- the register-level GSP start-up on GB10B (L332 rung 1, L346), freestanding.
 *
 * What NVIDIA's driver (open-gpu-kernel-modules 580.173.02) does on BAR0 to start GSP-RM on the GB10B
 * once the boot package is built in memory (L192 gb10_gsp_boot.c builds it; this file only talks to
 * the chip). GB10B has no FSP: SEC2 boots the GSP-FMC (kgspBootstrap_GH100, kernel_gsp_gh100.c:833-995,
 * SEC2 branch :868-872; GB10B timer setup is read-only, timer_gb10b.c:46-80, so there is no other
 * register write before it):
 *
 *   1. wait until SEC2 EMEM channel 0 is empty: QUEUE_HEAD(0) == QUEUE_TAIL(0)
 *        (ksec2CanSendPacket_GB10B, kernel_sec2_gb10b.c:343-357; polled by ksec2PollForCanSend)
 *   2. EMEMC(0) = offset 0, auto-increment on write          (_ksec2ConfigEmemc_GB10B :118-154, ksec2SendPacket_GB10B :700-733, call :723)
 *   3. EMEMD(0) = the packet, one dword at a time, a short tail zero-padded (_ksec2WriteToEmem_GB10B :166-213)
 *   4. EMEMC(0) read back: the write pointer moved by exactly the dwords written (:204-210)
 *   5. QUEUE_TAIL(0) = padded size - 4, THEN QUEUE_HEAD(0) = 0 (HEAD interrupts SEC2) (_ksec2UpdateQueueHeadTail_GB10B :72-84, called :730)
 *   6. wait until SEC2 MAILBOX0 no longer reads 0xc001cafe (command handled)  (:285-311, called from ksec2SendBootCommands_GB10B :640)
 *   7. wait until GSP HWCFG2 reads non-zero and not 0xBADF41xx (GSP target mask released by SEC2)
 *        (_ksec2IsGspTargetMaskReleased :264-283, ksec2WaitForGspTargetMaskReleased_GB10B :367-379)
 *   8. wait until GSP HWCFG2.RISCV_BR_PRIV_LOCKDOWN (bit 13) is UNLOCK or GSP MAILBOX0 is non-zero,
 *      where MAILBOX0:MAILBOX1 equal to the FMC boot-argument address means "not read yet, keep waiting"
 *        (_kgspLockdownReleasedOrFmcError, kernel_gsp_gh100.c:492-533)
 *   9. GSP MAILBOX0 non-zero after that = the FMC reported an error: stop (:940-946)
 *  10. GSP FALCON_OS = the GspRmBoot descriptor's appVersion (:954-956)
 *  11. RISC-V CPUCTL.ACTIVE_STAT (bit 7) must read ACTIVE (kflcnIsRiscvActive_GA102, kernel_falcon_ga102.c:47-55)
 * then the host links the status queue and waits for GSP_EV_INIT_DONE over the message queue (L192
 * gsp_mq_recv); every later command is announced by GSP QUEUE_HEAD(0) = 0 (kgspSetCmdQueueHead_TU102,
 * kernel_gsp_tu102.c:343-355; GB10B takes that HAL, g_kernel_gsp_nvoc.c:664-680).
 *
 * Polling only, no interrupts. Each wait has a poll budget; `pause` (optional) runs between polls so
 * the caller sets the time per poll (udelay on Linux, nothing on the simulator).
 * Nothing here has run on the chip: register behaviour beyond the driver source is UNVERIFIED. */
#ifndef CK_GSP_MMIO_H
#define CK_GSP_MMIO_H

#include <stdint.h>
#include "gmmu.h" /* struct ck_gpu_regs */

/* BAR0 offsets, GB10B (published/blackwell/gb10b/dev_sec_pri.h:27-56; dev_gsp.h:27 NV_PGSP 0x110000;
 * hopper/gh100/dev_falcon_v4.h:27,30,34,79 relative to NV_PGSP; NV_FALCON2_GSP_BASE 0x111000 +
 * hopper/gh100/dev_riscv_pri.h:58,68 NV_PRISCV_RISCV_CPUCTL 0x388; gb20b/dev_gsp.h:33 QUEUE_HEAD). */
#define CK_GSP_SEC2_MAILBOX0 0x00840040u
#define CK_GSP_SEC2_MAILBOX1 0x00840044u
#define CK_GSP_SEC2_EMEMC0 0x00840ac0u
#define CK_GSP_SEC2_EMEMD0 0x00840ac4u
#define CK_GSP_SEC2_QHEAD0 0x00840c00u
#define CK_GSP_SEC2_QTAIL0 0x00840c04u
#define CK_GSP_FLCN_MAILBOX0 0x00110040u
#define CK_GSP_FLCN_MAILBOX1 0x00110044u
#define CK_GSP_FLCN_OS 0x00110080u
#define CK_GSP_FLCN_HWCFG2 0x001100f4u
#define CK_GSP_QHEAD0 0x00110c00u
#define CK_GSP_RISCV_CPUCTL 0x00111388u

#define CK_GSP_EMEMC_AINCW (1u << 24)
#define CK_GSP_EMEM_DW_PER_BLK 64u
#define CK_GSP_EMEM_CHANNEL_BYTES 1024u          /* SEC2_EMEM_CHANNEL_RM_SIZE, kernel_sec2_gb10b.c:39 */
#define CK_GSP_SEC2_CMD_BUSY 0xc001cafeu         /* cmdHandlingState :292 */
#define CK_GSP_PRIV_LOCKED 0xBADF4100u           /* privErrTargetLocked :271 */
#define CK_GSP_PRIV_LOCKED_MASK 0xFFFFFF00u
#define CK_GSP_HWCFG2_LOCKDOWN (1u << 13)
#define CK_GSP_CPUCTL_ACTIVE (1u << 7)

/* Where the boot stopped. 0 = GSP-RM's RISC-V core runs (step 11 passed). */
enum ck_gsp_mmio_err {
    CK_GSP_OK = 0,
    CK_GSP_E_ARG = -1,      /* packet empty, over 1 KiB, NULL */
    CK_GSP_E_BUSY = -2,     /* step 1: SEC2 channel never emptied */
    CK_GSP_E_EMEM = -3,     /* step 4: write pointer did not move by the dwords written */
    CK_GSP_E_SEC2 = -4,     /* step 6: SEC2 never handled the command */
    CK_GSP_E_MASK = -5,     /* step 7: target mask never released */
    CK_GSP_E_LOCKDOWN = -6, /* step 8: neither lockdown release nor an FMC code */
    CK_GSP_E_FMC = -7,      /* step 9: FMC error code in GSP MAILBOX0 (out.gsp_mb0) */
    CK_GSP_E_RISCV = -8,    /* step 11: RISC-V core not active */
};

struct ck_gsp_mmio_in {
    const uint8_t *packet;   /* the SEC2 EMEM packet (MCTP + NVDM COT header + payload, L192 plan.cot_packet) */
    uint32_t len;            /* bytes, 1..1024 (L192: 868) */
    uint64_t fmc_args_pa;    /* GSP_FMC_BOOT_PARAMS address the COT payload points at */
    uint32_t app_version;    /* RM_RISCV_UCODE_DESC.appVersion of GspRmBoot_GB10B */
    /* poll budgets for steps 1, 6, 7, 8 (0 = check once) */
    uint32_t polls_can_send, polls_cmd, polls_mask, polls_lockdown;
    void (*pause)(void *ctx);
    void *pause_ctx;
};

struct ck_gsp_mmio_out {
    int32_t stage;                 /* enum ck_gsp_mmio_err */
    uint32_t polls[4];             /* polls used by steps 1, 6, 7, 8 */
    uint32_t dwords, ememc_after;  /* step 3/4 */
    uint32_t sec2_mb0, sec2_mb1;   /* after step 6 (or at its timeout) */
    uint32_t hwcfg2, gsp_mb0, gsp_mb1, cpuctl;
};

/* Steps 1-11. Returns out->stage. */
int ck_gsp_mmio_boot(const struct ck_gpu_regs *r, const struct ck_gsp_mmio_in *in, struct ck_gsp_mmio_out *out);
/* Announce new command-queue elements to GSP-RM (after the ring write and its fence). */
void ck_gsp_mmio_notify(const struct ck_gpu_regs *r);

#endif
