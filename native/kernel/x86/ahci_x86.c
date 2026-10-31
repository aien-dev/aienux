/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Drake Stapleton (AIEN / aien-dev)
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* ahci_x86.c -- AIENOS-IMAC-KERNEL-1: the SATA disks behind an AHCI
 * controller for the 2017 iMac (Intel 8086:a102, the 1 TB hard disk half of
 * the Fusion Drive) and QEMU q35 (ICH9). Read-only unless a write range is
 * opened.
 *
 * Each controller is driven polled,
 * with interrupts off: AHCI mode on, no controller or port reset (the links
 * the firmware brought up are used as they are), per port the command engine
 * stopped, the command list and received-FIS area moved into frames the
 * kernel owns, the engine started again. The PCI walk that finds the
 * controllers is pci_x86.c.
 *
 * Four ATA commands exist in this file: IDENTIFY DEVICE (0xEC), READ DMA
 * EXT (0x25), WRITE DMA EXT (0x35) and FLUSH CACHE EXT (0xEA); issue()
 * refuses every other opcode. Writes and flushes are refused unless the
 * transfer lies wholly inside a write range opened with ck_x86_ahci_allow,
 * and only x86/memory_x86.c opens ranges (the Makefile checks): the GPT
 * blocks of the approved hard disk while Allen's partition is added, then
 * the two memory copies inside that partition. A disk with no open range
 * cannot be changed. The IDENTIFY serial number words are never read. QEMU
 * runs of this path qualify nothing physical. */
#include "../core/ck_internal.h"
#include "../mm/frames.h"
#include "disk.h"
#include "disk_part.h"
#include "x86.h"

#define AHCI_MAX_CTRL 4u
#define AHCI_MAX_DISKS 8u
#define BOUNCE_BYTES (64u << 10)

/* HBA registers */
#define HBA_CAP 0x00
#define HBA_GHC 0x04
#define HBA_PI 0x0c
#define HBA_VS 0x10
#define HBA_CAP2 0x24
#define HBA_BOHC 0x28
#define CAP_S64A (1u << 31)
#define GHC_AE (1u << 31)
#define GHC_IE (1u << 1)
#define BOHC_BOS (1u << 0)
#define BOHC_OOS (1u << 1)
/* port registers, at 0x100 + 0x80 * port */
#define P_CLB 0x00
#define P_CLBU 0x04
#define P_FB 0x08
#define P_FBU 0x0c
#define P_IS 0x10
#define P_IE 0x14
#define P_CMD 0x18
#define P_TFD 0x20
#define P_SIG 0x24
#define P_SSTS 0x28
#define P_SERR 0x30
#define P_CI 0x38
#define CMD_ST (1u << 0)
#define CMD_FRE (1u << 4)
#define CMD_FR (1u << 14)
#define CMD_CR (1u << 15)
#define IS_TFES (1u << 30)
#define TFD_ERR 0x01u
#define TFD_DRQ 0x08u
#define TFD_BSY 0x80u
#define SIG_ATA 0x00000101u

#define ATA_IDENTIFY 0xec
#define ATA_READ_DMA_EXT 0x25
#define ATA_WRITE_DMA_EXT 0x35
#define ATA_FLUSH_CACHE_EXT 0xea
#define AHCI_MAX_ALLOW 3u

/* pci_x86.c: sector 0 and the GPT check of one disk, printed. */
void ck_x86_disk_report(const char *tag, unsigned ctrl, unsigned index, const disk_dev *dev);

struct ahci_disk {
    volatile uint8_t *port;
    uint8_t *cl, *fis, *ct, *bounce; /* kernel frames the controller reads and writes */
    uint64_t tsc_hz;
    unsigned ctrl, index;
    char model[41];
    uint64_t allow_first[AHCI_MAX_ALLOW], allow_last[AHCI_MAX_ALLOW]; /* open write ranges, inclusive */
    unsigned nallow;
    disk_dev dev;
};

static struct ahci_disk disks[AHCI_MAX_DISKS];
static unsigned ndisks;
static uint64_t g_tsc_hz;

static uint32_t mr(volatile uint8_t *b, unsigned off) { return *(volatile uint32_t *)(b + off); }
static void mw(volatile uint8_t *b, unsigned off, uint32_t v) { *(volatile uint32_t *)(b + off) = v; }

/* Waits until (reg & mask) == want, at most ms milliseconds of TSC. */
static int wait_reg(volatile uint8_t *b, unsigned off, uint32_t mask, uint32_t want, unsigned ms)
{
    uint64_t t0 = ck_rdtsc(), n = g_tsc_hz / 1000 * ms;
    while ((mr(b, off) & mask) != want) {
        if (ck_rdtsc() - t0 > n)
            return -1;
        ck_pause();
    }
    return 0;
}

static int alloc_low(struct ck_frames *f, uint64_t pages, int low4g, uint8_t **out)
{
    uint64_t pa;
    if (ck_frames_alloc(f, pages, CK_PAGE, &pa))
        return -1;
    if (low4g && pa + pages * CK_PAGE > (1ull << 32)) {
        ck_frames_free(f, pa, pages);
        return -1;
    }
    memset((void *)(uintptr_t)pa, 0, pages * CK_PAGE);
    *out = (uint8_t *)(uintptr_t)pa;
    return 0;
}

static uint64_t pa_of(const void *p) { return (uint64_t)(uintptr_t)p; }

/* One command in slot 0, polled. Only IDENTIFY DEVICE, READ DMA EXT, WRITE
 * DMA EXT (the data already in the bounce buffer) and FLUSH CACHE EXT (no
 * data, bytes 0). */
static int issue(struct ahci_disk *d, uint8_t cmd, uint64_t lba, uint32_t count, uint32_t bytes)
{
    int rw = cmd == ATA_READ_DMA_EXT || cmd == ATA_WRITE_DMA_EXT;
    if (cmd != ATA_IDENTIFY && !rw && cmd != ATA_FLUSH_CACHE_EXT)
        return DISK_EARG;
    if ((cmd == ATA_FLUSH_CACHE_EXT) != !bytes || bytes > BOUNCE_BYTES)
        return DISK_EARG;
    volatile uint8_t *p = d->port;
    if (wait_reg(p, P_TFD, TFD_BSY | TFD_DRQ, 0, 1000))
        return DISK_ESTATE;
    uint32_t *hdr = (uint32_t *)d->cl;
    /* CFL 5 dwords; W (bit 6) for a write; one PRD, none for a flush */
    hdr[0] = 5u | (cmd == ATA_WRITE_DMA_EXT ? 1u << 6 : 0u) | (bytes ? 1u << 16 : 0u);
    hdr[1] = 0;
    hdr[2] = (uint32_t)pa_of(d->ct);
    hdr[3] = (uint32_t)(pa_of(d->ct) >> 32);
    memset(d->ct, 0, 128 + 16);
    uint8_t *f = d->ct;
    f[0] = 0x27; /* host to device register FIS */
    f[1] = 0x80; /* command */
    f[2] = cmd;
    if (rw) {
        f[4] = (uint8_t)lba;
        f[5] = (uint8_t)(lba >> 8);
        f[6] = (uint8_t)(lba >> 16);
        f[7] = 0x40; /* LBA mode */
        f[8] = (uint8_t)(lba >> 24);
        f[9] = (uint8_t)(lba >> 32);
        f[10] = (uint8_t)(lba >> 40);
        f[12] = (uint8_t)count;
        f[13] = (uint8_t)(count >> 8);
    }
    uint32_t *prd = (uint32_t *)(d->ct + 128);
    prd[0] = (uint32_t)pa_of(d->bounce);
    prd[1] = (uint32_t)(pa_of(d->bounce) >> 32);
    prd[2] = 0;
    prd[3] = bytes ? bytes - 1u : 0u;
    __asm__ volatile("mfence" ::: "memory");
    mw(p, P_IS, 0xffffffffu);
    mw(p, P_CI, 1);
    uint64_t t0 = ck_rdtsc(), lim = d->tsc_hz * 30; /* a spun-down hard disk spins up first; a flush
                                                    * may also wait for the cache to reach the platters */
    for (;;) {
        if (mr(p, P_IS) & IS_TFES)
            return DISK_EIO;
        if (!(mr(p, P_CI) & 1))
            break;
        if (ck_rdtsc() - t0 > lim)
            return DISK_EIO;
        ck_pause();
    }
    if (mr(p, P_TFD) & TFD_ERR)
        return DISK_EIO;
    __asm__ volatile("mfence" ::: "memory");
    return hdr[1] == bytes ? DISK_OK : DISK_EIO;
}

static int ahci_read(void *ctx, uint64_t lba, uint32_t count, uint8_t *buf)
{
    struct ahci_disk *d = ctx;
    uint32_t bytes = count * d->dev.block_size;
    int rc = issue(d, ATA_READ_DMA_EXT, lba, count, bytes);
    if (rc == DISK_OK)
        memcpy(buf, d->bounce, bytes);
    return rc;
}

/* Inside one open write range, as a whole. */
static int allowed(const struct ahci_disk *d, uint64_t lba, uint32_t count)
{
    uint64_t last = lba + count - 1u;
    if (!count || last < lba)
        return 0;
    for (unsigned i = 0; i < d->nallow; i++)
        if (lba >= d->allow_first[i] && last <= d->allow_last[i])
            return 1;
    return 0;
}

static int ahci_write(void *ctx, uint64_t lba, uint32_t count, const uint8_t *buf)
{
    struct ahci_disk *d = ctx;
    if (!allowed(d, lba, count)) {
        ck_printf("ahci_write: ctrl=%u disk=%u lba=%llu count=%u refused: outside the open write ranges\n", d->ctrl,
                  d->index, (unsigned long long)lba, count);
        return DISK_ERANGE;
    }
    uint32_t bytes = count * d->dev.block_size;
    memcpy(d->bounce, buf, bytes);
    return issue(d, ATA_WRITE_DMA_EXT, lba, count, bytes);
}

static int ahci_flush(void *ctx)
{
    struct ahci_disk *d = ctx;
    if (!d->nallow)
        return DISK_ESTATE; /* nothing could have been written */
    return issue(d, ATA_FLUSH_CACHE_EXT, 0, 0, 0);
}

static void id_string(const uint16_t *id, unsigned w0, unsigned words, char *out)
{
    unsigned n = 0;
    for (unsigned i = 0; i < words; i++) {
        char a = (char)(id[w0 + i] >> 8), b = (char)id[w0 + i];
        out[n++] = a >= 0x20 && a < 0x7f ? a : '?';
        out[n++] = b >= 0x20 && b < 0x7f ? b : '?';
    }
    while (n && out[n - 1] == ' ')
        n--;
    out[n] = 0;
}

static void port_stop(volatile uint8_t *p)
{
    mw(p, P_CMD, mr(p, P_CMD) & ~CMD_ST);
    wait_reg(p, P_CMD, CMD_CR, 0, 500);
    mw(p, P_CMD, mr(p, P_CMD) & ~CMD_FRE);
    wait_reg(p, P_CMD, CMD_FR, 0, 500);
}

static void port_bringup(unsigned ctrl, volatile uint8_t *hba, unsigned pn, struct ck_frames *fr, int low4g)
{
    volatile uint8_t *p = hba + 0x100 + 0x80 * pn;
    uint32_t ssts = mr(p, P_SSTS), sig = mr(p, P_SIG);
    unsigned det = ssts & 0xf, spd = (ssts >> 4) & 0xf;
    if (det != 3) {
        ck_printf("ahci_port: ctrl=%u port=%u link=none det=%u\n", ctrl, pn, det);
        return;
    }
    if (sig != SIG_ATA) {
        ck_printf("ahci_port: ctrl=%u port=%u link=gen%u sig=0x%08x not an ATA disk, left alone\n", ctrl, pn, spd,
                  sig);
        return;
    }
    if (ndisks == AHCI_MAX_DISKS) {
        ck_printf("ahci_port: ctrl=%u port=%u skipped: more than %u disks\n", ctrl, pn, AHCI_MAX_DISKS);
        return;
    }
    struct ahci_disk *d = &disks[ndisks];
    uint8_t *page, *bounce;
    if (alloc_low(fr, 1, low4g, &page) || alloc_low(fr, BOUNCE_BYTES / CK_PAGE, low4g, &bounce)) {
        ck_printf("ahci_port: ctrl=%u port=%u no DMA frames\n", ctrl, pn);
        return;
    }
    d->port = p;
    d->cl = page;           /* 1 KiB command list */
    d->fis = page + 1024;   /* 256 B received FIS */
    d->ct = page + 2048;    /* command table: 128 B header + one PRD */
    d->bounce = bounce;
    d->tsc_hz = g_tsc_hz;
    d->ctrl = ctrl;
    d->index = ndisks;

    port_stop(p);
    if (mr(p, P_CMD) & (CMD_CR | CMD_FR)) {
        ck_printf("ahci_port: ctrl=%u port=%u engine did not stop cmd=0x%x\n", ctrl, pn, mr(p, P_CMD));
        return;
    }
    mw(p, P_CLB, (uint32_t)pa_of(d->cl));
    mw(p, P_CLBU, (uint32_t)(pa_of(d->cl) >> 32));
    mw(p, P_FB, (uint32_t)pa_of(d->fis));
    mw(p, P_FBU, (uint32_t)(pa_of(d->fis) >> 32));
    mw(p, P_IE, 0);
    mw(p, P_SERR, 0xffffffffu);
    mw(p, P_IS, 0xffffffffu);
    mw(p, P_CMD, mr(p, P_CMD) | CMD_FRE);
    if (wait_reg(p, P_TFD, TFD_BSY | TFD_DRQ, 0, 1000)) {
        ck_printf("ahci_port: ctrl=%u port=%u device busy tfd=0x%x\n", ctrl, pn, mr(p, P_TFD));
        return;
    }
    mw(p, P_CMD, mr(p, P_CMD) | CMD_ST);

    if (issue(d, ATA_IDENTIFY, 0, 0, 512) != DISK_OK) {
        ck_printf("ahci_port: ctrl=%u port=%u IDENTIFY failed tfd=0x%x is=0x%x\n", ctrl, pn, mr(p, P_TFD),
                  mr(p, P_IS));
        port_stop(p);
        return;
    }
    const uint16_t *id = (const uint16_t *)d->bounce;
    char model[41];
    id_string(id, 27, 20, model); /* words 10..19 (serial number) are never read */
    memcpy(d->model, model, sizeof d->model);
    d->nallow = 0;
    uint64_t sectors = 0;
    int lba48 = (id[83] & (1u << 10)) != 0;
    if (lba48)
        for (int i = 3; i >= 0; i--)
            sectors = sectors << 16 | id[100 + i];
    uint32_t bs = 512;
    if ((id[106] & 0xc000) == 0x4000 && (id[106] & (1u << 12)))
        bs = ((uint32_t)id[118] << 16 | id[117]) * 2u;
    if (!lba48 || !sectors || (bs != 512 && bs != 4096)) {
        ck_printf("ahci_port: ctrl=%u port=%u model=\"%s\" unsupported lba48=%d sectors=%llu block=%u\n", ctrl, pn,
                  model, lba48, (unsigned long long)sectors, bs);
        port_stop(p);
        return;
    }
    d->dev.ctx = d;
    d->dev.block_size = bs;
    d->dev.block_count = sectors;
    d->dev.read = ahci_read;
    d->dev.write = ahci_write;
    d->dev.flush = ahci_flush;
    d->dev.max_blocks_per_io = BOUNCE_BYTES / bs;
    ndisks++;
    ck_printf("ahci_disk: ctrl=%u disk=%u port=%u link=gen%u model=\"%s\" blocks=%llu block=%u mib=%llu "
              "access=read-only\n", ctrl, d->index, pn, spd, model, (unsigned long long)sectors, bs,
              (unsigned long long)(sectors * bs >> 20));
    ck_x86_disk_report("ahci", d->ctrl, d->index, &d->dev);
}

void ck_x86_ahci_start(unsigned ctrl, uint64_t abar, struct ck_frames *fr, uint64_t tsc_hz)
{
    g_tsc_hz = tsc_hz;
    volatile uint8_t *hba = (volatile uint8_t *)(uintptr_t)abar;
    uint32_t cap = mr(hba, HBA_CAP);
    /* Taking the controller from the firmware (AHCI 1.3 section 10.6): ask,
     * then wait up to 2 s for the firmware to let go. */
    if (mr(hba, HBA_CAP2) & 1u) {
        mw(hba, HBA_BOHC, mr(hba, HBA_BOHC) | BOHC_OOS);
        wait_reg(hba, HBA_BOHC, BOHC_BOS, 0, 2000);
    }
    mw(hba, HBA_GHC, (mr(hba, HBA_GHC) | GHC_AE) & ~GHC_IE);
    uint32_t pi = mr(hba, HBA_PI), vs = mr(hba, HBA_VS);
    ck_printf("ahci: ctrl=%u abar=0x%llx version=%x.%x%x ports=%u implemented=0x%x s64a=%u polled=yes\n", ctrl,
              (unsigned long long)abar, vs >> 16, (vs >> 8) & 0xff, vs & 0xff, (cap & 0x1f) + 1, pi,
              (cap & CAP_S64A) ? 1u : 0u);
    for (unsigned pn = 0; pn < 32; pn++)
        if (pi & (1u << pn))
            port_bringup(ctrl, hba, pn, fr, !(cap & CAP_S64A));
}

unsigned ck_x86_ahci_disks(void) { return ndisks; }

const disk_dev *ck_x86_ahci_dev(unsigned index, const char **model)
{
    if (index >= ndisks)
        return 0;
    if (model)
        *model = disks[index].model;
    return &disks[index].dev;
}

int ck_x86_ahci_allow(unsigned index, uint64_t first, uint64_t last)
{
    if (index >= ndisks)
        return -1;
    struct ahci_disk *d = &disks[index];
    if (d->nallow == AHCI_MAX_ALLOW || first == 0 || first > last || last >= d->dev.block_count)
        return -1; /* LBA 0 (the protective MBR) is never opened */
    d->allow_first[d->nallow] = first;
    d->allow_last[d->nallow] = last;
    d->nallow++;
    ck_printf("ahci_disk: ctrl=%u disk=%u writes allowed lba=%llu..%llu\n", d->ctrl, index,
              (unsigned long long)first, (unsigned long long)last);
    return 0;
}

void ck_x86_ahci_allow_none(unsigned index)
{
    if (index < ndisks && disks[index].nallow) {
        disks[index].nallow = 0;
        ck_printf("ahci_disk: ctrl=%u disk=%u write ranges closed\n", disks[index].ctrl, index);
    }
}
