/* rtl8127_bind.c -- RTL8127 binding and UDP boot report (see rtl8127_bind.h).
 * Freestanding. */
#include "rtl8127_bind.h"
#include "aienos_report.h"
#include "aienos_rtl8127.h"
#include "ck.h"
#include <string.h>

#define ROUND_US 2000000u
#define CLOSE_WAIT_US 100000u

static struct {
    pci_func fn;
    const pci_func *pf;
    volatile uint8_t *bar;
    rtl_dev dev;
    int bm_on, confined;
    uint32_t stream_id;
} g_rtl;
/* ck_dma_alloc memory is never freed: allocate once, reuse on a later bind. */
static uint8_t *g_mem;
static uint64_t g_phys;
static uint8_t frame[1600];
static uint8_t rxbuf[RTL_RX_BUF]; /* every frame RMS 1522 lets through, VLAN tag included */

static uint32_t o_read(void *ctx, uint32_t off, unsigned w)
{
    volatile uint8_t *p = (volatile uint8_t *)ctx + off;
    if (w == 1) return *p;
    if (w == 2) return *(volatile uint16_t *)p;
    return *(volatile uint32_t *)p;
}
static void o_write(void *ctx, uint32_t off, unsigned w, uint32_t v)
{
    volatile uint8_t *p = (volatile uint8_t *)ctx + off;
    ck_mb(); /* descriptors and buffers visible before the register write */
    if (w == 1) *p = (uint8_t)v;
    else if (w == 2) *(volatile uint16_t *)p = (uint16_t)v;
    else *(volatile uint32_t *)p = v;
    ck_mb();
}
static void o_delay(void *ctx, uint32_t us) { (void)ctx; ck_udelay(us); }
static void o_barrier(void *ctx) { (void)ctx; ck_mb(); }

int ck_rtl8127_live(void) { return g_rtl.pf && (g_rtl.bm_on || g_rtl.confined); }

void ck_rtl8127_release(void)
{
    if (!g_rtl.pf) return;
    if (g_rtl.bm_on) {
        /* Only a chip rtl_init identified as an RTL8127 (mcfg set) is written to. */
        if (g_rtl.bar && g_rtl.dev.mcfg)
            ck_printf("rtl8127: chip stop %s\n", rtl_err_name(rtl_stop(&g_rtl.dev)));
        else
            ck_printf("rtl8127: chip stop skipped (not identified as an RTL8127, nothing written)\n");
        if (pci_bus_master_off(g_rtl.pf) == 0) {
            g_rtl.bm_on = 0;
            ck_printf("dma_gate: rtl8127 bus master revoked\n");
        } else
            ck_printf("dma_gate: rtl8127 bus master revoke FAILED (command register still has BME)\n");
    }
    if (g_rtl.confined) {
        int urc = ck_dma_unconfine(g_rtl.stream_id);
        if (urc == 0) g_rtl.confined = 0;
        ck_printf("smmu: rtl8127 stream 0x%x %s (rc=%d)\n", g_rtl.stream_id, urc == 0 ? "returned to abort" : "abort FAILED",
                  urc);
    }
}

int ck_rtl8127_find(const pci_found *found, uint32_t n, pci_func *out)
{
    if (!out) return CK_RTL_E_ARG;
    for (uint32_t i = 0; found && i < n; i++) {
        const pci_found *c = &found[i];
        if (c->vendor != RTL_PCI_VENDOR || c->device != RTL_PCI_DEVICE) continue;
        ck_printf("rtl8127: found seg %04x %02x:%02x.%u rev 0x%02x\n", c->segment, c->bus, c->dev, c->fn, c->revision);
        pci_bus_access acc;
        if (pci_stage_disc_access(c->segment, c->bus, &acc) || pci_func_from_found(out, c, &acc)) {
            ck_printf("rtl8127: unavailable (cannot read the function as an endpoint; nothing written)\n");
            return CK_RTL_E_ARG;
        }
        return CK_RTL_OK;
    }
    ck_printf("rtl8127: absent (no 10ec:8127 function)\n");
    return CK_RTL_ABSENT;
}

/* Send every part once, then wait (bounded) for the chip to close them. */
static int send_round(rtl_dev *d, const char *log, size_t len, uint64_t run, unsigned *failed)
{
    unsigned parts = net_report_parts(len);
    for (unsigned p = 0; p < parts; p++) {
        size_t n = net_report_frame(NET_REPORT_TAG_SPARK, d->mac, run, log, len, p, frame, sizeof frame);
        rtl_err e = n ? rtl_tx(d, frame, n) : RTL_E_ARG;
        uint64_t t0 = ck_time_us();
        while (e == RTL_E_FULL && ck_time_us() - t0 < CLOSE_WAIT_US) {
            if (rtl_tx_reclaim(d, 0) != RTL_OK) return -1;
            e = rtl_tx(d, frame, n);
        }
        if (e != RTL_OK) {
            (*failed)++;
            if (e == RTL_E_DEVICE || e == RTL_E_BROKEN) return -1;
        }
    }
    uint64_t t0 = ck_time_us();
    while (rtl_tx_pending(d) && ck_time_us() - t0 < CLOSE_WAIT_US)
        if (rtl_tx_reclaim(d, 0) != RTL_OK) return -1;
    if (rtl_tx_pending(d)) (*failed)++;
    return 0;
}

int ck_rtl8127_report(const pci_func *f, const char *log, size_t len, uint32_t seconds)
{
    if (!f || !log || !net_report_parts(len)) return CK_RTL_E_ARG;
    if (ck_rtl8127_live()) ck_rtl8127_release();
    if (ck_rtl8127_live()) {
        ck_printf("rtl8127: release incomplete; retaining confined stream, refusing rebind\n");
        return CK_RTL_E_RELEASE;
    }
    memset(&g_rtl, 0, sizeof g_rtl);
    g_rtl.fn = *f;
    const pci_func *pf = g_rtl.pf = &g_rtl.fn;
    const pci_bar *b = &pf->bar[RTL_MMIO_BAR];
    if (b->io || !b->addr || b->size < RTL_MMIO_MIN) {
        ck_printf("rtl8127: unavailable (BAR 2 not a memory BAR of at least 0x%x bytes)\n", (unsigned)RTL_MMIO_MIN);
        return CK_RTL_E_ARG;
    }
    if (!g_mem) g_mem = ck_dma_alloc(RTL_MEM_SIZE, 4096, &g_phys);
    if (!g_mem) {
        ck_printf("rtl8127: unavailable (DMA allocation of %u bytes failed)\n", (unsigned)RTL_MEM_SIZE);
        return CK_RTL_E_DMA;
    }
    uint32_t rid = ((uint32_t)pf->bus << 8) | ((uint32_t)pf->dev << 3) | (uint32_t)pf->fn;
    uint64_t win = ((uint64_t)RTL_MEM_SIZE + 4095u) & ~(uint64_t)4095u;
    struct ck_dma_confinement cf;
    int src = ck_dma_confine(pf->segment, rid, g_phys, win, &cf);
    if (src != 0) {
        ck_printf("dma_gate: rtl8127 denied (%s rc=%d), bus master stays off\n",
                  src == CK_SMMU_ABSENT ? "NoSmmu" : "SmmuNotReady", src);
        return CK_RTL_E_DENIED;
    }
    g_rtl.confined = 1;
    g_rtl.stream_id = cf.stream_id;
    ck_printf("smmu_dma_window: rtl8127 only iova=0x%llx len=0x%llx rid=0x%x stream_id=0x%x\n",
              (unsigned long long)cf.iova, (unsigned long long)cf.len, rid, cf.stream_id);
    g_rtl.bar = (volatile uint8_t *)ck_mmio_map(b->addr, (size_t)b->size);
    pci_enable(pf, 1);
    g_rtl.bm_on = 1;
    ck_printf("dma_gate: rtl8127 granted (Confined), bus master on\n");

    rtl_ops ops = {(void *)g_rtl.bar, o_read, o_write, o_delay, o_barrier};
    rtl_dev *d = &g_rtl.dev;
    rtl_err e = rtl_init(d, &ops, g_mem, cf.iova, RTL_MEM_SIZE);
    ck_printf("rtl8127: init %s tcr=0x%08x mcfg=%u ic_unknown=%u ups_resume=%u phy_state=%u wait_timeouts=%u\n",
              rtl_err_name(e), d->tcr_id, d->mcfg, d->ic_unknown, d->ups_resume, d->phy_state, d->wait_timeouts);
    if (e != RTL_OK) {
        ck_rtl8127_release();
        return CK_RTL_E_INIT;
    }
    ck_printf("rtl8127: mac %02x:%02x:%02x:%02x:%02x:%02x source=%s clo_base=%u\n", d->mac[0], d->mac[1], d->mac[2],
              d->mac[3], d->mac[4], d->mac[5],
              d->mac_source == RTL_MAC_BACKUP ? "backup" : d->mac_source == RTL_MAC_MAC0 ? "mac0" : "fallback",
              d->clo_base);
    uint32_t mbps = 0;
    uint64_t t0 = ck_time_us();
    while (!rtl_link(d, &mbps) && ck_time_us() - t0 < CK_RTL_LINK_WAIT_US) ck_udelay(1000);
    ck_printf("rtl8127: link %s %u Mb/s after %u ms\n", mbps ? "up" : "DOWN", mbps,
              (unsigned)((ck_time_us() - t0) / 1000u));
    if (!mbps) {
        ck_rtl8127_release();
        return CK_RTL_E_LINK;
    }
    ck_printf("report: sending %u bytes to UDP port %u (broadcast, tag %s) every 2 s for %u s\n", (unsigned)len,
              (unsigned)NET_REPORT_PORT, NET_REPORT_TAG_SPARK, seconds);
    uint64_t run = ck_time_us() ^ (uint64_t)len << 48;
    unsigned rounds = 0, failed = 0, rx = 0;
    uint64_t start = ck_time_us(), next = start;
    int broken = 0;
    do {
        if (ck_time_us() >= next) {
            next += ROUND_US;
            if (send_round(d, log, len, run, &failed)) {
                broken = 1;
                break;
            }
            rounds++;
        }
        size_t n = 0;
        int got = 0;
        if (rtl_rx(d, rxbuf, sizeof rxbuf, &n, &got) != RTL_OK) {
            broken = 1;
            break;
        }
        rx += (unsigned)got;
    } while (ck_time_us() - start < (uint64_t)seconds * 1000000u);
    ck_printf("report: rounds=%u parts=%u send_failures=%u rx_frames=%u rx_dropped=%u device=%s\n", rounds,
              net_report_parts(len), failed, rx, d->rx_dropped, broken ? "BROKEN" : "ok");
    ck_rtl8127_release();
    return broken || failed || !rounds ? CK_RTL_E_TX : CK_RTL_OK;
}
