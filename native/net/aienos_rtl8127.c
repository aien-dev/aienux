/* RTL8127 driver core (see aienos_rtl8127.h). Freestanding: no heap, no
 * clock, no libc beyond memcpy/memset. Line numbers in comments refer to
 * Realtek r8127 v11.014.00 (github.com/openwrt/rtl8127 ec2f285), the only
 * reference: Realtek publishes no register document (DOCS SILENT). */
#include "aienos_rtl8127.h"

#include <string.h>

#define WAIT_LL_TRIES 10u          /* x100 us, r8127_n.c:4872-4882 */
#define WAIT_FIFO_TRIES 3000u      /* x50 us, r8127_n.c:4100-4116 */
#define WAIT_RESET_TRIES 100u      /* x100 us, r8127_n.c:4312-4318 */
#define WAIT_OCP_TRIES 20000u      /* x1 us, R8127_CHANNEL_WAIT_COUNT */
#define WAIT_PHY_STATE_TRIES 100u  /* x1 ms, r8127_n.c:7308-7318 */
#define PHY_BMCR 0xA400u           /* MII BMCR via PHY OCP: page 0xA40, reg 0x10 */
#define PHY_STATE 0xA420u          /* low 3 bits: 3 = powered up (r8127_n.c:7300-7304) */
#define BMCR_ANENABLE 0x1000u      /* IEEE 802.3 22.2.4.1.4 */

static const uint8_t fallback_mac[6] = {0x02, 0x41, 0x49, 0x45, 0x4e, 0x58}; /* locally administered */

static uint32_t r8(rtl_dev *d, uint32_t o) { return d->ops.read(d->ops.ctx, o, 1); }
static uint32_t r16(rtl_dev *d, uint32_t o) { return d->ops.read(d->ops.ctx, o, 2); }
static uint32_t r32(rtl_dev *d, uint32_t o) { return d->ops.read(d->ops.ctx, o, 4); }
static void w8(rtl_dev *d, uint32_t o, uint32_t v) { d->ops.write(d->ops.ctx, o, 1, v & 0xffu); }
static void w16(rtl_dev *d, uint32_t o, uint32_t v) { d->ops.write(d->ops.ctx, o, 2, v & 0xffffu); }
static void w32(rtl_dev *d, uint32_t o, uint32_t v) { d->ops.write(d->ops.ctx, o, 4, v); }
static void delay(rtl_dev *d, uint32_t us) { d->ops.delay_us(d->ops.ctx, us); }

/* MAC OCP: r8127_n.c:3287-3314 */
static uint32_t ocp_rd(rtl_dev *d, uint32_t addr)
{
    w32(d, RTL_MACOCP, (addr / 2u) << 16);
    return r32(d, RTL_MACOCP) & 0xffffu;
}
static void ocp_wr(rtl_dev *d, uint32_t addr, uint32_t v)
{
    w32(d, RTL_MACOCP, ((addr / 2u) << 16) | (v & 0xffffu) | RTL_OCP_WRITE);
}

/* PHY OCP: r8127_n.c:3057-3073 (write waits for the flag to clear), 3155-3167
 * (read waits for the flag to set). */
static void phy_wr(rtl_dev *d, uint32_t addr, uint32_t v)
{
    w32(d, RTL_PHYOCP, ((addr / 2u) << 16) | (v & 0xffffu) | RTL_OCP_WRITE);
    for (uint32_t i = 0; i < WAIT_OCP_TRIES; i++) {
        delay(d, 1);
        if (!(r32(d, RTL_PHYOCP) & RTL_OCP_FLAG))
            return;
    }
    d->wait_timeouts++;
}
static uint32_t phy_rd(rtl_dev *d, uint32_t addr)
{
    w32(d, RTL_PHYOCP, (addr / 2u) << 16);
    for (uint32_t i = 0; i < WAIT_OCP_TRIES; i++) {
        delay(d, 1);
        if (r32(d, RTL_PHYOCP) & RTL_OCP_FLAG)
            return r32(d, RTL_PHYOCP) & 0xffffu;
    }
    d->wait_timeouts++;
    return 0xffffu;
}

static void wait_ll_share_fifo(rtl_dev *d) /* r8127_n.c:4872-4882 */
{
    for (uint32_t i = 0; i < WAIT_LL_TRIES; i++) {
        delay(d, 100);
        if (r16(d, RTL_LLFIFO) & 0x0200u)
            return;
    }
    d->wait_timeouts++;
}

static void rx_filter_off(rtl_dev *d) /* r8127_n.c:4287-4293 */
{
    w32(d, RTL_RCR, r32(d, RTL_RCR) & ~RTL_RCR_ACCEPT_MASK);
}

/* r8127_n.c:4296-4321: gate RX, stop request, wait for both FIFOs and the
 * mitigation idle bits, clear the stop request, soft reset, reset RCR. The
 * vendor ignores a reset that never completes; this driver fails instead. */
static rtl_err nic_reset(rtl_dev *d)
{
    rx_filter_off(d);
    w8(d, RTL_RXDV_GATE, r8(d, RTL_RXDV_GATE) | 0x08u);
    w8(d, RTL_CHIPCMD, r8(d, RTL_CHIPCMD) | RTL_CMD_STOPREQ);
    delay(d, 200);
    uint32_t i;
    for (i = 0; i < WAIT_FIFO_TRIES; i++) {
        delay(d, 50);
        uint32_t want = RTL_MCU_TXFIFO_EMPTY | RTL_MCU_RXFIFO_EMPTY;
        if ((r8(d, RTL_MCUCMD) & want) == want)
            break;
    }
    if (i == WAIT_FIFO_TRIES)
        d->wait_timeouts++;
    for (i = 0; i < WAIT_FIFO_TRIES; i++) {
        delay(d, 50);
        if ((r16(d, RTL_INTRMIT) & 0x0103u) == 0x0103u)
            break;
    }
    if (i == WAIT_FIFO_TRIES)
        d->wait_timeouts++;
    w8(d, RTL_CHIPCMD, r8(d, RTL_CHIPCMD) & (RTL_CMD_TXEN | RTL_CMD_RXEN));
    w8(d, RTL_CHIPCMD, RTL_CMD_RESET);
    for (i = 0; i < WAIT_RESET_TRIES; i++) {
        delay(d, 100);
        if (!(r8(d, RTL_CHIPCMD) & RTL_CMD_RESET))
            break;
    }
    if (i == WAIT_RESET_TRIES)
        return RTL_E_RESET_TIMEOUT;
    w32(d, RTL_RCR, RTL_RCR_RESET);
    return RTL_OK;
}

/* r8127_n.c:4507-4519 with ISR version 1 (no MSI-X): interrupt type set to
 * version 1 (4324-4350), mask, acknowledge, timers off, then nic_reset. */
static rtl_err hw_reset(rtl_dev *d)
{
    w8(d, RTL_INT_CFG0, r8(d, RTL_INT_CFG0) & ~0x61u);
    w32(d, RTL_IMR0, 0);
    w32(d, RTL_ISR0, r32(d, RTL_ISR0));
    w32(d, RTL_TIMER_INT0, 0);
    w32(d, RTL_TIMER_INT1, 0);
    w32(d, RTL_TIMER_INT2, 0);
    w32(d, RTL_TIMER_INT3, 0);
    return nic_reset(d);
}

/* r8127_n.c:7340-7384 without DASH and RealWoW; the UPS-resume PHY wait is
 * not done, only recorded (UNKNOWN on the chip). */
static rtl_err exit_oob(rtl_dev *d)
{
    rx_filter_off(d);
    ocp_wr(d, 0xC0BC, 0x00FF);
    rtl_err e = nic_reset(d);
    if (e != RTL_OK)
        return e;
    w8(d, RTL_MCUCMD, r8(d, RTL_MCUCMD) & ~RTL_MCU_NOW_IS_OOB);
    ocp_wr(d, 0xE8DE, ocp_rd(d, 0xE8DE) & ~0x4000u);
    wait_ll_share_fifo(d);
    ocp_wr(d, 0xC0AA, 0x07D0);
    ocp_wr(d, 0xC0A6, 0x01B5);
    ocp_wr(d, 0xC01E, 0x5555);
    wait_ll_share_fifo(d);
    d->ups_resume = (ocp_rd(d, 0xD42C) & 0x0100u) ? 1 : 0;
    return RTL_OK;
}

/* r8127_n.c:5512-5519 and 13306-13318: PLL on, PHY out of power-down with
 * autonegotiation enabled, unless the board holds the PHY in disable mode
 * (4029-4072: 0xF2 bit 5 and MAC OCP 0xDC04 bit 13 low). */
static void powerup(rtl_dev *d)
{
    w8(d, RTL_PMCH, r8(d, RTL_PMCH) | 0xC0u);
    if ((r8(d, RTL_RXDV_GATE) & 0x20u) && !(ocp_rd(d, 0xDC04) & 0x2000u))
        return;
    phy_wr(d, PHY_BMCR, BMCR_ANENABLE);
    for (uint32_t i = 0; i < WAIT_PHY_STATE_TRIES; i++) {
        d->phy_state = (uint8_t)(phy_rd(d, PHY_STATE) & 7u);
        if (d->phy_state == 3)
            return;
        delay(d, 1000);
    }
    d->wait_timeouts++;
}

static int mac_valid(const uint8_t m[6])
{
    if (m[0] & 1u)
        return 0; /* multicast */
    return (m[0] | m[1] | m[2] | m[3] | m[4] | m[5]) != 0;
}

/* r8127_n.c:12418-12441 and 12482-12500 (rar_set). */
static void read_mac(rtl_dev *d)
{
    uint8_t m[6];
    uint32_t lo = r32(d, RTL_BACKUP_ADDR0), hi = r16(d, RTL_BACKUP_ADDR1);
    for (int i = 0; i < 4; i++)
        m[i] = (uint8_t)(lo >> (8 * i));
    m[4] = (uint8_t)hi;
    m[5] = (uint8_t)(hi >> 8);
    d->mac_source = RTL_MAC_BACKUP;
    if (!mac_valid(m)) {
        for (uint32_t i = 0; i < 6; i++)
            m[i] = (uint8_t)r8(d, RTL_MAC0 + i);
        d->mac_source = RTL_MAC_MAC0;
    }
    if (!mac_valid(m)) {
        memcpy(m, fallback_mac, 6);
        d->mac_source = RTL_MAC_FALLBACK;
    }
    memcpy(d->mac, m, 6);
    w8(d, RTL_CFG9346, r8(d, RTL_CFG9346) | RTL_CFG9346_UNLOCK);
    w32(d, RTL_MAC0, (uint32_t)m[0] | (uint32_t)m[1] << 8 | (uint32_t)m[2] << 16 | (uint32_t)m[3] << 24);
    w32(d, RTL_MAC4, (uint32_t)m[4] | (uint32_t)m[5] << 8);
    w8(d, RTL_CFG9346, r8(d, RTL_CFG9346) & ~RTL_CFG9346_UNLOCK);
}

/* Descriptor words are little-endian; opts1 is one 32-bit store so the OWN
 * bit and the length change together. */
static volatile uint32_t *word(rtl_dev *d, size_t off) { return (volatile uint32_t *)(void *)(d->mem + off); }
static uint32_t le(uint32_t v)
{
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return __builtin_bswap32(v);
#else
    return v;
#endif
}

static size_t txd(uint32_t i) { return RTL_OFF_TXD + (size_t)i * RTL_TXD_SIZE; }
static size_t rxd(uint32_t i) { return RTL_OFF_RXD + (size_t)i * RTL_RXD_SIZE; }

/* Give RX slot i to the chip: buffer address from the slot index, then
 * opts1 = OWN | EOR (last slot) | size (r8127_n.c:15263-15268). */
static void rx_post(rtl_dev *d, uint32_t i)
{
    uint64_t a = d->dma + RTL_OFF_RXBUF + (uint64_t)i * RTL_BUF_STRIDE;
    *word(d, rxd(i) + 4) = 0;
    *word(d, rxd(i) + 8) = le((uint32_t)a);
    *word(d, rxd(i) + 12) = le((uint32_t)(a >> 32));
    d->ops.barrier(d->ops.ctx);
    *word(d, rxd(i)) = le(RTL_DESC_OWN | (i == RTL_RX_N - 1 ? RTL_DESC_EOR : 0) | RTL_RX_BUF);
}

size_t rtl_mem_size(void) { return RTL_MEM_SIZE; }

rtl_err rtl_init(rtl_dev *d, const rtl_ops *ops, void *mem, uint64_t dma, size_t mem_len)
{
    if (!d || !ops || !ops->read || !ops->write || !ops->delay_us || !ops->barrier || !mem)
        return RTL_E_ARG;
    memset(d, 0, sizeof *d);
    d->ops = *ops;
    if (mem_len < RTL_MEM_SIZE || ((uintptr_t)mem & 4095u) || (dma & 4095u) || dma + mem_len < dma)
        return RTL_E_MEM;
    d->mem = mem;
    d->dma = dma;
    d->mem_len = mem_len;
    memset(mem, 0, RTL_MEM_SIZE);

    /* Identify (r8127_n.c:7210-7235) before writing anything. */
    uint32_t tcr = r32(d, RTL_TCR);
    d->tcr_id = tcr;
    if (tcr == 0xffffffffu)
        return RTL_E_ABSENT;
    if ((tcr & RTL_TCR_ID_MASK) != RTL_TCR_ID_8127)
        return RTL_E_CHIP;
    uint32_t icver = tcr & RTL_TCR_ICVER_MASK;
    d->mcfg = icver == 0 ? 1 : 2;
    d->ic_unknown = icver != 0 && icver != 0x00100000u;

    /* Probe order of r8127_n.c:14328-14342, then the parts of open/hw_config
     * this driver needs (14935-15182, 15182-15190). */
    rtl_err e = exit_oob(d);
    if (e != RTL_OK)
        return e;
    powerup(d);
    if ((e = hw_reset(d)) != RTL_OK)
        return e;
    read_mac(d);

    for (uint32_t i = 0; i < RTL_TX_N; i++)
        *word(d, txd(i)) = le(i == RTL_TX_N - 1 ? RTL_DESC_EOR : 0);
    for (uint32_t i = 0; i < RTL_RX_N; i++)
        rx_post(d, i);
    d->ops.barrier(d->ops.ctx);

    w8(d, RTL_CFG9346, r8(d, RTL_CFG9346) | RTL_CFG9346_UNLOCK);
    uint64_t tx_ring = dma + RTL_OFF_TXD, rx_ring = dma + RTL_OFF_RXD;
    w32(d, RTL_TNPDS_LO, (uint32_t)tx_ring); /* desc_addr_fill, 15585-15598 */
    w32(d, RTL_TNPDS_HI, (uint32_t)(tx_ring >> 32));
    w32(d, RTL_RDSAR_LO, (uint32_t)rx_ring);
    w32(d, RTL_RDSAR_HI, (uint32_t)(rx_ring >> 32));
    w32(d, RTL_TCR, RTL_TCR_VALUE); /* 14958-14963: burst, IFG, no-close bit 6 */
    w32(d, RTL_RSS_CTRL, 0);                                          /* 14983: no RSS, one RX queue */
    w16(d, RTL_Q_NUM_CTRL, r16(d, RTL_Q_NUM_CTRL) & ~0x001Cu);       /* 14876-14886: rx_q_num = ilog2(1) */
    ocp_wr(d, RTL_OCP_TXDESC_FMT, (ocp_rd(d, RTL_OCP_TXDESC_FMT) & ~0x3u) | 0x1u); /* 14992-14996: 32 B TX desc */
    w8(d, RTL_TXNOCLOSE_EN, r8(d, RTL_TXNOCLOSE_EN) | 0x04u);        /* 14998-15001 */
    w8(d, RTL_RXDESC_TYPE, r8(d, RTL_RXDESC_TYPE) & ~0x02u);         /* 15003-15010: type 1 */
    w16(d, RTL_RMS, RTL_RMS_VALUE | RTL_RMS_VLAN);                   /* 14470-14473 */
    w8(d, RTL_RXDV_GATE, r8(d, RTL_RXDV_GATE) & ~0x08u);             /* 4023-4027 */
    w32(d, RTL_RCR, RTL_RCR_CHIP | RTL_RCR_ACCEPT_BCAST | RTL_RCR_ACCEPT_MYPHYS);
    w8(d, RTL_CFG9346, r8(d, RTL_CFG9346) & ~RTL_CFG9346_UNLOCK);

    /* The vendor starts its close-pointer count at 0 after reset (4596-4605);
     * this driver uses whatever the chip reports as its base instead. */
    d->clo_base = r32(d, RTL_HW_CLO0) & RTL_CLO_MASK;
    w8(d, RTL_CHIPCMD, RTL_CMD_TXEN | RTL_CMD_RXEN); /* hw_start, 15182-15190; IRQs stay masked */
    d->ready = 1;
    return RTL_OK;
}

int rtl_link(rtl_dev *d, uint32_t *mbps)
{
    if (mbps)
        *mbps = 0;
    if (!d || !d->ops.read)
        return 0;
    uint32_t s = r16(d, RTL_PHYSTATUS); /* r8127_n.c:4537-4547 */
    if (s == 0xffffu || !(s & RTL_PHYS_LINK))
        return 0;
    if (mbps)
        *mbps = (s & 0x4000u) ? 10000 : (s & 0x1000u) ? 5000 : (s & 0x0400u) ? 2500 : (s & 0x0010u) ? 1000
              : (s & 0x0008u) ? 100 : (s & 0x0004u) ? 10 : 0;
    return 1;
}

static rtl_err usable(rtl_dev *d)
{
    if (!d)
        return RTL_E_ARG;
    if (d->broken)
        return RTL_E_BROKEN;
    return d->ready ? RTL_OK : RTL_E_NOT_READY;
}

unsigned rtl_tx_pending(const rtl_dev *d) { return d ? d->tx_cur - d->tx_dirty : 0; }

/* r8127_n.c:16427-16560 (one fragment) and the no-close doorbell 16412-16419. */
rtl_err rtl_tx(rtl_dev *d, const uint8_t *frame, size_t len)
{
    rtl_err e = usable(d);
    if (e != RTL_OK)
        return e;
    if (!frame || len < 14 || len > RTL_FRAME_MAX)
        return RTL_E_ARG;
    if (d->tx_cur - d->tx_dirty >= RTL_TX_N)
        return RTL_E_FULL;
    uint32_t i = d->tx_cur % RTL_TX_N;
    size_t n = len < RTL_FRAME_MIN ? RTL_FRAME_MIN : len;
    uint8_t *buf = d->mem + RTL_OFF_TXBUF + (size_t)i * RTL_BUF_STRIDE;
    memcpy(buf, frame, len);
    memset(buf + len, 0, n - len);
    uint64_t a = d->dma + RTL_OFF_TXBUF + (uint64_t)i * RTL_BUF_STRIDE;
    *word(d, txd(i) + 4) = 0;
    *word(d, txd(i) + 8) = le((uint32_t)a);
    *word(d, txd(i) + 12) = le((uint32_t)(a >> 32));
    d->ops.barrier(d->ops.ctx);
    *word(d, txd(i)) = le(RTL_DESC_OWN | RTL_DESC_FS | RTL_DESC_LS | (i == RTL_TX_N - 1 ? RTL_DESC_EOR : 0) |
                          (uint32_t)n);
    d->tx_cur++;
    /* Same origin as the close pointer (clo_base), so tail - close = frames in flight. */
    w32(d, RTL_SW_TAIL0, (d->clo_base + d->tx_cur) & RTL_CLO_MASK); /* write op orders the descriptor first */
    return RTL_OK;
}

/* r8127_n.c:16590-16620: the closed count is the close pointer's advance. */
rtl_err rtl_tx_reclaim(rtl_dev *d, unsigned *reclaimed)
{
    if (reclaimed)
        *reclaimed = 0;
    rtl_err e = usable(d);
    if (e != RTL_OK)
        return e;
    uint32_t clo = r32(d, RTL_HW_CLO0);
    uint32_t done = (clo - d->clo_base - d->tx_dirty) & RTL_CLO_MASK;
    if (clo == 0xffffffffu || done > d->tx_cur - d->tx_dirty) {
        d->broken = 1;
        return RTL_E_DEVICE;
    }
    d->tx_dirty += done;
    if (reclaimed)
        *reclaimed = done;
    return RTL_OK;
}

/* r8127_n.c:17036-17250 for one queue, type 1 descriptors, no fragments. */
rtl_err rtl_rx(rtl_dev *d, uint8_t *out, size_t cap, size_t *len, int *got)
{
    if (got)
        *got = 0;
    if (len)
        *len = 0;
    rtl_err e = usable(d);
    if (e != RTL_OK)
        return e;
    if (!out || !len || !got)
        return RTL_E_ARG;
    for (uint32_t tries = 0; tries < RTL_RX_N; tries++) {
        uint32_t i = d->rx_cur % RTL_RX_N;
        uint32_t s = le(*word(d, rxd(i)));
        if (s & RTL_DESC_OWN)
            return RTL_OK;
        d->ops.barrier(d->ops.ctx);
        uint32_t n = s & RTL_RX_LEN_MASK;
        if (n > RTL_RX_BUF) {
            d->broken = 1; /* the chip wrote past the size it was given */
            return RTL_E_DEVICE;
        }
        int whole = (s & (RTL_DESC_FS | RTL_DESC_LS)) == (RTL_DESC_FS | RTL_DESC_LS);
        size_t f = n >= 4 ? n - 4 : 0; /* FCS, r8127_n.c:17119-17126 */
        if (!whole || (s & RTL_RX_RES) || n < 14 + 4 || cap < f) {
            d->rx_dropped++; /* a frame too big for `cap` is dropped too, so the slot never blocks the ring */
        } else {
            memcpy(out, d->mem + RTL_OFF_RXBUF + (size_t)i * RTL_BUF_STRIDE, f);
            *len = f;
            *got = 1;
        }
        rx_post(d, i);
        d->rx_cur++;
        if (*got)
            return RTL_OK;
    }
    return RTL_OK;
}

rtl_err rtl_stop(rtl_dev *d)
{
    if (!d || !d->ops.write)
        return RTL_E_ARG;
    if (!d->mcfg)
        return RTL_E_NOT_READY; /* never identified as an RTL8127: write nothing */
    d->ready = 0;
    return hw_reset(d);
}

const char *rtl_err_name(rtl_err e)
{
    switch (e) {
    case RTL_OK: return "OK";
    case RTL_E_ARG: return "ARG";
    case RTL_E_MEM: return "MEM";
    case RTL_E_ABSENT: return "ABSENT";
    case RTL_E_CHIP: return "CHIP";
    case RTL_E_RESET_TIMEOUT: return "RESET_TIMEOUT";
    case RTL_E_FULL: return "FULL";
    case RTL_E_DEVICE: return "DEVICE";
    case RTL_E_BROKEN: return "BROKEN";
    case RTL_E_NOT_READY: return "NOT_READY";
    }
    return "?";
}
