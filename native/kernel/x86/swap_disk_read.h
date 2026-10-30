/* swap_disk_read.h -- bounds-checked read of the QEMU test-disk slot object.
 *
 * Header only so a hosted test can drive it with a mocked sector reader. The includer
 * supplies memcmp(), memcpy(), ck_puts() and ck_printf(); the kernel gets them from
 * ck_internal.h. */
#ifndef AIENOS_CK_SWAP_DISK_READ_H
#define AIENOS_CK_SWAP_DISK_READ_H
#include <stdint.h>
#include <stddef.h>
#include "../../boot/slot_format.h"

#define CK_SWAP_SECTOR 512u

/* Reads `count` sectors from `lba` into `buf`; 0 on success. */
typedef int (*ck_swap_sector_reader)(uint64_t lba, unsigned count, uint8_t *buf);

/* Sectors needed for the whole slot object (manifest unit + image) when the image is
 * img_len bytes and the buffer holds cap bytes. Sets *total to the object length and
 * *nsectors to the sector count. Returns 0 only if the sector-rounded read fits in cap:
 * the disk reads whole sectors, so the rounded length is what lands in the buffer.
 * Nothing here can overflow: img_len is compared against cap - CK_SLOT_UNIT before it
 * is added to anything. */
static inline int ck_swap_disk_extent(size_t cap, uint64_t img_len, size_t *total,
                                      unsigned *nsectors)
{
    if (cap < CK_SLOT_UNIT)
        return -1;
    if (img_len > (uint64_t)(cap - CK_SLOT_UNIT))
        return -1;
    size_t t = (size_t)CK_SLOT_UNIT + (size_t)img_len;
    size_t sectors = t / CK_SWAP_SECTOR + ((t % CK_SWAP_SECTOR) != 0);
    if (sectors == 0 || sectors > (size_t)~0u)
        return -1;
    if (sectors > cap / CK_SWAP_SECTOR)   /* sectors * 512 > cap, without the multiply */
        return -1;
    *total = t;
    *nsectors = (unsigned)sectors;
    return 0;
}

/* Reads the raw slot object at lba. Returns 0 and the object length on success; no
 * sector is read into buf past cap bytes. The disk content is untrusted. */
static inline int ck_swap_disk_load(ck_swap_sector_reader rd, uint64_t lba, uint8_t *buf,
                                    size_t cap, size_t *len_out)
{
    static uint8_t header[CK_SWAP_SECTOR];
    *len_out = 0;
    if (rd(lba, 1, header) != 0) {
        ck_puts("swap_inbox: test disk not readable\n");
        return -1;
    }
    if (memcmp(header, CK_SLOT_MANIFEST_MAGIC, 8) != 0) {
        ck_puts("swap_inbox: no slot object at the test LBA\n");
        return -1;
    }
    uint64_t img_len64;
    memcpy(&img_len64, header + 48, 8);   /* manifest.image_len (u64 at offset 48) */
    size_t total;
    unsigned nsectors;
    if (ck_swap_disk_extent(cap, img_len64, &total, &nsectors) != 0) {
        ck_puts("swap_inbox: slot too large\n");
        return -1;
    }
    if (rd(lba, nsectors, buf) != 0) {
        ck_puts("swap_inbox: read failed\n");
        return -1;
    }
    *len_out = total;
    ck_printf("swap: inbox at LBA %u img_len=%llu total=%zu\n", (unsigned)lba,
              (unsigned long long)img_len64, total);
    return 0;
}
#endif
