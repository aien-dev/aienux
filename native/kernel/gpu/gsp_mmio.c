/* gsp_mmio.c -- the register-level GSP start-up on GB10B (see gsp_mmio.h for the steps and sources). */
#include "gsp_mmio.h"

static uint32_t rd(const struct ck_gpu_regs *r, uint32_t off)
{
    return r->rd(r->ctx, off);
}

static void wr(const struct ck_gpu_regs *r, uint32_t off, uint32_t v)
{
    r->wr(r->ctx, off, v);
}

static void between_polls(const struct ck_gsp_mmio_in *in)
{
    if (in->pause)
        in->pause(in->pause_ctx);
}

static uint32_t emem_dwords(uint32_t ememc)
{
    return ((ememc >> 8) & 0xFFu) * CK_GSP_EMEM_DW_PER_BLK + ((ememc >> 2) & 0x3Fu);
}

static int mask_released(uint32_t hwcfg2)
{
    return hwcfg2 != 0 && (hwcfg2 & CK_GSP_PRIV_LOCKED_MASK) != CK_GSP_PRIV_LOCKED;
}

/* Step 8 condition. Reads MAILBOX0, MAILBOX1 only on a match, then HWCFG2, in the driver's order. */
static int lockdown_done(const struct ck_gpu_regs *r, const struct ck_gsp_mmio_in *in, struct ck_gsp_mmio_out *o)
{
    o->gsp_mb0 = rd(r, CK_GSP_FLCN_MAILBOX0);
    if (o->gsp_mb0 != 0 && o->gsp_mb0 == (uint32_t)in->fmc_args_pa) {
        o->gsp_mb1 = rd(r, CK_GSP_FLCN_MAILBOX1);
        if (o->gsp_mb1 == (uint32_t)(in->fmc_args_pa >> 32))
            return 0; /* boot arguments not read by the FMC yet */
    }
    o->hwcfg2 = rd(r, CK_GSP_FLCN_HWCFG2);
    return !(o->hwcfg2 & CK_GSP_HWCFG2_LOCKDOWN) || o->gsp_mb0 != 0;
}

int ck_gsp_mmio_boot(const struct ck_gpu_regs *r, const struct ck_gsp_mmio_in *in, struct ck_gsp_mmio_out *o)
{
    uint32_t i, n;

    for (i = 0; i < sizeof(*o); i++)
        ((uint8_t *)o)[i] = 0;
    o->stage = CK_GSP_E_ARG;
    if (!r || !in || !in->packet || in->len == 0 || in->len > CK_GSP_EMEM_CHANNEL_BYTES)
        return o->stage;

    /* 1 */
    for (n = 0;; n++) {
        uint32_t head = rd(r, CK_GSP_SEC2_QHEAD0); /* HEAD then TAIL, as _ksec2GetQueueHeadTail_GB10B :103-104 */
        if (head == rd(r, CK_GSP_SEC2_QTAIL0))
            break;
        if (n >= in->polls_can_send) {
            o->polls[0] = n;
            return o->stage = CK_GSP_E_BUSY;
        }
        between_polls(in);
    }
    o->polls[0] = n;

    /* 2, 3, 4 */
    wr(r, CK_GSP_SEC2_EMEMC0, CK_GSP_EMEMC_AINCW);
    uint32_t start = emem_dwords(rd(r, CK_GSP_SEC2_EMEMC0));
    for (i = 0; i + 4 <= in->len; i += 4)
        wr(r, CK_GSP_SEC2_EMEMD0, (uint32_t)in->packet[i] | (uint32_t)in->packet[i + 1] << 8 |
                                      (uint32_t)in->packet[i + 2] << 16 | (uint32_t)in->packet[i + 3] << 24);
    if (i < in->len) {
        uint32_t v = 0;
        for (uint32_t k = 0; i + k < in->len; k++)
            v |= (uint32_t)in->packet[i + k] << (8 * k);
        wr(r, CK_GSP_SEC2_EMEMD0, v);
    }
    o->dwords = (in->len + 3) / 4;
    o->ememc_after = rd(r, CK_GSP_SEC2_EMEMC0);
    if (emem_dwords(o->ememc_after) - start != o->dwords)
        return o->stage = CK_GSP_E_EMEM;

    /* 5: TAIL first, HEAD last (HEAD interrupts SEC2) */
    wr(r, CK_GSP_SEC2_QTAIL0, o->dwords * 4 - 4);
    wr(r, CK_GSP_SEC2_QHEAD0, 0);

    /* 6 */
    for (n = 0;; n++) {
        o->sec2_mb0 = rd(r, CK_GSP_SEC2_MAILBOX0);
        if (o->sec2_mb0 != CK_GSP_SEC2_CMD_BUSY)
            break;
        if (n >= in->polls_cmd) {
            o->polls[1] = n;
            o->sec2_mb1 = rd(r, CK_GSP_SEC2_MAILBOX1);
            return o->stage = CK_GSP_E_SEC2;
        }
        between_polls(in);
    }
    o->polls[1] = n;

    /* 7 */
    for (n = 0;; n++) {
        o->hwcfg2 = rd(r, CK_GSP_FLCN_HWCFG2);
        if (mask_released(o->hwcfg2))
            break;
        if (n >= in->polls_mask) {
            o->polls[2] = n;
            return o->stage = CK_GSP_E_MASK;
        }
        between_polls(in);
    }
    o->polls[2] = n;

    /* 8 */
    for (n = 0;; n++) {
        if (lockdown_done(r, in, o))
            break;
        if (n >= in->polls_lockdown) {
            o->polls[3] = n;
            o->gsp_mb1 = rd(r, CK_GSP_FLCN_MAILBOX1);
            return o->stage = CK_GSP_E_LOCKDOWN;
        }
        between_polls(in);
    }
    o->polls[3] = n;

    /* 9 */
    o->gsp_mb0 = rd(r, CK_GSP_FLCN_MAILBOX0);
    if (o->gsp_mb0 != 0)
        return o->stage = CK_GSP_E_FMC;

    /* 10, 11 */
    wr(r, CK_GSP_FLCN_OS, in->app_version);
    o->cpuctl = rd(r, CK_GSP_RISCV_CPUCTL);
    if (!(o->cpuctl & CK_GSP_CPUCTL_ACTIVE))
        return o->stage = CK_GSP_E_RISCV;
    return o->stage = CK_GSP_OK;
}

void ck_gsp_mmio_notify(const struct ck_gpu_regs *r)
{
    wr(r, CK_GSP_QHEAD0, 0);
}
