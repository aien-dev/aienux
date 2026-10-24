/* MediaTek MT7925 (PCIe 14c3:7925) Wi-Fi driver core for Aienux, polled,
 * hosted-testable.
 *
 * Source of every register, ring and command layout: a reading of the
 * open-source Linux driver, openwrt/mt76 @467ae2e3 (BSD-3-Clause-Clear),
 * files mt7925/pci.c, mt7925/regs.h, mt792x_regs.h, mt792x_dma.c,
 * mt792x_core.c, dma.c/.h, mt76_connac_mcu.c/.h, mt7925/mcu.c/.h. MediaTek
 * publishes no programming manual for this chip (DOCS SILENT). This is new C
 * written from that reading, not a copy. Nothing here has run on the chip:
 * every sequence is INFERRED from source and listed as UNKNOWN for an
 * attended boot in docs/SPARK_WIFI_MT7925.md.
 *
 * Device access goes only through mt7925_ops: 32-bit reads and writes at a
 * BAR0 byte offset (the driver turns chip bus addresses into BAR0 offsets
 * itself, through the fixed map or the L1/L2 remap windows), a microsecond
 * delay for bounded polls, and a DMA ordering barrier. No heap, no clock, no
 * I/O of its own. All rings and buffers live in ONE caller-provided region
 * (mem, len) that the chip sees at bus address `dma` (an SMMU IOVA on the
 * Spark). The chip's DMA mask is 32 bits (the Linux driver sets 32 and the
 * Spark reports dma_mask_bits=32), so the whole region must sit below 4 GiB
 * of bus space. Descriptors are written from the driver's own shadow and
 * checked against the region; anything the chip writes (ring indices,
 * descriptor done bits, lengths, event headers) is untrusted: an index out of
 * range, a length larger than the buffer, or a completion that was never
 * posted marks the device broken (sticky) until mt7925_init runs again.
 *
 * Interrupts are not used: the driver keeps the host interrupt mask at 0 and
 * polls the rings. */
#ifndef AIENOS_MT7925_H
#define AIENOS_MT7925_H

#include <stddef.h>
#include <stdint.h>

#include "aienos_mt7925_fw.h"

/* ---- PCI identity (OBSERVED on the Spark 2026-10-20, read-only sysfs) ---- */
#define MT7925_PCI_VENDOR 0x14c3u
#define MT7925_PCI_DEVICE 0x7925u
#define MT7925_PCI_DEVICE_ALT 0x0717u /* second id in the reference table */
#define MT7925_PCI_CLASS 0x028000u    /* network controller, other */
#define MT7925_BAR0_BYTES 0x200000u   /* 2 MiB, 64-bit, non-prefetchable */
#define MT7925_BAR2_BYTES 0x8000u     /* 32 KiB; the reference never maps it */
#define MT7925_DMA_LIMIT 0x100000000ull /* 32-bit DMA mask */

/* ---- chip bus addresses (reference names in comments) ---- */
#define MT7925_CONN_ON_LPCTL 0x7c060010u   /* MT_CONN_ON_LPCTL */
#define MT7925_LPCR_HOST_SET_OWN (1u << 0)
#define MT7925_LPCR_HOST_CLR_OWN (1u << 1)
#define MT7925_LPCR_HOST_OWN_SYNC (1u << 2)
#define MT7925_CONN_ON_MISC 0x7c0600f0u    /* MT_CONN_ON_MISC */
#define MT7925_MISC_FW_STATE_MASK 0x7u
#define MT7925_MISC_FW_PWR_ON 0x1u
#define MT7925_MISC_FW_N9_RDY 0x3u
#define MT7925_HW_CHIPID 0x70010200u
#define MT7925_HW_REV 0x70010204u
#define MT7925_HW_EMI_CTL 0x18011100u
#define MT7925_HW_EMI_CTL_SLPPROT_EN (1u << 1)
#define MT7925_WFSYS_SW_RST 0x7c000140u
#define MT7925_WFSYS_SW_RST_B (1u << 0)
#define MT7925_WFSYS_SW_INIT_DONE (1u << 4)
#define MT7925_UWFDMA0_GLO_CFG_EXT1 0x7c0242b4u
#define MT7925_WFDMA_DUMMY_CR 0x54000120u
#define MT7925_WFDMA_NEED_REINIT (1u << 1)

/* ---- BAR0 offsets (below 0x200000 an address is its own BAR0 offset) ---- */
#define MT7925_WFDMA0(o) (0xd4000u + (o))
#define MT7925_WFDMA0_RST MT7925_WFDMA0(0x100)
#define MT7925_MCU2HOST_SW_INT_ENA MT7925_WFDMA0(0x1f4)
#define MT7925_MCU_CMD_WAKE_RX_PCIE (1u << 0)
#define MT7925_HOST_INT_STA MT7925_WFDMA0(0x200)
#define MT7925_HOST_INT_ENA MT7925_WFDMA0(0x204)
#define MT7925_GLO_CFG MT7925_WFDMA0(0x208)
#define MT7925_GLO_TX_DMA_EN (1u << 0)
#define MT7925_GLO_TX_DMA_BUSY (1u << 1)
#define MT7925_GLO_RX_DMA_EN (1u << 2)
#define MT7925_GLO_RX_DMA_BUSY (1u << 3)
#define MT7925_GLO_DMA_SIZE_SHIFT 4u
#define MT7925_GLO_TX_WB_DDONE (1u << 6)
#define MT7925_GLO_FIFO_DIS_CHECK (1u << 11)
#define MT7925_GLO_FIFO_LITTLE_ENDIAN (1u << 12)
#define MT7925_GLO_RX_WB_DDONE (1u << 13)
#define MT7925_GLO_DISP_BASE_PTR_CHAIN_EN (1u << 15)
#define MT7925_GLO_OMIT_RX_INFO_PFET2 (1u << 21)
#define MT7925_GLO_OMIT_TX_INFO (1u << 28)
#define MT7925_GLO_CLK_GAT_DIS (1u << 30)
#define MT7925_RST_DTX_PTR MT7925_WFDMA0(0x20c)
#define MT7925_RST_DRX_PTR MT7925_WFDMA0(0x280)
#define MT7925_INT_RX_PRI MT7925_WFDMA0(0x298)
#define MT7925_INT_TX_PRI MT7925_WFDMA0(0x29c)
#define MT7925_PRI_DLY_INT_CFG0 MT7925_WFDMA0(0x2f0)
#define MT7925_TX_RING_BASE MT7925_WFDMA0(0x300)
#define MT7925_RX_RING_BASE MT7925_WFDMA0(0x500)
#define MT7925_TX_EXT_CTRL(q) MT7925_WFDMA0(0x600 + 4u * (q))
#define MT7925_RX_EXT_CTRL(q) MT7925_WFDMA0(0x680 + 4u * (q))
#define MT7925_PCIE_MAC_INT_ENABLE 0x10188u
#define MT7925_HIF_REMAP_L1 0x155024u
#define MT7925_HIF_REMAP_BASE_L1 0x130000u
#define MT7925_HIF_REMAP_L2 0x0120u
#define MT7925_HIF_REMAP_BASE_L2 0x18500000u

/* Ring register block: desc_base, ring_size, cpu_idx, dma_idx. */
#define MT7925_RING_DESC_BASE 0x0u
#define MT7925_RING_SIZE 0x4u
#define MT7925_RING_CPU_IDX 0x8u
#define MT7925_RING_DMA_IDX 0xcu

/* Hardware queue numbers (reference MT7925_TXQ_* / MT7925_RXQ_*). */
#define MT7925_TXQ_DATA 0u
#define MT7925_TXQ_MCU 15u
#define MT7925_TXQ_FWDL 16u
#define MT7925_RXQ_EVENT 0u
#define MT7925_RXQ_DATA 2u

/* 16-byte DMA descriptor {buf0, ctrl, buf1, info}, little endian. */
#define MT7925_DESC_BYTES 16u
#define MT7925_DESC_SD_LEN1_MASK 0x3fffu
#define MT7925_DESC_LAST_SEC1 (1u << 14)
#define MT7925_DESC_SD_LEN0_SHIFT 16u
#define MT7925_DESC_SD_LEN0_MASK (0x3fffu << 16)
#define MT7925_DESC_LAST_SEC0 (1u << 30)
#define MT7925_DESC_DMA_DONE (1u << 31)

/* Ring sizes and buffer strides chosen by this driver (the reference uses
 * 2048/256/128/512/1536 descriptors; smaller rings are UNKNOWN on hardware,
 * see the doc). */
#define MT7925_N_TX_DATA 32u
#define MT7925_N_TX_MCU 16u
#define MT7925_N_TX_FWDL 8u
#define MT7925_N_RX_EVENT 32u
#define MT7925_N_RX_DATA 32u
#define MT7925_RING_MAX 32u
#define MT7925_BUF_BYTES 2048u        /* RX buffers, TX data and MCU commands */
/* Frames that arrive on the event ring while a command waits for its reply
 * are held here and handed to on_rx by the next mt7925_poll_rings, so the
 * receive callback never runs inside a command (it may send commands). */
#define MT7925_RX_DEFER 4u
#define MT7925_CB_DEPTH 4u        /* nested RX callbacks (a callback that polls again) */
#define MT7925_FWDL_BUF_BYTES 4096u   /* one FW_SCATTER chunk */

/* MCU command framing. */
#define MT7925_TXD_BYTES 32u
#define MT7925_MCU_HDR_LEGACY 32u    /* legacy command header after the TXD */
#define MT7925_MCU_HDR_UNI 16u       /* UNI command header after the TXD */
#define MT7925_MCU_MAX_PAYLOAD (MT7925_BUF_BYTES - MT7925_TXD_BYTES - MT7925_MCU_HDR_LEGACY)
#define MT7925_MCU_Q_NA 3u           /* legacy set_query "not applicable" */
#define MT7925_MCU_PKT_ID 0xa0u
#define MT7925_MCU_PQ_ID ((1u << 15) | (0x20u << 10))
#define MT7925_MCU_Q_IDX 0x20u       /* MT_TX_MCU_PORT_RX_Q0 */
#define MT7925_TX_TYPE_CMD 2u
#define MT7925_HDR_FORMAT_CMD 1u
#define MT7925_UNI_OPT_SET 0x7u      /* ACK | UNI | SET */
#define MT7925_UNI_OPT_QUERY 0x3u    /* ACK | UNI */
#define MT7925_UNI_OPT_ACK 0x1u      /* MCU_CMD_ACK, mt76_connac_mcu.h:1210 */
#define MT7925_UNI_CMD_HIF_CTRL 0x07u    /* MCU_UNI_CMD_HIF_CTRL, mt76_connac_mcu.h:1323 */
#define MT7925_UNI_CMD_CHIP_CONFIG 0x0eu /* MCU_UNI_CMD_CHIP_CONFIG, mt76_connac_mcu.h:1328 */
#define MT7925_RXD_BYTES 32u         /* RX descriptor words before an event header */
#define MT7925_EVT_HDR_BYTES 44u     /* rxd[8], len, pkt_type_id, eid, seq, option, rsv, ext_eid, rsv2, s2d */
#define MT7925_EVT_LEN_OFF 32u
#define MT7925_EVT_EID_OFF 36u
#define MT7925_EVT_SEQ_OFF 37u
#define MT7925_EVT_OPTION_OFF 38u
#define MT7925_EVT_STATUS8_OFF 40u   /* patch semaphore / patch finish status byte */
#define MT7925_EVT_UNI_CID_OFF 44u
#define MT7925_EVT_UNI_STATUS_OFF 48u
#define MT7925_EVT_OPT_UNSOLICITED (1u << 2)
/* RXD word 0: length 15:0, packet type 31:27. */
#define MT7925_RXD0_LEN_MASK 0xffffu
#define MT7925_RXD0_PKT_TYPE_SHIFT 27u
#define MT7925_PKT_TYPE_TXS 0u
#define MT7925_PKT_TYPE_NORMAL 2u
#define MT7925_PKT_TYPE_TXRX_NOTIFY 6u
#define MT7925_PKT_TYPE_RX_EVENT 7u
#define MT7925_PKT_TYPE_NORMAL_MCU 8u

/* UNI command ids used here. */
#define MT7925_UNI_CMD_SCAN_REQ 0x16u
#define MT7925_UNI_EVENT_SCAN_DONE 0x0eu
#define MT7925_UNI_SCAN_REQ 1u
#define MT7925_UNI_SCAN_SSID 10u
#define MT7925_UNI_SCAN_BSSID 11u
#define MT7925_UNI_SCAN_CHANNEL 12u
#define MT7925_UNI_SCAN_IE 13u
#define MT7925_UNI_SCAN_MISC 14u
#define MT7925_SCAN_FUNC_SPLIT_SCAN (1u << 5)
#define MT7925_SCAN_MAX_SSIDS 10u
#define MT7925_SCAN_MAX_CHANNELS 64u
#define MT7925_MCU_CMD_NIC_POWER_CTRL 0x04u

/* Poll budgets, from the reference's timeouts. */
#define MT7925_OWN_TRIES 10u
#define MT7925_OWN_POLL_US 50000u
#define MT7925_RESET_HOLD_US 50000u
#define MT7925_RESET_POLL_US 500000u
#define MT7925_PWR_ON_POLL_US 1000000u
#define MT7925_N9_RDY_POLL_US 1500000u
#define MT7925_MCU_REPLY_US 3000000u
#define MT7925_POLL_STEP_US 1000u

typedef struct {
    void *ctx;
    uint32_t (*rd)(void *ctx, uint32_t bar0_off);           /* bar0_off < 2 MiB, 4-byte aligned */
    void (*wr)(void *ctx, uint32_t bar0_off, uint32_t val);
    void (*udelay)(void *ctx, uint32_t us);
    void (*dma_barrier)(void *ctx); /* CPU region writes visible before the next MMIO write, and
                                       device region writes visible after the last MMIO read */
} mt7925_ops;

typedef enum {
    MT7925_OK = 0,
    MT7925_E_ARG = 1,          /* NULL, bad sizes, payload too long */
    MT7925_E_IDENTITY = 2,     /* not 14c3:7925/0717, wrong class or BAR0 size */
    MT7925_E_MEM = 3,          /* region too small, misaligned, or not below 4 GiB of bus space */
    MT7925_E_FW_OWN = 4,       /* firmware-own handshake never synced */
    MT7925_E_DRV_OWN = 5,      /* driver-own handshake never synced */
    MT7925_E_RESET = 6,        /* WFSYS reset never reported init done */
    MT7925_E_FULL = 7,         /* no free TX descriptor after reclaim */
    MT7925_E_TIMEOUT = 8,      /* MCU reply or firmware state never arrived */
    MT7925_E_DEVICE = 9,       /* hostile/broken completion; device is now broken */
    MT7925_E_BROKEN = 10,      /* sticky: an earlier MT7925_E_DEVICE */
    MT7925_E_FW = 11,          /* firmware container refused (see mt7925_fw_err) */
    MT7925_E_PATCH_SEM = 12,   /* patch semaphore refused */
    MT7925_E_MCU_STATUS = 13,  /* MCU replied with a failure status */
    MT7925_E_STATE = 14,       /* call out of order (e.g. scan before firmware) */
} mt7925_err;

typedef struct {
    uint32_t regs;      /* BAR0 offset of the ring register block */
    uint16_t n;         /* descriptors */
    uint16_t head;      /* next slot the driver fills (TX) / expects done (RX) */
    uint16_t tail;      /* TX: oldest not yet reclaimed */
    uint16_t inflight;  /* TX: posted, not reclaimed */
    uint32_t desc_off;  /* region offset of the descriptor array */
    uint32_t buf_off;   /* region offset of buffer 0 */
    uint32_t buf_len;   /* bytes per buffer */
    uint16_t posted_len[MT7925_RING_MAX]; /* shadow: length posted per slot */
} mt7925_ring;

/* Unsolicited MCU event (scan done, beacon loss, ...). `ev` points at the
 * whole event (header included, `len` bytes) inside the RX buffer and is
 * only valid during the call. */
typedef void (*mt7925_event_fn)(void *ctx, uint8_t eid, const uint8_t *ev, uint32_t len);
/* Received data-ring frame (RX descriptor + payload), valid during the call. */
typedef void (*mt7925_rx_fn)(void *ctx, const uint8_t *buf, uint32_t len);

typedef struct {
    mt7925_ops ops;
    uint8_t *mem;
    uint64_t dma;
    size_t mem_len;
    uint32_t chip_rev;  /* CHIPID << 16 | (REV & 0xff) */
    uint8_t mcu_seq;    /* last used MCU sequence, 1..15 */
    uint8_t broken;
    uint8_t dma_on;
    uint8_t fw_running;
    mt7925_ring tx_data, tx_mcu, tx_fwdl, rx_evt, rx_data;
    mt7925_event_fn on_event;
    mt7925_rx_fn on_rx;
    void *cb_ctx;       /* passed to on_event */
    void *rx_ctx;       /* passed to on_rx */
    uint32_t events_seen, events_dropped;
    uint8_t defer[MT7925_RX_DEFER][MT7925_BUF_BYTES];
    uint16_t defer_len[MT7925_RX_DEFER];
    uint32_t defer_n, rx_deferred, rx_defer_dropped;
    /* One frame copy per callback nesting level (rx_one); deeper frames are
     * dropped and counted. */
    uint8_t cb_buf[MT7925_CB_DEPTH][MT7925_BUF_BYTES];
    uint32_t cb_depth, rx_nest_dropped;
    uint32_t tok_busy;  /* data TX slots whose frame the chip has not yet released (TX_FREE) */
    uint32_t tx_freed;  /* tokens released by TX_FREE */
    uint8_t roc_token;  /* last remain-on-channel token (aienos_mt7925_sta.c) */
    uint8_t roc_status; /* status byte of the last ROC grant */
} mt7925_dev;

/* Bytes of DMA region mt7925_init needs (rings + buffers). */
size_t mt7925_dma_bytes(void);

/* Identity check from PCI config values the caller read (no device access). */
mt7925_err mt7925_pci_identity(uint16_t vendor, uint16_t device, uint32_t class_code, uint64_t bar0_size,
                               int bar0_is_64, int bar0_is_mem);

/* Chip bus address -> BAR0 offset through the fixed map only. Returns 1 and
 * sets *off when the address is direct or fixed-mapped, 0 when it needs the
 * L1 or L2 remap window. */
int mt7925_fixed_map(uint32_t addr, uint32_t *off);
/* 1 if `addr` (not fixed-mapped) uses the L1 window, 0 for L2. */
int mt7925_uses_l1(uint32_t addr);

/* Register access by chip bus address (handles every remap case). */
uint32_t mt7925_rr(mt7925_dev *d, uint32_t addr);
void mt7925_wr(mt7925_dev *d, uint32_t addr, uint32_t v);
void mt7925_rmw(mt7925_dev *d, uint32_t addr, uint32_t clear, uint32_t set);
/* Poll `addr` until (value & mask) == want, at most budget_us. 1 = met. */
int mt7925_poll(mt7925_dev *d, uint32_t addr, uint32_t mask, uint32_t want, uint32_t budget_us);

/* Probe-time bring-up, in the reference's order: firmware own, driver own,
 * read chip revision, sleep protection, WFSYS reset, mask interrupts,
 * carve and program the five rings, enable WFDMA. Region (mem, len) at bus
 * address dma must satisfy mt7925_dma_bytes() and lie below 4 GiB. */
mt7925_err mt7925_init(mt7925_dev *d, const mt7925_ops *ops, uint8_t *mem, uint64_t dma, size_t len);

/* Quiesce for release (the reference mt792x_dma_disable then fw_pmctrl):
 * mask host interrupts, clear WFDMA TX/RX enable, wait (bounded) for both
 * busy bits to drop, then hand power ownership back to the firmware. After
 * this the caller may revoke bus mastering. MT7925_E_TIMEOUT if the DMA
 * never went idle (ownership is still returned). UNKNOWN on hardware. */
mt7925_err mt7925_stop(mt7925_dev *d);

/* Send one MCU command. uni selects the UNI header; `query` the UNI query
 * option. When reply/reply_cap are non-NULL the call waits (bounded) for the
 * event with the same sequence number and copies its bytes (header included).
 * Events with another sequence go to on_event. */
mt7925_err mt7925_mcu_send(mt7925_dev *d, uint16_t cmd, int uni, int query, const uint8_t *payload, uint32_t len,
                           int wait, uint8_t *reply, uint32_t reply_cap, uint32_t *reply_len);

/* Download ROM patch then RAM firmware and wait for the firmware to report
 * ready. Buffers are the raw (decompressed) files. This is the only call that
 * sends firmware to a chip; on the Spark it is an OPERATOR STEP. */
mt7925_err mt7925_fw_download(mt7925_dev *d, const uint8_t *patch, size_t patch_len, const uint8_t *ram,
                              size_t ram_len);

/* Hardware scan request (UNI_CMD_SCAN_REQ). n_ssids == 0 is a passive
 * wildcard scan; n_channels == 0 scans every channel the firmware allows.
 * Channel entries: band (1 = 2.4 GHz, 2 = 5 GHz, 3 = 6 GHz) and number.
 * Builds the TLVs into `out` (for tests) and sends them when send != 0. */
typedef struct {
    uint8_t band, channel;
} mt7925_chan;
typedef struct {
    uint8_t len;
    uint8_t ssid[32];
} mt7925_ssid;
mt7925_err mt7925_scan_build(uint8_t seq, uint8_t bss_idx, const mt7925_ssid *ssids, uint32_t n_ssids,
                             const mt7925_chan *chans, uint32_t n_channels, const uint8_t *ies, uint32_t ies_len,
                             uint8_t *out, uint32_t cap, uint32_t *out_len);
mt7925_err mt7925_scan_start(mt7925_dev *d, uint8_t seq, const mt7925_ssid *ssids, uint32_t n_ssids,
                             const mt7925_chan *chans, uint32_t n_channels, const uint8_t *ies, uint32_t ies_len);

/* Drain completed RX descriptors on both RX rings (events -> on_event, data
 * -> on_rx) and reclaim finished TX descriptors. Returns frames handled, or
 * a negative mt7925_err. */
int mt7925_poll_rings(mt7925_dev *d);

/* mt7925_mcu_send (UNI, set, waiting) for commands that carry key
 * material: afterwards the MCU ring slot is zeroed, whatever the outcome.
 * A reply means the chip has read the slot; on a timeout the chip may read
 * zeros, which only makes that command fail. */
mt7925_err mt7925_mcu_send_secret(mt7925_dev *d, uint16_t cmd, const uint8_t *payload, uint32_t len, uint8_t *reply,
                                  uint32_t reply_cap, uint32_t *reply_len);

/* Queue one frame on the data TX ring: `txd` (descriptor words the caller
 * built, txd_len bytes) followed by the frame. */
mt7925_err mt7925_tx_raw(mt7925_dev *d, const uint8_t *txd, uint32_t txd_len, const uint8_t *frame,
                         uint32_t frame_len);

/* ---- Data path (reference mt7925_mac_write_txwi, mt76_connac_write_hw_txp,
 * mt7925_mac_tx_free, mt7925_mac_fill_rx; read, not copied) ----
 *
 * TX is cut-through: the data-ring descriptor carries only the 32-byte TXD
 * plus the 32-byte hardware TXP; the TXP holds a token and the bus address of
 * the frame, which this driver keeps right after the TXP in the same slot
 * buffer. The chip reports DMA done once it has the TXD, but reads the frame
 * later and releases the token in a TXRX_NOTIFY (TX_FREE) record; the slot is
 * not reused before both. Frames are sent as 802.11 (HDR_FORMAT 802_11), so
 * aienos_wifi.c builds them and the chip adds CCMP when `protect` is set. */
#define MT7925_TXP_BYTES 32u
#define MT7925_TX_HDR_BYTES (MT7925_TXD_BYTES + MT7925_TXP_BYTES)
#define MT7925_TX_FRAME_MAX (MT7925_BUF_BYTES - MT7925_TX_HDR_BYTES)
#define MT7925_TXD0_TX_BYTES_MASK 0xffffu
#define MT7925_TXD0_PKT_FMT_SHIFT 23u
#define MT7925_TXD0_Q_IDX_SHIFT 25u
#define MT7925_TX_TYPE_CT 0u
#define MT7925_LMAC_AC_BE 1u     /* 3 - mac80211 AC_BE(2) */
#define MT7925_LMAC_ALTX0 0x10u  /* management frames */
#define MT7925_TXD1_WLAN_IDX_MASK 0xfffu
#define MT7925_TXD1_HDR_FORMAT_SHIFT 14u
#define MT7925_HDR_FORMAT_802_11 2u
#define MT7925_TXD1_HDR_INFO_SHIFT 16u
#define MT7925_TXD1_TID_SHIFT 21u
#define MT7925_TXD1_OWN_MAC_SHIFT 25u
#define MT7925_TXD1_FIXED_RATE (1u << 31)
#define MT7925_TXD2_SUB_TYPE_MASK 0xfu
#define MT7925_TXD2_FRAME_TYPE_SHIFT 4u
#define MT7925_TXD3_NO_ACK (1u << 0)
#define MT7925_TXD3_PROTECT_FRAME (1u << 1)
#define MT7925_TXD3_BCM (1u << 4)
#define MT7925_TXD3_REM_TX_COUNT_SHIFT 11u
#define MT7925_TXD3_BA_DISABLE (1u << 28)
#define MT7925_TXD6_DAS (1u << 2)
#define MT7925_TXD6_DIS_MAT (1u << 3)
#define MT7925_TXD6_MSDU_CNT_SHIFT 4u
#define MT7925_TXD6_TX_RATE_SHIFT 16u
#define MT7925_BASIC_RATES_TBL 14u
#define MT7925_MSDU_ID_VALID (1u << 15)
#define MT7925_TXP_LEN_LAST (1u << 15)
#define MT7925_TXP_LEN_MASK 0xfffu
/* TX_FREE record: word0 MSDU_CNT 25:16, word1 VER 19:16, then info words. */
#define MT7925_TXFREE0_MSDU_CNT_SHIFT 16u
#define MT7925_TXFREE0_MSDU_CNT_MASK 0x3ffu
#define MT7925_TXFREE1_VER_SHIFT 16u
#define MT7925_TXFREE_INFO_PAIR (1u << 31)
#define MT7925_TXFREE_INFO_HEADER (1u << 30)
#define MT7925_TXFREE_INFO_MSDU_ID 0x7fffu

/* RXD words 1..3 of a NORMAL frame. */
#define MT7925_RXD1_WLAN_IDX_MASK 0xfffu
#define MT7925_RXD1_GROUP_1 (1u << 16)
#define MT7925_RXD1_GROUP_2 (1u << 17)
#define MT7925_RXD1_GROUP_3 (1u << 18)
#define MT7925_RXD1_GROUP_4 (1u << 19)
#define MT7925_RXD1_GROUP_5 (1u << 20)
#define MT7925_RXD1_CM (1u << 23)
#define MT7925_RXD1_CLM (1u << 24)
#define MT7925_RXD1_ICV_ERR (1u << 25)
#define MT7925_RXD1_TKIP_MIC_ERR (1u << 26)
#define MT7925_RXD2_HDR_TRANS (1u << 7)
#define MT7925_RXD2_HDR_OFFSET_SHIFT 13u
#define MT7925_RXD2_HDR_OFFSET_MASK 0x7u
#define MT7925_RXD2_SEC_MODE_SHIFT 16u
#define MT7925_RXD2_SEC_MODE_MASK 0x1fu
#define MT7925_RXD2_AMSDU_ERR (1u << 23)
#define MT7925_RXD2_MAX_LEN_ERR (1u << 24)
#define MT7925_RXD2_FRAG (1u << 27)
#define MT7925_RXD3_CH_FREQ_SHIFT 8u
#define MT7925_RXD3_FCS_ERR (1u << 24)
#define MT7925_RXD4_PAYLOAD_FORMAT_MASK 0x3u
#define MT7925_CIPHER_CCMP 4u     /* reference MT_CIPHER_AES_CCMP */
#define MT7925_RXD0_SW_PKT_TYPE_SHIFT 16u
#define MT7925_RXD0_SW_PKT_TYPE_MAP 0x380fu
#define MT7925_RXD0_SW_PKT_TYPE_FRAME 0x3801u

typedef struct {
    uint16_t wlan_idx;  /* station record index (0 before association) */
    uint8_t omac_idx;   /* own-MAC index of our interface */
    uint8_t tid;        /* QoS TID for data; ignored for management */
    uint8_t protect;    /* chip encrypts with the installed pairwise/group key */
    uint8_t no_ack;
} mt7925_tx_opts;

/* Build the 64-byte TXD + TXP for one 802.11 frame. `frame_bus` is the bus
 * address of the frame bytes, `token` the MSDU id (< 0x7fff). Pure. */
mt7925_err mt7925_txd_build(uint8_t out[MT7925_TX_HDR_BYTES], const uint8_t *frame, uint32_t frame_len,
                            const mt7925_tx_opts *o, uint16_t token, uint32_t frame_bus);
/* Queue one 802.11 frame (header + body, no FCS) on the data ring. */
mt7925_err mt7925_tx_80211(mt7925_dev *d, const uint8_t *frame, uint32_t frame_len, const mt7925_tx_opts *o);
/* Apply one TX_FREE record (RXD included). Called by the poll loop; public
 * for tests. A token out of range or not in flight marks the device broken. */
mt7925_err mt7925_tx_free(mt7925_dev *d, const uint8_t *rec, uint32_t len);

typedef struct {
    const uint8_t *frame; /* 802.11 header + body inside the RX buffer */
    uint32_t frame_len;
    uint16_t wlan_idx;
    uint8_t channel;      /* RXD3 CH_FREQ: channel number */
    uint8_t sec_mode;     /* RXD2 SEC_MODE: 0 = not protected */
    uint8_t decrypted;    /* chip decrypted, IV and MIC stripped */
    uint8_t has_rssi;
    int8_t rssi;          /* chain 0, from P-RXV RCPI */
    uint8_t has_pn;       /* decrypted and RXD group 1 present */
    uint64_t pn;          /* CCMP packet number, group 1 bytes 0..5 = PN0..PN5 (reference mac.c) */
} mt7925_rx_info;

typedef enum {
    MT7925_RX_OK = 0,
    MT7925_RX_SHORT = 1,     /* descriptor groups run past the buffer */
    MT7925_RX_NOT_FRAME = 2, /* not a NORMAL frame */
    MT7925_RX_BAD_FCS = 3,
    MT7925_RX_BAD_ICV = 4,   /* ICV/CCMP MIC failure */
    MT7925_RX_BAD_MIC = 5,   /* TKIP Michael failure */
    MT7925_RX_CIPHER = 6,    /* cipher mismatch / not decrypted although protected */
    MT7925_RX_LEN_ERR = 7,   /* MAX_LEN or A-MSDU error from the chip */
    MT7925_RX_UNSUPPORTED = 8, /* header translation, A-MSDU, fragments */
} mt7925_rx_err;

/* Parse one NORMAL RX buffer (RXD + frame). Pure; never reads outside
 * buf[0..len). */
mt7925_rx_err mt7925_rx_parse(const uint8_t *buf, uint32_t len, mt7925_rx_info *ri);

const char *mt7925_strerror(mt7925_err e);

#endif
