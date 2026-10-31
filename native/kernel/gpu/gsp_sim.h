/* gsp_sim.h -- a simulated GB10B SEC2 + GSP falcon for host tests of gsp_mmio (L332, L346). NOT the chip.
 *
 * Models only what the driver source says the host sees on BAR0 during the GSP start-up: SEC2 EMEM
 * channel 0 (1 KiB, EMEMC/EMEMD with write auto-increment), its queue HEAD/TAIL and MAILBOX0/1, and the
 * GSP falcon's MAILBOX0/1, HWCFG2 (target-mask pattern, lockdown bit 13), FALCON_OS, RISC-V CPUCTL and
 * QUEUE_HEAD(0). Timing is counted in register reads of the polled register. Every access is logged in
 * order; accesses the driver never makes, and orders it never uses, are counted as violations.
 * Choices the source does not settle are marked SIM-CHOICE. */
#ifndef CK_GSP_SIM_H
#define CK_GSP_SIM_H

#include <stdint.h>
#include "gmmu.h"

#define CK_GSPSIM_LOG_MAX 1024u
#define CK_GSPSIM_NO_END 0xFFFFFFFFu /* a wait that never ends */

struct ck_gspsim_cfg {
    uint32_t busy_reads;      /* HEAD reads during which the SEC2 channel still holds an old message */
    uint32_t cmd_reads;       /* MAILBOX0 reads that return 0xc001cafe after the doorbell */
    uint32_t mask_reads;      /* HWCFG2 reads that return 0xBADF41A7 after the command is handled */
    uint32_t args_reads;      /* GSP MAILBOX0 reads that still return the boot-argument address */
    uint32_t lockdown_reads;  /* HWCFG2 reads with lockdown set after the target mask is released */
    uint32_t fmc_error;       /* non-zero: the FMC writes this code to GSP MAILBOX0 instead of releasing */
    uint32_t riscv_dead;      /* RISC-V never reports ACTIVE */
    uint32_t emem_no_inc;     /* hardware fault: EMEMC does not advance */
    uint64_t fmc_args_pa;     /* what SEC2 stuffs in GSP MAILBOX0/1 (SIM-CHOICE: the driver tolerates it) */
    uint32_t hwcfg2_zero_reads; /* HWCFG2 reads that return 0 before the target-mask pattern (SIM-CHOICE) */
};

struct ck_gspsim_ev {
    uint32_t off, val;
    uint8_t write;
};

struct ck_gspsim {
    struct ck_gspsim_cfg cfg;
    uint32_t emem[256];
    uint32_t ememc, qhead, qtail;
    uint32_t sec2_mb0, sec2_mb1;
    uint32_t gsp_mb0, gsp_mb1, hwcfg2, os, cpuctl, gsp_qhead_writes;
    /* phase: 0 idle, 1 command posted, 2 handled (mask), 3 FMC running (lockdown), 4 released, 9 rejected */
    uint32_t phase, left, zero_left;
    uint32_t packet_ok, os_written_at_phase;
    struct ck_gspsim_ev log[CK_GSPSIM_LOG_MAX];
    uint32_t nlog, violations;
};

void ck_gspsim_init(struct ck_gspsim *s, const struct ck_gspsim_cfg *cfg);
struct ck_gpu_regs ck_gspsim_regs(struct ck_gspsim *s);

#endif
