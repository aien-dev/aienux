/* gsp_boot_trace: every register access ck_gsp_mmio_boot and ck_gsp_mmio_notify make against the simulated SEC2 +
 * GSP, in a fixed list of scenarios, printed one per line. Built twice (hand gsp_mmio.c, generated gsb_contract.c);
 * the two outputs must be byte-identical (make gsp-contract-trace). Host only; nothing here has run on the chip. */
#include "gsp_mmio.h"
#include "gsp_sim.h"
#include <stdio.h>
#include <string.h>

#define ARGS_PA 0x0000000123456000ull

static struct ck_gspsim sim;
static uint8_t pkt[1024];

static void nop(void *ctx)
{
    (void)ctx;
}

static void scenario(const char *name, struct ck_gspsim_cfg c, uint32_t len, uint32_t budget, uint32_t hdr_ok)
{
    static const uint32_t hdr[3] = {0x00000001u, 0x1410DE7Eu, 1u | (860u << 16)};
    for (uint32_t i = 0; i < sizeof(pkt); i++)
        pkt[i] = (uint8_t)(i * 13u + 5u);
    if (hdr_ok)
        memcpy(pkt, hdr, sizeof(hdr));
    struct ck_gsp_mmio_in in = {pkt, len, ARGS_PA, 0x0005f3a1u, budget, budget, budget, budget, nop, 0};
    struct ck_gsp_mmio_out o;
    ck_gspsim_init(&sim, &c);
    struct ck_gpu_regs r = ck_gspsim_regs(&sim);
    int rc = ck_gsp_mmio_boot(&r, &in, &o);
    if (rc == CK_GSP_OK)
        ck_gsp_mmio_notify(&r);
    printf("== %s rc=%d stage=%d polls=%u,%u,%u,%u dwords=%u ememc=%#x sec2=%#x,%#x hwcfg2=%#x gsp=%#x,%#x cpuctl=%#x"
           " nlog=%u violations=%u phase=%u\n",
           name, rc, o.stage, o.polls[0], o.polls[1], o.polls[2], o.polls[3], o.dwords, o.ememc_after, o.sec2_mb0,
           o.sec2_mb1, o.hwcfg2, o.gsp_mb0, o.gsp_mb1, o.cpuctl, sim.nlog, sim.violations, sim.phase);
    for (uint32_t i = 0; i < sim.nlog && i < CK_GSPSIM_LOG_MAX; i++)
        printf("%c %06x %08x\n", sim.log[i].write ? 'W' : 'R', sim.log[i].off, sim.log[i].val);
}

int main(void)
{
    const struct ck_gspsim_cfg base = {0, 3, 4, 2, 5, 0, 0, 0, ARGS_PA, 0};
    struct ck_gspsim_cfg c;

    scenario("happy", base, 868, 100, 1);
    c = base; c.busy_reads = 3;
    scenario("busy3", c, 868, 100, 1);
    c = base; c.busy_reads = CK_GSPSIM_NO_END;
    scenario("busy-forever", c, 868, 5, 1);
    c = base; c.cmd_reads = CK_GSPSIM_NO_END;
    scenario("sec2-never", c, 868, 5, 1);
    c = base; c.mask_reads = CK_GSPSIM_NO_END;
    scenario("mask-never", c, 868, 5, 1);
    c = base; c.hwcfg2_zero_reads = 2;
    scenario("hwcfg2-zero2", c, 868, 100, 1);
    c = base; c.args_reads = CK_GSPSIM_NO_END;
    scenario("args-never", c, 868, 6, 1);
    c = base; c.lockdown_reads = CK_GSPSIM_NO_END;
    scenario("lockdown-never", c, 868, 6, 1);
    c = base; c.fmc_error = 0x23;
    scenario("fmc-error", c, 868, 100, 1);
    c = base; c.riscv_dead = 1;
    scenario("riscv-dead", c, 868, 100, 1);
    c = base; c.emem_no_inc = 1;
    scenario("emem-stuck", c, 868, 100, 1);
    scenario("bad-header", base, 868, 3, 0);
    scenario("odd-7", base, 7, 3, 1);
    scenario("full-1024", base, 1024, 3, 1);
    scenario("too-long", base, 1025, 3, 1);
    scenario("budget0", base, 868, 0, 1);
    return 0;
}
