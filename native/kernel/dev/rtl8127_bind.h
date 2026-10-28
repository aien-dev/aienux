/* rtl8127_bind.h -- bind native/net's polled RTL8127 driver
 * (aienos_rtl8127.c) to the Spark's Realtek 10 GbE function (10ec:8127,
 * segment 7 on the DGX Spark) through ck.h services, and send the boot report
 * as UDP broadcasts (native/net/aienos_report.c, tag AIENUX-SPARK-REPORT,
 * port 47031) so attended boot #2 needs no photograph.
 *
 * DMA rule (same as NVMe and virtio-net): no SMMU confinement means no DMA.
 * The driver's one region (rings + buffers, RTL_MEM_SIZE) is allocated first
 * and becomes the stream's only SMMU window (ck_dma_confine); only then are
 * memory decoding and bus mastering enabled. No SMMU, or an SMMU that did
 * not come up: the chip stays off. There is no unconfined bypass.
 *
 * Built in every image; called from the devices stage only in a CK_RTL8127=1
 * build (the attended-boot image), so QEMU images are unchanged. Never run
 * on the chip yet: every register sequence is from the vendor driver
 * (DOCS SILENT), UNKNOWNs in docs/SPARK_RTL8127.md. */
#ifndef AIENOS_CK_RTL8127_BIND_H
#define AIENOS_CK_RTL8127_BIND_H
#include <stddef.h>
#include <stdint.h>
#include "pci.h"
enum {
    CK_RTL_OK = 0,
    CK_RTL_ABSENT = -601,   /* no 10ec:8127 function in the discovery */
    CK_RTL_E_ARG = -602,    /* bad function, BAR 2 not memory or too small, ECAM unmapped */
    CK_RTL_E_DMA = -603,    /* DMA region allocation failed */
    CK_RTL_E_DENIED = -604, /* no SMMU confinement: chip left off */
    CK_RTL_E_INIT = -605,   /* rtl_init failed */
    CK_RTL_E_LINK = -606,   /* no link within the wait */
    CK_RTL_E_TX = -607,     /* a report frame could not be built, sent or closed */
};
#define CK_RTL_LINK_WAIT_US 10000000u /* autonegotiation at 10G can take seconds (not measured) */
/* First 10ec:8127 function of the multi-segment discovery, read as an
 * endpoint into *out. CK_RTL_ABSENT when there is none. */
int ck_rtl8127_find(const pci_found *found, uint32_t n, pci_func *out);
/* Gate, map BAR 2, rtl_init, wait for link, then send the report (log, len)
 * every 2 s for `seconds` (at least one round), counting received frames.
 * Always releases the chip (stop, bus master off, stream to abort) before it
 * returns. Prints rtl8127:/dma_gate:/smmu: lines. */
int ck_rtl8127_report(const pci_func *f, const char *log, size_t len, uint32_t seconds);
/* Stop the chip, revoke bus mastering, return the stream to abort, if live.
 * Idempotent. */
void ck_rtl8127_release(void);
int ck_rtl8127_live(void);
#endif
