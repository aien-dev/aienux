/* Realtek RTL8127 10 GbE (PCI 10ec:8127, the Spark's only wired NIC) driver
 * core, polled, hosted-testable. Gap D5 of docs/SPARK_REPLACE_LINUX.md.
 *
 * DOCS SILENT: Realtek publishes no register document for this chip (see
 * docs/SPARK_RTL8127.md). Every offset and bit below comes from Realtek's own
 * Linux driver r8127 v11.014.00, the version the Spark runs, read-only at
 * github.com/openwrt/rtl8127 ec2f285; comments cite r8127_n.c / r8127.h
 * lines there. None of it has been run on the chip. The register sequences
 * still UNKNOWN on the real chip are listed in docs/SPARK_RTL8127.md.
 *
 * Scope: MAC bring-up without the vendor's MCU patch code, PCIe EPHY tuning
 * and PHY micro-code (Aienux relies on the state the UEFI firmware leaves),
 * one TX queue in the vendor's "no close" mode (tail and close pointers, the
 * vendor build default), one RX queue with type 1 descriptors (RSS off, as
 * in the Spark's build), interrupts masked, no offloads, no jumbo frames.
 *
 * Device access goes through rtl_ops only: register reads and writes inside
 * BAR 2, a microsecond delay, and a memory barrier. No heap, no clock, no
 * I/O of its own. Both rings and every packet buffer live in ONE
 * caller-provided region (mem, len) that the chip sees at bus address `dma`
 * (the kernel makes it the device's only SMMU window). The driver writes
 * every descriptor from its own index (never trusts a descriptor address the
 * chip wrote back). Anything the chip reports is untrusted: a close pointer
 * that runs ahead of what was posted, or an RX length larger than the
 * buffer, marks the device broken (sticky); the caller must stop and
 * re-init. */
#ifndef AIENOS_RTL8127_H
#define AIENOS_RTL8127_H

#include <stddef.h>
#include <stdint.h>

#define RTL_PCI_VENDOR 0x10ecu
#define RTL_PCI_DEVICE 0x8127u
#define RTL_MMIO_BAR 2u            /* r8127_n.c init_board: BAR 2 is the MMIO region */
#define RTL_MMIO_MIN 0x5000u       /* highest register used is 0x4800 (Q_NUM_CTRL); Spark BAR 2 is 256 KiB */

/* Registers (offsets in BAR 2) */
#define RTL_MAC0 0x00u             /* r8127.h:1438 */
#define RTL_MAC4 0x04u
#define RTL_TNPDS_LO 0x20u         /* TxDescStartAddrLow, r8127.h:1444 */
#define RTL_TNPDS_HI 0x24u
#define RTL_INT_CFG0 0x34u         /* INT_CFG0_8125: bit 0 enable, 5 auto-clear IMR, 6 avoid-miss */
#define RTL_CHIPCMD 0x37u          /* r8127.h:1452 */
#define RTL_IMR0 0x38u             /* IMR0_8125 (32 bit, ISR version 1) */
#define RTL_ISR0 0x3Cu             /* ISR0_8125 */
#define RTL_TCR 0x40u              /* TxConfig */
#define RTL_RCR 0x44u              /* RxConfig */
#define RTL_CFG9346 0x50u
#define RTL_TIMER_INT0 0x58u       /* r8127_n.c:4360-4368 */
#define RTL_TIMER_INT1 0x5Cu
#define RTL_PHYSTATUS 0x6Cu        /* 16 bit */
#define RTL_PMCH 0x6Fu
#define RTL_TIMER_INT2 0x8Cu
#define RTL_MACOCP 0xB0u           /* MAC OCP window, r8127_n.c:3287-3314 */
#define RTL_PHYOCP 0xB8u           /* PHY OCP window, r8127_n.c:3057-3073 */
#define RTL_LLFIFO 0xD2u           /* 16 bit, bit 9 = LL share FIFO ready */
#define RTL_MCUCMD 0xD3u
#define RTL_RXDESC_TYPE 0xD8u      /* bit 1 = RX descriptor type 4 */
#define RTL_RMS 0xDAu              /* RxMaxSize, 16 bit */
#define RTL_INTRMIT 0xE2u          /* IntrMitigate, 16 bit */
#define RTL_RDSAR_LO 0xE4u         /* RxDescAddrLow */
#define RTL_RDSAR_HI 0xE8u
#define RTL_RXDV_GATE 0xF2u        /* bit 3 = gate closed */
#define RTL_TIMER_INT3 0xF4u
#define RTL_SW_TAIL0 0x0D30u       /* SW_TAIL_PTR0_8125BP, no-close version 6 */
#define RTL_HW_CLO0 0x0D34u        /* HW_CLO_PTR0_8125BP */
#define RTL_BACKUP_ADDR0 0x19E0u   /* 32 bit, the MAC address Linux uses */
#define RTL_BACKUP_ADDR1 0x19E4u   /* 16 bit */
#define RTL_TXNOCLOSE_EN 0x20E4u   /* bit 2 */
#define RTL_RSS_CTRL 0x4500u       /* RSS_CTRL_8125, r8127.h:1553 */
#define RTL_Q_NUM_CTRL 0x4800u     /* Q_NUM_CTRL_8125, r8127.h:1554; bits 4:2 = ilog2(RX queues) */
#define RTL_OCP_TXDESC_FMT 0xEB58u /* MAC OCP, bits 1:0 = 01 "new tx desc format" (32 B), r8127_n.c:14992 */

/* Bits */
#define RTL_CMD_STOPREQ 0x80u
#define RTL_CMD_RESET 0x10u
#define RTL_CMD_RXEN 0x08u
#define RTL_CMD_TXEN 0x04u
#define RTL_CFG9346_UNLOCK 0xC0u
#define RTL_RCR_ACCEPT_MASK 0x3Fu  /* Err Runt Bcast Mcast MyPhys AllPhys */
#define RTL_RCR_ACCEPT_BCAST 0x08u
#define RTL_RCR_ACCEPT_MYPHYS 0x02u
/* Chip table RCR for RTL8127 (r8127_n.c:119-122): Rx_Fetch_Number_8 |
 * Rx_Close_Multiple | RxCfg_pause_slot_en | EnableInnerVlan |
 * EnableOuterVlan | RX_DMA_BURST_512 << 8 */
#define RTL_RCR_CHIP 0x40E00D00u
#define RTL_RCR_RESET 0x00000500u  /* RX_DMA_BURST_512 << 8, r8127_n.c:4320-4321 */
/* TX_DMA_BURST_unlimited << 8 | InterFrameGap 3 << 24, plus bit 6 no-close */
#define RTL_TCR_VALUE 0x03000740u
#define RTL_TCR_ID_MASK 0x7c800000u
#define RTL_TCR_ID_8127 0x6C800000u
#define RTL_TCR_ICVER_MASK 0x00700000u
#define RTL_MCU_NOW_IS_OOB 0x80u
#define RTL_MCU_TXFIFO_EMPTY 0x20u
#define RTL_MCU_RXFIFO_EMPTY 0x10u
#define RTL_OCP_WRITE 0x80000000u
#define RTL_OCP_FLAG 0x80000000u
#define RTL_PHYS_LINK 0x0002u
#define RTL_PHYS_FULLDUP 0x0001u
#define RTL_RMS_VLAN 0x8000u       /* AcceppVlanPhys */

/* Descriptors (little-endian in memory) */
#define RTL_DESC_OWN 0x80000000u
#define RTL_DESC_EOR 0x40000000u   /* RingEnd */
#define RTL_DESC_FS 0x20000000u
#define RTL_DESC_LS 0x10000000u
#define RTL_RX_RES 0x00200000u     /* receive error summary (type 1) */
#define RTL_RX_LEN_MASK 0x3FFFu    /* includes the 4-byte FCS */
#define RTL_TXD_SIZE 32u           /* struct TxDesc, r8127.h:2056 */
#define RTL_RXD_SIZE 16u           /* struct RxDesc (type 1), r8127.h:2066 */
#define RTL_CLO_MASK 0x7FFFFFFFu   /* MAX_TX_NO_CLOSE_DESC_PTR_MASK_V4 */

/* Ring geometry used by this driver */
#define RTL_TX_N 16u
#define RTL_RX_N 16u
#define RTL_FRAME_MIN 60u          /* frames shorter than this are zero-padded */
#define RTL_FRAME_MAX 1514u        /* Ethernet II without FCS, no VLAN tag */
#define RTL_RX_BUF 1536u           /* posted RX buffer size */
#define RTL_RMS_VALUE 1522u        /* 1514 + 4 VLAN + 4 FCS */
#define RTL_BUF_STRIDE 2048u
#define RTL_OFF_TXD 0u
#define RTL_OFF_RXD 1024u
#define RTL_OFF_TXBUF 4096u
#define RTL_OFF_RXBUF (RTL_OFF_TXBUF + RTL_TX_N * RTL_BUF_STRIDE)
#define RTL_MEM_SIZE (RTL_OFF_RXBUF + RTL_RX_N * RTL_BUF_STRIDE)

typedef struct {
    void *ctx;
    /* width is 1, 2 or 4; off + width <= RTL_MMIO_MIN */
    uint32_t (*read)(void *ctx, uint32_t off, unsigned width);
    /* must make every earlier write to the shared region visible to the
     * device before the register write (the kernel issues a barrier first) */
    void (*write)(void *ctx, uint32_t off, unsigned width, uint32_t val);
    void (*delay_us)(void *ctx, uint32_t us);
    /* full memory barrier between descriptor reads and buffer reads */
    void (*barrier)(void *ctx);
} rtl_ops;

typedef enum {
    RTL_OK = 0,
    RTL_E_ARG = 1,            /* NULL, frame too short/long, buffer too small */
    RTL_E_MEM = 2,            /* region too small, misaligned, or bus range wraps */
    RTL_E_ABSENT = 3,         /* registers read all ones: no device behind the BAR */
    RTL_E_CHIP = 4,           /* TxConfig id is not an RTL8127 */
    RTL_E_RESET_TIMEOUT = 5,  /* ChipCmd reset bit never cleared */
    RTL_E_FULL = 6,           /* no free TX descriptor (reclaim first) */
    RTL_E_DEVICE = 7,         /* hostile or broken completion; device is now broken */
    RTL_E_BROKEN = 8,         /* sticky: an earlier RTL_E_DEVICE; stop and re-init */
    RTL_E_NOT_READY = 9,      /* rtl_init has not succeeded */
} rtl_err;

typedef enum { RTL_MAC_BACKUP = 1, RTL_MAC_MAC0 = 2, RTL_MAC_FALLBACK = 3 } rtl_mac_source;

typedef struct {
    rtl_ops ops;
    uint8_t *mem;
    uint64_t dma;
    size_t mem_len;
    uint8_t ready;
    uint8_t broken;
    uint8_t mcfg;             /* 1 or 2 (CFG_METHOD_n), from TxConfig */
    uint8_t ic_unknown;       /* TxConfig IC version neither 0 nor 1 */
    uint8_t mac[6];
    uint8_t mac_source;       /* rtl_mac_source */
    uint8_t ups_resume;       /* MAC OCP 0xD42C bit 8 was set (not handled) */
    uint8_t phy_state;        /* PHY OCP 0xA420 & 7 after power-up (3 = up) */
    uint32_t tcr_id;          /* TxConfig as read before reset */
    uint32_t wait_timeouts;   /* vendor waits that ran out (the vendor ignores them too) */
    uint32_t tx_cur;          /* frames posted (free running) */
    uint32_t tx_dirty;        /* frames known closed (free running) */
    uint32_t clo_base;        /* hardware close pointer read after reset */
    uint32_t rx_cur;          /* frames consumed (free running) */
    uint32_t rx_dropped;      /* error, runt or multi-descriptor frames dropped */
} rtl_dev;

/* Bytes of shared region the driver needs (RTL_MEM_SIZE). */
size_t rtl_mem_size(void);

/* Identify, stop, reset and bring up the chip: exit OOB, reset, PHY power
 * up, MAC address, rings, TX and RX enabled, interrupts masked. mem and dma
 * must be 4096-byte aligned, mem_len >= rtl_mem_size(), dma + mem_len must
 * not wrap. The region is zeroed. */
rtl_err rtl_init(rtl_dev *d, const rtl_ops *ops, void *mem, uint64_t dma, size_t mem_len);

/* Link state from PHYstatus: 1 up, 0 down or absent. *mbps (optional) gets
 * 10000/5000/2500/1000/100/10, or 0 when down or unknown. */
int rtl_link(rtl_dev *d, uint32_t *mbps);

/* Copy one Ethernet frame (14..RTL_FRAME_MAX bytes, no FCS; padded to 60)
 * into a free TX buffer, hand the descriptor to the chip, move the tail. */
rtl_err rtl_tx(rtl_dev *d, const uint8_t *frame, size_t len);

/* Consume TX completions from the close pointer; *reclaimed (optional). */
rtl_err rtl_tx_reclaim(rtl_dev *d, unsigned *reclaimed);

/* Frames posted and not yet closed. */
unsigned rtl_tx_pending(const rtl_dev *d);

/* Take at most one received frame: copies it (FCS stripped) to out, sets
 * *len and *got = 1, and gives the buffer back to the chip. *got 0: nothing
 * pending. Error, runt, multi-descriptor and larger-than-cap frames are
 * dropped (counted in rx_dropped) and their slot is reposted. A cap of
 * RTL_RX_BUF holds every frame the chip can deliver (VLAN tag included). */
rtl_err rtl_rx(rtl_dev *d, uint8_t *out, size_t cap, size_t *len, int *got);

/* Stop DMA (only on a chip rtl_init identified; RTL_E_NOT_READY otherwise,
 * nothing written): mask interrupts, close the RX gate, stop request, wait for the
 * FIFOs, soft reset. The rings are no longer in use when it returns OK. */
rtl_err rtl_stop(rtl_dev *d);

const char *rtl_err_name(rtl_err e);

#endif
