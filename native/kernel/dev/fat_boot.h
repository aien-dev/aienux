/* fat_boot.h -- the AIENOS boot section: a FAT32 volume (Microsoft FAT32
 * File System Specification 1.03; UEFI 2.10 section 13.3: FAT32 for a system
 * partition) on an EFI System Partition that AIENOS added itself, holding
 *
 *     \EFI\BOOT\BOOTX64.EFI   the kernel image, started by the firmware
 *     \EFI\BOOT\BOOTX64.MAN   its signed slot manifest (boot/slot_format.h)
 *
 * so a machine starts AIENOS from its own disk with no stick and no network.
 *
 * Layout, all fixed by the partition size (512-byte sectors only):
 *   sector 0 boot sector, 1 FSInfo, 6 and 7 their backups; the reserved
 *   region is padded so the data region starts on a 4 KiB boundary
 *   (UEFI: "aligned to the physical block boundary"); two FATs; one sector
 *   per cluster. Cluster 2 is the root (volume label AIENOS, directory EFI),
 *   3 is \EFI, 4 is \EFI\BOOT. The rest holds two image slots, A and B, each
 *   starting on a 128-cluster boundary so their FAT sectors never overlap:
 *   the manifest at the slot's first cluster, the image 128 clusters later,
 *   each one contiguous chain.
 *
 * Installing a new image never touches the files the firmware starts now:
 * the new manifest and image go into the other slot (its FAT sectors, then
 * its data), flush; then the one sector of \EFI\BOOT is rewritten to name
 * the new slot, flush; then the old slot's FAT sectors are freed, flush;
 * then everything is read back. A power cut leaves the old kernel or the new
 * one, never a mix (at worst the old slot stays allocated with no name,
 * "lost clusters", freed by the next install).
 *
 * A volume is "ours" only when its boot sector is byte for byte the one
 * ck_fatb_format writes for that size and volume serial; anything else is
 * formatted only if the caller says so (the partition itself must already
 * be AIENOS's, gpt_add.h ck_gpt_is_ours). Freestanding: no libc, no heap,
 * I/O through disk_read/disk_write/disk_flush on the partition view only.
 * Host tested in dev/tests/fat_boot_test.c (with fsck.fat and mtools as
 * outside readers). */
#ifndef AIENOS_CK_FAT_BOOT_H
#define AIENOS_CK_FAT_BOOT_H
#include <stdint.h>
#include "disk.h"

/* EFI System Partition type C12A7328-F81F-11D2-BA4B-00A0C93EC93B (UEFI 2.10
 * table 5-7), on-disk byte order. */
extern const uint8_t ck_esp_part_type[16];

#define CK_FATB_MAN_BYTES 4096u /* a slot manifest */
#define CK_FATB_MIN_MIB 64u     /* smallest partition handled */

enum {
    CK_FATB_OK = 0,
    CK_FATB_EGEOM = -440,   /* not 512-byte sectors, or too small for FAT32 with two slots */
    CK_FATB_EIO = -441,     /* a read, write or flush failed */
    CK_FATB_ENOTOURS = -442, /* the volume is not one ck_fatb_format wrote */
    CK_FATB_EBIG = -443,    /* the image is larger than a slot */
    CK_FATB_EVERIFY = -444, /* read back, the files are not what was written */
};

typedef struct {
    int ours;              /* the boot sector is ck_fatb_format's */
    int slot;              /* 0 or 1: the slot \EFI\BOOT names; -1: none (or not valid) */
    uint32_t image_len;    /* BOOTX64.EFI's size when slot >= 0 */
    uint32_t slot_max;     /* the largest image a slot holds, in bytes */
    /* when not ours: the first byte that differs from what ck_fatb_format
     * wrote (where = "boot sector", "FAT 1", "FAT 2", "root", "EFI" or
     * "BOOT"; 0 when the geometry or a read failed) */
    const char *where;
    uint32_t at;
    uint8_t got, want;
} ck_fatb_state;

/* Reads the volume: is it ours, and which slot holds a complete, valid pair
 * of files (directory entries, contiguous FAT chains ending in the
 * end-of-chain mark). Reads only. Fields that FAT drivers change in normal use
 * are not compared: the boot sector's BS_Reserved1 (the NT dirty flag),
 * FAT[1]'s ClnShutBitMask and HrdErrBitMask, each directory entry's
 * DIR_LstAccDate, and ATTR_ARCHIVE, DIR_WrtTime and DIR_WrtDate on the
 * expected directory entries (Apple's firmware sets them on the EFI directory
 * when it starts the kernel).
 * CK_FATB_OK (also for "not ours") or CK_FATB_EGEOM / CK_FATB_EIO. */
int ck_fatb_state_read(const disk_dev *p, uint32_t serial, ck_fatb_state *st);

/* Writes an empty AIENOS boot volume over the whole partition view p: both
 * FATs, the three directories, then (last, after a flush) the FSInfo and
 * boot sectors and their backups. */
int ck_fatb_format(const disk_dev *p, uint32_t serial);

typedef struct {
    int formatted;   /* 1: the volume was not ours and was formatted first */
    int written;     /* 1: a new image was installed; 0: the same one was already there */
    int slot;        /* the slot the firmware now starts */
    uint32_t sectors_written;
} ck_fatb_result;

/* Makes BOOTX64.EFI = img and BOOTX64.MAN = man (CK_FATB_MAN_BYTES bytes),
 * formatting first when the volume is not ours and format_ok is set. When
 * the current slot already holds exactly these bytes nothing is written.
 * `dos_date`/`dos_time`: the FAT time stamps for the new entries. */
int ck_fatb_install(const disk_dev *p, uint32_t serial, int format_ok, const uint8_t *man, const uint8_t *img,
                    uint32_t img_len, uint16_t dos_date, uint16_t dos_time, ck_fatb_result *out);

/* FAT date and time (spec "Date and Time Formats") of a Unix time, UTC;
 * times before 1980 give 1980-01-01 00:00. */
void ck_fatb_dos_time(uint64_t unix_s, uint16_t *date, uint16_t *time);

const char *ck_fatb_strerror(int rc);
#endif
