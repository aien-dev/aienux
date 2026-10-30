/* gsp_sim.c -- simulated GB10B SEC2 + GSP falcon (see gsp_sim.h). NOT the chip. */
#include "gsp_sim.h"
#include "gsp_mmio.h"

#define PHASE_IDLE 0u
#define PHASE_POSTED 1u
#define PHASE_MASK 2u
#define PHASE_FMC 3u
#define PHASE_UP 4u
#define PHASE_REJECTED 9u

static void logev(struct ck_gspsim *s, uint32_t off, uint32_t val, uint8_t write)
{
    if (s->nlog < CK_GSPSIM_LOG_MAX) {
        s->log[s->nlog].off = off;
        s->log[s->nlog].val = val;
        s->log[s->nlog].write = write;
    }
    s->nlog++;
}

void ck_gspsim_init(struct ck_gspsim *s, const struct ck_gspsim_cfg *cfg)
{
    for (uint32_t i = 0; i < sizeof(*s); i++)
        ((uint8_t *)s)[i] = 0;
    s->cfg = *cfg;
    /* An old message still in the channel: HEAD != TAIL until busy_reads HEAD reads have passed. */
    if (cfg->busy_reads) {
        s->qhead = 0;
        s->qtail = 16;
    }
    s->hwcfg2 = CK_GSP_PRIV_LOCKED | 0xA7u; /* SIM-CHOICE: target locked to SEC2; low byte = "extra error information" (:272) */
    s->sec2_mb0 = 0;                     /* SIM-CHOICE: idle SEC2 mailbox value unknown; 0 */
    s->left = cfg->busy_reads;
}

/* SEC2 checks the packet the driver's way round: MCTP dword 1 (the GB10B HAL constant, ksec2CreateMctpHeader_539ab4), NVDM vendor PCI 0x10de type COT, payload
 * version 1, size 860 (kernel_sec2.c:305-325, fsp_mctp_format.h, kern_fsp_cot_payload.h). Anything else:
 * SIM-CHOICE, SEC2 consumes it and never releases the target mask (a refused chain of trust). */
static uint32_t packet_ok(const struct ck_gspsim *s)
{
    return s->qtail == 864u && s->emem[0] == 0x00000001u && s->emem[1] == 0x1410DE7Eu &&
           (s->emem[2] & 0xFFFFu) == 1u && (s->emem[2] >> 16) == 860u;
}

static uint32_t sim_rd(void *ctx, uint32_t off)
{
    struct ck_gspsim *s = ctx;
    uint32_t v = 0;

    switch (off) {
    case CK_GSP_SEC2_QHEAD0:
        if (s->phase == PHASE_IDLE && s->qhead != s->qtail) {
            if (s->left == 0)
                s->qhead = s->qtail; /* busy_reads busy reads have passed: SEC2 drained the channel */
            else if (s->left != CK_GSPSIM_NO_END)
                s->left--;
        }
        v = s->qhead;
        break;
    case CK_GSP_SEC2_QTAIL0: v = s->qtail; break;
    case CK_GSP_SEC2_EMEMC0: v = s->ememc; break;
    case CK_GSP_SEC2_EMEMD0: v = s->emem[(((s->ememc >> 8) & 0xFFu) * 64u + ((s->ememc >> 2) & 0x3Fu)) & 255u]; break;
    case CK_GSP_SEC2_MAILBOX0:
        if (s->phase == PHASE_POSTED) {
            if (s->left == 0) {
                s->sec2_mb0 = 0;
                s->qhead = s->qtail; /* consumed */
                if (s->packet_ok) {
                    s->phase = PHASE_MASK;
                    s->left = s->cfg.mask_reads;
                    s->zero_left = s->cfg.hwcfg2_zero_reads;
                } else {
                    s->phase = PHASE_REJECTED;
                }
            } else if (s->left != CK_GSPSIM_NO_END) {
                s->left--;
            }
        }
        v = s->sec2_mb0;
        break;
    case CK_GSP_SEC2_MAILBOX1: v = s->sec2_mb1; break;
    case CK_GSP_FLCN_HWCFG2:
        if (s->phase == PHASE_MASK && s->zero_left) {
            if (s->zero_left != CK_GSPSIM_NO_END)
                s->zero_left--;
            logev(s, off, 0, 0);
            return 0; /* the driver's mask check treats 0 as not released (kernel_sec2_gb10b.c:282) */
        }
        if (s->phase == PHASE_MASK) {
            if (s->left == 0) {
                s->phase = PHASE_FMC;
                s->hwcfg2 = CK_GSP_HWCFG2_LOCKDOWN | 0x5u; /* SIM-CHOICE: other HWCFG2 bits */
                s->gsp_mb0 = (uint32_t)s->cfg.fmc_args_pa;
                s->gsp_mb1 = (uint32_t)(s->cfg.fmc_args_pa >> 32);
                s->left = s->cfg.lockdown_reads;
            } else if (s->left != CK_GSPSIM_NO_END) {
                s->left--;
            }
        } else if (s->phase == PHASE_FMC && s->gsp_mb0 != (uint32_t)s->cfg.fmc_args_pa) {
            /* the FMC has read its arguments: count down to release (or the error code) */
            if (s->left == 0) {
                if (s->cfg.fmc_error) {
                    s->gsp_mb0 = s->cfg.fmc_error;
                } else {
                    s->hwcfg2 &= ~CK_GSP_HWCFG2_LOCKDOWN;
                    s->phase = PHASE_UP;
                    if (!s->cfg.riscv_dead)
                        s->cpuctl = CK_GSP_CPUCTL_ACTIVE;
                }
            } else if (s->left != CK_GSPSIM_NO_END) {
                s->left--;
            }
        }
        v = s->hwcfg2;
        break;
    case CK_GSP_FLCN_MAILBOX0:
        if (s->phase == PHASE_FMC && s->gsp_mb0 == (uint32_t)s->cfg.fmc_args_pa) {
            if (s->cfg.args_reads == CK_GSPSIM_NO_END) {
            } else if (s->cfg.args_reads == 0) {
                s->gsp_mb0 = s->gsp_mb1 = 0;
            } else {
                s->cfg.args_reads--;
            }
        }
        v = s->gsp_mb0;
        break;
    case CK_GSP_FLCN_MAILBOX1: v = s->gsp_mb1; break;
    case CK_GSP_FLCN_OS: v = s->os; break;
    case CK_GSP_RISCV_CPUCTL: v = s->cpuctl; break;
    default: s->violations++; break;
    }
    logev(s, off, v, 0);
    return v;
}

static void sim_wr(void *ctx, uint32_t off, uint32_t val)
{
    struct ck_gspsim *s = ctx;
    logev(s, off, val, 1);
    switch (off) {
    case CK_GSP_SEC2_EMEMC0: s->ememc = val; break;
    case CK_GSP_SEC2_EMEMD0: {
        uint32_t dw = ((s->ememc >> 8) & 0xFFu) * 64u + ((s->ememc >> 2) & 0x3Fu);
        if (!(s->ememc & CK_GSP_EMEMC_AINCW) || dw >= 256u || s->phase != PHASE_IDLE) {
            s->violations++;
            break;
        }
        s->emem[dw] = val;
        if (!s->cfg.emem_no_inc) {
            dw++;
            s->ememc = (s->ememc & ~0xFFFCu) | ((dw / 64u) & 0xFFu) << 8 | (dw % 64u) << 2;
        }
        break;
    }
    case CK_GSP_SEC2_QTAIL0:
        if (s->phase != PHASE_IDLE || s->qhead != s->qtail)
            s->violations++; /* the driver writes only into an empty channel */
        s->qtail = val;
        break;
    case CK_GSP_SEC2_QHEAD0:
        /* the doorbell: TAIL must already point at the last dword of a message starting at 0 */
        if (s->phase != PHASE_IDLE || val != 0 || s->qtail == s->qhead)
            s->violations++;
        s->qhead = val;
        if (s->phase == PHASE_IDLE) {
            s->packet_ok = packet_ok(s);
            s->phase = PHASE_POSTED;
            s->sec2_mb0 = CK_GSP_SEC2_CMD_BUSY;
            s->left = s->cfg.cmd_reads;
        }
        break;
    case CK_GSP_FLCN_OS:
        s->os = val;
        s->os_written_at_phase = s->phase;
        if (s->phase != PHASE_UP)
            s->violations++; /* FALCON_OS before lockdown release */
        break;
    case CK_GSP_QHEAD0:
        s->gsp_qhead_writes++;
        if (val != 0)
            s->violations++;
        break;
    default: s->violations++; break;
    }
}

struct ck_gpu_regs ck_gspsim_regs(struct ck_gspsim *s)
{
    struct ck_gpu_regs r = {sim_rd, sim_wr, s};
    return r;
}
