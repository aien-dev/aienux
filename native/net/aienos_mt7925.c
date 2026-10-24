/* MediaTek MT7925 Wi-Fi driver core. See aienos_mt7925.h for the source of
 * every layout and the trust rules. */
#include "aienos_mt7925.h"

static void put_le16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint32_t get_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void zero(uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) p[i] = 0;
}
static void copy(uint8_t *d, const uint8_t *s, size_t n) {
    for (size_t i = 0; i < n; i++) d[i] = s[i];
}

/* ---- identity ---- */

mt7925_err mt7925_pci_identity(uint16_t vendor, uint16_t device, uint32_t class_code, uint64_t bar0_size,
                               int bar0_is_64, int bar0_is_mem) {
    if (vendor != MT7925_PCI_VENDOR || (device != MT7925_PCI_DEVICE && device != MT7925_PCI_DEVICE_ALT))
        return MT7925_E_IDENTITY;
    if ((class_code & 0xffff00u) != MT7925_PCI_CLASS || !bar0_is_mem || !bar0_is_64 || bar0_size != MT7925_BAR0_BYTES)
        return MT7925_E_IDENTITY;
    return MT7925_OK;
}

/* ---- address remap ---- */

/* Fixed chip-address -> BAR0 windows (reference mt7925/pci.c
 * default_fixed_map, MT7925 entries). {chip base, BAR0 offset, size}. */
static const struct { uint32_t phys, bar, size; } fixed_map[] = {
    {0x830c0000u, 0x000000u, 0x0001000u}, /* WF_MCU_BUS_CR_REMAP */
    {0x54000000u, 0x002000u, 0x0001000u}, /* WFDMA PCIE0 MCU DMA0 */
    {0x55000000u, 0x003000u, 0x0001000u}, /* WFDMA PCIE0 MCU DMA1 */
    {0x56000000u, 0x004000u, 0x0001000u}, /* WFDMA reserved */
    {0x57000000u, 0x005000u, 0x0001000u}, /* WFDMA MCU wrap CR */
    {0x58000000u, 0x006000u, 0x0001000u}, /* WFDMA PCIE1 MCU DMA0 (MEM_DMA) */
    {0x59000000u, 0x007000u, 0x0001000u}, /* WFDMA PCIE1 MCU DMA1 */
    {0x820c0000u, 0x008000u, 0x0004000u}, /* WF_UMAC_TOP (PLE) */
    {0x820c8000u, 0x00c000u, 0x0002000u}, /* WF_UMAC_TOP (PSE) */
    {0x820cc000u, 0x00e000u, 0x0002000u}, /* WF_UMAC_TOP (PP) */
    {0x74030000u, 0x010000u, 0x0001000u}, /* PCIe MAC */
    {0x820e0000u, 0x020000u, 0x0000400u}, /* LMAC BN0 WF_CFG */
    {0x820e1000u, 0x020400u, 0x0000200u}, /* LMAC BN0 WF_TRB */
    {0x820e2000u, 0x020800u, 0x0000400u}, /* LMAC BN0 WF_AGG */
    {0x820e3000u, 0x020c00u, 0x0000400u}, /* LMAC BN0 WF_ARB */
    {0x820e4000u, 0x021000u, 0x0000400u}, /* LMAC BN0 WF_TMAC */
    {0x820e5000u, 0x021400u, 0x0000800u}, /* LMAC BN0 WF_RMAC */
    {0x820ce000u, 0x021c00u, 0x0000200u}, /* LMAC WF_SEC */
    {0x820e7000u, 0x021e00u, 0x0000200u}, /* LMAC BN0 WF_DMA */
    {0x820cf000u, 0x022000u, 0x0001000u}, /* LMAC WF_PF */
    {0x820e9000u, 0x023400u, 0x0000200u}, /* LMAC BN0 WF_WTBLOFF */
    {0x820ea000u, 0x024000u, 0x0000200u}, /* LMAC BN0 WF_ETBF */
    {0x820eb000u, 0x024200u, 0x0000400u}, /* LMAC BN0 WF_LPON */
    {0x820ec000u, 0x024600u, 0x0000200u}, /* LMAC BN0 WF_INT */
    {0x820ed000u, 0x024800u, 0x0000800u}, /* LMAC BN0 WF_MIB */
    {0x820ca000u, 0x026000u, 0x0002000u}, /* LMAC BN0 WF_MUCOP */
    {0x820d0000u, 0x030000u, 0x0010000u}, /* LMAC WF_WTBLON */
    {0x40000000u, 0x070000u, 0x0010000u}, /* WF_UMAC_SYSRAM */
    {0x00400000u, 0x080000u, 0x0010000u}, /* WF_MCU_SYSRAM */
    {0x00410000u, 0x090000u, 0x0010000u}, /* WF_MCU_SYSRAM (configure register) */
    {0x820f0000u, 0x0a0000u, 0x0000400u}, /* LMAC BN1 WF_CFG */
    {0x820f1000u, 0x0a0600u, 0x0000200u}, /* LMAC BN1 WF_TRB */
    {0x820f2000u, 0x0a0800u, 0x0000400u}, /* LMAC BN1 WF_AGG */
    {0x820f3000u, 0x0a0c00u, 0x0000400u}, /* LMAC BN1 WF_ARB */
    {0x820f4000u, 0x0a1000u, 0x0000400u}, /* LMAC BN1 WF_TMAC */
    {0x820f5000u, 0x0a1400u, 0x0000800u}, /* LMAC BN1 WF_RMAC */
    {0x820f7000u, 0x0a1e00u, 0x0000200u}, /* LMAC BN1 WF_DMA */
    {0x820f9000u, 0x0a3400u, 0x0000200u}, /* LMAC BN1 WF_WTBLOFF */
    {0x820fa000u, 0x0a4000u, 0x0000200u}, /* LMAC BN1 WF_ETBF */
    {0x820fb000u, 0x0a4200u, 0x0000400u}, /* LMAC BN1 WF_LPON */
    {0x820fc000u, 0x0a4600u, 0x0000200u}, /* LMAC BN1 WF_INT */
    {0x820fd000u, 0x0a4800u, 0x0000800u}, /* LMAC BN1 WF_MIB */
    {0x820c4000u, 0x0a8000u, 0x0004000u}, /* LMAC BN1 WF_MUCOP */
    {0x820b0000u, 0x0ae000u, 0x0001000u}, /* [APB2] WFSYS_ON */
    {0x80020000u, 0x0b0000u, 0x0010000u}, /* WF_TOP_MISC_OFF */
    {0x81020000u, 0x0c0000u, 0x0010000u}, /* WF_TOP_MISC_ON */
    {0x7c020000u, 0x0d0000u, 0x0010000u}, /* CONN_INFRA, wfdma */
    {0x7c060000u, 0x0e0000u, 0x0010000u}, /* CONN_INFRA, conn_host_csr_top */
    {0x7c000000u, 0x0f0000u, 0x0010000u}, /* CONN_INFRA */
    {0x70020000u, 0x1f0000u, 0x0010000u}, /* reserved for CBTOP, can't switch */
    {0x7c500000u, 0x060000u, 0x0200000u}, /* remap */
};

int mt7925_fixed_map(uint32_t addr, uint32_t *off) {
    if (addr < MT7925_BAR0_BYTES) {
        *off = addr;
        return 1;
    }
    for (size_t i = 0; i < sizeof(fixed_map) / sizeof(fixed_map[0]); i++) {
        /* The reference accepts ofs == size (one past the window); we do not. */
        if (addr >= fixed_map[i].phys && addr - fixed_map[i].phys < fixed_map[i].size) {
            uint32_t o = fixed_map[i].bar + (addr - fixed_map[i].phys);
            /* The 0x7c500000 entry spans 2 MiB from BAR0 0x60000; keep inside BAR0. */
            if (o >= MT7925_BAR0_BYTES)
                return 0;
            *off = o;
            return 1;
        }
    }
    return 0;
}

int mt7925_uses_l1(uint32_t addr) {
    return (addr >= 0x18000000u && addr < 0x18c00000u) || (addr >= 0x70000000u && addr < 0x78000000u) ||
           (addr >= 0x7c000000u && addr < 0x7c400000u);
}

/* Access one remapped register: point the window at it, access, restore.
 * The reference restores lazily (and, on the L2 path, writes the saved L1
 * value into L2, never restoring L1); we save and restore both, immediately. */
static uint32_t remap_access(mt7925_dev *d, uint32_t addr, int write, uint32_t v) {
    const mt7925_ops *o = &d->ops;
    uint32_t l1 = o->rd(o->ctx, MT7925_HIF_REMAP_L1), r = 0;
    if (mt7925_uses_l1(addr)) {
        o->wr(o->ctx, MT7925_HIF_REMAP_L1, (l1 & 0xffffu) | (addr & 0xffff0000u));
        (void)o->rd(o->ctx, MT7925_HIF_REMAP_L1); /* read back pushes the write */
        uint32_t win = MT7925_HIF_REMAP_BASE_L1 + (addr & 0xffffu);
        if (write)
            o->wr(o->ctx, win, v);
        else
            r = o->rd(o->ctx, win);
        o->wr(o->ctx, MT7925_HIF_REMAP_L1, l1);
    } else {
        uint32_t l2 = o->rd(o->ctx, MT7925_HIF_REMAP_L2);
        o->wr(o->ctx, MT7925_HIF_REMAP_L1, (l1 & 0xffffu) | (MT7925_HIF_REMAP_BASE_L2 & 0xffff0000u));
        o->wr(o->ctx, MT7925_HIF_REMAP_L2, addr);
        (void)o->rd(o->ctx, MT7925_HIF_REMAP_L1);
        if (write)
            o->wr(o->ctx, MT7925_HIF_REMAP_BASE_L1, v);
        else
            r = o->rd(o->ctx, MT7925_HIF_REMAP_BASE_L1);
        o->wr(o->ctx, MT7925_HIF_REMAP_L2, l2);
        o->wr(o->ctx, MT7925_HIF_REMAP_L1, l1);
    }
    return r;
}

uint32_t mt7925_rr(mt7925_dev *d, uint32_t addr) {
    uint32_t off;
    if (mt7925_fixed_map(addr, &off))
        return d->ops.rd(d->ops.ctx, off);
    return remap_access(d, addr, 0, 0);
}

void mt7925_wr(mt7925_dev *d, uint32_t addr, uint32_t v) {
    uint32_t off;
    if (mt7925_fixed_map(addr, &off))
        d->ops.wr(d->ops.ctx, off, v);
    else
        (void)remap_access(d, addr, 1, v);
}

void mt7925_rmw(mt7925_dev *d, uint32_t addr, uint32_t clear, uint32_t set) {
    mt7925_wr(d, addr, (mt7925_rr(d, addr) & ~clear) | set);
}

int mt7925_poll(mt7925_dev *d, uint32_t addr, uint32_t mask, uint32_t want, uint32_t budget_us) {
    for (uint32_t spent = 0;; spent += MT7925_POLL_STEP_US) {
        if ((mt7925_rr(d, addr) & mask) == want)
            return 1;
        if (spent >= budget_us)
            return 0;
        d->ops.udelay(d->ops.ctx, MT7925_POLL_STEP_US);
    }
}

/* ---- power ownership and reset ---- */

static mt7925_err own(mt7925_dev *d, uint32_t bit, uint32_t want, mt7925_err fail) {
    for (uint32_t i = 0; i < MT7925_OWN_TRIES; i++) {
        mt7925_wr(d, MT7925_CONN_ON_LPCTL, bit);
        if (mt7925_poll(d, MT7925_CONN_ON_LPCTL, MT7925_LPCR_HOST_OWN_SYNC, want, MT7925_OWN_POLL_US))
            return MT7925_OK;
    }
    return fail;
}

static mt7925_err wfsys_reset(mt7925_dev *d) {
    mt7925_rmw(d, MT7925_WFSYS_SW_RST, MT7925_WFSYS_SW_RST_B, 0);
    d->ops.udelay(d->ops.ctx, MT7925_RESET_HOLD_US);
    mt7925_rmw(d, MT7925_WFSYS_SW_RST, 0, MT7925_WFSYS_SW_RST_B);
    if (!mt7925_poll(d, MT7925_WFSYS_SW_RST, MT7925_WFSYS_SW_INIT_DONE, MT7925_WFSYS_SW_INIT_DONE,
                     MT7925_RESET_POLL_US))
        return MT7925_E_RESET;
    return MT7925_OK;
}

#define STOP_IDLE_US 100000u

mt7925_err mt7925_stop(mt7925_dev *d) {
    if (!d) return MT7925_E_ARG;
    mt7925_wr(d, MT7925_HOST_INT_ENA, 0);
    mt7925_rmw(d, MT7925_GLO_CFG,
               MT7925_GLO_TX_DMA_EN | MT7925_GLO_RX_DMA_EN | MT7925_GLO_DISP_BASE_PTR_CHAIN_EN |
                   MT7925_GLO_OMIT_TX_INFO | MT7925_GLO_OMIT_RX_INFO_PFET2,
               0);
    int idle = mt7925_poll(d, MT7925_GLO_CFG, MT7925_GLO_TX_DMA_BUSY | MT7925_GLO_RX_DMA_BUSY, 0, STOP_IDLE_US);
    d->dma_on = 0;
    d->fw_running = 0;
    mt7925_err e = own(d, MT7925_LPCR_HOST_SET_OWN, MT7925_LPCR_HOST_OWN_SYNC, MT7925_E_FW_OWN);
    return idle ? e : MT7925_E_TIMEOUT;
}

/* ---- DMA region and rings ---- */

#define DESC_SLOT 4096u
static uint32_t ring_bytes(uint32_t n, uint32_t len) { return n * len; }

size_t mt7925_dma_bytes(void) {
    return 5u * DESC_SLOT + ring_bytes(MT7925_N_TX_DATA, MT7925_BUF_BYTES) + ring_bytes(MT7925_N_TX_MCU, MT7925_BUF_BYTES) +
           ring_bytes(MT7925_N_TX_FWDL, MT7925_FWDL_BUF_BYTES) + ring_bytes(MT7925_N_RX_EVENT, MT7925_BUF_BYTES) +
           ring_bytes(MT7925_N_RX_DATA, MT7925_BUF_BYTES);
}

static uint8_t *desc_ptr(mt7925_dev *d, const mt7925_ring *r, uint32_t i) {
    return d->mem + r->desc_off + (size_t)i * MT7925_DESC_BYTES;
}
static uint8_t *buf_ptr(mt7925_dev *d, const mt7925_ring *r, uint32_t i) {
    return d->mem + r->buf_off + (size_t)i * r->buf_len;
}
static uint32_t buf_bus(const mt7925_dev *d, const mt7925_ring *r, uint32_t i) {
    return (uint32_t)(d->dma + r->buf_off + (uint64_t)i * r->buf_len);
}

static void write_desc(mt7925_dev *d, const mt7925_ring *r, uint32_t i, uint32_t buf0, uint32_t ctrl) {
    uint8_t *p = desc_ptr(d, r, i);
    put_le32(p, buf0);
    put_le32(p + 8, 0);
    put_le32(p + 12, 0);
    put_le32(p + 4, ctrl);
}

static void ring_layout(mt7925_ring *r, uint32_t regs, uint32_t n, uint32_t desc_off, uint32_t buf_off,
                        uint32_t buf_len) {
    *r = (mt7925_ring){0};
    r->regs = regs;
    r->n = (uint16_t)n;
    r->desc_off = desc_off;
    r->buf_off = buf_off;
    r->buf_len = buf_len;
}

/* Program a ring's base and size and read back the device index. A fresh
 * ring after reset must report index 0. */
static mt7925_err ring_program(mt7925_dev *d, mt7925_ring *r) {
    mt7925_wr(d, r->regs + MT7925_RING_SIZE, r->n);
    mt7925_wr(d, r->regs + MT7925_RING_DESC_BASE, (uint32_t)(d->dma + r->desc_off));
    uint32_t didx = mt7925_rr(d, r->regs + MT7925_RING_DMA_IDX);
    if (didx >= r->n)
        return MT7925_E_DEVICE;
    r->head = r->tail = (uint16_t)didx;
    return MT7925_OK;
}

static mt7925_err tx_ring_setup(mt7925_dev *d, mt7925_ring *r) {
    for (uint32_t i = 0; i < r->n; i++)
        write_desc(d, r, i, 0, MT7925_DESC_DMA_DONE);
    d->ops.dma_barrier(d->ops.ctx);
    mt7925_err e = ring_program(d, r);
    if (e == MT7925_OK)
        mt7925_wr(d, r->regs + MT7925_RING_CPU_IDX, r->head);
    return e;
}

/* RX rings hold n-1 posted buffers; cpu_idx is one past the last posted. */
static mt7925_err rx_ring_setup(mt7925_dev *d, mt7925_ring *r) {
    mt7925_err e = ring_program(d, r);
    if (e != MT7925_OK)
        return e;
    for (uint32_t k = 0; k + 1 < r->n; k++) {
        uint32_t i = (r->head + k) % r->n;
        write_desc(d, r, i, buf_bus(d, r, i), (r->buf_len << MT7925_DESC_SD_LEN0_SHIFT) & MT7925_DESC_SD_LEN0_MASK);
        r->posted_len[i] = (uint16_t)r->buf_len;
    }
    write_desc(d, r, (r->head + r->n - 1u) % r->n, 0, 0);
    d->ops.dma_barrier(d->ops.ctx);
    mt7925_wr(d, r->regs + MT7925_RING_CPU_IDX, (r->head + r->n - 1u) % r->n);
    return MT7925_OK;
}

static void dma_enable(mt7925_dev *d) {
    /* prefetch: base << 16 | depth (reference mt7925_dma_prefetch_setup) */
    mt7925_wr(d, MT7925_RX_EXT_CTRL(0), (0x0000u << 16) | 0x4u);
    mt7925_wr(d, MT7925_RX_EXT_CTRL(1), (0x0040u << 16) | 0x4u);
    mt7925_wr(d, MT7925_RX_EXT_CTRL(2), (0x0080u << 16) | 0x4u);
    mt7925_wr(d, MT7925_RX_EXT_CTRL(3), (0x00c0u << 16) | 0x4u);
    mt7925_wr(d, MT7925_TX_EXT_CTRL(0), (0x0100u << 16) | 0x10u);
    mt7925_wr(d, MT7925_TX_EXT_CTRL(1), (0x0200u << 16) | 0x10u);
    mt7925_wr(d, MT7925_TX_EXT_CTRL(2), (0x0300u << 16) | 0x10u);
    mt7925_wr(d, MT7925_TX_EXT_CTRL(3), (0x0400u << 16) | 0x10u);
    mt7925_wr(d, MT7925_TX_EXT_CTRL(15), (0x0500u << 16) | 0x4u);
    mt7925_wr(d, MT7925_TX_EXT_CTRL(16), (0x0540u << 16) | 0x4u);
    mt7925_wr(d, MT7925_RST_DTX_PTR, 0xffffffffu);
    mt7925_wr(d, MT7925_RST_DRX_PTR, 0xffffffffu);
    mt7925_wr(d, MT7925_PRI_DLY_INT_CFG0, 0);
    mt7925_rmw(d, MT7925_GLO_CFG, 0,
               MT7925_GLO_TX_WB_DDONE | MT7925_GLO_FIFO_LITTLE_ENDIAN | MT7925_GLO_CLK_GAT_DIS |
                   MT7925_GLO_OMIT_TX_INFO | (3u << MT7925_GLO_DMA_SIZE_SHIFT) | MT7925_GLO_FIFO_DIS_CHECK |
                   MT7925_GLO_RX_WB_DDONE | MT7925_GLO_DISP_BASE_PTR_CHAIN_EN | MT7925_GLO_OMIT_RX_INFO_PFET2);
    mt7925_rmw(d, MT7925_GLO_CFG, 0, MT7925_GLO_TX_DMA_EN | MT7925_GLO_RX_DMA_EN);
    mt7925_rmw(d, MT7925_UWFDMA0_GLO_CFG_EXT1, 1u << 28, 1u << 28);
    mt7925_rmw(d, MT7925_INT_RX_PRI, 0, 0x0f00u);
    mt7925_rmw(d, MT7925_INT_TX_PRI, 0, 0x7f00u);
    mt7925_rmw(d, MT7925_WFDMA_DUMMY_CR, 0, MT7925_WFDMA_NEED_REINIT);
    /* Polled driver: host interrupt sources stay masked (reference enables them). */
    mt7925_wr(d, MT7925_HOST_INT_ENA, 0);
    mt7925_rmw(d, MT7925_MCU2HOST_SW_INT_ENA, 0, MT7925_MCU_CMD_WAKE_RX_PCIE);
}

mt7925_err mt7925_init(mt7925_dev *d, const mt7925_ops *ops, uint8_t *mem, uint64_t dma, size_t len) {
    if (!d || !ops || !ops->rd || !ops->wr || !ops->udelay || !ops->dma_barrier || !mem)
        return MT7925_E_ARG;
    mt7925_event_fn ev = d->on_event;
    mt7925_rx_fn rx = d->on_rx;
    void *cb = d->cb_ctx, *rc = d->rx_ctx;
    zero((uint8_t *)d, sizeof(*d));
    d->on_event = ev;
    d->on_rx = rx;
    d->cb_ctx = cb;
    d->rx_ctx = rc;
    d->ops = *ops;
    if (len < mt7925_dma_bytes() || (dma & (DESC_SLOT - 1u)) || dma >= MT7925_DMA_LIMIT ||
        len > MT7925_DMA_LIMIT - dma)
        return MT7925_E_MEM;
    d->mem = mem;
    d->dma = dma;
    d->mem_len = len;

    mt7925_err e = own(d, MT7925_LPCR_HOST_SET_OWN, MT7925_LPCR_HOST_OWN_SYNC, MT7925_E_FW_OWN);
    if (e == MT7925_OK)
        e = own(d, MT7925_LPCR_HOST_CLR_OWN, 0, MT7925_E_DRV_OWN);
    if (e != MT7925_OK)
        return e;
    d->chip_rev = (mt7925_rr(d, MT7925_HW_CHIPID) << 16) | (mt7925_rr(d, MT7925_HW_REV) & 0xffu);
    mt7925_rmw(d, MT7925_HW_EMI_CTL, 0, MT7925_HW_EMI_CTL_SLPPROT_EN);
    if ((e = wfsys_reset(d)) != MT7925_OK)
        return e;
    mt7925_wr(d, MT7925_HOST_INT_ENA, 0);
    mt7925_wr(d, MT7925_PCIE_MAC_INT_ENABLE, 0);

    zero(mem, mt7925_dma_bytes());
    uint32_t off = 5u * DESC_SLOT;
    ring_layout(&d->tx_data, MT7925_TX_RING_BASE + MT7925_TXQ_DATA * 0x10u, MT7925_N_TX_DATA, 0 * DESC_SLOT, off,
                MT7925_BUF_BYTES);
    off += ring_bytes(MT7925_N_TX_DATA, MT7925_BUF_BYTES);
    ring_layout(&d->tx_mcu, MT7925_TX_RING_BASE + MT7925_TXQ_MCU * 0x10u, MT7925_N_TX_MCU, 1 * DESC_SLOT, off,
                MT7925_BUF_BYTES);
    off += ring_bytes(MT7925_N_TX_MCU, MT7925_BUF_BYTES);
    ring_layout(&d->tx_fwdl, MT7925_TX_RING_BASE + MT7925_TXQ_FWDL * 0x10u, MT7925_N_TX_FWDL, 2 * DESC_SLOT, off,
                MT7925_FWDL_BUF_BYTES);
    off += ring_bytes(MT7925_N_TX_FWDL, MT7925_FWDL_BUF_BYTES);
    ring_layout(&d->rx_evt, MT7925_RX_RING_BASE + MT7925_RXQ_EVENT * 0x10u, MT7925_N_RX_EVENT, 3 * DESC_SLOT, off,
                MT7925_BUF_BYTES);
    off += ring_bytes(MT7925_N_RX_EVENT, MT7925_BUF_BYTES);
    ring_layout(&d->rx_data, MT7925_RX_RING_BASE + MT7925_RXQ_DATA * 0x10u, MT7925_N_RX_DATA, 4 * DESC_SLOT, off,
                MT7925_BUF_BYTES);

    mt7925_ring *tx[3] = {&d->tx_data, &d->tx_mcu, &d->tx_fwdl};
    for (int i = 0; i < 3 && e == MT7925_OK; i++)
        e = tx_ring_setup(d, tx[i]);
    if (e == MT7925_OK)
        e = rx_ring_setup(d, &d->rx_evt);
    if (e == MT7925_OK)
        e = rx_ring_setup(d, &d->rx_data);
    if (e != MT7925_OK) {
        d->broken = 1;
        return e;
    }
    dma_enable(d);
    /* dma_enable resets every ring index; each must now match our shadow. */
    mt7925_ring *all[5] = {&d->tx_data, &d->tx_mcu, &d->tx_fwdl, &d->rx_evt, &d->rx_data};
    for (int i = 0; i < 5; i++)
        if (mt7925_rr(d, all[i]->regs + MT7925_RING_DMA_IDX) != all[i]->head) {
            d->broken = 1;
            return MT7925_E_DEVICE;
        }
    d->dma_on = 1;
    return MT7925_OK;
}

/* ---- TX ---- */

static mt7925_err tx_reclaim(mt7925_dev *d, mt7925_ring *r) {
    uint32_t didx = mt7925_rr(d, r->regs + MT7925_RING_DMA_IDX);
    if (didx >= r->n) {
        d->broken = 1;
        return MT7925_E_DEVICE;
    }
    uint32_t done = (didx + r->n - r->tail) % r->n;
    if (done > r->inflight) { /* device claims slots that were never posted */
        d->broken = 1;
        return MT7925_E_DEVICE;
    }
    r->tail = (uint16_t)didx;
    r->inflight = (uint16_t)(r->inflight - done);
    return MT7925_OK;
}

/* Copy up to three pieces into the next slot and post it. The descriptor
 * covers the first `desc_len` bytes (0 = all of them): cut-through data
 * frames post only TXD + TXP and leave the frame behind it for the chip to
 * fetch through the TXP pointer. */
static mt7925_err tx_post(mt7925_dev *d, mt7925_ring *r, const uint8_t *a, uint32_t al, const uint8_t *b,
                          uint32_t bl, const uint8_t *c, uint32_t cl, uint32_t desc_len) {
    if (d->broken)
        return MT7925_E_BROKEN;
    if ((uint64_t)al + bl + cl > r->buf_len || al + bl + cl == 0)
        return MT7925_E_ARG;
    mt7925_err e = tx_reclaim(d, r);
    if (e != MT7925_OK)
        return e;
    for (uint32_t spent = 0; r->inflight + 1u >= r->n; spent += MT7925_POLL_STEP_US) {
        if (spent >= MT7925_MCU_REPLY_US)
            return MT7925_E_FULL;
        d->ops.udelay(d->ops.ctx, MT7925_POLL_STEP_US);
        if ((e = tx_reclaim(d, r)) != MT7925_OK)
            return e;
    }
    /* A cut-through slot stays held until TX_FREE releases its token; that
     * arrives on an RX ring, so the caller polls and retries (no nested RX
     * processing from inside a send). */
    if (r == &d->tx_data && ((d->tok_busy >> r->head) & 1u))
        return MT7925_E_FULL;
    uint32_t i = r->head, n = al + bl + cl, dl = desc_len ? desc_len : n;
    uint8_t *p = buf_ptr(d, r, i);
    if (al) copy(p, a, al);
    if (bl) copy(p + al, b, bl);
    if (cl) copy(p + al + bl, c, cl);
    write_desc(d, r, i, buf_bus(d, r, i),
               ((dl << MT7925_DESC_SD_LEN0_SHIFT) & MT7925_DESC_SD_LEN0_MASK) | MT7925_DESC_LAST_SEC0);
    r->posted_len[i] = (uint16_t)dl;
    r->head = (uint16_t)((i + 1u) % r->n);
    r->inflight++;
    d->ops.dma_barrier(d->ops.ctx);
    mt7925_wr(d, r->regs + MT7925_RING_CPU_IDX, r->head);
    return MT7925_OK;
}

mt7925_err mt7925_tx_raw(mt7925_dev *d, const uint8_t *txd, uint32_t txd_len, const uint8_t *frame,
                         uint32_t frame_len) {
    if (!d || (!txd && txd_len) || (!frame && frame_len))
        return MT7925_E_ARG;
    if (!d->dma_on)
        return MT7925_E_STATE;
    return tx_post(d, &d->tx_data, txd, txd_len, frame, frame_len, 0, 0, 0);
}

/* ---- Data TX ---- */

/* 802.11 header length for the frames this driver sends (management, plain
 * or QoS data, three addresses); 0 for anything else. */
static uint32_t hdr_len_80211(const uint8_t *f, uint32_t len) {
    if (len < 24u || (f[0] & 3u))
        return 0;
    uint32_t type = (f[0] >> 2) & 3u;
    if (type == 0u)
        return 24u;
    if (type != 2u || (f[1] & 3u) == 3u)
        return 0;
    uint32_t h = (f[0] & 0x80u) ? 26u : 24u;
    return h <= len ? h : 0;
}

mt7925_err mt7925_txd_build(uint8_t out[MT7925_TX_HDR_BYTES], const uint8_t *frame, uint32_t frame_len,
                            const mt7925_tx_opts *o, uint16_t token, uint32_t frame_bus) {
    if (!out || !frame || !o || frame_len > MT7925_TX_FRAME_MAX || token >= MT7925_TXFREE_INFO_MSDU_ID ||
        o->wlan_idx > MT7925_TXD1_WLAN_IDX_MASK || o->omac_idx > 0x3fu || o->tid > 7u)
        return MT7925_E_ARG;
    uint32_t hl = hdr_len_80211(frame, frame_len);
    if (!hl)
        return MT7925_E_ARG;
    uint32_t type = (frame[0] >> 2) & 3u, subtype = frame[0] >> 4u;
    int data = type == 2u, mcast = frame[4] & 1u;
    uint32_t w[8] = {0};
    w[0] = ((frame_len + MT7925_TXD_BYTES) & MT7925_TXD0_TX_BYTES_MASK) |
           (MT7925_TX_TYPE_CT << MT7925_TXD0_PKT_FMT_SHIFT) |
           ((data ? MT7925_LMAC_AC_BE : MT7925_LMAC_ALTX0) << MT7925_TXD0_Q_IDX_SHIFT);
    w[1] = o->wlan_idx | ((uint32_t)o->omac_idx << MT7925_TXD1_OWN_MAC_SHIFT) |
           (MT7925_HDR_FORMAT_802_11 << MT7925_TXD1_HDR_FORMAT_SHIFT) | ((hl / 2u) << MT7925_TXD1_HDR_INFO_SHIFT) |
           ((data ? o->tid : 0u) << MT7925_TXD1_TID_SHIFT);
    w[2] = (type << MT7925_TXD2_FRAME_TYPE_SHIFT) | (subtype & MT7925_TXD2_SUB_TYPE_MASK);
    w[3] = (15u << MT7925_TXD3_REM_TX_COUNT_SHIFT) | (mcast ? MT7925_TXD3_BCM : 0u) |
           (o->protect ? MT7925_TXD3_PROTECT_FRAME : 0u) | (o->no_ack ? MT7925_TXD3_NO_ACK : 0u);
    w[6] = MT7925_TXD6_DAS | MT7925_TXD6_DIS_MAT | (1u << MT7925_TXD6_MSDU_CNT_SHIFT);
    if (!data || mcast) { /* management and group frames go at the basic rate */
        w[1] |= MT7925_TXD1_FIXED_RATE;
        w[3] |= MT7925_TXD3_BA_DISABLE;
        w[6] |= MT7925_BASIC_RATES_TBL << MT7925_TXD6_TX_RATE_SHIFT;
    }
    for (int k = 0; k < 8; k++)
        put_le32(out + 4 * k, w[k]);
    /* Hardware TXP: msdu_id[4], then {buf0, len0, len1, buf1} x 2. */
    uint8_t *t = out + MT7925_TXD_BYTES;
    for (uint32_t k = 0; k < MT7925_TXP_BYTES; k++)
        t[k] = 0;
    uint16_t id = (uint16_t)(token | MT7925_MSDU_ID_VALID);
    t[0] = (uint8_t)id;
    t[1] = (uint8_t)(id >> 8);
    put_le32(t + 8, frame_bus);
    uint16_t l0 = (uint16_t)((frame_len & MT7925_TXP_LEN_MASK) | MT7925_TXP_LEN_LAST);
    t[12] = (uint8_t)l0;
    t[13] = (uint8_t)(l0 >> 8);
    return MT7925_OK;
}

mt7925_err mt7925_tx_80211(mt7925_dev *d, const uint8_t *frame, uint32_t frame_len, const mt7925_tx_opts *o) {
    if (!d || !frame || !o)
        return MT7925_E_ARG;
    if (!d->dma_on || !d->fw_running)
        return MT7925_E_STATE;
    if (d->broken)
        return MT7925_E_BROKEN;
    mt7925_ring *r = &d->tx_data;
    uint32_t i = r->head; /* tx_post fills exactly this slot or fails */
    uint8_t hdr[MT7925_TX_HDR_BYTES];
    mt7925_err e = mt7925_txd_build(hdr, frame, frame_len, o, (uint16_t)i, buf_bus(d, r, i) + MT7925_TX_HDR_BYTES);
    if (e != MT7925_OK)
        return e;
    e = tx_post(d, r, hdr, sizeof hdr, frame, frame_len, 0, 0, MT7925_TX_HDR_BYTES);
    if (e != MT7925_OK)
        return e;
    d->tok_busy |= 1u << i;
    return MT7925_OK;
}

mt7925_err mt7925_tx_free(mt7925_dev *d, const uint8_t *rec, uint32_t len) {
    if (!d || !rec)
        return MT7925_E_ARG;
    if (d->broken)
        return MT7925_E_BROKEN;
    if (len < 8u || ((get_le32(rec + 4) >> MT7925_TXFREE1_VER_SHIFT) & 0xfu) < 4u)
        goto bad;
    uint32_t total = (get_le32(rec) >> MT7925_TXFREE0_MSDU_CNT_SHIFT) & MT7925_TXFREE0_MSDU_CNT_MASK, count = 0;
    for (uint32_t off = 8; count < total; off += 4) {
        if (off + 4u > len)
            goto bad; /* record ends before the count it announced */
        uint32_t info = get_le32(rec + off);
        if (info & (MT7925_TXFREE_INFO_PAIR | MT7925_TXFREE_INFO_HEADER))
            continue; /* station pair / retry-count header: nothing to release */
        for (int k = 0; k < 2; k++) {
            uint32_t id = (info >> (15 * k)) & MT7925_TXFREE_INFO_MSDU_ID;
            if (id == MT7925_TXFREE_INFO_MSDU_ID)
                continue;
            count++;
            if (id >= d->tx_data.n || !((d->tok_busy >> id) & 1u))
                goto bad; /* the chip releases a frame it does not hold */
            d->tok_busy &= ~(1u << id);
            d->tx_freed++;
        }
    }
    return MT7925_OK;
bad:
    d->broken = 1;
    return MT7925_E_DEVICE;
}

/* ---- Data RX ---- */

mt7925_rx_err mt7925_rx_parse(const uint8_t *buf, uint32_t len, mt7925_rx_info *ri) {
    if (!buf || !ri || len < MT7925_RXD_BYTES)
        return MT7925_RX_SHORT;
    *ri = (mt7925_rx_info){0};
    uint32_t rxd0 = get_le32(buf), rxd1 = get_le32(buf + 4), rxd2 = get_le32(buf + 8), rxd3 = get_le32(buf + 12);
    uint32_t type = rxd0 >> MT7925_RXD0_PKT_TYPE_SHIFT;
    if (type != MT7925_PKT_TYPE_NORMAL &&
        ((rxd0 >> MT7925_RXD0_SW_PKT_TYPE_SHIFT) & MT7925_RXD0_SW_PKT_TYPE_MAP) != MT7925_RXD0_SW_PKT_TYPE_FRAME)
        return MT7925_RX_NOT_FRAME;
    /* The RXD's own length field must agree with what the ring delivered. */
    uint32_t rlen = rxd0 & MT7925_RXD0_LEN_MASK;
    if (rlen < MT7925_RXD_BYTES || rlen > len)
        return MT7925_RX_SHORT;
    len = rlen;
    if (rxd2 & (MT7925_RXD2_AMSDU_ERR | MT7925_RXD2_MAX_LEN_ERR))
        return MT7925_RX_LEN_ERR;
    if (rxd2 & MT7925_RXD2_HDR_TRANS)
        return MT7925_RX_UNSUPPORTED; /* this driver never enables header translation */
    if (rxd3 & MT7925_RXD3_FCS_ERR)
        return MT7925_RX_BAD_FCS;
    if (rxd1 & MT7925_RXD1_ICV_ERR)
        return MT7925_RX_BAD_ICV;
    if (rxd1 & MT7925_RXD1_TKIP_MIC_ERR)
        return MT7925_RX_BAD_MIC;
    uint32_t off = MT7925_RXD_BYTES;
    const uint8_t *rxv = 0;
    /* Optional groups in the reference's order: 4, 1, 2, 3 (+5 inside 3). */
    if (rxd1 & MT7925_RXD1_GROUP_4) off += 16u;
    const uint8_t *g1 = 0;
    if (rxd1 & MT7925_RXD1_GROUP_1) {
        if (off + 16u > len) return MT7925_RX_SHORT;
        g1 = buf + off;
        off += 16u;
    }
    if (rxd1 & MT7925_RXD1_GROUP_2) off += 16u;
    if (rxd1 & MT7925_RXD1_GROUP_3) {
        if (off + 16u > len) return MT7925_RX_SHORT;
        rxv = buf + off;
        off += 16u;
        if (rxd1 & MT7925_RXD1_GROUP_5) off += 96u;
    }
    if ((buf[16] & MT7925_RXD4_PAYLOAD_FORMAT_MASK) != 0)
        return MT7925_RX_UNSUPPORTED; /* A-MSDU subframe */
    off += 2u * ((rxd2 >> MT7925_RXD2_HDR_OFFSET_SHIFT) & MT7925_RXD2_HDR_OFFSET_MASK);
    if (off + 24u > len)
        return MT7925_RX_SHORT;
    ri->frame = buf + off;
    ri->frame_len = len - off;
    ri->wlan_idx = (uint16_t)(rxd1 & MT7925_RXD1_WLAN_IDX_MASK);
    ri->channel = (uint8_t)(rxd3 >> MT7925_RXD3_CH_FREQ_SHIFT);
    ri->sec_mode = (uint8_t)((rxd2 >> MT7925_RXD2_SEC_MODE_SHIFT) & MT7925_RXD2_SEC_MODE_MASK);
    int protected_bit = (ri->frame[1] & 0x40u) != 0;
    if (ri->sec_mode && !(rxd1 & (MT7925_RXD1_CM | MT7925_RXD1_CLM))) {
        if (ri->sec_mode != MT7925_CIPHER_CCMP)
            return MT7925_RX_CIPHER; /* only CCMP-128 keys are ever installed */
        if (rxd2 & MT7925_RXD2_FRAG)
            return MT7925_RX_UNSUPPORTED; /* fragments keep the CCMP header: not handled */
        ri->decrypted = 1;
        /* The chip strips the CCMP header and does not check the packet
         * number; the replay check is the host's (wifi_sta). */
        if (g1) {
            ri->has_pn = 1;
            for (int k = 5; k >= 0; k--) ri->pn = (ri->pn << 8) | g1[k];
        }
    }
    if (protected_bit && !ri->decrypted)
        return MT7925_RX_CIPHER; /* ciphertext the chip did not open: never pass it on */
    if (rxv) {
        ri->has_rssi = 1;
        ri->rssi = (int8_t)(((int)rxv[12] - 220) / 2); /* P-RXV word 3 RCPI0 */
    }
    return MT7925_RX_OK;
}

/* ---- RX ---- */

/* Handle one completed RX descriptor at r->head if there is one. Returns 1
 * if a buffer was consumed, 0 if none ready, negative mt7925_err. */
static int rx_one(mt7925_dev *d, mt7925_ring *r, int events, uint8_t want_seq, uint8_t *reply, uint32_t cap,
                  uint32_t *reply_len, int *matched) {
    uint32_t i = r->head;
    uint32_t cpu = (r->head + r->n - 1u) % r->n; /* slot that is not posted */
    d->ops.dma_barrier(d->ops.ctx);
    uint32_t ctrl = get_le32(desc_ptr(d, r, i) + 4);
    if (!(ctrl & MT7925_DESC_DMA_DONE))
        return 0;
    if (!r->posted_len[i]) { /* done bit on a slot we never posted */
        d->broken = 1;
        return -(int)MT7925_E_DEVICE;
    }
    uint32_t len = (ctrl & MT7925_DESC_SD_LEN0_MASK) >> MT7925_DESC_SD_LEN0_SHIFT;
    if (len > r->posted_len[i]) {
        d->broken = 1;
        return -(int)MT7925_E_DEVICE;
    }
    int ret = 1;
    /* Callbacks run after the slot is consumed and re-armed, on a copy in
     * this nesting level's buffer: a callback may poll again (TX ring full,
     * a key install waiting on the MCU), and the nested poll must see this
     * slot as done and must not overwrite the frame being delivered. */
    uint8_t *cb = 0;
    int cb_eid = -1;
    const uint8_t *b = buf_ptr(d, r, i);
    int last = (ctrl & MT7925_DESC_LAST_SEC0) != 0;
    if (!last || len < 4) {
        d->events_dropped++; /* multi-buffer frames are not supported: drop */
    } else {
        uint32_t rxd0 = get_le32(b);
        uint32_t type = rxd0 >> MT7925_RXD0_PKT_TYPE_SHIFT;
        /* Frames the MCU forwards look like events with PKT_FLAG 1 (reference
         * SW_PKT_TYPE_FRAME / NORMAL_MCU): treat them as frames. */
        if (type != MT7925_PKT_TYPE_NORMAL &&
            ((rxd0 >> MT7925_RXD0_SW_PKT_TYPE_SHIFT) & MT7925_RXD0_SW_PKT_TYPE_MAP) == MT7925_RXD0_SW_PKT_TYPE_FRAME)
            type = MT7925_PKT_TYPE_NORMAL_MCU;
        if (events && type == MT7925_PKT_TYPE_RX_EVENT && len >= MT7925_EVT_HDR_BYTES) {
            uint8_t seq = b[MT7925_EVT_SEQ_OFF], opt = b[MT7925_EVT_OPTION_OFF];
            d->events_seen++;
            if (matched && !*matched && want_seq && seq == want_seq && !(opt & MT7925_EVT_OPT_UNSOLICITED)) {
                uint32_t n = len < cap ? len : cap;
                if (reply && n) copy(reply, b, n);
                if (reply_len) *reply_len = len;
                *matched = 1;
            } else if (d->on_event) {
                if (d->cb_depth < MT7925_CB_DEPTH) {
                    cb = d->cb_buf[d->cb_depth];
                    copy(cb, b, len);
                    cb_eid = b[MT7925_EVT_EID_OFF];
                } else {
                    d->rx_nest_dropped++;
                }
            }
        } else if (type == MT7925_PKT_TYPE_TXRX_NOTIFY) {
            mt7925_err e = mt7925_tx_free(d, b, len);
            if (e != MT7925_OK)
                ret = -(int)e;
        } else if ((type == MT7925_PKT_TYPE_NORMAL || type == MT7925_PKT_TYPE_NORMAL_MCU) && d->on_rx) {
            if (matched) { /* inside a command wait: hold it for the next poll */
                if (d->defer_n < MT7925_RX_DEFER) {
                    copy(d->defer[d->defer_n], b, len);
                    d->defer_len[d->defer_n++] = (uint16_t)len;
                    d->rx_deferred++;
                } else {
                    d->rx_defer_dropped++;
                }
            } else {
                if (d->cb_depth < MT7925_CB_DEPTH) {
                    cb = d->cb_buf[d->cb_depth];
                    copy(cb, b, len);
                } else {
                    d->rx_nest_dropped++;
                }
            }
        } else {
            d->events_dropped++;
        }
    }
    /* Re-arm this buffer at the old not-posted slot's position: the ring
     * moves forward by one, keeping n-1 posted. */
    write_desc(d, r, cpu, buf_bus(d, r, cpu), (r->buf_len << MT7925_DESC_SD_LEN0_SHIFT) & MT7925_DESC_SD_LEN0_MASK);
    r->posted_len[cpu] = (uint16_t)r->buf_len;
    write_desc(d, r, i, 0, 0);
    r->posted_len[i] = 0;
    r->head = (uint16_t)((i + 1u) % r->n);
    d->ops.dma_barrier(d->ops.ctx);
    mt7925_wr(d, r->regs + MT7925_RING_CPU_IDX, i);
    if (cb) {
        d->cb_depth++;
        if (cb_eid >= 0)
            d->on_event(d->cb_ctx, (uint8_t)cb_eid, cb, len);
        else
            d->on_rx(d->rx_ctx, cb, len);
        d->cb_depth--;
    }
    return ret;
}

/* Deliver the frames held during MCU waits, in arrival order. Each is taken
 * off the queue before delivery, so a nested poll from inside on_rx never
 * sees it again; frames held while these are delivered (a key install
 * waits on the MCU) wait for the next drain. */
static int drain_held(mt7925_dev *d) {
    int total = 0;
    uint32_t held = d->defer_n;
    for (uint32_t k = 0; k < held && d->defer_n && d->on_rx; k++) {
        if (d->cb_depth >= MT7925_CB_DEPTH) break;
        uint8_t *f = d->cb_buf[d->cb_depth];
        uint32_t fl = d->defer_len[0];
        copy(f, d->defer[0], fl);
        for (uint32_t j = 1; j < d->defer_n; j++) {
            copy(d->defer[j - 1], d->defer[j], d->defer_len[j]);
            d->defer_len[j - 1] = d->defer_len[j];
        }
        d->defer_n--;
        d->cb_depth++;
        d->on_rx(d->rx_ctx, f, fl);
        d->cb_depth--;
        total++;
    }
    return total;
}

int mt7925_poll_rings(mt7925_dev *d) {
    if (!d)
        return -(int)MT7925_E_ARG;
    if (d->broken)
        return -(int)MT7925_E_BROKEN;
    if (!d->dma_on)
        return -(int)MT7925_E_STATE;
    int total = drain_held(d);
    for (uint32_t k = 0; k < MT7925_RING_MAX; k++) {
        int a = rx_one(d, &d->rx_evt, 1, 0, 0, 0, 0, 0);
        if (a < 0) return a;
        int b = rx_one(d, &d->rx_data, 0, 0, 0, 0, 0, 0);
        if (b < 0) return b;
        if (!a && !b) break;
        total += a + b;
        /* Frames held by an MCU wait inside these callbacks are older than
         * what is still on the rings: deliver them first. */
        total += drain_held(d);
    }
    mt7925_ring *tx[3] = {&d->tx_data, &d->tx_mcu, &d->tx_fwdl};
    for (int i = 0; i < 3; i++) {
        mt7925_err e = tx_reclaim(d, tx[i]);
        if (e != MT7925_OK) return -(int)e;
    }
    return total;
}

/* ---- MCU ---- */

static uint8_t next_seq(mt7925_dev *d) {
    d->mcu_seq = (uint8_t)((d->mcu_seq + 1u) & 0xfu);
    if (!d->mcu_seq)
        d->mcu_seq = 1;
    return d->mcu_seq;
}

mt7925_err mt7925_mcu_send(mt7925_dev *d, uint16_t cmd, int uni, int query, const uint8_t *payload, uint32_t len,
                           int wait, uint8_t *reply, uint32_t reply_cap, uint32_t *reply_len) {
    if (!d || (!payload && len) || len > MT7925_MCU_MAX_PAYLOAD || (!uni && cmd > 0xffu))
        return MT7925_E_ARG;
    if (d->broken)
        return MT7925_E_BROKEN;
    if (!d->dma_on)
        return MT7925_E_STATE;
    uint8_t h[MT7925_TXD_BYTES + MT7925_MCU_HDR_LEGACY];
    uint32_t hl = MT7925_TXD_BYTES + (uni ? MT7925_MCU_HDR_UNI : MT7925_MCU_HDR_LEGACY);
    uint32_t total = hl + len;
    uint8_t seq = next_seq(d);
    zero(h, sizeof(h));
    put_le32(h, (total & 0xffffu) | (MT7925_TX_TYPE_CMD << 23) | (MT7925_MCU_Q_IDX << 25));
    /* connac3 layout: TXD1 HDR_FORMAT is bits 15:14 (mt76_connac3_mac.h MT_TXD1_HDR_FORMAT, used by
     * mt7925/mcu.c mt7925_mcu_fill_message); bits 17:16 is the older connac2 layout. */
    put_le32(h + 4, MT7925_HDR_FORMAT_CMD << MT7925_TXD1_HDR_FORMAT_SHIFT);
    uint8_t *m = h + MT7925_TXD_BYTES;
    put_le16(m, (uint16_t)(total - MT7925_TXD_BYTES));
    if (uni) {
        put_le16(m + 2, cmd);
        m[5] = MT7925_MCU_PKT_ID;
        m[7] = seq;
        m[10] = 0; /* s2d: host to N9 */
        m[11] = (uint8_t)(query ? MT7925_UNI_OPT_QUERY : MT7925_UNI_OPT_SET);
        if (cmd == MT7925_UNI_CMD_HIF_CTRL || cmd == MT7925_UNI_CMD_CHIP_CONFIG)
            m[11] &= (uint8_t)~MT7925_UNI_OPT_ACK; /* mt7925/mcu.c: these two never carry the ACK bit */
    } else {
        put_le16(m + 2, (uint16_t)MT7925_MCU_PQ_ID);
        m[4] = (uint8_t)cmd;
        m[5] = MT7925_MCU_PKT_ID;
        m[6] = MT7925_MCU_Q_NA;
        m[7] = seq;
    }
    mt7925_err e = tx_post(d, &d->tx_mcu, h, hl, payload, len, 0, 0, 0);
    if (e != MT7925_OK || !wait)
        return e;
    int matched = 0;
    for (uint32_t spent = 0;;) {
        int r = rx_one(d, &d->rx_evt, 1, seq, reply, reply_cap, reply_len, &matched);
        if (r < 0)
            return (mt7925_err)(-r);
        if (matched)
            return MT7925_OK;
        /* Every pass counts against the budget (an unrelated event costs 1 us of it), so a chip that keeps
         * sending other events cannot hold the wait open forever. */
        if (spent >= MT7925_MCU_REPLY_US)
            return MT7925_E_TIMEOUT;
        if (r == 0) {
            d->ops.udelay(d->ops.ctx, MT7925_POLL_STEP_US);
            spent += MT7925_POLL_STEP_US;
        } else {
            spent += 1u;
        }
    }
}

mt7925_err mt7925_mcu_send_secret(mt7925_dev *d, uint16_t cmd, const uint8_t *payload, uint32_t len, uint8_t *reply,
                                  uint32_t reply_cap, uint32_t *reply_len) {
    if (!d)
        return MT7925_E_ARG;
    uint32_t slot = d->tx_mcu.head; /* the slot tx_post fills, if it posts at all */
    mt7925_err e = mt7925_mcu_send(d, cmd, 1, 0, payload, len, 1, reply, reply_cap, reply_len);
    if (d->mem) {
        volatile uint8_t *p = buf_ptr(d, &d->tx_mcu, slot);
        for (uint32_t i = 0; i < d->tx_mcu.buf_len; i++)
            p[i] = 0;
        d->ops.dma_barrier(d->ops.ctx);
    }
    return e;
}

/* Send a legacy command and return the status byte (sem/finish replies). */
static mt7925_err mcu_status8(mt7925_dev *d, uint8_t cmd, const uint8_t *p, uint32_t len, uint8_t *status) {
    uint8_t ev[MT7925_EVT_HDR_BYTES + 8];
    uint32_t got = 0;
    mt7925_err e = mt7925_mcu_send(d, cmd, 0, 0, p, len, 1, ev, sizeof(ev), &got);
    if (e != MT7925_OK)
        return e;
    if (got <= MT7925_EVT_STATUS8_OFF) {
        d->broken = 1;
        return MT7925_E_DEVICE;
    }
    *status = ev[MT7925_EVT_STATUS8_OFF];
    return MT7925_OK;
}

static mt7925_err init_download(mt7925_dev *d, const mt7925_fw_dl *dl) {
    uint8_t req[12];
    put_le32(req, dl->addr);
    put_le32(req + 4, dl->len);
    put_le32(req + 8, dl->mode);
    return mt7925_mcu_send(d, dl->cmd, 0, 0, req, sizeof(req), 1, 0, 0, 0);
}

static mt7925_err scatter(mt7925_dev *d, const uint8_t *data, uint32_t len) {
    for (uint32_t done = 0; done < len;) {
        uint32_t n = len - done < MT7925_FW_SCATTER_MAX ? len - done : MT7925_FW_SCATTER_MAX;
        /* FW_SCATTER goes raw on the FWDL ring: no TXD, no reply. */
        mt7925_err e = tx_post(d, &d->tx_fwdl, data + done, n, 0, 0, 0, 0, 0);
        if (e != MT7925_OK)
            return e;
        done += n;
    }
    return MT7925_OK;
}

mt7925_err mt7925_fw_download(mt7925_dev *d, const uint8_t *patch, size_t patch_len, const uint8_t *ram,
                              size_t ram_len) {
    if (!d || !patch || !ram)
        return MT7925_E_ARG;
    if (d->broken)
        return MT7925_E_BROKEN;
    if (!d->dma_on)
        return MT7925_E_STATE;
    mt7925_fw_patch p;
    mt7925_fw_ram r;
    if (mt7925_fw_patch_parse(patch, patch_len, &p) != MT7925_FW_OK ||
        mt7925_fw_ram_parse(ram, ram_len, &r) != MT7925_FW_OK)
        return MT7925_E_FW;

    uint8_t pm[4] = {1, 0, 0, 0}; /* NIC_POWER_CTRL power_mode 1: restart download */
    mt7925_err e = mt7925_mcu_send(d, MT7925_MCU_CMD_NIC_POWER_CTRL, 0, 0, pm, sizeof(pm), 0, 0, 0, 0);
    if (e != MT7925_OK)
        return e;
    if (!mt7925_poll(d, MT7925_CONN_ON_MISC, MT7925_MISC_FW_STATE_MASK, MT7925_MISC_FW_PWR_ON, MT7925_PWR_ON_POLL_US))
        return MT7925_E_TIMEOUT; /* the reference only warns here; we stop */

    uint8_t sem[4] = {MT7925_PATCH_SEM_GET, 0, 0, 0}, st = 0;
    if ((e = mcu_status8(d, MT7925_MCU_CMD_PATCH_SEM_CONTROL, sem, sizeof(sem), &st)) != MT7925_OK)
        return e;
    if (st == MT7925_PATCH_NOT_DL_SEM_SUCCESS) {
        mt7925_err pe = MT7925_OK;
        for (uint32_t i = 0; i < p.n_sections && pe == MT7925_OK; i++) {
            pe = init_download(d, &p.dl[i]);
            if (pe == MT7925_OK)
                pe = scatter(d, patch + p.dl[i].file_off, p.dl[i].len);
        }
        if (pe == MT7925_OK) {
            uint8_t fin[4] = {0, 0, 0, 0}; /* check_crc = 0 */
            uint8_t fst = 0;
            pe = mcu_status8(d, MT7925_MCU_CMD_PATCH_FINISH_REQ, fin, sizeof(fin), &fst);
            if (pe == MT7925_OK && fst != 0)
                pe = MT7925_E_MCU_STATUS;
        }
        /* Always release the semaphore, as the reference does. */
        sem[0] = MT7925_PATCH_SEM_RELEASE;
        mt7925_err re = mcu_status8(d, MT7925_MCU_CMD_PATCH_SEM_CONTROL, sem, sizeof(sem), &st);
        if (pe != MT7925_OK)
            return pe;
        if (re != MT7925_OK)
            return re;
        if (st != MT7925_PATCH_REL_SEM_SUCCESS)
            return MT7925_E_PATCH_SEM;
    } else if (st != MT7925_PATCH_IS_DL) {
        return MT7925_E_PATCH_SEM;
    }

    for (uint32_t i = 0; i < r.n_dl; i++) {
        if ((e = init_download(d, &r.dl[i])) != MT7925_OK)
            return e;
        if ((e = scatter(d, ram + r.dl[i].file_off, r.dl[i].len)) != MT7925_OK)
            return e;
    }
    uint8_t start[8];
    put_le32(start, r.start_option);
    put_le32(start + 4, r.start_addr);
    if ((e = mt7925_mcu_send(d, MT7925_MCU_CMD_FW_START_REQ, 0, 0, start, sizeof(start), 1, 0, 0, 0)) != MT7925_OK)
        return e;
    if (!mt7925_poll(d, MT7925_CONN_ON_MISC, MT7925_MISC_FW_N9_RDY, MT7925_MISC_FW_N9_RDY, MT7925_N9_RDY_POLL_US))
        return MT7925_E_TIMEOUT;
    d->fw_running = 1;
    return MT7925_OK;
}

/* ---- scan ---- */

static uint8_t *tlv(uint8_t *out, uint32_t *at, uint32_t cap, uint16_t tag, uint16_t len) {
    if (*at > cap || len > cap - *at)
        return 0;
    uint8_t *t = out + *at;
    zero(t, len);
    put_le16(t, tag);
    put_le16(t + 2, len);
    *at += len;
    return t;
}

mt7925_err mt7925_scan_build(uint8_t seq, uint8_t bss_idx, const mt7925_ssid *ssids, uint32_t n_ssids,
                             const mt7925_chan *chans, uint32_t n_channels, const uint8_t *ies, uint32_t ies_len,
                             uint8_t *out, uint32_t cap, uint32_t *out_len) {
    if (!out || !out_len || (n_ssids && !ssids) || (n_channels && !chans) || (ies_len && !ies) ||
        n_ssids > MT7925_SCAN_MAX_SSIDS || n_channels > MT7925_SCAN_MAX_CHANNELS || ies_len > 600u || cap < 4)
        return MT7925_E_ARG;
    for (uint32_t i = 0; i < n_ssids; i++)
        if (ssids[i].len == 0 || ssids[i].len > 32)
            return MT7925_E_ARG;
    for (uint32_t i = 0; i < n_channels; i++)
        if (chans[i].band < 1 || chans[i].band > 3 || chans[i].channel == 0)
            return MT7925_E_ARG;
    uint32_t at = 4;
    zero(out, 4);
    out[0] = (uint8_t)(seq & 0x7fu); /* band 0 in bit 7 */
    out[1] = bss_idx;
    uint8_t *req = tlv(out, &at, cap, MT7925_UNI_SCAN_REQ, 20);
    uint8_t *ss = tlv(out, &at, cap, MT7925_UNI_SCAN_SSID, (uint16_t)(8 + 36 * MT7925_SCAN_MAX_SSIDS));
    uint8_t *bs = tlv(out, &at, cap, MT7925_UNI_SCAN_BSSID, 16);
    uint8_t *ch = tlv(out, &at, cap, MT7925_UNI_SCAN_CHANNEL, (uint16_t)(8 + 2 * MT7925_SCAN_MAX_CHANNELS));
    uint8_t *mi = tlv(out, &at, cap, MT7925_UNI_SCAN_MISC, 12);
    uint16_t ie_tlv = (uint16_t)(((8u + ies_len) + 3u) & ~3u);
    uint8_t *ie = tlv(out, &at, cap, MT7925_UNI_SCAN_IE, ie_tlv);
    if (!req || !ss || !bs || !ch || !mi || !ie)
        return MT7925_E_ARG;
    req[4] = n_ssids ? 1 : 0;          /* active scan when SSIDs are given */
    req[5] = n_ssids ? 2 : 0;          /* probe requests per SSID */
    req[6] = MT7925_SCAN_FUNC_SPLIT_SCAN;
    ss[4] = n_ssids ? (1u << 2) : (1u << 0); /* specified + wildcard, or wildcard */
    ss[5] = (uint8_t)n_ssids;
    for (uint32_t i = 0; i < n_ssids; i++) {
        put_le32(ss + 8 + 36 * i, ssids[i].len);
        copy(ss + 12 + 36 * i, ssids[i].ssid, ssids[i].len);
    }
    for (int i = 0; i < 6; i++) bs[4 + i] = 0xff; /* wildcard BSSID */
    ch[4] = n_channels ? 4 : 0;
    ch[5] = (uint8_t)n_channels;
    for (uint32_t i = 0; i < n_channels; i++) {
        ch[8 + 2 * i] = chans[i].band;
        ch[9 + 2 * i] = chans[i].channel;
    }
    put_le16(ie + 4, (uint16_t)ies_len);
    ie[6] = 0; /* band: all */
    if (ies_len) copy(ie + 8, ies, ies_len);
    *out_len = at;
    return MT7925_OK;
}

mt7925_err mt7925_scan_start(mt7925_dev *d, uint8_t seq, const mt7925_ssid *ssids, uint32_t n_ssids,
                             const mt7925_chan *chans, uint32_t n_channels, const uint8_t *ies, uint32_t ies_len) {
    uint8_t buf[MT7925_MCU_MAX_PAYLOAD], ev[MT7925_EVT_HDR_BYTES + 8];
    uint32_t n = 0, got = 0;
    if (!d)
        return MT7925_E_ARG;
    if (!d->fw_running)
        return MT7925_E_STATE;
    mt7925_err e = mt7925_scan_build(seq, 0, ssids, n_ssids, chans, n_channels, ies, ies_len, buf, sizeof(buf), &n);
    if (e != MT7925_OK)
        return e;
    /* The reference reads no status from the SCAN_REQ reply (only DEV_INFO,
     * BSS_INFO, STA_REC, OFFLOAD, SUSPEND and NAN replies carry one). */
    return mt7925_mcu_send(d, MT7925_UNI_CMD_SCAN_REQ, 1, 0, buf, n, 1, ev, sizeof(ev), &got);
}

const char *mt7925_strerror(mt7925_err e) {
    switch (e) {
    case MT7925_OK: return "ok";
    case MT7925_E_ARG: return "bad argument";
    case MT7925_E_IDENTITY: return "not an MT7925";
    case MT7925_E_MEM: return "bad DMA region";
    case MT7925_E_FW_OWN: return "firmware-own handshake timed out";
    case MT7925_E_DRV_OWN: return "driver-own handshake timed out";
    case MT7925_E_RESET: return "WFSYS reset timed out";
    case MT7925_E_FULL: return "TX ring full";
    case MT7925_E_TIMEOUT: return "MCU timeout";
    case MT7925_E_DEVICE: return "device broke a ring rule";
    case MT7925_E_BROKEN: return "device broken (sticky)";
    case MT7925_E_FW: return "firmware container refused";
    case MT7925_E_PATCH_SEM: return "patch semaphore refused";
    case MT7925_E_MCU_STATUS: return "MCU reported failure";
    case MT7925_E_STATE: return "call out of order";
    }
    return "?";
}

