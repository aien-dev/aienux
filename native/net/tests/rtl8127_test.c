/* rtl8127_test.c -- the RTL8127 driver core (aienos_rtl8127.c) against a chip
 * model: a register array with the side effects the vendor reference relies
 * on (reset bit, OCP windows, FIFO-empty bits, close pointer) and a ring
 * simulator that walks the driver's TX and RX descriptors the way the chip
 * would, refusing any DMA address outside the driver's region (the SMMU
 * window on the Spark). The model is written from the same vendor source as
 * the driver (DOCS SILENT); it checks that the driver does what the
 * reference does, not that the reference is right for the chip.
 * Prints RTL8127_MODEL: PASS / FAIL. */
#include "../aienos_rtl8127.h"
#include "../aienos_report.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;
#define CHECK(c, ...)                                                    \
    do {                                                                 \
        checks++;                                                        \
        if (!(c)) {                                                      \
            failures++;                                                  \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                  \
            printf(__VA_ARGS__);                                         \
            printf("\n");                                                \
        }                                                                \
    } while (0)

/* ---- chip model ---- */
typedef struct {
    uint8_t reg[RTL_MMIO_MIN];
    uint16_t ocp[0x8000];     /* MAC OCP space, by address/2 */
    uint16_t phy[0x8000];     /* PHY OCP space, by address/2 */
    uint32_t ocp_latch, phy_latch;
    int phy_busy;             /* PHY OCP flag phase */
    /* configuration */
    uint32_t tcr_id;
    int absent, reset_stuck, fifo_never_empty, clo_after_reset, hostile_clo;
    /* DMA region the chip may touch (the SMMU window) */
    uint8_t *mem;
    uint64_t dma;
    size_t len;
    unsigned dma_faults;
    /* ring state inside the chip */
    uint32_t tx_hw, rx_hw, clo, tail;
    uint32_t tx_origin;       /* close pointer after reset: tx_hw, clo and tail count from it */
    int defer_tx;            /* close descriptors only when asked */
    unsigned resets, writes, ocp_writes;
    int mac_written_unlocked, mac_written_locked;
    int enabled_before_rings; /* TE|RE written before both ring bases */
    int tnpds_set, rdsar_set;
    uint8_t tx_frames[64][RTL_FRAME_MAX + 64];
    size_t tx_len[64];
    unsigned tx_count;
    unsigned rx_overflow;
} chip;

static chip C;

static uint32_t get(uint32_t off, unsigned w)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < w; i++)
        v |= (uint32_t)C.reg[off + i] << (8 * i);
    return v;
}
static void put(uint32_t off, unsigned w, uint32_t v)
{
    for (unsigned i = 0; i < w; i++)
        C.reg[off + i] = (uint8_t)(v >> (8 * i));
}

static uint8_t *dma_ptr(uint64_t a, size_t n)
{
    if (a < C.dma || a + n > C.dma + C.len || a + n < a) {
        C.dma_faults++;
        return 0;
    }
    return C.mem + (a - C.dma);
}
static uint32_t rd32le(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr32le(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}
static uint64_t ring_base(uint32_t lo)
{
    return (uint64_t)get(lo, 4) | (uint64_t)get(lo + 4, 4) << 32;
}

static void chip_reset_state(void)
{
    C.rx_hw = 0;
    C.tx_hw = C.tx_origin = (uint32_t)C.clo_after_reset; /* the chip's TX index counts from its close base */
    C.clo = (uint32_t)C.clo_after_reset;
    C.tail = 0;
    put(RTL_HW_CLO0, 4, C.clo);
    C.resets++;
}

/* The chip fetches every descriptor between its index and the tail. */
static void chip_tx_run(void)
{
    uint8_t *ring = dma_ptr(ring_base(RTL_TNPDS_LO), RTL_TX_N * RTL_TXD_SIZE);
    if (!ring || !(get(RTL_CHIPCMD, 1) & RTL_CMD_TXEN))
        return;
    if (((C.tail - C.tx_hw) & RTL_CLO_MASK) > RTL_TX_N) {
        C.dma_faults += 1000; /* tail more than a ring ahead of the chip: tail and close pointer disagree */
        return;
    }
    while (C.tx_hw != (C.tail & RTL_CLO_MASK)) {
        uint32_t slot = (C.tx_hw - C.tx_origin) % RTL_TX_N;
        uint8_t *d = ring + slot * RTL_TXD_SIZE;
        uint32_t o1 = rd32le(d);
        if (!(o1 & RTL_DESC_OWN))
            break;
        uint64_t a = rd32le(d + 8) | (uint64_t)rd32le(d + 12) << 32;
        size_t n = o1 & 0x3ffffu;
        uint8_t *b = dma_ptr(a, n);
        if (!b)
            break;
        if (C.tx_count < 64) {
            memcpy(C.tx_frames[C.tx_count], b, n < sizeof C.tx_frames[0] ? n : sizeof C.tx_frames[0]);
            C.tx_len[C.tx_count] = n;
        }
        C.tx_count++;
        int eor = (o1 & RTL_DESC_EOR) != 0;
        if (eor != (slot == RTL_TX_N - 1))
            C.dma_faults += 1000; /* EOR on the wrong slot: flagged loudly */
        C.tx_hw = (C.tx_hw + 1) & RTL_CLO_MASK;
        if (!C.defer_tx)
            C.clo++;
    }
    put(RTL_HW_CLO0, 4, C.clo & RTL_CLO_MASK);
}

static uint32_t m_read(void *ctx, uint32_t off, unsigned w)
{
    (void)ctx;
    if (C.absent)
        return w == 4 ? 0xffffffffu : w == 2 ? 0xffffu : 0xffu;
    if (off + w > RTL_MMIO_MIN) {
        C.dma_faults += 10000;
        return 0;
    }
    if (off == RTL_CHIPCMD && (C.reg[off] & RTL_CMD_RESET) && !C.reset_stuck) {
        C.reg[off] &= (uint8_t)~RTL_CMD_RESET; /* reset finishes on the first poll */
        chip_reset_state();
    }
    if (off == RTL_MACOCP && w == 4)
        return C.ocp[(C.ocp_latch >> 16) & 0x7fff];
    if (off == RTL_PHYOCP && w == 4) {
        if (C.phy_busy == 1) { /* write in flight: flag clears on first poll */
            C.phy_busy = 0;
            return 0;
        }
        if (C.phy_busy == 2) { /* read: flag set with data */
            uint32_t a = (C.phy_latch >> 16) & 0x7fff;
            uint32_t v = C.phy[a];
            if (a == 0xA420u / 2) /* PHY state: 3 once BMCR has autoneg on */
                v = (C.phy[0xA400u / 2] & 0x1000u) ? 3 : 1;
            return RTL_OCP_FLAG | v;
        }
        return 0;
    }
    if (off == RTL_HW_CLO0 && C.hostile_clo)
        return C.clo + 1000;
    return get(off, w);
}

static void m_write(void *ctx, uint32_t off, unsigned w, uint32_t v)
{
    (void)ctx;
    C.writes++;
    if (C.absent)
        return;
    if (off + w > RTL_MMIO_MIN) {
        C.dma_faults += 10000;
        return;
    }
    switch (off) {
    case RTL_MACOCP:
        C.ocp_latch = v;
        if (v & RTL_OCP_WRITE) {
            C.ocp[(v >> 16) & 0x7fff] = (uint16_t)v;
            C.ocp_writes++;
        }
        return;
    case RTL_PHYOCP:
        C.phy_latch = v;
        if (v & RTL_OCP_WRITE) {
            C.phy[(v >> 16) & 0x7fff] = (uint16_t)v;
            C.phy_busy = 1;
        } else
            C.phy_busy = 2;
        return;
    case RTL_MAC0:
    case RTL_MAC4:
        if (w == 4) {
            if ((get(RTL_CFG9346, 1) & RTL_CFG9346_UNLOCK) == RTL_CFG9346_UNLOCK)
                C.mac_written_unlocked++;
            else
                C.mac_written_locked++;
        }
        break;
    case RTL_TNPDS_LO:
        C.tnpds_set = 1;
        break;
    case RTL_RDSAR_LO:
        C.rdsar_set = 1;
        break;
    case RTL_SW_TAIL0:
        put(off, w, v);
        C.tail = v;
        chip_tx_run();
        return;
    case RTL_CHIPCMD:
        if ((v & (RTL_CMD_TXEN | RTL_CMD_RXEN)) == (RTL_CMD_TXEN | RTL_CMD_RXEN) && !(C.tnpds_set && C.rdsar_set))
            C.enabled_before_rings = 1;
        break;
    case RTL_ISR0:
        put(off, w, get(off, w) & ~v); /* write one to clear */
        return;
    }
    put(off, w, v);
}

static void m_delay(void *ctx, uint32_t us) { (void)ctx, (void)us; }
static void m_barrier(void *ctx) { (void)ctx; }
static const rtl_ops OPS = {0, m_read, m_write, m_delay, m_barrier};

/* Fixed registers of a freshly powered chip. */
static void chip_init(uint8_t *mem, uint64_t dma, size_t len)
{
    memset(&C, 0, sizeof C);
    C.mem = mem;
    C.dma = dma;
    C.len = len;
    C.tcr_id = RTL_TCR_ID_8127 | 0x00100000u; /* CFG_METHOD_2 */
    put(RTL_TCR, 4, C.tcr_id);
    /* 0xD2 is 16 bit, so its bit 9 (LL share FIFO ready) is bit 1 of MCUCmd 0xD3 */
    put(RTL_LLFIFO, 2, 0x0200);
    C.reg[RTL_MCUCMD] |= RTL_MCU_TXFIFO_EMPTY | RTL_MCU_RXFIFO_EMPTY | RTL_MCU_NOW_IS_OOB;
    put(RTL_INTRMIT, 2, 0x0103);
    put(RTL_RXDESC_TYPE, 1, 0x02); /* type 4 left on by a previous owner */
    put(RTL_TXNOCLOSE_EN, 1, 0);
    put(RTL_RSS_CTRL, 4, 0x00000f01u);         /* RSS on from a previous owner */
    put(RTL_Q_NUM_CTRL, 2, 0x0108u | 0x0008u); /* 4 RX queues (bits 4:2 = 2), other bits kept */
    C.ocp[RTL_OCP_TXDESC_FMT / 2] = 0x8002u;   /* old 16-byte TX descriptor format selected */
    put(RTL_RXDV_GATE, 1, 0);
    put(RTL_PHYSTATUS, 2, 0x4000 | RTL_PHYS_LINK | RTL_PHYS_FULLDUP);
    put(RTL_ISR0, 4, 0x00000085u);
    put(RTL_IMR0, 4, 0xffffffffu);
    put(RTL_BACKUP_ADDR0, 4, 0x33221100u ^ 0x01u); /* 00:11:22:33 after the flip below */
    C.reg[RTL_BACKUP_ADDR0] = 0x48;               /* 48:11:22:33:44:55, unicast */
    put(RTL_BACKUP_ADDR1, 2, 0x5544u);
    for (int i = 0; i < 6; i++)
        C.reg[RTL_MAC0 + i] = (uint8_t)(0xa0 + i);
}

/* Model receives a frame from the wire: needs a descriptor the chip owns,
 * writes data + 4-byte FCS and the status word. */
static int chip_rx(const uint8_t *f, size_t n, uint32_t extra_status, uint32_t force_len)
{
    uint8_t *ring = dma_ptr(ring_base(RTL_RDSAR_LO), RTL_RX_N * RTL_RXD_SIZE);
    if (!ring || !(get(RTL_CHIPCMD, 1) & RTL_CMD_RXEN))
        return -1;
    uint8_t *d = ring + (C.rx_hw % RTL_RX_N) * RTL_RXD_SIZE;
    uint32_t o1 = rd32le(d);
    if (!(o1 & RTL_DESC_OWN)) {
        C.rx_overflow++;
        return -2;
    }
    uint32_t size = o1 & RTL_RX_LEN_MASK;
    uint64_t a = rd32le(d + 8) | (uint64_t)rd32le(d + 12) << 32;
    uint8_t *b = dma_ptr(a, size);
    if (!b)
        return -3;
    if (n + 4 > size)
        return -4;
    memcpy(b, f, n);
    memset(b + n, 0xcc, 4);
    uint32_t l = force_len ? force_len : (uint32_t)(n + 4);
    wr32le(d, RTL_DESC_FS | RTL_DESC_LS | extra_status | l);
    C.rx_hw++;
    return 0;
}

/* ---- helpers ---- */
static uint8_t *region;
static const uint64_t DMA = 0x0000004500000000ull; /* above 4 GiB: hi words used */

static rtl_err up(rtl_dev *d)
{
    chip_init(region, DMA, RTL_MEM_SIZE);
    return rtl_init(d, &OPS, region, DMA, RTL_MEM_SIZE);
}

static void frame_fill(uint8_t *f, size_t n, unsigned seed)
{
    for (size_t i = 0; i < n; i++)
        f[i] = (uint8_t)(seed * 31u + i * 7u);
}

/* ---- tests ---- */
static void test_init(void)
{
    rtl_dev d;
    rtl_err e = up(&d);
    CHECK(e == RTL_OK, "init %s", rtl_err_name(e));
    CHECK(d.ready && !d.broken, "ready");
    CHECK(d.mcfg == 2 && !d.ic_unknown, "mcfg %u", d.mcfg);
    static const uint8_t want[6] = {0x48, 0x11, 0x22, 0x33, 0x44, 0x55};
    CHECK(memcmp(d.mac, want, 6) == 0 && d.mac_source == RTL_MAC_BACKUP, "mac from backup");
    CHECK(get(RTL_MAC0, 4) == 0x33221148u && get(RTL_MAC4, 2) == 0x5544u, "rar_set wrote the address");
    CHECK(C.mac_written_unlocked == 2 && C.mac_written_locked == 0, "MAC written only while Cfg9346 unlocked");
    CHECK((get(RTL_CFG9346, 1) & RTL_CFG9346_UNLOCK) == 0, "Cfg9346 locked at the end");
    CHECK(ring_base(RTL_TNPDS_LO) == DMA + RTL_OFF_TXD, "TX ring base");
    CHECK(ring_base(RTL_RDSAR_LO) == DMA + RTL_OFF_RXD, "RX ring base");
    CHECK(!C.enabled_before_rings, "TE|RE only after ring bases");
    CHECK(get(RTL_TCR, 4) == RTL_TCR_VALUE, "TCR 0x%08x", get(RTL_TCR, 4));
    CHECK(get(RTL_RCR, 4) == (RTL_RCR_CHIP | RTL_RCR_ACCEPT_BCAST | RTL_RCR_ACCEPT_MYPHYS), "RCR");
    CHECK(get(RTL_RMS, 2) == (RTL_RMS_VALUE | RTL_RMS_VLAN), "RMS");
    CHECK(get(RTL_TXNOCLOSE_EN, 1) & 0x04, "no-close enabled");
    CHECK(C.ocp[RTL_OCP_TXDESC_FMT / 2] == 0x8001u, "32-byte TX descriptor format, other bits kept (0x%04x)",
          C.ocp[RTL_OCP_TXDESC_FMT / 2]);
    CHECK(get(RTL_RSS_CTRL, 4) == 0, "RSS off");
    CHECK(get(RTL_Q_NUM_CTRL, 2) == 0x0100u, "one RX queue, other bits kept (0x%04x)", get(RTL_Q_NUM_CTRL, 2));
    CHECK(!(get(RTL_RXDESC_TYPE, 1) & 0x02), "RX descriptor type 1");
    CHECK(!(get(RTL_RXDV_GATE, 1) & 0x08), "RX DV gate open");
    CHECK(get(RTL_IMR0, 4) == 0, "interrupts masked");
    CHECK(get(RTL_ISR0, 4) == 0, "interrupt status acknowledged");
    CHECK(!(get(RTL_INT_CFG0, 1) & 0x61), "interrupt type 1");
    CHECK(get(RTL_CHIPCMD, 1) == (RTL_CMD_TXEN | RTL_CMD_RXEN), "TE|RE");
    CHECK(!(get(RTL_MCUCMD, 1) & RTL_MCU_NOW_IS_OOB), "now_is_oob cleared");
    CHECK(C.ocp[0xC0BC / 2] == 0x00FF && C.ocp[0xC0AA / 2] == 0x07D0 && C.ocp[0xC0A6 / 2] == 0x01B5 &&
              C.ocp[0xC01E / 2] == 0x5555,
          "exit_oob MAC OCP values");
    CHECK(C.phy[0xA400 / 2] == 0x1000 && d.phy_state == 3, "PHY powered up, autoneg on");
    CHECK((get(RTL_PMCH, 1) & 0xC0) == 0xC0, "PLL on");
    CHECK(C.resets == 2, "two soft resets (exit_oob, hw_reset): %u", C.resets);
    CHECK(d.wait_timeouts == 0, "no wait ran out: %u", d.wait_timeouts);
    for (uint32_t i = 0; i < RTL_RX_N; i++) {
        const uint8_t *x = region + RTL_OFF_RXD + i * RTL_RXD_SIZE;
        uint32_t o1 = rd32le(x);
        uint64_t a = rd32le(x + 8) | (uint64_t)rd32le(x + 12) << 32;
        CHECK((o1 & RTL_DESC_OWN) && (o1 & RTL_RX_LEN_MASK) == RTL_RX_BUF, "RX slot %u posted", i);
        CHECK(((o1 & RTL_DESC_EOR) != 0) == (i == RTL_RX_N - 1), "EOR only on the last RX slot");
        CHECK(a == DMA + RTL_OFF_RXBUF + (uint64_t)i * RTL_BUF_STRIDE, "RX buffer address %u", i);
    }
    uint32_t mbps = 0;
    CHECK(rtl_link(&d, &mbps) == 1 && mbps == 10000, "link up 10G");
    CHECK(C.dma_faults == 0, "no DMA outside the window");
}

static void test_refusals(void)
{
    rtl_dev d;
    chip_init(region, DMA, RTL_MEM_SIZE);
    C.absent = 1;
    CHECK(rtl_init(&d, &OPS, region, DMA, RTL_MEM_SIZE) == RTL_E_ABSENT, "absent");
    CHECK(C.writes == 0, "nothing written to an absent device");
    CHECK(rtl_tx(&d, (const uint8_t *)"0123456789abcdef", 16) == RTL_E_NOT_READY, "tx before init");

    chip_init(region, DMA, RTL_MEM_SIZE);
    put(RTL_TCR, 4, 0x54000000u); /* an RTL8125 id */
    CHECK(rtl_init(&d, &OPS, region, DMA, RTL_MEM_SIZE) == RTL_E_CHIP, "wrong chip");
    CHECK(C.writes == 0, "nothing written to a wrong chip");
    CHECK(rtl_stop(&d) == RTL_E_NOT_READY && C.writes == 0, "stop writes nothing to a wrong chip");

    chip_init(region, DMA, RTL_MEM_SIZE);
    put(RTL_TCR, 4, RTL_TCR_ID_8127 | 0x00300000u);
    CHECK(rtl_init(&d, &OPS, region, DMA, RTL_MEM_SIZE) == RTL_OK && d.ic_unknown && d.mcfg == 2, "unknown IC ver");

    chip_init(region, DMA, RTL_MEM_SIZE);
    C.reset_stuck = 1;
    CHECK(rtl_init(&d, &OPS, region, DMA, RTL_MEM_SIZE) == RTL_E_RESET_TIMEOUT, "reset timeout");
    CHECK(!d.ready, "not ready after a reset timeout");

    chip_init(region, DMA, RTL_MEM_SIZE);
    C.fifo_never_empty = 1;
    put(RTL_MCUCMD, 1, 0);
    put(RTL_INTRMIT, 2, 0);
    put(RTL_LLFIFO, 2, 0);
    CHECK(rtl_init(&d, &OPS, region, DMA, RTL_MEM_SIZE) == RTL_OK && d.wait_timeouts == 2 * 2 + 2,
          "FIFO waits counted, not fatal (vendor ignores them): %u", d.wait_timeouts);

    chip_init(region, DMA, RTL_MEM_SIZE);
    CHECK(rtl_init(&d, &OPS, region + 16, DMA, RTL_MEM_SIZE) == RTL_E_MEM, "misaligned mem");
    CHECK(rtl_init(&d, &OPS, region, DMA + 256, RTL_MEM_SIZE) == RTL_E_MEM, "misaligned dma");
    CHECK(rtl_init(&d, &OPS, region, DMA, RTL_MEM_SIZE - 1) == RTL_E_MEM, "short region");
    CHECK(rtl_init(&d, &OPS, region, 0xfffffffffffff000ull, RTL_MEM_SIZE) == RTL_E_MEM, "bus range wraps");
    CHECK(C.writes == 0, "no register writes on a bad region");
    rtl_ops bad = OPS;
    bad.barrier = 0;
    CHECK(rtl_init(&d, &bad, region, DMA, RTL_MEM_SIZE) == RTL_E_ARG, "missing op");
}

static void test_mac_sources(void)
{
    rtl_dev d;
    chip_init(region, DMA, RTL_MEM_SIZE);
    put(RTL_BACKUP_ADDR0, 4, 0);
    put(RTL_BACKUP_ADDR1, 2, 0);
    CHECK(rtl_init(&d, &OPS, region, DMA, RTL_MEM_SIZE) == RTL_OK && d.mac_source == RTL_MAC_MAC0 && d.mac[0] == 0xa0,
          "MAC0 when the backup address is zero");
    chip_init(region, DMA, RTL_MEM_SIZE);
    put(RTL_BACKUP_ADDR0, 4, 0x000000ffu); /* multicast */
    for (int i = 0; i < 6; i++)
        C.reg[RTL_MAC0 + i] = 0xff;
    CHECK(rtl_init(&d, &OPS, region, DMA, RTL_MEM_SIZE) == RTL_OK && d.mac_source == RTL_MAC_FALLBACK &&
              (d.mac[0] & 2) && !(d.mac[0] & 1),
          "locally administered fallback");
}

static void test_tx(void)
{
    rtl_dev d;
    CHECK(up(&d) == RTL_OK, "up");
    uint8_t f[RTL_FRAME_MAX];
    frame_fill(f, sizeof f, 1);
    CHECK(rtl_tx(&d, f, 13) == RTL_E_ARG && rtl_tx(&d, f, RTL_FRAME_MAX + 1) == RTL_E_ARG, "length bounds");
    CHECK(rtl_tx(&d, f, 42) == RTL_OK, "short frame");
    CHECK(C.tx_count == 1 && C.tx_len[0] == 60 && memcmp(C.tx_frames[0], f, 42) == 0, "sent and padded to 60");
    int zero = 1;
    for (int i = 42; i < 60; i++)
        zero &= C.tx_frames[0][i] == 0;
    CHECK(zero, "padding is zero");
    CHECK(get(RTL_SW_TAIL0, 4) == 1, "tail moved to 1");
    unsigned r = 0;
    CHECK(rtl_tx_reclaim(&d, &r) == RTL_OK && r == 1 && rtl_tx_pending(&d) == 0, "reclaimed 1");

    /* Ring full with completions held back, then released. */
    C.defer_tx = 1;
    for (unsigned i = 0; i < RTL_TX_N; i++)
        CHECK(rtl_tx(&d, f, 100 + i) == RTL_OK, "post %u", i);
    CHECK(rtl_tx(&d, f, 100) == RTL_E_FULL, "full");
    CHECK(rtl_tx_reclaim(&d, &r) == RTL_OK && r == 0, "nothing closed yet");
    C.clo += 5;
    put(RTL_HW_CLO0, 4, C.clo);
    CHECK(rtl_tx_reclaim(&d, &r) == RTL_OK && r == 5 && rtl_tx_pending(&d) == RTL_TX_N - 5, "5 closed");
    C.defer_tx = 0;
    C.clo += RTL_TX_N - 5;
    put(RTL_HW_CLO0, 4, C.clo);
    CHECK(rtl_tx_reclaim(&d, &r) == RTL_OK && rtl_tx_pending(&d) == 0, "all closed");

    /* Many trips around the ring: every frame arrives intact, in order. */
    unsigned before = C.tx_count, bad = 0;
    for (unsigned k = 0; k < 200; k++) {
        size_t n = 60 + (k * 37u) % (RTL_FRAME_MAX - 60);
        frame_fill(f, n, k);
        if (rtl_tx(&d, f, n) != RTL_OK) {
            bad++;
            continue;
        }
        unsigned idx = C.tx_count - 1;
        if (idx < 64 && (C.tx_len[idx] != n || memcmp(C.tx_frames[idx], f, n) != 0))
            bad++;
        if (rtl_tx_reclaim(&d, &r) != RTL_OK || r != 1)
            bad++;
        if (C.tx_count >= 64)
            C.tx_count = 0, before = 0;
    }
    (void)before;
    CHECK(bad == 0, "200 frames around the ring: %u bad", bad);
    CHECK(C.dma_faults == 0, "TX stayed in the window (faults %u)", C.dma_faults);
}

static void test_tx_hostile(void)
{
    rtl_dev d;
    CHECK(up(&d) == RTL_OK, "up");
    uint8_t f[64] = {0};
    CHECK(rtl_tx(&d, f, 60) == RTL_OK, "tx");
    C.hostile_clo = 1;
    CHECK(rtl_tx_reclaim(&d, 0) == RTL_E_DEVICE && d.broken, "close pointer ahead of posted");
    CHECK(rtl_tx(&d, f, 60) == RTL_E_BROKEN, "sticky on tx");
    size_t n;
    int got;
    uint8_t out[RTL_FRAME_MAX];
    CHECK(rtl_rx(&d, out, sizeof out, &n, &got) == RTL_E_BROKEN, "sticky on rx");
    C.hostile_clo = 0;
    CHECK(rtl_stop(&d) == RTL_OK && !d.ready, "stop");
    chip_init(region, DMA, RTL_MEM_SIZE);
    CHECK(rtl_init(&d, &OPS, region, DMA, RTL_MEM_SIZE) == RTL_OK && !d.broken, "re-init clears broken");

    /* A close-pointer base the chip did not reset to zero. */
    chip_init(region, DMA, RTL_MEM_SIZE);
    C.clo_after_reset = 7;
    CHECK(rtl_init(&d, &OPS, region, DMA, RTL_MEM_SIZE) == RTL_OK && d.clo_base == 7, "close base 7");
    unsigned r = 0;
    CHECK(rtl_tx(&d, f, 60) == RTL_OK && rtl_tx_reclaim(&d, &r) == RTL_OK && r == 1, "reclaim with base 7");
    CHECK(get(RTL_SW_TAIL0, 4) == 8 && C.tx_count == 1 && C.dma_faults == 0, "tail counts from base 7 (tail %u)",
          get(RTL_SW_TAIL0, 4));
    for (unsigned k = 0; k < 3 * RTL_TX_N; k++)
        if (rtl_tx(&d, f, 60) != RTL_OK || rtl_tx_reclaim(&d, 0) != RTL_OK) {
            CHECK(0, "round %u with base 7", k);
            break;
        }
    CHECK(C.tx_count == 1 + 3 * RTL_TX_N && C.dma_faults == 0, "three rings of frames with base 7 (%u sent)",
          C.tx_count);
}

static void test_rx(void)
{
    rtl_dev d;
    CHECK(up(&d) == RTL_OK, "up");
    uint8_t f[RTL_FRAME_MAX], out[RTL_FRAME_MAX];
    size_t n = 0;
    int got = 1;
    CHECK(rtl_rx(&d, out, sizeof out, &n, &got) == RTL_OK && !got, "nothing pending");
    unsigned bad = 0;
    for (unsigned k = 0; k < 100; k++) {
        size_t len = 14 + (k * 53u) % (RTL_FRAME_MAX - 14);
        frame_fill(f, len, k + 7);
        if (chip_rx(f, len, 0, 0) != 0) {
            bad++;
            continue;
        }
        if (rtl_rx(&d, out, sizeof out, &n, &got) != RTL_OK || !got || n != len || memcmp(out, f, len) != 0)
            bad++;
    }
    CHECK(bad == 0, "100 frames around the RX ring: %u bad", bad);

    /* Errors, runts and split frames are dropped and the slot reposted. */
    frame_fill(f, 100, 3);
    CHECK(chip_rx(f, 100, RTL_RX_RES, 0) == 0, "inject error frame");
    CHECK(chip_rx(f, 10, 0, 0) == 0, "inject runt");
    CHECK(chip_rx(f, 100, 0, 0) == 0, "inject good");
    uint8_t *ring = region + RTL_OFF_RXD;
    uint32_t slot = (d.rx_cur + 2) % RTL_RX_N; /* the third frame: error, runt, then this one */
    wr32le(ring + slot * RTL_RXD_SIZE, rd32le(ring + slot * RTL_RXD_SIZE) & ~RTL_DESC_LS); /* not last */
    uint8_t f2[100];
    frame_fill(f2, 100, 4);
    CHECK(chip_rx(f2, 100, 0, 0) == 0, "inject good 2");
    unsigned dropped = d.rx_dropped;
    CHECK(rtl_rx(&d, out, sizeof out, &n, &got) == RTL_OK && got && n == 100 && memcmp(out, f2, 100) == 0,
          "good 2 after drops");
    CHECK(d.rx_dropped == dropped + 3, "error frame, runt and split frame dropped");

    /* Output buffer smaller than the frame: dropped and reposted, the ring keeps moving. */
    CHECK(chip_rx(f, 100, 0, 0) == 0, "inject for small cap");
    dropped = d.rx_dropped;
    CHECK(rtl_rx(&d, out, 50, &n, &got) == RTL_OK && !got && d.rx_dropped == dropped + 1, "cap too small: dropped");
    CHECK(chip_rx(f2, 100, 0, 0) == 0, "inject after small cap");
    CHECK(rtl_rx(&d, out, sizeof out, &n, &got) == RTL_OK && got && n == 100 && memcmp(out, f2, 100) == 0,
          "next frame still arrives");

    /* Largest frame RMS 1522 lets through: 1518 bytes (VLAN tag) without FCS. */
    static uint8_t big[RTL_RX_BUF], bigout[RTL_RX_BUF];
    frame_fill(big, 1518, 9);
    CHECK(chip_rx(big, 1518, 0, 0) == 0, "inject 1518-byte frame");
    CHECK(rtl_rx(&d, bigout, sizeof bigout, &n, &got) == RTL_OK && got && n == 1518 && memcmp(bigout, big, 1518) == 0,
          "1518-byte frame received whole");

    /* Overflow: the chip finds no free slot when the driver never reads. */
    for (unsigned i = 0; i < RTL_RX_N + 3; i++)
        chip_rx(f, 64, 0, 0);
    CHECK(C.rx_overflow == 3, "chip saw 3 overflows");
    unsigned taken = 0;
    while (rtl_rx(&d, out, sizeof out, &n, &got) == RTL_OK && got)
        taken++;
    CHECK(taken == RTL_RX_N, "driver drained %u", taken);
    CHECK(chip_rx(f, 64, 0, 0) == 0, "ring usable after drain");

    /* Hostile length: more than the buffer the chip was given. */
    CHECK(rtl_rx(&d, out, sizeof out, &n, &got) == RTL_OK && got, "drain");
    CHECK(chip_rx(f, 64, 0, RTL_RX_BUF + 4) == 0, "inject oversize length");
    CHECK(rtl_rx(&d, out, sizeof out, &n, &got) == RTL_E_DEVICE && d.broken, "oversize length breaks the device");
    CHECK(C.dma_faults == 0, "RX stayed in the window (faults %u)", C.dma_faults);
}

/* Seeded random: the chip writes random status words into RX descriptors it
 * does not own; the driver must never return a length beyond the buffer. */
static void test_rx_random(void)
{
    rtl_dev d;
    uint8_t out[RTL_FRAME_MAX];
    unsigned long seed = 0x8127;
    unsigned breaks = 0, frames = 0;
    for (unsigned round = 0; round < 2000; round++) {
        if (up(&d) != RTL_OK) {
            CHECK(0, "up");
            return;
        }
        for (unsigned k = 0; k < 40; k++) {
            seed = seed * 6364136223846793005ul + 1442695040888963407ul;
            uint32_t s = (uint32_t)(seed >> 32) & ~RTL_DESC_OWN;
            uint32_t i = d.rx_cur % RTL_RX_N;
            wr32le(region + RTL_OFF_RXD + i * RTL_RXD_SIZE, s);
            size_t n = 0;
            int got = 0;
            rtl_err e = rtl_rx(&d, out, sizeof out, &n, &got);
            if (e == RTL_E_DEVICE) {
                breaks++;
                break;
            }
            if (e != RTL_OK || (got && (n > RTL_RX_BUF - 4 || n < 14))) {
                CHECK(0, "random status 0x%08x gave %s n=%zu", s, rtl_err_name(e), n);
                return;
            }
            frames += (unsigned)got;
        }
    }
    CHECK(breaks > 0 && frames > 0, "random statuses: %u frames, %u breaks", frames, breaks);
}

static void test_link(void)
{
    rtl_dev d;
    CHECK(up(&d) == RTL_OK, "up");
    uint32_t m = 1;
    put(RTL_PHYSTATUS, 2, 0);
    CHECK(rtl_link(&d, &m) == 0 && m == 0, "down");
    put(RTL_PHYSTATUS, 2, 0x0400 | RTL_PHYS_LINK);
    CHECK(rtl_link(&d, &m) == 1 && m == 2500, "2.5G");
    put(RTL_PHYSTATUS, 2, 0x0010 | RTL_PHYS_LINK);
    CHECK(rtl_link(&d, &m) == 1 && m == 1000, "1G");
    put(RTL_PHYSTATUS, 2, 0xffff);
    CHECK(rtl_link(&d, &m) == 0, "all ones is down");
}

/* The boot report travels through the driver: every frame the chip puts on
 * the wire must parse back to the report bytes. */
static void test_report_over_driver(void)
{
    rtl_dev d;
    CHECK(up(&d) == RTL_OK, "up");
    static char log[5000];
    for (size_t i = 0; i < sizeof log; i++)
        log[i] = (char)('a' + i % 26);
    unsigned parts = net_report_parts(sizeof log);
    CHECK(parts == 5, "parts %u", parts);
    uint8_t fr[1600];
    C.tx_count = 0;
    for (unsigned p = 0; p < parts; p++) {
        size_t n = net_report_frame(NET_REPORT_TAG_SPARK, d.mac, 0x1122334455667788ull, log, sizeof log, p, fr,
                                    sizeof fr);
        CHECK(n > 0 && rtl_tx(&d, fr, n) == RTL_OK, "part %u", p);
        rtl_tx_reclaim(&d, 0);
    }
    char back[sizeof log];
    size_t got = 0;
    for (unsigned p = 0; p < parts && p < C.tx_count; p++) {
        net_report_part rp;
        int rc = net_report_parse(NET_REPORT_TAG_SPARK, C.tx_frames[p], C.tx_len[p], &rp);
        CHECK(rc == 0 && rp.part == p && rp.parts == parts && rp.total == sizeof log && rp.run == 0x1122334455667788ull,
              "parse part %u rc=%d", p, rc);
        if (rc == 0 && rp.offset + rp.len <= sizeof back) {
            memcpy(back + rp.offset, rp.data, rp.len);
            got += rp.len;
        }
    }
    CHECK(got == sizeof log && memcmp(back, log, sizeof log) == 0, "report rebuilt from the wire");
    CHECK(memcmp(C.tx_frames[0], "\xff\xff\xff\xff\xff\xff", 6) == 0 && memcmp(C.tx_frames[0] + 6, d.mac, 6) == 0,
          "broadcast from the chip's MAC");
}

int main(void)
{
    region = aligned_alloc(4096, RTL_MEM_SIZE);
    if (!region)
        return 2;
    test_init();
    test_refusals();
    test_mac_sources();
    test_tx();
    test_tx_hostile();
    test_rx();
    test_rx_random();
    test_link();
    test_report_over_driver();
    free(region);
    printf("RTL8127_MODEL: %s (%d checks, %d failures)\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
