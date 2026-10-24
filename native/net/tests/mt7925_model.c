/* MT7925 chip model for the host suite. See mt7925_model.h for what a PASS
 * against it does and does not prove. Host libc only. */
#include "mt7925_model.h"

#include <string.h>

#include "../../argus/sha256.h"
#include "wpa_toy.h"

#define RING_TX(q) (MT7925_TX_RING_BASE + (q) * 0x10u)
#define RING_RX(q) (MT7925_RX_RING_BASE + (q) * 0x10u)
#define BAR(m, off) ((m)->bar[(off) / 4u])


static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

static void trace(mt7925_model *m, uint32_t op, uint32_t addr, uint32_t val) {
    if (m->n_trace < MODEL_TRACE_MAX)
        m->trace[m->n_trace++] = (model_trace){op, addr, val};
}

int model_trace_find(const mt7925_model *m, uint32_t op, uint32_t addr, int from) {
    for (uint32_t i = from < 0 ? 0 : (uint32_t)from; i < m->n_trace; i++)
        if (m->trace[i].op == op && m->trace[i].addr == addr)
            return (int)i;
    return -1;
}

uint32_t model_chipreg(const mt7925_model *m, uint32_t addr) {
    for (uint32_t i = 0; i < m->n_chipreg; i++)
        if (m->chipreg[i].addr == addr)
            return m->chipreg[i].val;
    return 0;
}

static void chipreg_set(mt7925_model *m, uint32_t addr, uint32_t v) {
    for (uint32_t i = 0; i < m->n_chipreg; i++)
        if (m->chipreg[i].addr == addr) { m->chipreg[i].val = v; return; }
    if (m->n_chipreg < MODEL_CHIPREG_MAX)
        m->chipreg[m->n_chipreg++] = (typeof(m->chipreg[0])){addr, v};
}

/* Bus address -> host pointer, refusing anything outside the window. */
static uint8_t *bus(mt7925_model *m, uint64_t addr, uint64_t len) {
    if (!m->mem || addr < m->dma || addr - m->dma > m->mem_len || len > m->mem_len - (addr - m->dma)) {
        m->dma_faults++;
        m->last_fault_addr = addr;
        return NULL;
    }
    return m->mem + (addr - m->dma);
}

static uint32_t ring_n(const mt7925_model *m, uint32_t regs) { return BAR(m, regs + MT7925_RING_SIZE); }

/* Write one buffer into the next posted RX slot of ring `regs`. 1 = delivered. */
static int rx_deliver(mt7925_model *m, uint32_t regs, const uint8_t *buf, uint32_t len) {
    uint32_t n = ring_n(m, regs), didx = BAR(m, regs + MT7925_RING_DMA_IDX), cidx = BAR(m, regs + MT7925_RING_CPU_IDX);
    if (!n || didx == cidx)
        return 0; /* no posted buffer */
    uint8_t *d = bus(m, (uint64_t)BAR(m, regs + MT7925_RING_DESC_BASE) + (uint64_t)didx * 16u, 16);
    if (!d) return 0;
    uint32_t buf0 = le32(d), ctrl = le32(d + 4);
    uint32_t cap = (ctrl & MT7925_DESC_SD_LEN0_MASK) >> MT7925_DESC_SD_LEN0_SHIFT;
    if (len > cap) return 0; /* a real chip would split; the model refuses */
    uint8_t *b = bus(m, buf0, len);
    if (!b) return 0;
    memcpy(b, buf, len);
    uint32_t claimed = (m->faults & MODEL_F_RX_OVERLONG) ? cap + 16u : len;
    put32(d + 4, MT7925_DESC_DMA_DONE | MT7925_DESC_LAST_SEC0 | ((claimed << MT7925_DESC_SD_LEN0_SHIFT) & MT7925_DESC_SD_LEN0_MASK));
    BAR(m, regs + MT7925_RING_DMA_IDX) = (didx + 1u) % n;
    return 1;
}


static void flush_queues(mt7925_model *m) {
    while (m->evq_n && rx_deliver(m, RING_RX(MT7925_RXQ_EVENT), m->evq[0], m->evq_len[0])) {
        memmove(m->evq, m->evq + 1, (m->evq_n - 1) * sizeof(m->evq[0]));
        memmove(m->evq_len, m->evq_len + 1, (m->evq_n - 1) * sizeof(m->evq_len[0]));
        m->evq_n--;
    }
    while (m->rxq_n && rx_deliver(m, RING_RX(MT7925_RXQ_DATA), m->rxq[0], m->rxq_len[0])) {
        memmove(m->rxq, m->rxq + 1, (m->rxq_n - 1) * sizeof(m->rxq[0]));
        memmove(m->rxq_len, m->rxq_len + 1, (m->rxq_n - 1) * sizeof(m->rxq_len[0]));
        m->rxq_n--;
    }
}

void model_queue_evt_raw(mt7925_model *m, const uint8_t *buf, uint32_t len) {
    if (m->evq_n < 16 && len <= MODEL_FRAME_MAX) {
        memcpy(m->evq[m->evq_n], buf, len);
        m->evq_len[m->evq_n++] = len;
    }
}

void model_queue_rx(mt7925_model *m, uint32_t type, const uint8_t *buf, uint32_t len) {
    (void)type;
    if (m->rxq_n < 8 && len <= MODEL_FRAME_MAX) {
        memcpy(m->rxq[m->rxq_n], buf, len);
        m->rxq_len[m->rxq_n++] = len;
    }
    flush_queues(m);
}

/* Build and queue an MCU event. */
static void event(mt7925_model *m, uint8_t eid, uint8_t seq, uint8_t option, uint8_t status8, int uni, uint8_t cid,
                  uint32_t status32, const uint8_t *body, uint32_t body_len) {
    uint8_t e[MODEL_FRAME_MAX];
    uint32_t len = MT7925_EVT_HDR_BYTES + (uni ? 8u : 4u) + body_len;
    if (m->evq_n >= 16 || len > sizeof(e)) return;
    memset(e, 0, len);
    put32(e, (len & MT7925_RXD0_LEN_MASK) | (MT7925_PKT_TYPE_RX_EVENT << MT7925_RXD0_PKT_TYPE_SHIFT));
    put16(e + MT7925_EVT_LEN_OFF, (uint16_t)(len - MT7925_RXD_BYTES));
    put16(e + 34, MT7925_MCU_PKT_ID);
    e[MT7925_EVT_EID_OFF] = eid;
    e[MT7925_EVT_SEQ_OFF] = seq;
    e[MT7925_EVT_OPTION_OFF] = option;
    e[MT7925_EVT_STATUS8_OFF] = status8;
    if (uni) {
        e[MT7925_EVT_UNI_CID_OFF] = cid;
        put32(e + MT7925_EVT_UNI_STATUS_OFF, status32);
        if (body_len) memcpy(e + MT7925_EVT_HDR_BYTES + 8, body, body_len);
    } else if (body_len) {
        memcpy(e + MT7925_EVT_HDR_BYTES + 4, body, body_len);
    }
    memcpy(m->evq[m->evq_n], e, len);
    m->evq_len[m->evq_n++] = len;
}

static void reply(mt7925_model *m, uint8_t eid, uint8_t seq, uint8_t status8) {
    if (m->faults & MODEL_F_MCU_SILENT) return;
    if (m->faults & MODEL_F_WRONG_SEQ_FIRST)
        event(m, eid, (uint8_t)((seq + 7u) & 0xfu), 0, 0xee, 0, 0, 0, 0, 0);
    event(m, eid, seq, 0, status8, 0, 0, 0, 0, 0);
}

/* Beacon of AP `a` as the chip would hand it up: an RXD in front of the frame. */
void model_emit_beacon(mt7925_model *m, const model_ap *a);

/* Event whose payload starts right after the 44-byte header (query replies,
 * ROC grants), as the reference reads them. */
static void event_raw(mt7925_model *m, uint8_t eid, uint8_t seq, uint8_t option, const uint8_t *body, uint32_t bl) {
    uint32_t len = MT7925_EVT_HDR_BYTES + bl;
    if (m->evq_n >= 16 || len > MODEL_FRAME_MAX) return;
    uint8_t *e = m->evq[m->evq_n];
    memset(e, 0, len);
    put32(e, (len & MT7925_RXD0_LEN_MASK) | (MT7925_PKT_TYPE_RX_EVENT << MT7925_RXD0_PKT_TYPE_SHIFT));
    put16(e + MT7925_EVT_LEN_OFF, (uint16_t)(len - MT7925_RXD_BYTES));
    put16(e + 34, MT7925_MCU_PKT_ID);
    e[MT7925_EVT_EID_OFF] = eid;
    e[MT7925_EVT_SEQ_OFF] = seq;
    e[MT7925_EVT_OPTION_OFF] = option;
    memcpy(e + MT7925_EVT_HDR_BYTES, body, bl);
    m->evq_len[m->evq_n++] = len;
}

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

/* Walk a BSS_INFO / STA_REC TLV list after a `hl`-byte header whose bytes
 * 2..3 count the TLVs. Calls `fn` per TLV; returns 0 and counts uni_bad if
 * the count or lengths do not add up. */
static int tlv_walk(mt7925_model *m, const uint8_t *p, uint32_t pl, uint32_t hl, int counted,
                    void (*fn)(mt7925_model *, uint16_t, const uint8_t *, uint32_t)) {
    uint32_t n = 0, off = hl;
    if (pl < hl) { m->uni_bad++; return 0; }
    while (off < pl) {
        if (pl - off < 4u) { m->uni_bad++; return 0; }
        uint32_t tl = le16(p + off + 2);
        if (tl < 4u || tl > pl - off) { m->uni_bad++; return 0; }
        fn(m, le16(p + off), p + off, tl);
        off += tl;
        n++;
    }
    if (counted && n != le16(p + 2)) { m->uni_bad++; return 0; }
    return 1;
}

static void bss_tlv(mt7925_model *m, uint16_t tag, const uint8_t *t, uint32_t tl) {
    if (tag == 0 && tl == 32) { m->bss_conn_state = t[12]; memcpy(m->bss_bssid, t + 14, 6); }
    else if (tag == 16 && tl == 8) m->bss_cipher = t[6];
    else if (tag == 2 && tl == 16) m->bss_has_rlm = 1;
    else if (!(tag == 11 && tl == 16) && !(tag == 15 && tl == 8) && !(tag == 26 && tl == 20) &&
             !(tag == 23 && tl == 20) && !(tag == 6 && tl == 8))
        m->uni_bad++; /* a TLV this reading does not know, or a wrong size */
}

static void sta_tlv(mt7925_model *m, uint16_t tag, const uint8_t *t, uint32_t tl) {
    if (tag == 0x00 && tl == 20) memcpy(m->sta_peer, t + 12, 6);
    else if (tag == 0x07 && tl == 16) m->sta_state = t[4];
    else if (tag == 0x27 && tl == 68) {
        if (t[4] != 1 || t[15] != MT7925_CIPHER_CCMP || t[17] != 16) { m->uni_bad++; return; }
        if (t[6]) { memcpy(m->key_pairwise, t + 20, 16); m->keys |= 1; }
        else { memcpy(m->key_group, t + 20, 16); m->keys |= 2; }
    } else if (!(tag == 0x15 && tl == 12) && !(tag == 0x01 && tl == 16) && !(tag == 0x20 && tl == 28) &&
               !(tag == 0x2b && tl == 8))
        m->uni_bad++;
}

static void uni_cmd(mt7925_model *m, uint16_t cid, uint8_t seq, const uint8_t *p, uint32_t pl) {
    uint32_t status = 0;
    int silent = (m->faults & MODEL_F_MCU_SILENT) != 0;
    switch (cid) {
    case MT7925_UNI_CMD_SCAN_REQ: {
        m->scans++;
        if (!silent) event(m, 0x01, seq, 0x02, 0, 1, (uint8_t)cid, 0, 0, 0);
        for (uint32_t i = 0; i < m->n_ap; i++)
            model_emit_beacon(m, &m->ap[i]);
        uint8_t done[4] = {pl ? p[0] : 0, 0, 0, 0};
        event(m, MT7925_UNI_EVENT_SCAN_DONE, 0, 0x02 | MT7925_EVT_OPT_UNSOLICITED, 0, 1, 0, 0, done, 4);
        return;
    }
    case 0x01: /* DEV_INFO_UPDATE: header + one 12-byte ACTIVE TLV */
        if (pl != 16 || le16(p + 4) != 0 || le16(p + 6) != 12) m->uni_bad++;
        else { m->dev_active = p[8]; memcpy(m->own_mac, p + 10, 6); }
        break;
    case 0x02: /* BSS_INFO_UPDATE */
        m->bss_records++;
        if (pl == 36 && le16(p + 2) == 0) m->bss_conn_state = p[16]; /* own-interface form, fixed struct */
        else tlv_walk(m, p, pl, 4, 1, bss_tlv);
        break;
    case 0x03: /* STA_REC_UPDATE */
        m->sta_records++;
        if (pl >= 8) m->sta_wcid = (uint16_t)(p[1] | ((p[6] & 7u) << 8));
        if (pl < 8 || p[4] != 1) m->uni_bad++;
        else tlv_walk(m, p, pl, 8, 1, sta_tlv);
        if (m->faults & MODEL_F_STA_FAIL) status = 0xc0000001u;
        break;
    case 0x0e: { /* CHIP_CONFIG query: NIC capability with the efuse MAC */
        uint8_t r[4 + 8 + 12] = {2, 0, 0, 0};
        put16(r + 4, 0); put16(r + 6, 8); /* TX_RESOURCE, 4 bytes of data */
        put16(r + 12, MT7925_NIC_CAP_MAC_ADDR); put16(r + 14, 12);
        memcpy(r + 16, m->efuse_mac, 6);
        if (pl != 8 || le16(p + 4) != MT7925_NIC_CAP_TAG) m->uni_bad++;
        if (!silent) event_raw(m, 0x0e, seq, 0x02, r, sizeof r);
        return;
    }
    case 0x27: { /* ROC */
        if (!silent) event(m, 0x01, seq, 0x02, 0, 1, (uint8_t)cid, 0, 0, 0);
        if (pl == 28 && le16(p + 4) == 0 && le16(p + 6) == 24) {
            m->roc_token = p[9];
            m->roc_channel = p[10];
            if (!(m->faults & MODEL_F_ROC_SILENT)) {
                uint8_t g[4 + 20] = {0};
                put16(g + 4, 0); put16(g + 6, 20);
                g[8] = 0; g[9] = m->roc_token; g[10] = 0; g[11] = m->roc_channel;
                put32(g + 20, le32(p + 20));
                m->roc_active = 1;
                event_raw(m, MT7925_UNI_EVENT_ROC, 0, 0x02 | MT7925_EVT_OPT_UNSOLICITED, g, sizeof g);
            }
        } else if (pl == 16 && le16(p + 4) == 1 && le16(p + 6) == 12) {
            m->roc_active = 0;
        } else {
            m->uni_bad++;
        }
        return;
    }
    default:
        break;
    }
    if (!silent) event(m, 0x01, seq, 0x02, 0, 1, (uint8_t)cid, status, 0, 0);
}

static void mcu_cmd(mt7925_model *m, const uint8_t *b, uint32_t len) {
    m->mcu_cmds++;
    if (len < MT7925_TXD_BYTES + MT7925_MCU_HDR_UNI) { m->mcu_bad++; return; }
    uint32_t txd0 = le32(b), txd1 = le32(b + 4);
    const uint8_t *h = b + MT7925_TXD_BYTES;
    if ((txd0 & 0xffffu) != len || ((txd0 >> 23) & 3u) != MT7925_TX_TYPE_CMD || (txd0 >> 25) != MT7925_MCU_Q_IDX ||
        /* connac3: HDR_FORMAT at bits 15:14 and nothing in the connac2 position 17:16 */
        txd1 != (MT7925_HDR_FORMAT_CMD << MT7925_TXD1_HDR_FORMAT_SHIFT) || h[5] != MT7925_MCU_PKT_ID ||
        (uint32_t)(h[0] | (h[1] << 8)) != len - MT7925_TXD_BYTES) {
        m->mcu_bad++;
        return;
    }
    int uni = (h[11] & 2u) != 0;
    uint8_t seq = h[7];
    uint32_t hl = MT7925_TXD_BYTES + (uni ? MT7925_MCU_HDR_UNI : MT7925_MCU_HDR_LEGACY);
    if (len < hl || seq == 0 || seq > 15) { m->mcu_bad++; return; }
    const uint8_t *p = b + hl;
    uint32_t pl = len - hl;
    m->last_seq = seq;
    m->last_uni = (uint8_t)uni;
    m->last_payload_len = pl < sizeof(m->last_payload) ? pl : (uint32_t)sizeof(m->last_payload);
    memcpy(m->last_payload, p, m->last_payload_len);
    if (uni) {
        uint16_t cid = (uint16_t)(h[2] | (h[3] << 8));
        m->last_uni_cid = cid;
        /* mt7925/mcu.c mt7925_mcu_fill_message: HIF_CTRL and CHIP_CONFIG go without the ACK bit; a set is
         * EXT_ACK (7) and a query QUERY_ACK (3) otherwise. */
        uint8_t want = (h[11] & 4u) ? MT7925_UNI_OPT_SET : MT7925_UNI_OPT_QUERY;
        if (cid == MT7925_UNI_CMD_HIF_CTRL || cid == MT7925_UNI_CMD_CHIP_CONFIG) want &= (uint8_t)~MT7925_UNI_OPT_ACK;
        if (h[11] != want) { m->mcu_bad++; return; }
        uni_cmd(m, cid, seq, p, pl);
        return;
    }
    uint8_t cid = h[4];
    m->last_cmd = cid;
    if ((uint32_t)(h[2] | (h[3] << 8)) != MT7925_MCU_PQ_ID || h[6] != MT7925_MCU_Q_NA) { m->mcu_bad++; return; }
    switch (cid) {
    case MT7925_MCU_CMD_NIC_POWER_CTRL:
        m->restarted = 1;
        BAR(m, 0xe00f0u) = MT7925_MISC_FW_PWR_ON; /* CONN_ON_MISC via fixed map */
        break; /* no reply: the driver does not wait */
    case MT7925_MCU_CMD_PATCH_SEM_CONTROL: {
        uint32_t op = pl >= 4 ? le32(p) : 99;
        uint8_t st;
        if (op == MT7925_PATCH_SEM_GET) {
            st = (m->faults & MODEL_F_SEM_BUSY) ? MT7925_PATCH_NOT_DL_SEM_FAIL
                 : (m->faults & MODEL_F_PATCH_LOADED) ? MT7925_PATCH_IS_DL : MT7925_PATCH_NOT_DL_SEM_SUCCESS;
            m->sem_held = st == MT7925_PATCH_NOT_DL_SEM_SUCCESS;
        } else if (op == MT7925_PATCH_SEM_RELEASE && m->sem_held) {
            st = MT7925_PATCH_REL_SEM_SUCCESS;
            m->sem_held = 0;
        } else {
            st = 0xff;
            m->mcu_bad++;
        }
        reply(m, cid, seq, st);
        break;
    }
    case MT7925_MCU_CMD_PATCH_START_REQ:
    case MT7925_MCU_CMD_TARGET_ADDRESS_LEN_REQ:
        if (pl != 12 || m->dl_got != m->dl_len || !m->restarted) { m->mcu_bad++; reply(m, cid, seq, 0xff); break; }
        m->dl_cmd = cid;
        m->dl_addr = le32(p);
        m->dl_len = le32(p + 4);
        m->dl_mode = le32(p + 8);
        m->dl_got = 0;
        if (m->dl_count < 32) {
            m->dl_log_addr[m->dl_count] = m->dl_addr;
            m->dl_log_len[m->dl_count] = m->dl_len;
            m->dl_log_mode[m->dl_count] = m->dl_mode;
        }
        sha256_init(&m->dl_sha);
        if (mt7925_fw_dl_cmd(m->dl_addr) != cid) m->mcu_bad++;
        reply(m, cid, seq, 0);
        break;
    case MT7925_MCU_CMD_PATCH_FINISH_REQ:
        if (!m->sem_held || m->dl_got != m->dl_len) m->mcu_bad++;
        m->patch_done = 1;
        reply(m, cid, seq, (m->faults & MODEL_F_FINISH_FAIL) ? 1 : 0);
        break;
    case MT7925_MCU_CMD_FW_START_REQ:
        if (pl != 8 || m->dl_got != m->dl_len || m->sem_held) m->mcu_bad++;
        m->start_option = le32(p);
        m->start_addr = le32(p + 4);
        m->fw_started = 1;
        if (!(m->faults & MODEL_F_NO_N9_READY))
            BAR(m, 0xe00f0u) = MT7925_MISC_FW_N9_RDY;
        reply(m, cid, seq, 0);
        break;
    default:
        m->mcu_bad++;
        reply(m, cid, seq, 0);
    }
}

static void fwdl(mt7925_model *m, const uint8_t *b, uint32_t len) {
    m->scatter_bytes += len;
    if ((uint64_t)m->dl_got + len > m->dl_len) { m->mcu_bad++; return; }
    sha256_update(&m->dl_sha, b, len);
    m->dl_got += len;
    if (m->dl_got == m->dl_len) {
        if (m->dl_count < 32) sha256_final(&m->dl_sha, m->dl_log_digest[m->dl_count]);
        m->dl_count++;
    }
}

void model_radio_tx(mt7925_model *m, const uint8_t *b, uint32_t len);
static void data_tx(mt7925_model *m, const uint8_t *b, uint32_t len);

static void tx_ring_kick(mt7925_model *m, uint32_t q) {
    uint32_t regs = RING_TX(q), n = ring_n(m, regs);
    if (!n) return;
    uint32_t cidx = BAR(m, regs + MT7925_RING_CPU_IDX) % n;
    uint32_t didx = BAR(m, regs + MT7925_RING_DMA_IDX) % n;
    while (didx != cidx) {
        uint8_t *d = bus(m, (uint64_t)BAR(m, regs + MT7925_RING_DESC_BASE) + (uint64_t)didx * 16u, 16);
        if (!d) break;
        uint32_t ctrl = le32(d + 4), len = (ctrl & MT7925_DESC_SD_LEN0_MASK) >> MT7925_DESC_SD_LEN0_SHIFT;
        uint8_t *b = bus(m, le32(d), len);
        if (b && (ctrl & MT7925_DESC_LAST_SEC0)) {
            if (q == MT7925_TXQ_MCU) mcu_cmd(m, b, len);
            else if (q == MT7925_TXQ_FWDL) fwdl(m, b, len);
            else data_tx(m, b, len);
        } else {
            m->mcu_bad++;
        }
        put32(d + 4, ctrl | MT7925_DESC_DMA_DONE);
        didx = (didx + 1u) % n;
    }
    BAR(m, regs + MT7925_RING_DMA_IDX) = (m->faults & MODEL_F_BAD_DIDX) ? (didx + 3u) % n : didx;
    flush_queues(m);
}

/* ---- register file ---- */

static uint32_t chip_read(mt7925_model *m, uint32_t addr) {
    if (addr == MT7925_HW_CHIPID) return 0x7925u;
    if (addr == MT7925_HW_REV) return 0x0000u; /* UNKNOWN on the Spark; read in an attended boot */
    return model_chipreg(m, addr);
}

static uint32_t rd(void *ctx, uint32_t off) {
    mt7925_model *m = ctx;
    uint32_t v;
    if (off >= MT7925_BAR0_BYTES || (off & 3u)) { m->mcu_bad++; return 0xffffffffu; }
    if (off >= MT7925_HIF_REMAP_BASE_L1 && off < MT7925_HIF_REMAP_BASE_L1 + 0x10000u) {
        uint32_t l1 = BAR(m, MT7925_HIF_REMAP_L1) & 0xffff0000u, addr;
        addr = l1 == (MT7925_HIF_REMAP_BASE_L2 & 0xffff0000u) ? BAR(m, MT7925_HIF_REMAP_L2) + (off - MT7925_HIF_REMAP_BASE_L1)
                                                                : l1 | (off - MT7925_HIF_REMAP_BASE_L1);
        v = chip_read(m, addr);
        trace(m, 'R', addr, v);
        return v;
    }
    v = BAR(m, off);
    if (m->faults & MODEL_F_RING_IDX_BAD && off == RING_TX(MT7925_TXQ_DATA) + MT7925_RING_DMA_IDX)
        v = ring_n(m, RING_TX(MT7925_TXQ_DATA)) + 5u;
    trace(m, 'R', off, v);
    return v;
}

static void wr(void *ctx, uint32_t off, uint32_t v) {
    mt7925_model *m = ctx;
    if (off >= MT7925_BAR0_BYTES || (off & 3u)) { m->mcu_bad++; return; }
    if (off >= MT7925_HIF_REMAP_BASE_L1 && off < MT7925_HIF_REMAP_BASE_L1 + 0x10000u) {
        uint32_t l1 = BAR(m, MT7925_HIF_REMAP_L1) & 0xffff0000u, addr;
        addr = l1 == (MT7925_HIF_REMAP_BASE_L2 & 0xffff0000u) ? BAR(m, MT7925_HIF_REMAP_L2) + (off - MT7925_HIF_REMAP_BASE_L1)
                                                                : l1 | (off - MT7925_HIF_REMAP_BASE_L1);
        trace(m, 'W', addr, v);
        chipreg_set(m, addr, v);
        return;
    }
    trace(m, 'W', off, v);
    switch (off) {
    case 0xe0010u: /* CONN_ON_LPCTL */
        if ((v & MT7925_LPCR_HOST_SET_OWN) && !(m->faults & MODEL_F_OWN_STUCK))
            BAR(m, off) |= MT7925_LPCR_HOST_OWN_SYNC;
        if ((v & MT7925_LPCR_HOST_CLR_OWN) && !(m->faults & MODEL_F_DRV_OWN_STUCK))
            BAR(m, off) &= ~MT7925_LPCR_HOST_OWN_SYNC;
        return;
    case 0xf0140u: /* WFSYS_SW_RST */
        if (!(v & MT7925_WFSYS_SW_RST_B)) {
            BAR(m, off) = v & ~MT7925_WFSYS_SW_INIT_DONE;
            for (uint32_t q = 0; q < 17; q++) BAR(m, RING_TX(q) + MT7925_RING_DMA_IDX) = 0;
        } else {
            BAR(m, off) = v | ((m->faults & MODEL_F_RESET_STUCK) ? 0 : MT7925_WFSYS_SW_INIT_DONE);
        }
        return;
    case MT7925_RST_DTX_PTR:
        for (uint32_t q = 0; q < 17; q++) { BAR(m, RING_TX(q) + MT7925_RING_DMA_IDX) = 0; BAR(m, RING_TX(q) + MT7925_RING_CPU_IDX) = 0; }
        return;
    case MT7925_RST_DRX_PTR:
        for (uint32_t q = 0; q < 4; q++) BAR(m, RING_RX(q) + MT7925_RING_DMA_IDX) = 0;
        return;
    default:
        break;
    }
    BAR(m, off) = v;
    if (off == RING_TX(MT7925_TXQ_DATA) + MT7925_RING_CPU_IDX) tx_ring_kick(m, MT7925_TXQ_DATA);
    else if (off == RING_TX(MT7925_TXQ_MCU) + MT7925_RING_CPU_IDX) tx_ring_kick(m, MT7925_TXQ_MCU);
    else if (off == RING_TX(MT7925_TXQ_FWDL) + MT7925_RING_CPU_IDX) tx_ring_kick(m, MT7925_TXQ_FWDL);
    else if (off == RING_RX(MT7925_RXQ_EVENT) + MT7925_RING_CPU_IDX || off == RING_RX(MT7925_RXQ_DATA) + MT7925_RING_CPU_IDX)
        flush_queues(m);
}

static void udelay(void *ctx, uint32_t us) {
    mt7925_model *m = ctx;
    m->time_us += us;
    flush_queues(m);
}

static void barrier(void *ctx) { (void)ctx; }

void model_init(mt7925_model *m, uint32_t faults) {
    memset(m, 0, sizeof(*m));
    m->faults = faults;
    m->evq_n = 0;
    m->sta_mac[0] = 0x02; /* model MAC; the real one comes from the chip's efuse (UNKNOWN path) */
    m->sta_mac[5] = 0x25;
    memcpy(m->efuse_mac, m->sta_mac, 6);
    m->assoc_ap = -1;
}

void model_attach_dma(mt7925_model *m, uint8_t *mem, uint64_t dma, size_t len) {
    m->mem = mem;
    m->dma = dma;
    m->mem_len = len;
}

mt7925_ops model_ops(mt7925_model *m) { return (mt7925_ops){m, rd, wr, udelay, barrier}; }

void model_add_ap(mt7925_model *m, const model_ap *ap) {
    if (m->n_ap < MODEL_AP_MAX) m->ap[m->n_ap++] = *ap;
}

/* ---- radio ---- */

/* Beacon (or probe response) from AP `a`, as an RX NORMAL buffer: a 32-byte
 * RXD with no optional groups, then the 802.11 frame. The real chip's RXD
 * groups for a beacon are UNKNOWN; tests also feed group-bearing RXDs. */
static uint32_t build_beacon(const model_ap *a, uint8_t *f, int probe_resp, const uint8_t *da) {
    uint32_t n = 0;
    f[n++] = probe_resp ? 0x50 : 0x80; f[n++] = 0;          /* frame control */
    f[n++] = 0; f[n++] = 0;                                  /* duration */
    for (int i = 0; i < 6; i++) f[n++] = da ? da[i] : 0xff;  /* addr1 */
    for (int i = 0; i < 6; i++) f[n++] = a->bssid[i];        /* addr2 */
    for (int i = 0; i < 6; i++) f[n++] = a->bssid[i];        /* addr3 */
    f[n++] = 0x10; f[n++] = 0;                               /* seq ctrl */
    for (int i = 0; i < 8; i++) f[n++] = (uint8_t)(i + 1);   /* timestamp */
    f[n++] = 100; f[n++] = 0;                                /* beacon interval */
    f[n++] = (uint8_t)(0x01 | (a->rsn_len ? 0x10 : 0)); f[n++] = 0x04; /* ESS, privacy, short slot */
    f[n++] = 0; f[n++] = a->ssid_len; memcpy(f + n, a->ssid, a->ssid_len); n += a->ssid_len;
    static const uint8_t rates[] = {0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24};
    f[n++] = 1; f[n++] = sizeof(rates); memcpy(f + n, rates, sizeof(rates)); n += sizeof(rates);
    f[n++] = 3; f[n++] = 1; f[n++] = a->channel;
    if (a->rsn_len) { f[n++] = 48; f[n++] = a->rsn_len; memcpy(f + n, a->rsn, a->rsn_len); n += a->rsn_len; }
    return n;
}

static void rx_frame(mt7925_model *m, const uint8_t *frame, uint32_t len, uint8_t channel) {
    uint8_t b[MODEL_FRAME_MAX];
    if (len + 32u > sizeof(b)) return;
    memset(b, 0, 32);
    put32(b, ((len + 32u) & MT7925_RXD0_LEN_MASK) | (MT7925_PKT_TYPE_NORMAL << MT7925_RXD0_PKT_TYPE_SHIFT));
    put32(b + 12, (uint32_t)channel << 8); /* RXD3 CH_FREQ */
    memcpy(b + 32, frame, len);
    model_queue_rx(m, MT7925_PKT_TYPE_NORMAL, b, len + 32u);
}

void model_emit_beacon(mt7925_model *m, const model_ap *a) {
    uint8_t f[512];
    uint32_t n = build_beacon(a, f, 0, NULL);
    rx_frame(m, f, n, a->channel);
}


/* ---- access point side ---- */

static int eq6(const uint8_t *a, const uint8_t *b) { return memcmp(a, b, 6) == 0; }

/* 802.11 frame from AP `a` to the station, as an RX NORMAL buffer. A
 * protected frame arrives the way the reference says the chip hands up a
 * decrypted one: SEC_MODE CCMP in RXD2, Protected bit still set, CCMP
 * header and MIC already stripped. */
static void ap_send(mt7925_model *m, const model_ap *a, const uint8_t *frame, uint32_t len, int protect) {
    uint8_t b[MODEL_FRAME_MAX];
    /* RXD group 1 carries the CCMP packet number (MODEL_F_NO_PN: the chip leaves it out) */
    uint32_t g = protect && !(m->faults & MODEL_F_NO_PN) ? 16u : 0u;
    if (len + 32u + g > sizeof(b)) return;
    memset(b, 0, 32u + g);
    put32(b, ((len + 32u + g) & MT7925_RXD0_LEN_MASK) | (MT7925_PKT_TYPE_NORMAL << MT7925_RXD0_PKT_TYPE_SHIFT));
    put32(b + 4, MT7925_WCID_AP | (g ? MT7925_RXD1_GROUP_1 : 0u));
    if (protect) put32(b + 8, MT7925_CIPHER_CCMP << MT7925_RXD2_SEC_MODE_SHIFT);
    put32(b + 12, (uint32_t)a->channel << 8);
    if (g) {
        uint64_t pn = ++m->ap_pn;
        for (int k = 0; k < 6; k++) b[32 + k] = (uint8_t)(pn >> (8 * k));
    }
    memcpy(b + 32 + g, frame, len);
    if (protect) b[33 + g] |= 0x40;
    if (protect && len + 32u + g <= sizeof m->last_prot) { /* kept for the replay test */
        memcpy(m->last_prot, b, len + 32u + g);
        m->last_prot_len = len + 32u + g;
    }
    model_queue_rx(m, MT7925_PKT_TYPE_NORMAL, b, len + 32u + g);
}

static uint32_t mgmt(uint8_t *f, uint8_t subtype, const uint8_t *da, const uint8_t *bssid) {
    memset(f, 0, 24);
    f[0] = (uint8_t)(subtype << 4);
    memcpy(f + 4, da, 6);
    memcpy(f + 10, bssid, 6);
    memcpy(f + 16, bssid, 6);
    return 24;
}

static uint16_t csum(const uint8_t *p, uint32_t n) {
    uint32_t s = 0;
    for (uint32_t i = 0; i + 1 < n; i += 2) s += (uint32_t)(p[i] << 8 | p[i + 1]);
    if (n & 1) s += (uint32_t)p[n - 1] << 8;
    while (s >> 16) s = (s & 0xffff) + (s >> 16);
    return (uint16_t)~s;
}

/* Data frame FromDS to the station carrying `l3` (ethertype + payload). */
static void ap_data(mt7925_model *m, const model_ap *a, uint16_t ethertype, const uint8_t *pl, uint32_t n, int protect) {
    uint8_t f[MODEL_FRAME_MAX];
    if (n + 32u > sizeof(f)) return;
    memset(f, 0, 24);
    f[0] = 0x08; /* data */
    f[1] = 0x02; /* FromDS */
    memcpy(f + 4, m->sta_mac, 6);
    memcpy(f + 10, a->bssid, 6);
    /* SA: the gateway behind the AP; key messages come from the AP itself
     * (the authenticator address is the BSSID) unless a test forges them. */
    memcpy(f + 16, ethertype == 0x888e && !m->eapol_from_gw ? a->bssid : a->mac, 6);
    static const uint8_t snap[6] = {0xaa, 0xaa, 0x03, 0, 0, 0};
    memcpy(f + 24, snap, 6);
    f[30] = (uint8_t)(ethertype >> 8);
    f[31] = (uint8_t)ethertype;
    memcpy(f + 32, pl, n);
    ap_send(m, a, f, 32u + n, protect);
}

/* ---- AP authenticator: AKM 6, descriptor version 3, test doubles ---- */

void model_set_pmk(mt7925_model *m, const uint8_t pmk[32]) { memcpy(m->ap_pmk, pmk, 32); }

static void ap_key(mt7925_model *m, const model_ap *a, uint16_t info, const uint8_t *kd, uint32_t kl, int mic) {
    uint8_t out[WPA_FRAME_MAX], wrapped[WPA_KEYDATA_MAX];
    wpa_key_frame k = {0};
    uint32_t n = 0;
    k.version = 2;
    k.key_info = (uint16_t)(info | WPA_VER_CMAC_AES);
    k.key_len = 16;
    m->ap_replay++;
    for (int i = 0; i < 8; i++) k.replay[i] = (uint8_t)(m->ap_replay >> (56 - 8 * i));
    memcpy(k.nonce, m->ap_anonce, 32);
    if (kd) {
        k.data_len = (uint16_t)toy_wrap(m->ap_ptk + 16, kd, kl, wrapped);
        k.data = wrapped;
    }
    if (wpa_key_build(&k, out, sizeof out, &n) != WPA_OK) return;
    if (mic) toy_mic(0, m->ap_ptk, out, n, out + WPA_MIC_OFF);
    if (mic && (m->faults & MODEL_F_M3_BAD_MIC)) out[WPA_MIC_OFF] ^= 1;
    ap_data(m, a, 0x888e, out, n, m->eapol_protect);
}

static void ap_wpa_start(mt7925_model *m, const model_ap *a) {
    m->ap_wpa = 0;
    if (!a->rsn_len || (m->faults & MODEL_F_NO_M1)) return;
    for (int i = 0; i < 32; i++) m->ap_anonce[i] = (uint8_t)(0x5a ^ (i * 3) ^ m->assocs);
    for (int i = 0; i < 16; i++) m->ap_gtk[i] = (uint8_t)(0xc0 + i);
    m->ap_wpa = 1;
    ap_key(m, a, WPA_KI_PAIRWISE | WPA_KI_ACK, 0, 0, 0);
}

static int ap_mic_ok(const mt7925_model *m, const uint8_t *p, uint32_t n) {
    uint8_t copy[WPA_FRAME_MAX], mic[16];
    if (n > sizeof copy) return 0;
    memcpy(copy, p, n);
    memset(copy + WPA_MIC_OFF, 0, 16);
    toy_mic(0, m->ap_ptk, copy, n, mic);
    return memcmp(mic, p + WPA_MIC_OFF, 16) == 0;
}

static void ap_eapol(mt7925_model *m, const model_ap *a, const uint8_t *p, uint32_t n) {
    wpa_key_frame k;
    m->eapol_rx++;
    if (wpa_key_parse(p, n, &k) != WPA_OK) { m->eapol_bad++; return; }
    if (m->ap_wpa == 1 && k.key_info == (WPA_KI_PAIRWISE | WPA_KI_MIC | WPA_VER_CMAC_AES)) {
        uint8_t data[76];
        const uint8_t *aa = a->bssid, *spa = m->sta_mac, *sn = k.nonce;
        int af = memcmp(aa, spa, 6) < 0, nf = memcmp(m->ap_anonce, sn, 32) < 0;
        memcpy(data, af ? aa : spa, 6);
        memcpy(data + 6, af ? spa : aa, 6);
        memcpy(data + 12, nf ? m->ap_anonce : sn, 32);
        memcpy(data + 44, nf ? sn : m->ap_anonce, 32);
        wpa_kdf_sha256(m->ap_pmk, "Pairwise key expansion", data, sizeof data, m->ap_ptk, 48);
        if (!ap_mic_ok(m, p, k.total)) { m->eapol_bad++; return; }
        /* key data: our RSNE (as in the beacon) then the GTK KDE, padded to 8 */
        uint8_t kd[128];
        uint32_t kl = 0;
        kd[kl++] = 0x30;
        kd[kl++] = a->rsn_len;
        memcpy(kd + kl, a->rsn, a->rsn_len);
        if (m->faults & MODEL_F_M3_DOWNGRADE) kd[kl + a->rsn_len - 2] = 0;
        kl += a->rsn_len;
        static const uint8_t kde[] = {0xdd, 22, 0x00, 0x0f, 0xac, 0x01, 0x01, 0x00};
        memcpy(kd + kl, kde, sizeof kde);
        memcpy(kd + kl + sizeof kde, m->ap_gtk, 16);
        kl += sizeof kde + 16u;
        if (kl % 8u) { kd[kl++] = 0xdd; while (kl % 8u) kd[kl++] = 0; }
        m->ap_wpa = 2;
        ap_key(m, a, WPA_KI_PAIRWISE | WPA_KI_INSTALL | WPA_KI_ACK | WPA_KI_MIC | WPA_KI_SECURE | WPA_KI_ENC, kd, kl, 1);
    } else if (m->ap_wpa == 2 && k.key_info == (WPA_KI_PAIRWISE | WPA_KI_MIC | WPA_KI_SECURE | WPA_VER_CMAC_AES)) {
        if (!ap_mic_ok(m, p, k.total)) { m->eapol_bad++; return; }
        m->ap_wpa = 3;
    } else {
        m->eapol_bad++;
    }
}

static void ap_handle(mt7925_model *m, const uint8_t *f, uint32_t len, int protect) {
    if (len < 24) return;
    uint8_t type = (f[0] >> 2) & 3u, st = f[0] >> 4;
    for (uint32_t i = 0; i < m->n_ap; i++) {
        model_ap *a = &m->ap[i];
        uint8_t r[512];
        uint32_t n;
        if (type == 0 && st == 4) { /* probe request: answer on our channel */
            n = build_beacon(a, r, 1, f + 10);
            ap_send(m, a, r, n, 0);
            continue;
        }
        if (!eq6(f + 4, a->bssid)) continue;
        /* The radio must be on the AP's channel (the ROC the reference takes). */
        if (!m->roc_active && m->assoc_ap != (int)i) continue;
        if (m->roc_active && m->roc_channel != a->channel) continue;
        if (type == 0 && st == 11 && len >= 30) { /* authentication */
            m->auths++;
            if (m->faults & MODEL_F_AP_SILENT) return;
            if (!m->sta_records || !eq6(m->sta_peer, a->bssid)) return; /* chip has no record for the AP */
            n = mgmt(r, 11, f + 10, a->bssid);
            put16(r + n, 0); put16(r + n + 2, 2);
            put16(r + n + 4, (m->faults & MODEL_F_AP_DEAUTH) ? 1u : 0u);
            ap_send(m, a, r, n + 6, 0);
        } else if (type == 0 && st == 0 && len >= 28) { /* association request */
            m->assocs++;
            n = mgmt(r, 1, f + 10, a->bssid);
            put16(r + n, 0x0401);
            put16(r + n + 2, (m->faults & MODEL_F_ASSOC_DENY) ? 17u : 0u);
            put16(r + n + 4, 0xc001); /* AID 1 */
            r[n + 6] = 1; r[n + 7] = 1; r[n + 8] = 0x82;
            ap_send(m, a, r, n + 9, 0);
            if (!(m->faults & MODEL_F_ASSOC_DENY)) {
                m->assoc_ap = (int)i;
                ap_wpa_start(m, a);
            }
        } else if (type == 2 && (f[1] & 3u) == 1u && m->assoc_ap == (int)i && len >= 32) { /* data, ToDS */
            m->data_rx++;
            const uint8_t *l = f + 24;
            uint16_t et = (uint16_t)(l[6] << 8 | l[7]);
            const uint8_t *p = l + 8;
            uint32_t pn = len - 32u;
            if (et == 0x888e) {
                ap_eapol(m, a, p, pn);
                return;
            }
            if (a->rsn_len && (m->ap_wpa != 3 || !protect)) { /* port closed until the handshake, then CCMP only */
                m->unprot_dropped++;
                return;
            }
            if (et == 0x0806 && pn >= 28 && p[7] == 1 && !memcmp(p + 24, a->ip, 4)) { /* ARP who-has us */
                uint8_t rep[28];
                memcpy(rep, p, 8);
                rep[7] = 2;
                memcpy(rep + 8, a->mac, 6);
                memcpy(rep + 14, a->ip, 4);
                memcpy(rep + 18, p + 8, 10);
                m->arp_replies++;
                ap_data(m, a, 0x0806, rep, 28, protect);
            } else if (et == 0x0800 && pn >= 28 && (p[0] >> 4) == 4 && p[9] == 1 && !memcmp(p + 16, a->ip, 4)) {
                uint32_t ihl = (p[0] & 0xfu) * 4u, tot = (uint32_t)(p[2] << 8 | p[3]);
                if (ihl < 20 || tot > pn || tot < ihl + 8 || p[ihl] != 8) return; /* echo request only */
                uint8_t rep[MODEL_FRAME_MAX];
                memcpy(rep, p, tot);
                memcpy(rep + 12, p + 16, 4);
                memcpy(rep + 16, p + 12, 4);
                rep[10] = rep[11] = 0;
                uint16_t c = csum(rep, ihl);
                rep[10] = (uint8_t)(c >> 8); rep[11] = (uint8_t)c;
                rep[ihl] = 0;
                rep[ihl + 2] = rep[ihl + 3] = 0;
                c = csum(rep + ihl, tot - ihl);
                rep[ihl + 2] = (uint8_t)(c >> 8); rep[ihl + 3] = (uint8_t)c;
                m->echo_replies++;
                ap_data(m, a, 0x0800, rep, tot, protect);
            }
        } else if (type == 0 && (st == 12 || st == 10)) {
            m->deauth_rx++;
            if (len >= 26) m->last_deauth_reason = (uint16_t)(f[24] | f[25] << 8);
            if (m->assoc_ap == (int)i) {
                m->assoc_ap = -1;
                m->ap_wpa = 0;
            }
        }
        return;
    }
}

void model_radio_tx(mt7925_model *m, const uint8_t *b, uint32_t len) {
    m->tx_frames++;
    m->last_tx_len = len < MODEL_FRAME_MAX ? len : MODEL_FRAME_MAX;
    memcpy(m->last_tx, b, m->last_tx_len);
}

static void tx_free_flush(mt7925_model *m) {
    if (!m->n_tok_pending || (m->faults & MODEL_F_TXFREE_HOLD)) return;
    uint8_t r[8 + 4 * 32];
    uint32_t n = m->n_tok_pending, w = 0;
    memset(r, 0, sizeof r);
    uint32_t announce = (m->faults & MODEL_F_TXFREE_SHORT) ? n + 4u : n;
    for (uint32_t k = 0; k < n; k += 2) {
        uint32_t a = m->tok_pending[k], b = k + 1 < n ? m->tok_pending[k + 1] : 0x7fffu;
        if (m->faults & MODEL_F_TXFREE_BOGUS) a = (a + 7u) % 32u;
        put32(r + 8 + 4 * w++, a | (b << 15));
    }
    put32(r + 8 + 4 * w++, MT7925_TXFREE_INFO_PAIR | (MT7925_WCID_AP << 12)); /* a pair word, nothing to free */
    uint32_t len = 8 + 4 * w;
    put32(r, len | (announce << MT7925_TXFREE0_MSDU_CNT_SHIFT) | (MT7925_PKT_TYPE_TXRX_NOTIFY << MT7925_RXD0_PKT_TYPE_SHIFT));
    put32(r + 4, 5u << MT7925_TXFREE1_VER_SHIFT);
    m->tx_freed_sent += n;
    m->n_tok_pending = 0;
    model_queue_rx(m, MT7925_PKT_TYPE_TXRX_NOTIFY, r, len);
}

/* Data ring slot: cut-through TXD + TXP; the frame is fetched through the
 * TXP pointer (window-checked like everything else), handed to the radio,
 * and its token released in a TX_FREE record. */
static void data_tx(mt7925_model *m, const uint8_t *b, uint32_t len) {
    if (len != MT7925_TX_HDR_BYTES || ((le32(b) >> MT7925_TXD0_PKT_FMT_SHIFT) & 3u) != MT7925_TX_TYPE_CT) {
        m->mcu_bad++;
        model_radio_tx(m, b, len);
        return;
    }
    memcpy(m->last_txd, b, MT7925_TX_HDR_BYTES);
    const uint8_t *t = b + MT7925_TXD_BYTES;
    uint16_t id = le16(t), l0 = le16(t + 12);
    uint32_t flen = l0 & MT7925_TXP_LEN_MASK;
    if (!(id & MT7925_MSDU_ID_VALID) || !(l0 & MT7925_TXP_LEN_LAST) || (le32(b) & 0xffffu) != flen + MT7925_TXD_BYTES) {
        m->mcu_bad++;
        return;
    }
    const uint8_t *f = bus(m, le32(t + 8), flen);
    if (!f) return;
    m->ct_frames++;
    uint8_t copy[MODEL_FRAME_MAX];
    memcpy(copy, f, flen); /* the chip's copy: the driver may reuse the slot only after TX_FREE */
    int protect = (le32(b + 12) & MT7925_TXD3_PROTECT_FRAME) != 0;
    if (m->n_tok_pending < 32) m->tok_pending[m->n_tok_pending++] = (uint16_t)(id & 0x7fffu);
    if (protect) m->protected_tx++;
    if (protect && !(m->keys & 1)) {
        m->protected_dropped++; /* no pairwise key: not sent, but the token still comes back */
    } else {
        model_radio_tx(m, copy, flen);
        ap_handle(m, copy, flen, protect);
    }
    tx_free_flush(m);
}

void model_release_tokens(mt7925_model *m) {
    uint32_t f = m->faults;
    m->faults &= ~(uint32_t)MODEL_F_TXFREE_HOLD;
    tx_free_flush(m);
    m->faults = f;
}

void model_ap_send_m1(mt7925_model *m) {
    if (m->assoc_ap < 0) return;
    ap_key(m, &m->ap[m->assoc_ap], WPA_KI_PAIRWISE | WPA_KI_ACK, 0, 0, 0);
}

void model_replay_last(mt7925_model *m) {
    if (m->last_prot_len) model_queue_rx(m, MT7925_PKT_TYPE_NORMAL, m->last_prot, m->last_prot_len);
}
