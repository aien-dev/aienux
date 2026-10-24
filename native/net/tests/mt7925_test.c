/* MT7925 driver core against the chip model (tests/mt7925_model.c).
 *
 *   mt7925_test                 identity, remap, bring-up, rings, MCU, firmware
 *                               download (synthetic files), scan, hostile device
 *   mt7925_test real PATCH RAM  the real (decompressed) firmware files through the
 *                               model: every byte the model receives is digested
 *                               and compared with the file. Nothing touches a chip. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../argus/sha256.h"
#include "../aienos_mt7925.h"
#include "../aienos_mt7925_sta.h"
#include "../aienos_net.h"
#include "mt7925_model.h"
#include "wpa_toy.h"

static int checks, failures;
#define CHECK(c) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define DMA_BASE 0x80000000ull
static uint8_t region[1u << 20] __attribute__((aligned(4096)));
static mt7925_model M;

static void put_be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void put_le32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

typedef struct {
    uint32_t events, scan_done, rx, rx_beacons;
    uint8_t last_eid;
} cb_state;
static cb_state CB;
static void on_event(void *ctx, uint8_t eid, const uint8_t *ev, uint32_t len) {
    (void)ev; (void)len;
    cb_state *s = ctx;
    s->events++;
    s->last_eid = eid;
    if (eid == MT7925_UNI_EVENT_SCAN_DONE) s->scan_done++;
}
static void on_rx(void *ctx, const uint8_t *buf, uint32_t len) {
    cb_state *s = ctx;
    s->rx++;
    if (len > 32 && buf[32] == 0x80) s->rx_beacons++;
}

static mt7925_err bring_up(mt7925_dev *d, uint32_t faults) {
    model_init(&M, faults);
    model_attach_dma(&M, region, DMA_BASE, sizeof(region));
    mt7925_ops ops = model_ops(&M);
    memset(d, 0, sizeof(*d));
    memset(&CB, 0, sizeof(CB));
    d->on_event = on_event;
    d->on_rx = on_rx;
    d->cb_ctx = &CB;
    d->rx_ctx = &CB;
    return mt7925_init(d, &ops, region, DMA_BASE, sizeof(region));
}

static void test_identity(void) {
    CHECK(mt7925_pci_identity(0x14c3, 0x7925, 0x028000, 0x200000, 1, 1) == MT7925_OK);
    CHECK(mt7925_pci_identity(0x14c3, 0x0717, 0x028000, 0x200000, 1, 1) == MT7925_OK);
    CHECK(mt7925_pci_identity(0x14c4, 0x7925, 0x028000, 0x200000, 1, 1) == MT7925_E_IDENTITY);
    CHECK(mt7925_pci_identity(0x14c3, 0x7927, 0x028000, 0x200000, 1, 1) == MT7925_E_IDENTITY);
    CHECK(mt7925_pci_identity(0x14c3, 0x7925, 0x020000, 0x200000, 1, 1) == MT7925_E_IDENTITY);
    CHECK(mt7925_pci_identity(0x14c3, 0x7925, 0x028000, 0x100000, 1, 1) == MT7925_E_IDENTITY);
    CHECK(mt7925_pci_identity(0x14c3, 0x7925, 0x028000, 0x200000, 0, 1) == MT7925_E_IDENTITY);
    CHECK(mt7925_pci_identity(0x14c3, 0x7925, 0x028000, 0x200000, 1, 0) == MT7925_E_IDENTITY);
}

static void test_remap(void) {
    uint32_t off = 0;
    CHECK(mt7925_fixed_map(MT7925_CONN_ON_LPCTL, &off) && off == 0xe0010u);
    CHECK(mt7925_fixed_map(MT7925_CONN_ON_MISC, &off) && off == 0xe00f0u);
    CHECK(mt7925_fixed_map(MT7925_WFSYS_SW_RST, &off) && off == 0xf0140u);
    CHECK(mt7925_fixed_map(MT7925_UWFDMA0_GLO_CFG_EXT1, &off) && off == 0xd42b4u);
    CHECK(mt7925_fixed_map(MT7925_WFDMA_DUMMY_CR, &off) && off == 0x2120u);
    CHECK(mt7925_fixed_map(0x830c0fffu, &off) && off == 0xfffu);
    CHECK(!mt7925_fixed_map(0x830c1000u, &off)); /* one past the window: the reference maps it, we do not */
    CHECK(mt7925_fixed_map(0x1fffffu, &off) && off == 0x1fffffu);
    CHECK(!mt7925_fixed_map(MT7925_HW_CHIPID, &off) && mt7925_uses_l1(MT7925_HW_CHIPID));
    CHECK(!mt7925_fixed_map(MT7925_HW_EMI_CTL, &off) && mt7925_uses_l1(MT7925_HW_EMI_CTL));
    CHECK(!mt7925_fixed_map(0x81040000u, &off) && !mt7925_uses_l1(0x81040000u));
    CHECK(!mt7925_fixed_map(0x7c6a0000u, &off)); /* the 2 MiB "remap" entry would leave BAR0 */

    mt7925_dev d;
    CHECK(bring_up(&d, 0) == MT7925_OK);
    M.bar[MT7925_HIF_REMAP_L1 / 4] = 0x0000abcdu;
    M.bar[MT7925_HIF_REMAP_L2 / 4] = 0x11112222u;
    mt7925_wr(&d, 0x18011104u, 0xdeadbeefu);
    CHECK(model_chipreg(&M, 0x18011104u) == 0xdeadbeefu);
    CHECK(mt7925_rr(&d, 0x18011104u) == 0xdeadbeefu);
    CHECK(M.bar[MT7925_HIF_REMAP_L1 / 4] == 0x0000abcdu);
    mt7925_wr(&d, 0x81040010u, 0x12345678u);
    CHECK(model_chipreg(&M, 0x81040010u) == 0x12345678u);
    CHECK(mt7925_rr(&d, 0x81040010u) == 0x12345678u);
    CHECK(M.bar[MT7925_HIF_REMAP_L1 / 4] == 0x0000abcdu && M.bar[MT7925_HIF_REMAP_L2 / 4] == 0x11112222u);
}

static void test_bring_up(void) {
    mt7925_dev d;
    CHECK(bring_up(&d, 0) == MT7925_OK);
    CHECK(d.chip_rev == 0x79250000u);
    CHECK(M.dma_faults == 0 && M.mcu_bad == 0);
    int set = model_trace_find(&M, 'W', 0xe0010u, 0);
    CHECK(set >= 0 && M.trace[set].val == MT7925_LPCR_HOST_SET_OWN);
    int clr = model_trace_find(&M, 'W', 0xe0010u, set + 1);
    CHECK(clr > set && M.trace[clr].val == MT7925_LPCR_HOST_CLR_OWN);
    int chip = model_trace_find(&M, 'R', MT7925_HW_CHIPID, 0);
    CHECK(chip > clr);
    int emi = model_trace_find(&M, 'W', MT7925_HW_EMI_CTL, 0);
    CHECK(emi > chip && (M.trace[emi].val & MT7925_HW_EMI_CTL_SLPPROT_EN));
    int rst0 = model_trace_find(&M, 'W', 0xf0140u, 0);
    CHECK(rst0 > emi && !(M.trace[rst0].val & MT7925_WFSYS_SW_RST_B));
    int rst1 = model_trace_find(&M, 'W', 0xf0140u, rst0 + 1);
    CHECK(rst1 > rst0 && (M.trace[rst1].val & MT7925_WFSYS_SW_RST_B));
    CHECK(M.time_us >= MT7925_RESET_HOLD_US);
    int glo = model_trace_find(&M, 'W', MT7925_GLO_CFG, rst1);
    CHECK(glo > rst1);
    uint32_t want = MT7925_GLO_TX_WB_DDONE | MT7925_GLO_FIFO_LITTLE_ENDIAN | MT7925_GLO_CLK_GAT_DIS |
                    MT7925_GLO_OMIT_TX_INFO | (3u << 4) | MT7925_GLO_FIFO_DIS_CHECK | MT7925_GLO_RX_WB_DDONE |
                    MT7925_GLO_DISP_BASE_PTR_CHAIN_EN | MT7925_GLO_OMIT_RX_INFO_PFET2 | MT7925_GLO_TX_DMA_EN |
                    MT7925_GLO_RX_DMA_EN;
    CHECK(M.bar[MT7925_GLO_CFG / 4] == want);
    /* every ring: base inside the region, size as chosen, RX holds n-1 buffers */
    struct { uint32_t regs, n; } r[5] = {{MT7925_TX_RING_BASE, MT7925_N_TX_DATA},
                                         {MT7925_TX_RING_BASE + 15 * 16, MT7925_N_TX_MCU},
                                         {MT7925_TX_RING_BASE + 16 * 16, MT7925_N_TX_FWDL},
                                         {MT7925_RX_RING_BASE, MT7925_N_RX_EVENT},
                                         {MT7925_RX_RING_BASE + 2 * 16, MT7925_N_RX_DATA}};
    for (int i = 0; i < 5; i++) {
        uint32_t base = M.bar[(r[i].regs + MT7925_RING_DESC_BASE) / 4];
        CHECK(base >= DMA_BASE && base + r[i].n * 16u <= DMA_BASE + mt7925_dma_bytes());
        CHECK((base & 0xfffu) == 0);
        CHECK(M.bar[(r[i].regs + MT7925_RING_SIZE) / 4] == r[i].n);
    }
    CHECK(M.bar[(MT7925_RX_RING_BASE + MT7925_RING_CPU_IDX) / 4] == MT7925_N_RX_EVENT - 1);
    CHECK(M.bar[MT7925_HOST_INT_ENA / 4] == 0);
    CHECK(model_chipreg(&M, 0) == 0);
    CHECK(mt7925_dma_bytes() <= sizeof(region));
    /* release: interrupts masked, WFDMA disabled, ownership back to the firmware */
    uint32_t lp = 0;
    CHECK(mt7925_fixed_map(MT7925_CONN_ON_LPCTL, &lp));
    CHECK((M.bar[lp / 4] & MT7925_LPCR_HOST_OWN_SYNC) == 0);
    CHECK(mt7925_stop(&d) == MT7925_OK && !d.dma_on && !d.fw_running);
    CHECK((M.bar[MT7925_GLO_CFG / 4] & (MT7925_GLO_TX_DMA_EN | MT7925_GLO_RX_DMA_EN)) == 0);
    CHECK((M.bar[lp / 4] & MT7925_LPCR_HOST_OWN_SYNC) != 0 && M.bar[MT7925_HOST_INT_ENA / 4] == 0);
    CHECK(mt7925_stop(0) == MT7925_E_ARG);
}

static void test_bring_up_faults(void) {
    mt7925_dev d;
    CHECK(bring_up(&d, MODEL_F_OWN_STUCK) == MT7925_E_FW_OWN);
    CHECK(M.time_us >= (uint64_t)MT7925_OWN_TRIES * MT7925_OWN_POLL_US);
    CHECK(bring_up(&d, MODEL_F_DRV_OWN_STUCK) == MT7925_E_DRV_OWN);
    CHECK(bring_up(&d, MODEL_F_RESET_STUCK) == MT7925_E_RESET);
    CHECK(bring_up(&d, MODEL_F_RING_IDX_BAD) == MT7925_E_DEVICE && d.broken);
    mt7925_ops ops = model_ops(&M);
    model_init(&M, 0);
    CHECK(mt7925_init(&d, &ops, region, DMA_BASE + 8, sizeof(region)) == MT7925_E_MEM);
    CHECK(mt7925_init(&d, &ops, region, 0xfff80000ull, sizeof(region)) == MT7925_E_MEM);
    CHECK(mt7925_init(&d, &ops, region, 0x100000000ull, sizeof(region)) == MT7925_E_MEM);
    CHECK(mt7925_init(&d, &ops, region, DMA_BASE, mt7925_dma_bytes() - 1) == MT7925_E_MEM);
    CHECK(mt7925_init(&d, NULL, region, DMA_BASE, sizeof(region)) == MT7925_E_ARG);
    CHECK(M.n_trace == 0); /* refused before touching the device */
}

static void test_mcu(void) {
    mt7925_dev d;
    uint8_t ev[128], p[4] = {MT7925_PATCH_SEM_GET, 0, 0, 0};
    uint32_t got = 0;
    CHECK(bring_up(&d, 0) == MT7925_OK);
    /* a legacy command with a reply: model checks TXD, header and sequence */
    uint8_t pm[4] = {1, 0, 0, 0};
    CHECK(mt7925_mcu_send(&d, MT7925_MCU_CMD_NIC_POWER_CTRL, 0, 0, pm, 4, 0, 0, 0, 0) == MT7925_OK);
    for (int i = 0; i < 40; i++) {
        CHECK(mt7925_mcu_send(&d, MT7925_MCU_CMD_PATCH_SEM_CONTROL, 0, 0, p, 4, 1, ev, sizeof(ev), &got) == MT7925_OK);
        CHECK(got >= MT7925_EVT_HDR_BYTES && ev[MT7925_EVT_SEQ_OFF] == d.mcu_seq && d.mcu_seq >= 1 && d.mcu_seq <= 15);
        p[0] = (uint8_t)(i & 1 ? MT7925_PATCH_SEM_GET : MT7925_PATCH_SEM_RELEASE);
    }
    CHECK(M.mcu_bad == 0 && M.dma_faults == 0);
    uint8_t big[MT7925_MCU_MAX_PAYLOAD + 1];
    memset(big, 0, sizeof(big));
    CHECK(mt7925_mcu_send(&d, 1, 1, 0, big, sizeof(big), 0, 0, 0, 0) == MT7925_E_ARG);
    CHECK(mt7925_mcu_send(&d, 0x100, 0, 0, p, 4, 0, 0, 0, 0) == MT7925_E_ARG);
    /* UNI header: cid 16 bit, option SET/QUERY */
    CHECK(mt7925_mcu_send(&d, 0x1234, 1, 1, p, 4, 1, ev, sizeof(ev), &got) == MT7925_OK);
    CHECK(M.last_uni && M.last_uni_cid == 0x1234);
    /* a stale-sequence reply goes to on_event; the real one is matched */
    CHECK(bring_up(&d, MODEL_F_WRONG_SEQ_FIRST) == MT7925_OK);
    p[0] = MT7925_PATCH_SEM_GET;
    CHECK(mt7925_mcu_send(&d, MT7925_MCU_CMD_PATCH_SEM_CONTROL, 0, 0, p, 4, 1, ev, sizeof(ev), &got) == MT7925_OK);
    CHECK(ev[MT7925_EVT_STATUS8_OFF] == MT7925_PATCH_NOT_DL_SEM_SUCCESS && CB.events == 1);
    /* silent MCU: bounded wait */
    CHECK(bring_up(&d, MODEL_F_MCU_SILENT) == MT7925_OK);
    uint64_t t0 = M.time_us;
    CHECK(mt7925_mcu_send(&d, MT7925_MCU_CMD_PATCH_SEM_CONTROL, 0, 0, p, 4, 1, ev, sizeof(ev), &got) == MT7925_E_TIMEOUT);
    CHECK(M.time_us - t0 >= MT7925_MCU_REPLY_US && M.time_us - t0 <= MT7925_MCU_REPLY_US + 10 * MT7925_POLL_STEP_US);
    /* a device that reports consuming slots never posted breaks the device */
    CHECK(bring_up(&d, MODEL_F_BAD_DIDX) == MT7925_OK);
    CHECK(mt7925_mcu_send(&d, MT7925_MCU_CMD_PATCH_SEM_CONTROL, 0, 0, p, 4, 0, 0, 0, 0) == MT7925_OK);
    CHECK(mt7925_mcu_send(&d, MT7925_MCU_CMD_PATCH_SEM_CONTROL, 0, 0, p, 4, 0, 0, 0, 0) == MT7925_E_DEVICE);
    CHECK(d.broken && mt7925_mcu_send(&d, 1, 0, 0, p, 4, 0, 0, 0, 0) == MT7925_E_BROKEN);
    CHECK(mt7925_poll_rings(&d) == -(int)MT7925_E_BROKEN);
    /* an RX completion longer than the posted buffer breaks the device */
    CHECK(bring_up(&d, MODEL_F_RX_OVERLONG) == MT7925_OK);
    CHECK(mt7925_mcu_send(&d, MT7925_MCU_CMD_PATCH_SEM_CONTROL, 0, 0, p, 4, 1, ev, sizeof(ev), &got) == MT7925_E_DEVICE);
    CHECK(d.broken);
}

/* Same synthetic containers as mt7925_fw_test. */
static size_t make_patch(uint8_t *b) {
    memset(b, 0, 9000);
    memcpy(b, "20240101000000a\0", 16);
    memcpy(b + 16, "ALPS", 4);
    put_be32(b + 44, 2);
    put_be32(b + 96, 0x30002); put_be32(b + 100, 224); put_be32(b + 108, 0x200000); put_be32(b + 112, 5000);
    put_be32(b + 116, 0x01000002);
    put_be32(b + 160, 0x2); put_be32(b + 164, 5224); put_be32(b + 172, 0x00e00000); put_be32(b + 176, 60);
    put_be32(b + 180, 0xffffffffu);
    for (int i = 224; i < 5284; i++) b[i] = (uint8_t)(i * 7);
    return 5284;
}
static size_t make_ram(uint8_t *b) {
    memset(b, 0, 12000);
    uint32_t lens[3] = {9000, 50, 120};
    size_t off = 9170;
    for (int i = 0; i < 9170; i++) b[i] = (uint8_t)(i * 13 + 1);
    for (int i = 0; i < 3; i++, off += 40) {
        uint8_t *r = b + off;
        put_le32(r + 16, i == 0 ? 0x00900000u : (i == 1 ? 0 : 0x02000000u));
        put_le32(r + 20, lens[i]);
        r[24] = i == 1 ? 0x40 : (i == 2 ? 0x20 : 0x00);
        r[25] = i == 1 ? 2 : 0;
    }
    uint8_t *t = b + off;
    t[0] = 0x25; t[2] = 3;
    memcpy(t + 7, "____010000", 10);
    return off + 36;
}

static int digest_eq(const uint8_t *data, uint32_t len, const uint8_t *want) {
    uint8_t d[32];
    sha256_hash(data, len, d);
    return memcmp(d, want, 32) == 0;
}

static void check_download(const uint8_t *patch, size_t pl, const uint8_t *ram, size_t rl, int patch_loaded) {
    mt7925_fw_patch p;
    mt7925_fw_ram r;
    CHECK(mt7925_fw_patch_parse(patch, pl, &p) == MT7925_FW_OK && mt7925_fw_ram_parse(ram, rl, &r) == MT7925_FW_OK);
    uint32_t k = 0;
    CHECK(M.dl_count == (patch_loaded ? 0 : p.n_sections) + r.n_dl);
    if (!patch_loaded)
        for (uint32_t i = 0; i < p.n_sections && k < 32; i++, k++) {
            CHECK(M.dl_log_addr[k] == p.dl[i].addr && M.dl_log_len[k] == p.dl[i].len && M.dl_log_mode[k] == p.dl[i].mode);
            CHECK(digest_eq(patch + p.dl[i].file_off, p.dl[i].len, M.dl_log_digest[k]));
        }
    for (uint32_t i = 0; i < r.n_dl && k < 32; i++, k++) {
        CHECK(M.dl_log_addr[k] == r.dl[i].addr && M.dl_log_len[k] == r.dl[i].len && M.dl_log_mode[k] == r.dl[i].mode);
        CHECK(digest_eq(ram + r.dl[i].file_off, r.dl[i].len, M.dl_log_digest[k]));
    }
    CHECK(M.start_option == r.start_option && M.start_addr == r.start_addr);
    CHECK(!M.sem_held && M.fw_started);
}

static uint8_t PB[9000], RB[12000];

static void test_fw_download(void) {
    mt7925_dev d;
    size_t pl = make_patch(PB), rl = make_ram(RB);
    CHECK(bring_up(&d, 0) == MT7925_OK);
    CHECK(mt7925_scan_start(&d, 1, 0, 0, 0, 0, 0, 0) == MT7925_E_STATE);
    CHECK(mt7925_fw_download(&d, PB, pl, RB, rl) == MT7925_OK);
    CHECK(d.fw_running && M.mcu_bad == 0 && M.dma_faults == 0 && M.patch_done);
    check_download(PB, pl, RB, rl, 0);
    CHECK(M.scatter_bytes == 5000 + 60 + 9000 + 120);
    CHECK(M.start_option == MT7925_FW_START_OVERRIDE && M.start_addr == 0x02000000u);

    CHECK(bring_up(&d, MODEL_F_PATCH_LOADED) == MT7925_OK);
    CHECK(mt7925_fw_download(&d, PB, pl, RB, rl) == MT7925_OK);
    check_download(PB, pl, RB, rl, 1);

    CHECK(bring_up(&d, MODEL_F_SEM_BUSY) == MT7925_OK);
    CHECK(mt7925_fw_download(&d, PB, pl, RB, rl) == MT7925_E_PATCH_SEM && M.dl_count == 0);
    CHECK(bring_up(&d, MODEL_F_FINISH_FAIL) == MT7925_OK);
    CHECK(mt7925_fw_download(&d, PB, pl, RB, rl) == MT7925_E_MCU_STATUS && !M.sem_held && !M.fw_started);
    CHECK(bring_up(&d, MODEL_F_NO_N9_READY) == MT7925_OK);
    CHECK(mt7925_fw_download(&d, PB, pl, RB, rl) == MT7925_E_TIMEOUT && !d.fw_running);
    CHECK(bring_up(&d, MODEL_F_MCU_SILENT) == MT7925_OK);
    CHECK(mt7925_fw_download(&d, PB, pl, RB, rl) == MT7925_E_TIMEOUT);
    CHECK(bring_up(&d, 0) == MT7925_OK);
    uint8_t bad[400];
    memcpy(bad, PB, 400);
    CHECK(mt7925_fw_download(&d, bad, 400, RB, rl) == MT7925_E_FW && M.mcu_cmds == 0);
}

static void test_scan(void) {
    uint8_t out[2048];
    uint32_t n = 0;
    mt7925_ssid s = {6, "aienos"};
    mt7925_chan ch[3] = {{1, 1}, {1, 6}, {2, 36}};
    uint8_t ies[5] = {0x7f, 3, 0, 0, 0x04};
    CHECK(mt7925_scan_build(5, 0, &s, 1, ch, 3, ies, 5, out, sizeof(out), &n) == MT7925_OK);
    CHECK(out[0] == 5 && out[1] == 0);
    CHECK(le32(out + 4) == (1u | (20u << 16)));           /* REQ tag 1 len 20 */
    CHECK(out[8] == 1 && out[9] == 2 && out[10] == MT7925_SCAN_FUNC_SPLIT_SCAN);
    CHECK(le32(out + 24) == (10u | (368u << 16)));        /* SSID tag 10 len 368 */
    CHECK(out[28] == 4 && out[29] == 1 && le32(out + 32) == 6 && memcmp(out + 36, "aienos", 6) == 0);
    CHECK(le32(out + 392) == (11u | (16u << 16)));        /* BSSID */
    CHECK(out[396] == 0xff && out[401] == 0xff);
    CHECK(le32(out + 408) == (12u | (136u << 16)));       /* CHANNEL */
    CHECK(out[412] == 4 && out[413] == 3 && out[416] == 1 && out[417] == 1 && out[420] == 2 && out[421] == 36);
    CHECK(le32(out + 544) == (14u | (12u << 16)));        /* MISC */
    CHECK(le32(out + 556) == (13u | (16u << 16)));        /* IE, 8 + 5 padded to 16 */
    CHECK(out[560] == 5 && memcmp(out + 564, ies, 5) == 0 && n == 572);
    /* passive wildcard */
    CHECK(mt7925_scan_build(1, 0, 0, 0, 0, 0, 0, 0, out, sizeof(out), &n) == MT7925_OK);
    CHECK(out[8] == 0 && out[9] == 0 && out[28] == 1 && out[412] == 0);
    /* refusals */
    mt7925_ssid bad = {33, {0}};
    CHECK(mt7925_scan_build(1, 0, &bad, 1, 0, 0, 0, 0, out, sizeof(out), &n) == MT7925_E_ARG);
    mt7925_chan badc = {4, 1};
    CHECK(mt7925_scan_build(1, 0, 0, 0, &badc, 1, 0, 0, out, sizeof(out), &n) == MT7925_E_ARG);
    CHECK(mt7925_scan_build(1, 0, 0, 0, 0, 0, 0, 0, out, 100, &n) == MT7925_E_ARG);
    for (uint32_t cap = 0; cap < 556; cap += 37)
        CHECK(mt7925_scan_build(1, 0, 0, 0, 0, 0, 0, 0, out, cap, &n) == MT7925_E_ARG);

    /* through the model: two APs answer, scan-done arrives unsolicited */
    mt7925_dev d;
    size_t pl = make_patch(PB), rl = make_ram(RB);
    CHECK(bring_up(&d, 0) == MT7925_OK);
    model_ap a = {{0x02, 0, 0, 0, 0, 1}, 6, "aienos", 6, -40, {0}, 0, {10, 0, 0, 1}, {0x02, 0, 0, 0, 0, 0x99}};
    model_ap b = {{0x02, 0, 0, 0, 0, 2}, 4, "lab2", 11, -60, {0}, 0, {10, 0, 1, 1}, {0x02, 0, 0, 0, 1, 0x99}};
    model_add_ap(&M, &a);
    model_add_ap(&M, &b);
    CHECK(mt7925_fw_download(&d, PB, pl, RB, rl) == MT7925_OK);
    CHECK(mt7925_scan_start(&d, 7, &s, 1, ch, 3, 0, 0) == MT7925_OK);
    CHECK(M.scans == 1 && M.last_uni_cid == MT7925_UNI_CMD_SCAN_REQ);
    for (int i = 0; i < 4; i++) mt7925_poll_rings(&d);
    CHECK(CB.rx_beacons == 2 && CB.scan_done == 1);
    CHECK(M.dma_faults == 0 && M.mcu_bad == 0);
}

/* Hostile RX: random completion words in posted RX descriptors never crash
 * the driver and never let it read past a posted buffer. */
static void test_hostile_rx(void) {
    mt7925_dev d;
    uint32_t x = 0x925u, broke = 0, ok = 0;
    for (int it = 0; it < 3000; it++) {
        if (bring_up(&d, 0) != MT7925_OK) { CHECK(0); return; }
        uint32_t base = M.bar[(MT7925_RX_RING_BASE + MT7925_RING_DESC_BASE) / 4];
        for (int k = 0; k < 4; k++) {
            x = x * 1664525u + 1013904223u;
            uint8_t *desc = region + (base - DMA_BASE) + (size_t)((x >> 8) % (MT7925_N_RX_EVENT - 1)) * 16u;
            x = x * 1664525u + 1013904223u;
            put_le32(desc + 4, x);
            x = x * 1664525u + 1013904223u;
            uint8_t *buf = region + (le32(desc) ? le32(desc) - DMA_BASE : 0);
            if (le32(desc)) put_le32(buf, x);
        }
        int r = mt7925_poll_rings(&d);
        if (r < 0) { broke++; CHECK(d.broken); } else ok++;
        CHECK(M.dma_faults == 0);
    }
    CHECK(broke > 0 && ok > 0);
}

/* ---- data path: TXD/TXP, TX_FREE, RX descriptors ---- */

static const uint8_t AP_BSSID[6] = {0x02, 0, 0, 0, 0, 1};

static uint32_t mk_frame(uint8_t *f, uint8_t fc0, uint8_t fc1, const uint8_t *a1, uint32_t body) {
    memset(f, 0, 24 + body);
    f[0] = fc0;
    f[1] = fc1;
    memcpy(f + 4, a1, 6);
    f[10] = 0x02; f[15] = 0x25;
    memcpy(f + 16, AP_BSSID, 6);
    for (uint32_t i = 0; i < body; i++) f[24 + i] = (uint8_t)(i * 7u);
    return 24 + body;
}

static void test_txd(void) {
    uint8_t f[256], o[MT7925_TX_HDR_BYTES];
    mt7925_tx_opts op = {MT7925_WCID_AP, 0, 5, 0, 0};
    /* data frame, unicast: AC_BE, TID, no fixed rate */
    uint32_t n = mk_frame(f, 0x08, 0x01, AP_BSSID, 40);
    CHECK(mt7925_txd_build(o, f, n, &op, 7, 0x80001040u) == MT7925_OK);
    uint32_t w0 = le32(o), w1 = le32(o + 4), w2 = le32(o + 8), w3 = le32(o + 12), w6 = le32(o + 24);
    CHECK((w0 & 0xffffu) == n + 32u && ((w0 >> 23) & 3u) == MT7925_TX_TYPE_CT && (w0 >> 25) == MT7925_LMAC_AC_BE);
    CHECK((w1 & 0xfffu) == MT7925_WCID_AP && ((w1 >> 14) & 3u) == MT7925_HDR_FORMAT_802_11);
    CHECK(((w1 >> 16) & 0x1fu) == 12u && ((w1 >> 21) & 7u) == 5u && !(w1 & MT7925_TXD1_FIXED_RATE));
    CHECK(w2 == (2u << 4) && !(w3 & (MT7925_TXD3_PROTECT_FRAME | MT7925_TXD3_BA_DISABLE | MT7925_TXD3_BCM)));
    CHECK(((w3 >> 11) & 0x1fu) == 15u);
    CHECK(w6 == (MT7925_TXD6_DAS | MT7925_TXD6_DIS_MAT | (1u << MT7925_TXD6_MSDU_CNT_SHIFT)));
    CHECK(le32(o + 20) == 0 && le32(o + 28) == 0);
    CHECK((uint32_t)(o[32] | (o[33] << 8)) == (7u | MT7925_MSDU_ID_VALID) && le32(o + 40) == 0x80001040u);
    CHECK((uint32_t)(o[44] | (o[45] << 8)) == (n | MT7925_TXP_LEN_LAST));
    for (int i = 46; i < 64; i++) CHECK(o[i] == 0);
    /* protected data */
    op.protect = 1;
    CHECK(mt7925_txd_build(o, f, n, &op, 0, 0) == MT7925_OK && (le32(o + 12) & MT7925_TXD3_PROTECT_FRAME));
    /* management: ALTX0, fixed basic rate, BA off, no TID */
    op.protect = 0;
    n = mk_frame(f, 0xb0, 0, AP_BSSID, 6);
    CHECK(mt7925_txd_build(o, f, n, &op, 1, 0) == MT7925_OK);
    w0 = le32(o); w1 = le32(o + 4); w2 = le32(o + 8); w3 = le32(o + 12); w6 = le32(o + 24);
    CHECK((w0 >> 25) == MT7925_LMAC_ALTX0 && (w1 & MT7925_TXD1_FIXED_RATE) && ((w1 >> 21) & 7u) == 0);
    CHECK(w2 == ((0u << 4) | 11u) && (w3 & MT7925_TXD3_BA_DISABLE));
    CHECK((w6 >> MT7925_TXD6_TX_RATE_SHIFT) == MT7925_BASIC_RATES_TBL);
    /* multicast data: BCM, basic rate */
    static const uint8_t bc[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    n = mk_frame(f, 0x08, 0x01, bc, 10);
    CHECK(mt7925_txd_build(o, f, n, &op, 2, 0) == MT7925_OK && (le32(o + 12) & MT7925_TXD3_BCM) &&
          (le32(o + 4) & MT7925_TXD1_FIXED_RATE));
    /* QoS data: 26-byte header */
    n = mk_frame(f, 0x88, 0x01, AP_BSSID, 10);
    CHECK(mt7925_txd_build(o, f, n, &op, 2, 0) == MT7925_OK && ((le32(o + 4) >> 16) & 0x1fu) == 13u);
    /* refusals */
    CHECK(mt7925_txd_build(o, f, 10, &op, 0, 0) == MT7925_E_ARG);              /* shorter than a header */
    CHECK(mt7925_txd_build(o, f, MT7925_TX_FRAME_MAX + 1u, &op, 0, 0) == MT7925_E_ARG);
    CHECK(mt7925_txd_build(o, f, n, &op, 0x7fff, 0) == MT7925_E_ARG);
    op.tid = 8;
    CHECK(mt7925_txd_build(o, f, n, &op, 0, 0) == MT7925_E_ARG);
    op.tid = 0; op.wlan_idx = 0x1000;
    CHECK(mt7925_txd_build(o, f, n, &op, 0, 0) == MT7925_E_ARG);
    f[0] = 0x04; /* control frame */
    op.wlan_idx = 1;
    CHECK(mt7925_txd_build(o, f, 24, &op, 0, 0) == MT7925_E_ARG);
}

static size_t PL, RL;
static mt7925_err up_fw(mt7925_dev *d, uint32_t faults) {
    mt7925_err e = bring_up(d, faults);
    return e != MT7925_OK ? e : mt7925_fw_download(d, PB, PL, RB, RL);
}

static void test_tx_path(void) {
    mt7925_dev d;
    uint8_t f[256];
    mt7925_tx_opts op = {MT7925_WCID_AP, 0, 0, 0, 0};
    uint32_t n = mk_frame(f, 0x08, 0x01, AP_BSSID, 100);

    CHECK(bring_up(&d, 0) == MT7925_OK);
    CHECK(mt7925_tx_80211(&d, f, n, &op) == MT7925_E_STATE); /* no firmware yet */

    /* happy path: frame reaches the radio through the TXP pointer; token comes back */
    CHECK(up_fw(&d, 0) == MT7925_OK);
    CHECK(mt7925_tx_80211(&d, f, n, &op) == MT7925_OK && d.tok_busy == 1u);
    CHECK(M.ct_frames == 1 && M.last_tx_len == n && memcmp(M.last_tx, f, n) == 0);
    CHECK(mt7925_poll_rings(&d) >= 0 && d.tok_busy == 0 && d.tx_freed == 1 && !d.broken);
    for (int i = 0; i < 100; i++) {
        f[30] = (uint8_t)i;
        CHECK(mt7925_tx_80211(&d, f, n, &op) == MT7925_OK);
        CHECK(mt7925_poll_rings(&d) >= 0);
    }
    CHECK(d.tok_busy == 0 && d.tx_freed == 101 && M.ct_frames == 101 && M.last_tx[30] == 99);
    CHECK(M.dma_faults == 0 && M.mcu_bad == 0);

    /* tokens held: the ring refuses once every token is in flight, then recovers */
    CHECK(up_fw(&d, MODEL_F_TXFREE_HOLD) == MT7925_OK);
    uint32_t posted = 0;
    mt7925_err e;
    while ((e = mt7925_tx_80211(&d, f, n, &op)) == MT7925_OK && posted < 64) posted++;
    CHECK(e == MT7925_E_FULL && posted >= MT7925_N_TX_DATA - 1u && posted <= MT7925_N_TX_DATA);
    model_release_tokens(&M);
    CHECK(mt7925_poll_rings(&d) >= 0 && d.tok_busy == 0 && d.tx_freed == posted);
    CHECK(mt7925_tx_80211(&d, f, n, &op) == MT7925_OK);

    /* a TX_FREE for a token not in flight is a broken chip */
    CHECK(up_fw(&d, MODEL_F_TXFREE_BOGUS) == MT7925_OK);
    CHECK(mt7925_tx_80211(&d, f, n, &op) == MT7925_OK);
    CHECK(mt7925_poll_rings(&d) < 0 && d.broken);
    CHECK(mt7925_tx_80211(&d, f, n, &op) == MT7925_E_BROKEN);
    /* a record that announces more ids than it carries */
    CHECK(up_fw(&d, MODEL_F_TXFREE_SHORT) == MT7925_OK);
    CHECK(mt7925_tx_80211(&d, f, n, &op) == MT7925_OK);
    CHECK(mt7925_poll_rings(&d) < 0 && d.broken);

    /* direct records */
    uint8_t r[16];
    CHECK(up_fw(&d, MODEL_F_TXFREE_HOLD) == MT7925_OK);
    CHECK(mt7925_tx_80211(&d, f, n, &op) == MT7925_OK && mt7925_tx_80211(&d, f, n, &op) == MT7925_OK);
    put_le32(r, 16u | (2u << 16)); put_le32(r + 4, 4u << 16);
    put_le32(r + 8, MT7925_TXFREE_INFO_HEADER); put_le32(r + 12, 0u | (1u << 15));
    CHECK(mt7925_tx_free(&d, r, 16) == MT7925_OK && d.tok_busy == 0);
    put_le32(r + 4, 3u << 16); /* version 3: not this chip's format */
    CHECK(mt7925_tx_free(&d, r, 16) == MT7925_E_DEVICE && d.broken);
    CHECK(up_fw(&d, MODEL_F_TXFREE_HOLD) == MT7925_OK);
    put_le32(r, 12u | (1u << 16)); put_le32(r + 4, 4u << 16); put_le32(r + 8, 40u | (0x7fffu << 15));
    CHECK(mt7925_tx_free(&d, r, 12) == MT7925_E_DEVICE); /* id past the ring */
    CHECK(up_fw(&d, MODEL_F_TXFREE_HOLD) == MT7925_OK);
    CHECK(mt7925_tx_free(&d, r, 7) == MT7925_E_DEVICE);
    /* hostile records never read past `len` */
    uint32_t x = 77;
    for (int it = 0; it < 4000; it++) {
        uint8_t h[64];
        x = x * 1664525u + 1013904223u;
        uint32_t len = 8u + (x >> 26) % 57u;
        for (uint32_t k = 0; k < sizeof h; k++) { x = x * 1664525u + 1013904223u; h[k] = (uint8_t)(x >> 24); }
        put_le32(h + 4, (le32(h + 4) & ~0xf0000u) | (4u << 16));
        d.broken = 0;
        d.tok_busy = 0xffffffffu;
        (void)mt7925_tx_free(&d, h, len); /* ASan build checks the bounds */
    }
}

static uint32_t rxbuf(uint8_t *b, uint32_t rxd0x, uint32_t rxd1, uint32_t rxd2, uint32_t rxd3, uint32_t gap,
                      const uint8_t *f, uint32_t fl) {
    memset(b, 0, 32 + gap);
    uint32_t len = 32 + gap + fl;
    put_le32(b, len | (MT7925_PKT_TYPE_NORMAL << MT7925_RXD0_PKT_TYPE_SHIFT) | rxd0x);
    put_le32(b + 4, rxd1);
    put_le32(b + 8, rxd2);
    put_le32(b + 12, rxd3);
    memcpy(b + 32 + gap, f, fl);
    return len;
}

/* A receive callback that polls the rings again (what a station reply does
 * when the TX ring is full, or a key install waiting on the MCU): every
 * frame arrives once, in order, the nesting stops at MT7925_CB_DEPTH with
 * the rest counted as dropped, and the ring stays usable. */
static mt7925_dev *NEST_D;
static uint8_t nest_tag[16];
static uint32_t nest_n;
static void on_rx_nest(void *ctx, const uint8_t *buf, uint32_t len) {
    (void)ctx;
    if (len > 40 && nest_n < 16) nest_tag[nest_n++] = buf[40];
    (void)mt7925_poll_rings(NEST_D);
}

static void test_rx_nested(void) {
    mt7925_dev d;
    uint8_t f[64], b[128];
    static const uint8_t me[6] = {0x02, 0, 0, 0, 0, 0x25};
    CHECK(bring_up(&d, 0) == MT7925_OK);
    NEST_D = &d;
    nest_n = 0;
    d.on_rx = on_rx_nest;
    uint32_t fl = mk_frame(f, 0x08, 0x02, me, 40);
    for (uint8_t t = 0; t < 6; t++) {
        f[8] = t; /* lands at buf[40]: 32 RXD bytes + 8 */
        uint32_t n = rxbuf(b, 0, 0, 0, 0, 0, f, fl);
        model_queue_rx(&M, MT7925_PKT_TYPE_NORMAL, b, n);
    }
    CHECK(mt7925_poll_rings(&d) >= 0 && !d.broken && d.cb_depth == 0);
    CHECK(nest_n == MT7925_CB_DEPTH && d.rx_nest_dropped == 6u - MT7925_CB_DEPTH);
    for (uint32_t i = 0; i < nest_n; i++) CHECK(nest_tag[i] == i);
    /* the ring still moves: one more frame, one delivery */
    d.on_rx = on_rx;
    f[8] = 9;
    uint32_t n = rxbuf(b, 0, 0, 0, 0, 0, f, fl);
    uint32_t rx0 = CB.rx;
    model_queue_rx(&M, MT7925_PKT_TYPE_NORMAL, b, n);
    CHECK(mt7925_poll_rings(&d) == 1 && CB.rx == rx0 + 1u && !d.broken && M.dma_faults == 0);
}

static void test_rx_parse(void) {
    uint8_t f[200], b[600];
    mt7925_rx_info ri;
    static const uint8_t me[6] = {0x02, 0, 0, 0, 0, 0x25};
    uint32_t fl = mk_frame(f, 0x08, 0x02, me, 60), n;
    /* plain */
    n = rxbuf(b, 0, 1, 0, 6u << 8, 0, f, fl);
    CHECK(mt7925_rx_parse(b, n, &ri) == MT7925_RX_OK && ri.frame == b + 32 && ri.frame_len == fl);
    CHECK(ri.wlan_idx == 1 && ri.channel == 6 && !ri.decrypted && !ri.has_rssi);
    CHECK(mt7925_rx_parse(b, n + 100, &ri) == MT7925_RX_OK && ri.frame_len == fl); /* RXD length wins */
    /* groups 4, 1, 2, 3: frame after 64 bytes, RCPI in group 3 */
    uint32_t g = MT7925_RXD1_GROUP_1 | MT7925_RXD1_GROUP_2 | MT7925_RXD1_GROUP_3 | MT7925_RXD1_GROUP_4;
    n = rxbuf(b, 0, g, 0, 0, 64, f, fl);
    b[32 + 48 + 12] = 140; /* -40 dBm */
    CHECK(mt7925_rx_parse(b, n, &ri) == MT7925_RX_OK && ri.frame == b + 96 && ri.has_rssi && ri.rssi == -40);
    /* group 3 + 5 */
    n = rxbuf(b, 0, MT7925_RXD1_GROUP_3 | MT7925_RXD1_GROUP_5, 0, 0, 112, f, fl);
    CHECK(mt7925_rx_parse(b, n, &ri) == MT7925_RX_OK && ri.frame == b + 144);
    /* group 5 without 3 adds nothing (it lives inside group 3) */
    n = rxbuf(b, 0, MT7925_RXD1_GROUP_5, 0, 0, 0, f, fl);
    CHECK(mt7925_rx_parse(b, n, &ri) == MT7925_RX_OK && ri.frame == b + 32);
    /* header offset */
    n = rxbuf(b, 0, 0, 1u << MT7925_RXD2_HDR_OFFSET_SHIFT, 0, 2, f, fl);
    CHECK(mt7925_rx_parse(b, n, &ri) == MT7925_RX_OK && ri.frame == b + 34);
    /* decrypted CCMP, protected bit set */
    f[1] |= 0x40;
    n = rxbuf(b, 0, 0, MT7925_CIPHER_CCMP << MT7925_RXD2_SEC_MODE_SHIFT, 0, 0, f, fl);
    CHECK(mt7925_rx_parse(b, n, &ri) == MT7925_RX_OK && ri.decrypted && ri.sec_mode == MT7925_CIPHER_CCMP);
    /* protected but not opened: never passed on */
    n = rxbuf(b, 0, 0, 0, 0, 0, f, fl);
    CHECK(mt7925_rx_parse(b, n, &ri) == MT7925_RX_CIPHER);
    n = rxbuf(b, 0, MT7925_RXD1_CM, MT7925_CIPHER_CCMP << MT7925_RXD2_SEC_MODE_SHIFT, 0, 0, f, fl);
    CHECK(mt7925_rx_parse(b, n, &ri) == MT7925_RX_CIPHER);
    n = rxbuf(b, 0, MT7925_RXD1_CLM, MT7925_CIPHER_CCMP << MT7925_RXD2_SEC_MODE_SHIFT, 0, 0, f, fl);
    CHECK(mt7925_rx_parse(b, n, &ri) == MT7925_RX_CIPHER);
    n = rxbuf(b, 0, 0, 2u << MT7925_RXD2_SEC_MODE_SHIFT, 0, 0, f, fl);
    CHECK(mt7925_rx_parse(b, n, &ri) == MT7925_RX_CIPHER);
    n = rxbuf(b, 0, 0, (MT7925_CIPHER_CCMP << MT7925_RXD2_SEC_MODE_SHIFT) | MT7925_RXD2_FRAG, 0, 0, f, fl);
    CHECK(mt7925_rx_parse(b, n, &ri) == MT7925_RX_UNSUPPORTED);
    f[1] &= (uint8_t)~0x40u;
    /* chip-flagged errors */
    struct { uint32_t r1, r2, r3; mt7925_rx_err want; } errs[] = {
        {0, MT7925_RXD2_AMSDU_ERR, 0, MT7925_RX_LEN_ERR},
        {0, MT7925_RXD2_MAX_LEN_ERR, 0, MT7925_RX_LEN_ERR},
        {0, MT7925_RXD2_HDR_TRANS, 0, MT7925_RX_UNSUPPORTED},
        {0, 0, MT7925_RXD3_FCS_ERR, MT7925_RX_BAD_FCS},
        {MT7925_RXD1_ICV_ERR, 0, 0, MT7925_RX_BAD_ICV},
        {MT7925_RXD1_TKIP_MIC_ERR, 0, 0, MT7925_RX_BAD_MIC},
    };
    for (size_t i = 0; i < sizeof errs / sizeof errs[0]; i++) {
        n = rxbuf(b, 0, errs[i].r1, errs[i].r2, errs[i].r3, 0, f, fl);
        CHECK(mt7925_rx_parse(b, n, &ri) == errs[i].want);
    }
    n = rxbuf(b, 0, 0, 0, 0, 0, f, fl);
    b[16] = 1; /* A-MSDU subframe */
    CHECK(mt7925_rx_parse(b, n, &ri) == MT7925_RX_UNSUPPORTED);
    /* length disagreements */
    n = rxbuf(b, 0, 0, 0, 0, 0, f, fl);
    CHECK(mt7925_rx_parse(b, n - 1, &ri) == MT7925_RX_SHORT);
    CHECK(mt7925_rx_parse(b, 31, &ri) == MT7925_RX_SHORT);
    put_le32(b, 20u | (MT7925_PKT_TYPE_NORMAL << MT7925_RXD0_PKT_TYPE_SHIFT));
    CHECK(mt7925_rx_parse(b, n, &ri) == MT7925_RX_SHORT);
    n = rxbuf(b, 0, 0, 0, 0, 0, f, 23);
    CHECK(mt7925_rx_parse(b, n, &ri) == MT7925_RX_SHORT);
    n = rxbuf(b, 0, MT7925_RXD1_GROUP_3, 0, 0, 0, f, 10);
    CHECK(mt7925_rx_parse(b, 42, &ri) == MT7925_RX_SHORT);
    /* not a frame: an event; the SW-tagged frame form is one */
    n = rxbuf(b, 0, 0, 0, 0, 0, f, fl);
    put_le32(b, n | (MT7925_PKT_TYPE_RX_EVENT << MT7925_RXD0_PKT_TYPE_SHIFT));
    CHECK(mt7925_rx_parse(b, n, &ri) == MT7925_RX_NOT_FRAME);
    put_le32(b, n | (MT7925_RXD0_SW_PKT_TYPE_FRAME << MT7925_RXD0_SW_PKT_TYPE_SHIFT));
    CHECK(mt7925_rx_parse(b, n, &ri) == MT7925_RX_OK);
    /* random descriptors: an accepted frame always lies inside the buffer.
     * Even iterations are pure noise (almost always refused); odd ones are
     * the valid buffer above with 1 to 3 bytes of the first 64 changed and
     * the length cut by 0 to 7, so the accept path is reached too. */
    uint8_t seed[600];
    uint32_t sn = n, x = 3, ok = 0;
    memcpy(seed, b, sn);
    for (int it = 0; it < 50000; it++) {
        x = x * 1664525u + 1013904223u;
        uint32_t len = x >> 23; /* 0..511 */
        if (it & 1) {
            len = sn - ((x >> 8) & 7u);
            memcpy(b, seed, sn);
            for (uint32_t k = 0; k <= ((x >> 12) % 3u); k++) {
                x = x * 1664525u + 1013904223u;
                b[(x >> 20) % 64u] = (uint8_t)(x >> 8);
            }
        } else {
            for (uint32_t k = 0; k < len; k++) { x = x * 1664525u + 1013904223u; b[k] = (uint8_t)(x >> 24); }
            if (len >= 4 && (x & 1)) put_le32(b, (le32(b) & 0xffff0000u) | (x % (len + 8u)));
        }
        if (mt7925_rx_parse(b, len, &ri) == MT7925_RX_OK) {
            ok++;
            CHECK(ri.frame >= b + 32 && ri.frame_len >= 24 && ri.frame + ri.frame_len <= b + len);
        }
    }
    CHECK(ok > 0);
}

/* ---- station join commands ---- */

/* TLV list after an `hl`-byte header: count at bytes 2..3 must match and the
 * lengths must add up to n exactly. */
static int tlvs_ok(const uint8_t *p, uint32_t n, uint32_t hl, uint32_t want_count) {
    uint32_t off = hl, c = 0;
    while (off < n) {
        if (n - off < 4) return 0;
        uint32_t tl = (uint32_t)(p[off + 2] | (p[off + 3] << 8));
        if (tl < 4 || tl > n - off) return 0;
        off += tl;
        c++;
    }
    return off == n && c == want_count && (uint32_t)(p[2] | (p[3] << 8)) == want_count;
}

static mt7925_join_cfg join_cfg(void) {
    mt7925_join_cfg c = {{0x02, 0, 0, 0, 0, 0x25}, {0x02, 0, 0, 0, 0, 1}, 1, 6, 100, 1, 0, 1, 0, 0, -40};
    return c;
}

static void test_join_builders(void) {
    uint8_t b[MT7925_JOIN_CMD_MAX];
    mt7925_join_cfg c = join_cfg();
    static const uint8_t key[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    CHECK(mt7925_build_nic_cap_query(b, sizeof b) == 8 && b[4] == MT7925_NIC_CAP_TAG && b[6] == 4);
    CHECK(mt7925_build_dev_info(b, sizeof b, c.own_mac, 1) == 16 && b[8] == 1 && memcmp(b + 10, c.own_mac, 6) == 0);
    CHECK(b[1] == MT7925_BAND_AUTO && b[6] == 12);
    CHECK(mt7925_build_bss_own(b, sizeof b, 1) == 36 && b[2] == 0 && b[6] == 32 && b[8] == 1 && b[16] == 1);
    CHECK(le32(b + 12) == MT7925_CONN_INFRA_STA && b[24] == MT7925_WCID_OWN && b[30] == MT7925_WCID_OWN);
    uint32_t n = mt7925_build_bss_info(b, sizeof b, &c, 0);
    CHECK(n == 108 && tlvs_ok(b, n, 4, 6) && b[4] == 0 && b[16] == 1 && memcmp(b + 18, c.bssid, 6) == 0);
    CHECK(b[29] == 0x06 && b[30] == MT7925_WCID_AP && b[40] == 0 && b[41] == 1); /* phymode B|G; SEC open */
    c.cipher = MT7925_CIPHER_CCMP;
    n = mt7925_build_bss_info(b, sizeof b, &c, 1);
    CHECK(n == 132 && tlvs_ok(b, n, 4, 8) && b[16] == 0 && b[40] == 7 && b[41] == 6 && b[42] == MT7925_CIPHER_CCMP);
    CHECK(b[108] == 2 && b[112] == 6 && b[120] == 1); /* RLM: channel 6, band 2.4 GHz */
    c.band = 2; c.channel = 36;
    n = mt7925_build_bss_info(b, sizeof b, &c, 0);
    CHECK(n == 108 && b[29] == 0x01 && b[44 + 13] == MT7925_BASIC_RATES_TBL + 4u);
    c = join_cfg();
    CHECK(mt7925_build_roc(b, sizeof b, 9, 6, 1, 1000, 0) == 28 && b[9] == 9 && b[10] == 6 && b[12] == 1);
    CHECK(le32(b + 20) == 1000 && b[19] == MT7925_ROC_REQ_JOIN);
    CHECK(mt7925_build_roc(b, sizeof b, 9, 0, 0, 0, 1) == 16 && b[4] == 1 && b[9] == 9);
    CHECK(mt7925_build_roc(b, sizeof b, 9, 0, 1, 1000, 0) == 0 && mt7925_build_roc(b, sizeof b, 9, 6, 4, 1, 0) == 0);
    n = mt7925_build_sta_rec(b, sizeof b, &c, MT7925_WCID_AP, MT7925_STA_STATE_NONE);
    CHECK(n == 80 && tlvs_ok(b, n, 8, 5) && b[1] == MT7925_WCID_AP && b[4] == 1 && b[5] == 0);
    CHECK(le32(b + 12) == MT7925_CONN_INFRA_AP && memcmp(b + 20, c.bssid, 6) == 0 && b[26] == 3);
    CHECK((b[32] | (b[33] << 8)) == 0x000f && b[34] == 0x03 && b[37] == 140); /* basic 1/2/5.5/11; RCPI -40 */
    CHECK((b[44] | (b[45] << 8)) == 0x3fcf);                                   /* every 2.4 GHz rate, RA layout */
    n = mt7925_build_sta_rec(b, sizeof b, &c, MT7925_WCID_AP, MT7925_STA_STATE_ASSOC);
    CHECK(n == 108 && tlvs_ok(b, n, 8, 6) && b[26] == 1 && b[60] == MT7925_STA_STATE_ASSOC);
    n = mt7925_build_sta_rec(b, sizeof b, &c, MT7925_WCID_OWN, MT7925_STA_STATE_ASSOC);
    CHECK(n == 16 && tlvs_ok(b, n, 8, 1) && b[1] == MT7925_WCID_OWN && b[5] == 0x0e && b[8] == 0x2b && b[14] == 1);
    n = mt7925_build_sta_rec(b, sizeof b, &c, 0x123, MT7925_STA_STATE_NONE);
    CHECK(n && b[1] == 0x23 && b[6] == 1);
    CHECK(mt7925_build_sta_rec(b, sizeof b, &c, 0x800, 0) == 0 && mt7925_build_sta_rec(b, sizeof b, &c, 1, 3) == 0);
    n = mt7925_build_key(b, sizeof b, &c, 1, 0, key);
    CHECK(n == 76 && tlvs_ok(b, n, 8, 1) && b[8] == 0x27 && b[10] == 68 && b[13] == 1 && b[23] == MT7925_CIPHER_CCMP);
    CHECK(memcmp(b + 28, key, 16) == 0 && b[1] == MT7925_WCID_AP);
    n = mt7925_build_key(b, sizeof b, &c, 0, 1, key);
    CHECK(n == 76 && b[1] == MT7925_WCID_OWN && b[13] == 0 && b[24] == 1 && b[5] == 0x0e);
    CHECK(mt7925_build_key(b, sizeof b, &c, 0, 4, key) == 0);
    /* every builder refuses a buffer one byte short, at every size below its own */
    for (uint32_t cap = 0; cap < 132; cap++) {
        CHECK(cap >= 108 || mt7925_build_bss_info(b, cap, &c, 0) == 0);
        CHECK(mt7925_build_bss_info(b, cap, &c, 1) == 0);
        CHECK(cap >= 108 || mt7925_build_sta_rec(b, cap, &c, MT7925_WCID_AP, MT7925_STA_STATE_ASSOC) == 0);
        CHECK(cap >= 76 || mt7925_build_key(b, cap, &c, 1, 0, key) == 0);
        CHECK(cap >= 36 || mt7925_build_bss_own(b, cap, 1) == 0);
        CHECK(cap >= 28 || mt7925_build_roc(b, cap, 1, 6, 1, 1, 0) == 0);
    }
    c.band = 0;
    CHECK(mt7925_build_bss_info(b, sizeof b, &c, 0) == 0 && mt7925_build_sta_rec(b, sizeof b, &c, 1, 0) == 0);
    c.band = 1; c.channel = 0;
    CHECK(mt7925_build_bss_info(b, sizeof b, &c, 0) == 0);

    /* NIC capability reply: the MAC element after another element */
    uint8_t ev[MT7925_EVT_HDR_BYTES + 32] = {0}, mac[6];
    uint8_t *p = ev + MT7925_EVT_HDR_BYTES;
    p[0] = 2;
    p[4] = 0; p[6] = 8;
    p[12] = MT7925_NIC_CAP_MAC_ADDR; p[14] = 12;
    memcpy(p + 16, c.own_mac, 6);
    CHECK(mt7925_parse_nic_cap(ev, MT7925_EVT_HDR_BYTES + 24, mac) == MT7925_OK && memcmp(mac, c.own_mac, 6) == 0);
    CHECK(mt7925_parse_nic_cap(ev, MT7925_EVT_HDR_BYTES + 20, mac) == MT7925_E_DEVICE);
    p[0] = 1;
    CHECK(mt7925_parse_nic_cap(ev, MT7925_EVT_HDR_BYTES + 24, mac) == MT7925_E_STATE);
    p[0] = 2; p[6] = 0; /* zero-length element would never advance */
    CHECK(mt7925_parse_nic_cap(ev, MT7925_EVT_HDR_BYTES + 24, mac) == MT7925_E_DEVICE);
    p[0] = 0xff; p[6] = 8; p[14] = 9;
    CHECK(mt7925_parse_nic_cap(ev, MT7925_EVT_HDR_BYTES + 24, mac) == MT7925_E_DEVICE);
    CHECK(mt7925_parse_nic_cap(ev, MT7925_EVT_HDR_BYTES + 3, mac) == MT7925_E_DEVICE);
    /* ROC grant */
    memset(ev, 0, sizeof ev);
    ev[MT7925_EVT_EID_OFF] = MT7925_UNI_EVENT_ROC;
    uint8_t tok = 0, st = 0xee;
    p[4] = 0; p[6] = 20; p[9] = 7; p[10] = 0;
    CHECK(mt7925_parse_roc_grant(ev, MT7925_EVT_HDR_BYTES + 24, &tok, &st) == 1 && tok == 7 && st == 0);
    CHECK(mt7925_parse_roc_grant(ev, MT7925_EVT_HDR_BYTES + 23, &tok, &st) == 0);
    p[4] = 1;
    CHECK(mt7925_parse_roc_grant(ev, MT7925_EVT_HDR_BYTES + 24, &tok, &st) == 0);
    p[4] = 0; ev[MT7925_EVT_EID_OFF] = 0x01;
    CHECK(mt7925_parse_roc_grant(ev, MT7925_EVT_HDR_BYTES + 24, &tok, &st) == 0);
}

static int find_bytes(const uint8_t *h, size_t hl, const uint8_t *n, size_t nl) {
    for (size_t i = 0; i + nl <= hl; i++)
        if (memcmp(h + i, n, nl) == 0) return 1;
    return 0;
}

static void test_join_model(void) {
    mt7925_dev d;
    uint8_t mac[6];
    mt7925_join_cfg c = join_cfg();
    static const uint8_t key[16] = {0xa5, 0x5a, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee};

    CHECK(bring_up(&d, 0) == MT7925_OK);
    CHECK(mt7925_read_mac(&d, mac) == MT7925_E_STATE && mt7925_join_prepare(&d, &c) == MT7925_E_STATE);
    CHECK(up_fw(&d, 0) == MT7925_OK);
    CHECK(mt7925_read_mac(&d, mac) == MT7925_OK && memcmp(mac, M.efuse_mac, 6) == 0);
    CHECK(mt7925_join_iface_up(&d, mac) == MT7925_OK && M.dev_active && memcmp(M.own_mac, mac, 6) == 0);
    CHECK(M.bss_records == 1 && M.bss_conn_state == 1);
    CHECK(mt7925_join_prepare(&d, &c) == MT7925_OK && M.roc_active && M.roc_channel == 6);
    CHECK(M.roc_token == d.roc_token && M.sta_state == MT7925_STA_STATE_NONE && memcmp(M.sta_peer, c.bssid, 6) == 0);
    CHECK(memcmp(M.bss_bssid, c.bssid, 6) == 0 && M.bss_conn_state == 1 && !M.bss_has_rlm);
    c.aid = 1;
    CHECK(mt7925_join_assoc_done(&d, &c) == MT7925_OK && M.bss_conn_state == 0 && M.bss_has_rlm);
    CHECK(M.sta_state == MT7925_STA_STATE_ASSOC && M.sta_wcid == MT7925_WCID_OWN);
    c.cipher = MT7925_CIPHER_CCMP;
    CHECK(mt7925_join_install_key(&d, &c, 1, 0, key) == MT7925_OK && (M.keys & 1) && memcmp(M.key_pairwise, key, 16) == 0);
    CHECK(!find_bytes(region, sizeof region, key, sizeof key)); /* no copy left in DMA memory */
    CHECK(mt7925_join_install_key(&d, &c, 0, 1, key) == MT7925_OK && (M.keys & 2));
    CHECK(!find_bytes(region, sizeof region, key, sizeof key));
    CHECK(mt7925_join_authorized(&d) == MT7925_OK && !M.roc_active);
    CHECK(M.uni_bad == 0 && M.mcu_bad == 0 && M.dma_faults == 0);
    /* protected TX: sent with the key, refused by the chip without one */
    uint8_t f[128];
    mt7925_tx_opts op = {MT7925_WCID_AP, 0, 0, 1, 0};
    uint32_t n = mk_frame(f, 0x08, 0x41, AP_BSSID, 40);
    CHECK(mt7925_tx_80211(&d, f, n, &op) == MT7925_OK && M.protected_tx == 1 && M.protected_dropped == 0);
    CHECK(le32(M.last_txd + 12) & MT7925_TXD3_PROTECT_FRAME);
    CHECK(mt7925_poll_rings(&d) >= 0 && d.tok_busy == 0);

    const mt7925_join_cfg c0 = join_cfg();
    /* ROC never granted: bounded wait, then the ROC is aborted */
    CHECK(up_fw(&d, MODEL_F_ROC_SILENT) == MT7925_OK);
    uint64_t t0 = M.time_us;
    CHECK(mt7925_join_prepare(&d, &c0) == MT7925_E_TIMEOUT && !M.roc_active);
    CHECK(M.time_us - t0 >= MT7925_MCU_REPLY_US && M.time_us - t0 < 2u * MT7925_MCU_REPLY_US + 100000u);
    CHECK(d.on_event == on_event && d.cb_ctx == &CB); /* the hook is gone */
    /* the chip refuses the station record */
    CHECK(up_fw(&d, MODEL_F_STA_FAIL) == MT7925_OK);
    CHECK(mt7925_join_prepare(&d, &c0) == MT7925_E_MCU_STATUS && !M.roc_active);
    /* silent MCU */
    CHECK(up_fw(&d, 0) == MT7925_OK);
    M.faults |= MODEL_F_MCU_SILENT;
    CHECK(mt7925_join_iface_up(&d, mac) == MT7925_E_TIMEOUT);
    CHECK(mt7925_read_mac(&d, mac) == MT7925_E_TIMEOUT);
    /* bad arguments */
    c.band = 9;
    CHECK(mt7925_join_prepare(&d, &c) == MT7925_E_ARG && mt7925_join_assoc_done(&d, &c) == MT7925_E_ARG);
    c = join_cfg();
    CHECK(mt7925_join_install_key(&d, &c, 1, 4, key) == MT7925_E_ARG);
}

/* ---- the whole station over the model: scan, join, ARP, ping ---- */

static wifi_sta S;
static mt7925_link L;
static const model_ap AP_OPEN = {{0x02, 0, 0, 0, 0, 1}, 6, "aienos", 6, -40, {0}, 0, {10, 0, 0, 1}, {0x02, 0, 0, 0, 0, 0x99}};
static const model_ap AP_RSN = {{0x02, 0, 0, 0, 0, 2}, 7, "aienwpa", 11, -30,
                                {1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 2, 0, 0},
                                20, {10, 0, 1, 1}, {0x02, 0, 0, 0, 1, 0x99}};
/* WPA2-PSK-SHA256 (AKM 6, MFPC): the one AKM the 4-way runs for in the model. */
static const model_ap AP_SHA = {{0x02, 0, 0, 0, 0, 3}, 7, "aiensha", 11, -35,
                                {1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 6, 0x80, 0},
                                20, {10, 0, 2, 1}, {0x02, 0, 0, 0, 2, 0x99}};

static mt7925_err station_up(mt7925_dev *d, uint32_t faults) {
    mt7925_err e = up_fw(d, faults);
    if (e != MT7925_OK) return e;
    model_add_ap(&M, &AP_OPEN);
    model_add_ap(&M, &AP_RSN);
    model_add_ap(&M, &AP_SHA);
    uint8_t mac[6];
    if ((e = mt7925_read_mac(d, mac)) != MT7925_OK || (e = mt7925_join_iface_up(d, mac)) != MT7925_OK) return e;
    mt7925_link_attach(&L, d, &S, mac);
    mt7925_chan ch[2] = {{1, 6}, {1, 11}};
    if ((e = mt7925_scan_start(d, 1, 0, 0, ch, 2, 0, 0)) != MT7925_OK) return e;
    for (int i = 0; i < 4; i++) mt7925_poll_rings(d);
    return MT7925_OK;
}

static int recv_eth(net_eth_frame *ef, uint8_t *buf, uint32_t cap) {
    uint32_t n = 0;
    for (int i = 0; i < 8 && !wifi_sta_recv_eth(&S, buf, cap, &n); i++) mt7925_poll_rings(L.d);
    return n && net_eth_parse(buf, n, ef) == NET_OK;
}

static void test_station(void) {
    mt7925_dev d;
    uint8_t eth[1600], pl[1500], rx[1600];
    size_t n = 0, pn = 0;
    net_eth_frame ef;
    static const net_ipv4 me = {{10, 0, 0, 2}}, gw = {{10, 0, 0, 1}};

    CHECK(station_up(&d, 0) == MT7925_OK);
    CHECK(S.scan.n == 3);
    /* WPA2 network: refused before any frame goes out, with the reason */
    CHECK(wifi_sta_connect(&S, (const uint8_t *)"aienwpa", 7, 200000) == WIFI_E_CRYPTO);
    CHECK(S.sec == WIFI_SEC_WPA2_PSK && wifi_sta_missing_crypto(S.sec) != 0 && M.auths == 0 && M.ct_frames == 0);
    CHECK(wifi_sta_connect(&S, (const uint8_t *)"nowhere", 7, 200000) == WIFI_E_NOT_FOUND);
    /* open network */
    CHECK(wifi_sta_connect(&S, (const uint8_t *)"aienos", 6, 200000) == WIFI_OK);
    CHECK(S.st == WIFI_STA_RUN && S.aid == 1 && M.assoc_ap == 0 && M.auths == 1 && M.assocs == 1);
    CHECK(M.sta_state == MT7925_STA_STATE_ASSOC && M.bss_has_rlm && M.bss_conn_state == 0 && M.bss_cipher == 0);
    CHECK(L.cfg.channel == 6 && L.cfg.band == 1 && L.cfg.aid == 1);
    CHECK(mt7925_join_authorized(&d) == MT7925_OK && !M.roc_active);
    /* A data frame on the event ring while a command waits: held, not handed
     * to the station inside the command, delivered by the next poll. */
    {
        uint8_t df[64] = {0x08, 0x02}, rb[128], mac[6];
        memcpy(df + 4, S.mac, 6); memcpy(df + 10, AP_OPEN.bssid, 6); memcpy(df + 16, AP_OPEN.mac, 6);
        static const uint8_t snap[8] = {0xaa, 0xaa, 0x03, 0, 0, 0, 0x08, 0x06};
        memcpy(df + 24, snap, 8);
        uint32_t rl = rxbuf(rb, 0, 1, 0, 6u << 8, 0, df, 60);
        uint32_t before = S.rx_frames;
        model_queue_evt_raw(&M, rb, rl);
        CHECK(mt7925_read_mac(&d, mac) == MT7925_OK);
        CHECK(d.rx_deferred == 1 && d.defer_n == 1 && S.rx_frames == before);
        CHECK(mt7925_poll_rings(&d) >= 1 && d.defer_n == 0 && S.rx_frames == before + 1);
        uint32_t n2 = 0;
        CHECK(wifi_sta_recv_eth(&S, rx, sizeof rx, &n2) && n2 == 60u - 24u - 8u + 14u && rx[12] == 0x08 && rx[13] == 0x06);
    }

    /* ARP for the gateway through aienos_net */
    net_arp_packet arp = {NET_ARP_REQUEST, {{0}}, me, {{0}}, gw};
    memcpy(arp.sender_mac.b, S.mac, 6);
    CHECK(net_arp_build(&arp, pl, sizeof pl, &pn) == NET_OK);
    net_eth_frame out = {{{0xff, 0xff, 0xff, 0xff, 0xff, 0xff}}, {{0}}, NET_ETHERTYPE_ARP, pl, pn};
    memcpy(out.source.b, S.mac, 6);
    CHECK(net_eth_build(&out, eth, sizeof eth, &n) == NET_OK);
    CHECK(wifi_sta_send_eth(&S, eth, (uint32_t)n) == WIFI_OK && M.arp_replies == 1);
    CHECK(recv_eth(&ef, rx, sizeof rx) && ef.ethertype == NET_ETHERTYPE_ARP);
    net_arp_packet ar;
    CHECK(net_arp_parse(ef.payload, ef.payload_len, &ar) == NET_OK && ar.operation == NET_ARP_REPLY);
    CHECK(memcmp(ar.sender_mac.b, AP_OPEN.mac, 6) == 0 && memcmp(ar.sender_ip.b, gw.b, 4) == 0);
    CHECK(memcmp(ef.destination.b, S.mac, 6) == 0 && memcmp(ef.source.b, AP_OPEN.mac, 6) == 0);

    /* ping the gateway, several sizes */
    for (uint16_t seq = 1; seq <= 5; seq++) {
        uint8_t data[1200], icmp[1300], ip[1400];
        size_t dl = 8u + seq * 200u, il = 0, ipl = 0;
        for (size_t i = 0; i < dl; i++) data[i] = (uint8_t)(i ^ seq);
        net_icmp_echo req = {0, 0x925, seq, dl};
        CHECK(net_icmp_echo_build(&req, data, dl, icmp, sizeof icmp, &il) == NET_OK);
        net_ipv4_header h = {0, seq, 64, NET_IPPROTO_ICMP, me, gw};
        CHECK(net_ipv4_build(&h, icmp, il, ip, sizeof ip, &ipl) == NET_OK);
        net_eth_frame e4 = {{{0}}, {{0}}, NET_ETHERTYPE_IPV4, ip, ipl};
        memcpy(e4.destination.b, ar.sender_mac.b, 6);
        memcpy(e4.source.b, S.mac, 6);
        CHECK(net_eth_build(&e4, eth, sizeof eth, &n) == NET_OK);
        CHECK(wifi_sta_send_eth(&S, eth, (uint32_t)n) == WIFI_OK);
        CHECK(recv_eth(&ef, rx, sizeof rx) && ef.ethertype == NET_ETHERTYPE_IPV4);
        net_ipv4_header rh;
        const uint8_t *ipp = 0;
        size_t ippl = 0;
        net_icmp_echo rep;
        CHECK(net_ipv4_parse(ef.payload, ef.payload_len, &rh, &ipp, &ippl) == NET_OK);
        CHECK(memcmp(rh.source.b, gw.b, 4) == 0 && rh.protocol == NET_IPPROTO_ICMP);
        CHECK(net_icmp_echo_parse(ipp, ippl, &rep) == NET_OK && rep.reply && rep.sequence == seq);
        CHECK(rep.identifier == 0x925 && rep.data_len == dl && memcmp(ipp + 8, data, dl) == 0);
    }
    CHECK(M.echo_replies == 5 && S.rx_dropped == 0 && S.rx_unprotected_dropped == 0);
    CHECK(d.tok_busy == 0 && d.tx_freed == M.ct_frames && M.ct_frames == 8);
    for (uint32_t i = 0; i < MT7925_LINK_RX_ERRS; i++) CHECK(L.rx_refused[i] == 0);
    CHECK(M.dma_faults == 0 && M.mcu_bad == 0 && M.uni_bad == 0 && !d.broken);
    /* a frame too big for one slot is refused, nothing posted */
    memset(eth, 0, sizeof eth);
    CHECK(wifi_sta_send_eth(&S, eth, WIFI_STA_ETH_MAX + 1u) == WIFI_E_SPACE);
    /* leave */
    CHECK(wifi_sta_disconnect(&S) == WIFI_OK && M.assoc_ap == -1 && S.st == WIFI_STA_IDLE);
    CHECK(wifi_sta_send_eth(&S, eth, 60) == WIFI_E_STATE);

    /* AP failures */
    CHECK(station_up(&d, MODEL_F_AP_SILENT) == MT7925_OK);
    CHECK(wifi_sta_connect(&S, (const uint8_t *)"aienos", 6, 50000) == WIFI_E_TIMEOUT && S.st == WIFI_STA_IDLE);
    CHECK(station_up(&d, MODEL_F_AP_DEAUTH) == MT7925_OK);
    CHECK(wifi_sta_connect(&S, (const uint8_t *)"aienos", 6, 50000) == WIFI_E_REFUSED && S.last_status == 1);
    CHECK(station_up(&d, MODEL_F_ASSOC_DENY) == MT7925_OK);
    CHECK(wifi_sta_connect(&S, (const uint8_t *)"aienos", 6, 50000) == WIFI_E_REFUSED && S.last_status == 17);
    CHECK(station_up(&d, MODEL_F_ROC_SILENT) == MT7925_OK);
    CHECK(wifi_sta_connect(&S, (const uint8_t *)"aienos", 6, 50000) == WIFI_E_RADIO && M.auths == 0);
    CHECK(station_up(&d, MODEL_F_STA_FAIL) == MT7925_OK);
    CHECK(wifi_sta_connect(&S, (const uint8_t *)"aienos", 6, 50000) == WIFI_E_RADIO && M.auths == 0);
    /* tokens stuck at the chip: data is refused, never overwritten in flight */
    CHECK(station_up(&d, MODEL_F_TXFREE_HOLD) == MT7925_OK);
    CHECK(wifi_sta_connect(&S, (const uint8_t *)"aienos", 6, 50000) == WIFI_OK);
    uint32_t sent = 0;
    eth[12] = 0x08; /* IPv4: an all-zero header is an 802.3 length frame, refused before the ring */
    wifi_err we;
    while ((we = wifi_sta_send_eth(&S, eth, 60)) == WIFI_OK && sent < 64) sent++;
    CHECK(we == WIFI_E_RADIO && sent + 2u >= MT7925_N_TX_DATA - 1u && sent < MT7925_N_TX_DATA);
}

/* WPA2-PSK-SHA256 end to end over the chip model: 4-way handshake through
 * the data ring, keys into the chip through STA_REC KEY, then ARP and ping
 * over the protected link. The MIC and key wrap are the wpa_toy.h test
 * doubles (native/ has neither); the PTK derivation is the real KDF. */
/* Radio TX wrapper: after the station sends 4-way message 4 (EAPOL-Key with
 * Secure and without Ack), hand it a deauthentication from the AP at once,
 * as a nested poll inside a full-ring TX would. */
static int (*ORIG_TX)(void *ctx, const uint8_t *frame, uint32_t len, int protect);
static int M4_SEEN;
static int tx_deauth_on_m4(void *ctx, const uint8_t *f, uint32_t len, int protect) {
    int r = ORIG_TX(ctx, f, len, protect);
    if (len > 40 && (f[0] & 0x0c) == 0x08 && f[30] == 0x88 && f[31] == 0x8e && (f[37] & 0x02) && !(f[38] & 0x80)) {
        uint8_t d[26] = {0xc0, 0x00};
        memcpy(d + 4, S.mac, 6);
        memcpy(d + 10, AP_SHA.bssid, 6);
        memcpy(d + 16, AP_SHA.bssid, 6);
        d[24] = 1;
        M4_SEEN++;
        wifi_sta_rx(&S, d, sizeof d, 0, -30);
    }
    return r;
}

static void test_station_wpa(void) {
    mt7925_dev d;
    uint8_t pmk[32], bad_pmk[32], eth[1600], pl[200], rx[1600];
    size_t n = 0, pn = 0;
    net_eth_frame ef;
    toy_ctx t = {1, 0x77};
    for (int i = 0; i < 32; i++) { pmk[i] = (uint8_t)(0x10 + i); bad_pmk[i] = (uint8_t)(0x11 + i); }
    wpa_crypto toy = toy_crypto(WIFI_SEC_WPA2_PSK_SHA256, &t);

    CHECK(station_up(&d, 0) == MT7925_OK);
    model_set_pmk(&M, pmk);
    /* no PSK, then the native table: refused before any frame */
    CHECK(wifi_sta_connect(&S, (const uint8_t *)"aiensha", 7, 200000) == WIFI_E_CRYPTO && M.auths == 0);
    wifi_sta_set_psk(&S, pmk, 0);
    CHECK(wifi_sta_connect(&S, (const uint8_t *)"aiensha", 7, 200000) == WIFI_E_CRYPTO && M.auths == 0);
    CHECK(S.sec == WIFI_SEC_WPA2_PSK_SHA256);
    /* with a platform entropy source the native table still lacks the MIC and key wrap */
    S.r.random = toy_random;
    S.r.rng_ctx = &t;
    CHECK(wifi_sta_connect(&S, (const uint8_t *)"aiensha", 7, 200000) == WIFI_E_CRYPTO && M.auths == 0);
    CHECK(S.tbl.random && S.tbl.prf && !S.tbl.mic && !S.tbl.unwrap);
    /* with a complete table */
    wifi_sta_set_psk(&S, pmk, &toy);
    CHECK(wifi_sta_connect(&S, (const uint8_t *)"aiensha", 7, 200000) == WIFI_OK);
    CHECK(S.st == WIFI_STA_RUN && M.ap_wpa == 3 && M.eapol_bad == 0 && S.eapol_refused == 0 && S.keys_done == 1);
    CHECK(M.keys == 3 && memcmp(M.key_pairwise, M.ap_ptk + 32, 16) == 0 && memcmp(M.key_group, M.ap_gtk, 16) == 0);
    CHECK(M.bss_cipher == MT7925_CIPHER_CCMP && S.ptk_set && M.protected_dropped == 0);
    /* message 2 and 4 went out unprotected (no pairwise key yet), data after is protected */
    uint32_t prot0 = M.protected_tx;
    CHECK(prot0 == 0);
    static const net_ipv4 me = {{10, 0, 2, 2}}, gw = {{10, 0, 2, 1}};
    net_arp_packet arp = {NET_ARP_REQUEST, {{0}}, me, {{0}}, gw};
    memcpy(arp.sender_mac.b, S.mac, 6);
    CHECK(net_arp_build(&arp, pl, sizeof pl, &pn) == NET_OK);
    net_eth_frame out = {{{0xff, 0xff, 0xff, 0xff, 0xff, 0xff}}, {{0}}, NET_ETHERTYPE_ARP, pl, pn};
    memcpy(out.source.b, S.mac, 6);
    CHECK(net_eth_build(&out, eth, sizeof eth, &n) == NET_OK);
    CHECK(wifi_sta_send_eth(&S, eth, (uint32_t)n) == WIFI_OK && M.arp_replies == 1 && M.protected_tx == 1);
    CHECK(recv_eth(&ef, rx, sizeof rx) && ef.ethertype == NET_ETHERTYPE_ARP);
    net_arp_packet ar;
    CHECK(net_arp_parse(ef.payload, ef.payload_len, &ar) == NET_OK && ar.operation == NET_ARP_REPLY &&
          memcmp(ar.sender_mac.b, AP_SHA.mac, 6) == 0);
    CHECK(M.unprot_dropped == 0 && S.rx_unprotected_dropped == 0 && M.dma_faults == 0 && M.uni_bad == 0);
    /* the same protected frame again, packet number and all: a replay, dropped */
    model_replay_last(&M);
    CHECK(!recv_eth(&ef, rx, sizeof rx) && S.rx_replay_dropped == 1 && S.st == WIFI_STA_RUN);
    /* an unprotected data frame from the AP on this link is dropped by the station */
    uint8_t f[64] = {0x08, 0x02};
    memcpy(f + 4, S.mac, 6); memcpy(f + 10, AP_SHA.bssid, 6); memcpy(f + 16, AP_SHA.mac, 6);
    static const uint8_t snap[8] = {0xaa, 0xaa, 0x03, 0, 0, 0, 0x08, 0x06};
    memcpy(f + 24, snap, 8);
    wifi_sta_rx(&S, f, 60, 0, -30);
    CHECK(S.rx_unprotected_dropped == 1);
    /* key messages forged from behind the AP, or in the clear after the PTK: dropped, link stays up */
    uint32_t refused0 = S.eapol_refused;
    M.eapol_protect = 1;
    M.eapol_from_gw = 1;
    model_ap_send_m1(&M);
    CHECK(mt7925_poll_rings(&d) >= 0 && S.eapol_refused == refused0 + 1u && S.st == WIFI_STA_RUN);
    M.eapol_protect = 0;
    M.eapol_from_gw = 0;
    model_ap_send_m1(&M);
    CHECK(mt7925_poll_rings(&d) >= 0 && S.eapol_refused == refused0 + 2u && S.rx_unprotected_dropped == 2);
    CHECK(S.st == WIFI_STA_RUN && S.ptk_set && M.deauth_rx == 0);
    /* a real PTK rekey (protected message 1 from the AP) is not supported: the station leaves */
    M.eapol_protect = 1;
    model_ap_send_m1(&M);
    M.eapol_protect = 0;
    CHECK(mt7925_poll_rings(&d) >= 0 && S.st == WIFI_STA_IDLE && !S.ptk_set && S.wpa.st == WPA_ST_IDLE);
    CHECK(M.deauth_rx == 1 && M.last_deauth_reason == WIFI_REASON_UNSPECIFIED && M.assoc_ap == -1);
    CHECK(wifi_sta_send_eth(&S, eth, (uint32_t)n) == WIFI_E_STATE);
    CHECK(wifi_sta_disconnect(&S) == WIFI_OK && S.ptk_set == 0 && S.wpa.st == WPA_ST_IDLE && M.ap_wpa == 0);

    /* wrong PSK: the AP refuses message 2, nothing installed, no data port */
    CHECK(station_up(&d, 0) == MT7925_OK);
    model_set_pmk(&M, pmk);
    wifi_sta_set_psk(&S, bad_pmk, &toy);
    CHECK(wifi_sta_connect(&S, (const uint8_t *)"aiensha", 7, 50000) == WIFI_E_TIMEOUT);
    /* the station told the AP the handshake timed out (reason 15), so the AP dropped its state */
    CHECK(M.eapol_bad == 1 && M.ap_wpa == 0 && M.keys == 0 && S.st == WIFI_STA_IDLE);
    CHECK(M.deauth_rx == 1 && M.last_deauth_reason == WIFI_REASON_4WAY_TIMEOUT);
    /* AP-side attacks on message 3 and a silent AP */
    static const uint32_t faults[3] = {MODEL_F_M3_BAD_MIC, MODEL_F_M3_DOWNGRADE, MODEL_F_NO_M1};
    for (int i = 0; i < 3; i++) {
        CHECK(station_up(&d, faults[i]) == MT7925_OK);
        model_set_pmk(&M, pmk);
        wifi_sta_set_psk(&S, pmk, &toy);
        CHECK(wifi_sta_connect(&S, (const uint8_t *)"aiensha", 7, 50000) == WIFI_E_TIMEOUT);
        CHECK(M.keys == 0 && S.st == WIFI_STA_IDLE && !S.ptk_set && S.eapol_refused == (i < 2 ? 1u : 0u));
        CHECK(M.deauth_rx == 1 && M.last_deauth_reason == WIFI_REASON_4WAY_TIMEOUT);
        CHECK(wifi_sta_send_eth(&S, eth, (uint32_t)n) == WIFI_E_STATE);
    }

    /* a chip that reports no packet number: protected data is dropped, never trusted */
    CHECK(station_up(&d, MODEL_F_NO_PN) == MT7925_OK);
    model_set_pmk(&M, pmk);
    wifi_sta_set_psk(&S, pmk, &toy);
    CHECK(wifi_sta_connect(&S, (const uint8_t *)"aiensha", 7, 200000) == WIFI_OK && S.st == WIFI_STA_RUN);
    memcpy(out.source.b, S.mac, 6);
    CHECK(net_eth_build(&out, eth, sizeof eth, &n) == NET_OK);
    CHECK(wifi_sta_send_eth(&S, eth, (uint32_t)n) == WIFI_OK && M.arp_replies == 1);
    CHECK(!recv_eth(&ef, rx, sizeof rx) && S.rx_replay_dropped == 1);
    /* protected management frames are on (AKM 6, MFPC): an unprotected broadcast deauth (needs BIP, not
     * implemented) and an unprotected unicast one are both refused; the link stays up */
    uint8_t bd[26] = {0xc0, 0x00};
    memset(bd + 4, 0xff, 6);
    memcpy(bd + 10, AP_SHA.bssid, 6);
    memcpy(bd + 16, AP_SHA.bssid, 6);
    bd[24] = 7;
    wifi_sta_rx(&S, bd, sizeof bd, 0, -30);
    CHECK(S.st == WIFI_STA_RUN && S.deauths == 0 && S.ptk_set && S.mgmt_unprotected_dropped == 1);
    uint8_t ud[26];
    memcpy(ud, bd, sizeof ud);
    memcpy(ud + 4, S.mac, 6);
    wifi_sta_rx(&S, ud, sizeof ud, 0, -30);
    CHECK(S.st == WIFI_STA_RUN && S.deauths == 0 && S.mgmt_unprotected_dropped == 2);
    /* decrypted under the PTK but with a packet number not above the last one (a replay): refused;
     * with a fresh packet number it ends the link */
    wifi_sta_rx_pn(&S, ud, sizeof ud, 1, 1, 0, -30);
    CHECK(S.st == WIFI_STA_RUN && S.deauths == 0 && S.mgmt_unprotected_dropped == 3);
    wifi_sta_rx_pn(&S, ud, sizeof ud, 1, 1, 5, -30);
    CHECK(S.st == WIFI_STA_IDLE && S.deauths == 1 && !S.ptk_set && S.wpa.st == WPA_ST_IDLE);
    /* ... but not from another BSSID */
    CHECK(station_up(&d, 0) == MT7925_OK);
    model_set_pmk(&M, pmk);
    wifi_sta_set_psk(&S, pmk, &toy);
    CHECK(wifi_sta_connect(&S, (const uint8_t *)"aiensha", 7, 200000) == WIFI_OK);
    bd[10] ^= 1;
    wifi_sta_rx(&S, bd, sizeof bd, 0, -30);
    CHECK(S.st == WIFI_STA_RUN && S.deauths == 0);

    /* a deauthentication delivered while message 4 is being sent (the TX
     * polled the chip): nothing the wiped supplicant derived is installed */
    CHECK(station_up(&d, 0) == MT7925_OK);
    model_set_pmk(&M, pmk);
    wifi_sta_set_psk(&S, pmk, &toy);
    ORIG_TX = S.r.tx;
    S.r.tx = tx_deauth_on_m4;
    CHECK(wifi_sta_connect(&S, (const uint8_t *)"aiensha", 7, 200000) == WIFI_E_REFUSED);
    CHECK(M4_SEEN == 1 && M.keys == 0 && !S.ptk_set && S.st == WIFI_STA_IDLE && S.eapol_refused == 1);
    S.r.tx = ORIG_TX;
}

static uint8_t *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = n > 0 ? malloc((size_t)n) : NULL;
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    *len = (size_t)n;
    return b;
}

static int real(const char *pp, const char *rp) {
    size_t pl, rl;
    uint8_t *p = slurp(pp, &pl), *r = slurp(rp, &rl);
    if (!p || !r) { printf("MT7925_MODEL_REAL_FW: FAIL (cannot read files)\n"); return 1; }
    mt7925_dev d;
    CHECK(bring_up(&d, 0) == MT7925_OK);
    mt7925_err e = mt7925_fw_download(&d, p, pl, r, rl);
    printf("real firmware through the model: %s, %u downloads, %u scatter bytes, %u MCU commands, start option 0x%x addr 0x%08x\n",
           mt7925_strerror(e), M.dl_count, M.scatter_bytes, M.mcu_cmds, M.start_option, M.start_addr);
    CHECK(e == MT7925_OK && M.mcu_bad == 0 && M.dma_faults == 0);
    if (e == MT7925_OK) check_download(p, pl, r, rl, 0);
    free(p);
    free(r);
    printf("MT7925_MODEL_REAL_FW: %s (%d checks, %d failures)\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}

int main(int argc, char **argv) {
    if (argc == 4 && strcmp(argv[1], "real") == 0)
        return real(argv[2], argv[3]);
    test_identity();
    test_remap();
    test_bring_up();
    test_bring_up_faults();
    test_mcu();
    test_fw_download();
    test_scan();
    test_hostile_rx();
    PL = make_patch(PB);
    RL = make_ram(RB);
    test_txd();
    test_tx_path();
    test_rx_parse();
    test_rx_nested();
    test_join_builders();
    test_join_model();
    test_station();
    test_station_wpa();
    printf("MT7925_DRIVER_TESTS: %s (%d checks, %d failures)\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
