/* test_gsp_mmio: the GB10B GSP start-up sequence (gpu/gsp_mmio.c) against the simulated SEC2 + GSP falcon
 * (gpu/gsp_sim.c). Host only; nothing here has run on the chip. */
#include "gsp_mmio.h"
#include "gsp_sim.h"
#include "ck_test.h"
#include <string.h>

#define ARGS_PA 0x0000000123456000ull
#define APP_VERSION 0x0005f3a1u

static uint8_t pkt[868];
static struct ck_gspsim sim;
static struct ck_gpu_regs regs;
static unsigned pauses;

static void count_pause(void *ctx)
{
    (void)ctx;
    pauses++;
}

/* A well-formed SEC2 boot packet, laid out like L192 gsp_boot_build: MCTP dword, NVDM COT dword, then
 * NVDM_PAYLOAD_COT (version 1, size 860, ...). Payload bytes past the header are a pattern. */
static void make_packet(void)
{
    static const uint32_t hdr[3] = {0x00000001u, 0x1410DE7Eu, 1u | (860u << 16)};
    memcpy(pkt, hdr, sizeof(hdr));
    for (unsigned i = sizeof(hdr); i < sizeof(pkt); i++)
        pkt[i] = (uint8_t)(i * 7u + 3u);
}

static struct ck_gsp_mmio_in base_in(void)
{
    struct ck_gsp_mmio_in in = {pkt, sizeof(pkt), ARGS_PA, APP_VERSION, 100, 100, 100, 100, count_pause, 0};
    return in;
}

static struct ck_gspsim_cfg base_cfg(void)
{
    struct ck_gspsim_cfg c = {0, 3, 4, 2, 5, 0, 0, 0, ARGS_PA, 0};
    return c;
}

static int run(const struct ck_gspsim_cfg *c, const struct ck_gsp_mmio_in *in, struct ck_gsp_mmio_out *o)
{
    ck_gspsim_init(&sim, c);
    regs = ck_gspsim_regs(&sim);
    pauses = 0;
    return ck_gsp_mmio_boot(&regs, in, o);
}

static uint32_t writes_to(uint32_t off)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < sim.nlog && i < CK_GSPSIM_LOG_MAX; i++)
        n += sim.log[i].write && sim.log[i].off == off;
    return n;
}

static int index_of_write(uint32_t off)
{
    for (uint32_t i = 0; i < sim.nlog && i < CK_GSPSIM_LOG_MAX; i++)
        if (sim.log[i].write && sim.log[i].off == off)
            return (int)i;
    return -1;
}

static void test_happy(void)
{
    struct ck_gspsim_cfg c = base_cfg();
    struct ck_gsp_mmio_in in = base_in();
    struct ck_gsp_mmio_out o;
    CHECK(run(&c, &in, &o) == CK_GSP_OK);
    CHECK(o.stage == CK_GSP_OK);
    CHECK(sim.violations == 0);
    CHECK(memcmp(sim.emem, pkt, sizeof(pkt)) == 0);          /* EMEM holds the packet byte for byte */
    CHECK(o.dwords == 217u);
    CHECK(o.ememc_after == (CK_GSP_EMEMC_AINCW | 3u << 8 | 25u << 2)); /* 217 = 3 blocks of 64 + 25 */
    CHECK(sim.qtail == 864u);                                 /* padded size minus one dword */
    CHECK(index_of_write(CK_GSP_SEC2_QTAIL0) >= 0);
    CHECK(index_of_write(CK_GSP_SEC2_QTAIL0) < index_of_write(CK_GSP_SEC2_QHEAD0)); /* TAIL then HEAD */
    CHECK(writes_to(CK_GSP_SEC2_QHEAD0) == 1 && writes_to(CK_GSP_SEC2_QTAIL0) == 1);
    CHECK(writes_to(CK_GSP_SEC2_EMEMC0) == 1 && writes_to(CK_GSP_SEC2_EMEMD0) == 217u);
    /* the first accesses, in order: HEAD, TAIL reads, EMEMC config, EMEMC read */
    CHECK(sim.log[0].off == CK_GSP_SEC2_QHEAD0 && !sim.log[0].write);
    CHECK(sim.log[1].off == CK_GSP_SEC2_QTAIL0 && !sim.log[1].write);
    CHECK(sim.log[2].off == CK_GSP_SEC2_EMEMC0 && sim.log[2].write && sim.log[2].val == CK_GSP_EMEMC_AINCW);
    CHECK(sim.log[3].off == CK_GSP_SEC2_EMEMC0 && !sim.log[3].write);
    CHECK(sim.os == APP_VERSION && writes_to(CK_GSP_FLCN_OS) == 1);
    CHECK(o.cpuctl & CK_GSP_CPUCTL_ACTIVE);
    CHECK(o.polls[0] == 0);
    CHECK(o.polls[1] == 3);  /* cmd_reads busy reads, then handled */
    CHECK(o.polls[2] == 4);  /* mask_reads locked reads, then released */
    CHECK(o.polls[3] >= 5);  /* args twice, then lockdown_reads */
    CHECK(pauses == o.polls[0] + o.polls[1] + o.polls[2] + o.polls[3]);
    CHECK(o.gsp_mb0 == 0 && o.sec2_mb0 == 0);
    /* the last two accesses: FALCON_OS write, CPUCTL read */
    uint32_t n = sim.nlog;
    CHECK(n >= 2 && sim.log[n - 2].off == CK_GSP_FLCN_OS && sim.log[n - 2].write);
    CHECK(sim.log[n - 1].off == CK_GSP_RISCV_CPUCTL && !sim.log[n - 1].write);
}

static void test_busy(void)
{
    struct ck_gspsim_cfg c = base_cfg();
    struct ck_gsp_mmio_in in = base_in();
    struct ck_gsp_mmio_out o;
    c.busy_reads = 3;
    CHECK(run(&c, &in, &o) == CK_GSP_OK);
    CHECK(o.polls[0] == 3 && sim.violations == 0);

    c.busy_reads = CK_GSPSIM_NO_END;
    CHECK(run(&c, &in, &o) == CK_GSP_E_BUSY);
    CHECK(o.polls[0] == 100);
    CHECK(writes_to(CK_GSP_SEC2_EMEMC0) == 0 && writes_to(CK_GSP_SEC2_EMEMD0) == 0); /* nothing written */
    CHECK(writes_to(CK_GSP_SEC2_QHEAD0) == 0);

    in.polls_can_send = 0; /* budget 0: check once */
    CHECK(run(&c, &in, &o) == CK_GSP_E_BUSY && o.polls[0] == 0 && pauses == 0);
}

static void test_args(void)
{
    struct ck_gsp_mmio_out o;
    struct ck_gsp_mmio_in in = base_in();
    struct ck_gspsim_cfg c = base_cfg();
    in.packet = 0;
    CHECK(run(&c, &in, &o) == CK_GSP_E_ARG && sim.nlog == 0);
    in = base_in();
    in.len = 0;
    CHECK(run(&c, &in, &o) == CK_GSP_E_ARG && sim.nlog == 0);
    in.len = CK_GSP_EMEM_CHANNEL_BYTES + 1;
    CHECK(run(&c, &in, &o) == CK_GSP_E_ARG && sim.nlog == 0);
    CHECK(ck_gsp_mmio_boot(0, &in, &o) == CK_GSP_E_ARG);
    CHECK(ck_gsp_mmio_boot(&regs, 0, &o) == CK_GSP_E_ARG);
    /* 1024 bytes exactly fits the channel */
    static uint8_t big[1024];
    memcpy(big, pkt, sizeof(pkt));
    in = base_in();
    in.packet = big;
    in.len = sizeof(big);
    c.mask_reads = CK_GSPSIM_NO_END; /* the sim rejects a non-868 packet; we only check the send here */
    in.polls_mask = 2;
    CHECK(run(&c, &in, &o) == CK_GSP_E_MASK);
    CHECK(o.dwords == 256u && sim.qtail == 1020u && sim.violations == 0);
}

static void test_odd_length(void)
{
    struct ck_gspsim_cfg c = base_cfg();
    struct ck_gsp_mmio_in in = base_in();
    struct ck_gsp_mmio_out o;
    in.len = 7; /* one full dword and a 3-byte tail, zero-padded */
    in.polls_mask = 1;
    CHECK(run(&c, &in, &o) == CK_GSP_E_MASK); /* the sim refuses a short packet: mask never released */
    CHECK(o.dwords == 2u && sim.qtail == 4u);
    CHECK(sim.emem[0] == 0x00000001u);
    CHECK(sim.emem[1] == (0x1410DE7Eu & 0x00FFFFFFu)); /* bytes 4..6, top byte zero */
    CHECK(sim.violations == 0);
}

static void test_failures(void)
{
    struct ck_gsp_mmio_out o;
    struct ck_gsp_mmio_in in = base_in();
    struct ck_gspsim_cfg c;

    c = base_cfg(); c.cmd_reads = CK_GSPSIM_NO_END;
    CHECK(run(&c, &in, &o) == CK_GSP_E_SEC2);
    CHECK(o.polls[1] == 100 && o.sec2_mb0 == CK_GSP_SEC2_CMD_BUSY);
    CHECK(writes_to(CK_GSP_FLCN_OS) == 0);

    c = base_cfg(); c.mask_reads = CK_GSPSIM_NO_END;
    CHECK(run(&c, &in, &o) == CK_GSP_E_MASK);
    CHECK((o.hwcfg2 & CK_GSP_PRIV_LOCKED_MASK) == CK_GSP_PRIV_LOCKED && writes_to(CK_GSP_FLCN_OS) == 0);

    c = base_cfg(); c.args_reads = CK_GSPSIM_NO_END; /* the FMC never takes its arguments */
    CHECK(run(&c, &in, &o) == CK_GSP_E_LOCKDOWN);
    CHECK(o.gsp_mb0 == (uint32_t)ARGS_PA && o.gsp_mb1 == (uint32_t)(ARGS_PA >> 32));
    CHECK(writes_to(CK_GSP_FLCN_OS) == 0);

    c = base_cfg(); c.lockdown_reads = CK_GSPSIM_NO_END;
    CHECK(run(&c, &in, &o) == CK_GSP_E_LOCKDOWN && writes_to(CK_GSP_FLCN_OS) == 0);

    c = base_cfg(); c.fmc_error = 0x23;
    CHECK(run(&c, &in, &o) == CK_GSP_E_FMC);
    CHECK(o.gsp_mb0 == 0x23 && writes_to(CK_GSP_FLCN_OS) == 0 && sim.violations == 0);

    c = base_cfg(); c.riscv_dead = 1;
    CHECK(run(&c, &in, &o) == CK_GSP_E_RISCV);
    CHECK(writes_to(CK_GSP_FLCN_OS) == 1 && !(o.cpuctl & CK_GSP_CPUCTL_ACTIVE));

    c = base_cfg(); c.emem_no_inc = 1;
    CHECK(run(&c, &in, &o) == CK_GSP_E_EMEM);
    CHECK(writes_to(CK_GSP_SEC2_QHEAD0) == 0 && writes_to(CK_GSP_SEC2_QTAIL0) == 0); /* no doorbell */

    /* a wrong packet (bad NVDM type): SEC2 consumes it and the target mask is never released */
    make_packet();
    pkt[7] = 0x15;
    c = base_cfg();
    CHECK(run(&c, &in, &o) == CK_GSP_E_MASK && sim.phase == 9u);
    make_packet();
}

/* MAILBOX0 equal to the low half of the boot-argument address but MAILBOX1 not the high half: the
 * driver treats that as an FMC code (kernel_gsp_gh100.c:521-528), so must we. */
static void test_mailbox_half_match(void)
{
    struct ck_gsp_mmio_out o;
    struct ck_gsp_mmio_in in = base_in();
    struct ck_gspsim_cfg c = base_cfg();
    c.args_reads = CK_GSPSIM_NO_END;
    c.fmc_args_pa = ARGS_PA + (1ull << 32); /* sim stuffs a different high half */
    CHECK(run(&c, &in, &o) == CK_GSP_E_FMC);
    CHECK(o.gsp_mb0 == (uint32_t)ARGS_PA && writes_to(CK_GSP_FLCN_OS) == 0);
}

static void test_notify(void)
{
    struct ck_gspsim_cfg c = base_cfg();
    ck_gspsim_init(&sim, &c);
    regs = ck_gspsim_regs(&sim);
    ck_gsp_mmio_notify(&regs);
    CHECK(sim.nlog == 1 && sim.log[0].off == CK_GSP_QHEAD0 && sim.log[0].val == 0 && sim.log[0].write);
    CHECK(sim.gsp_qhead_writes == 1 && sim.violations == 0);
}

static void test_zero_hwcfg2(void)
{
    /* HWCFG2 reading 0 is not a released mask (kernel_sec2_gb10b.c:282): keep waiting */
    struct ck_gspsim_cfg c = base_cfg();
    struct ck_gsp_mmio_in in = base_in();
    struct ck_gsp_mmio_out o;
    c.hwcfg2_zero_reads = 2;
    c.mask_reads = 0;
    CHECK(run(&c, &in, &o) == CK_GSP_OK && sim.violations == 0);
    CHECK(o.polls[2] == 2);
    c.hwcfg2_zero_reads = CK_GSPSIM_NO_END;
    CHECK(run(&c, &in, &o) == CK_GSP_E_MASK && o.hwcfg2 == 0 && writes_to(CK_GSP_FLCN_OS) == 0);
}

int main(void)
{
    make_packet();
    test_happy();
    test_busy();
    test_args();
    test_odd_length();
    test_failures();
    test_mailbox_half_match();
    test_notify();
    test_zero_hwcfg2();
    return ck_t_verdict("test_gsp_mmio");
}
