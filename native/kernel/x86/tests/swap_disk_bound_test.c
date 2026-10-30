/* Hosted verification oracle for swap_disk_read.h (QEMU-only test-disk inbox). A mocked
 * sector reader stands in for AHCI and records every sector it is asked to write, so the
 * test sees both the accept/reject decision and that a rejected object is never read. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#define ck_puts(s)    ((void)(s))
#define ck_printf(...) ((void)0)
#include "../swap_disk_read.h"

#define CHECK(v) do { if (!(v)) { fprintf(stderr, "swap disk bound failed at %d\n", __LINE__); return 1; } count++; } while (0)

static uint64_t g_img_len;       /* image_len the mocked disk reports in the manifest */
static unsigned g_reads;         /* reads of more than the header sector */
static unsigned g_max_sectors;   /* largest bulk read requested */
static int g_bad_magic, g_fail_bulk;

static int mock_reader(uint64_t lba, unsigned count, uint8_t *buf)
{
    (void)lba;
    memset(buf, 0xA5, (size_t)count * CK_SWAP_SECTOR);
    if (count == 1) {
        memset(buf, 0, CK_SWAP_SECTOR);
        memcpy(buf, g_bad_magic ? "NOTASLOT" : CK_SLOT_MANIFEST_MAGIC, 8);
        memcpy(buf + 48, &g_img_len, 8);
        return 0;
    }
    g_reads++;
    if (g_fail_bulk)
        return -1;
    if (count > g_max_sectors)
        g_max_sectors = count;
    return 0;
}

/* Returns the load result; fills *bulk with the number of bulk reads issued. */
static int load(size_t cap, uint64_t img_len, size_t *len, unsigned *bulk, uint8_t *buf)
{
    g_img_len = img_len; g_reads = 0; g_max_sectors = 0;
    int rc = ck_swap_disk_load(mock_reader, 2048, buf, cap, len);
    *bulk = g_reads;
    return rc;
}

int main(void)
{
    unsigned count = 0, bulk;
    size_t len, total;
    unsigned ns;
    enum { CAP = 4u << 20 };
    uint8_t *buf = malloc(CAP + 8192);
    CHECK(buf);

    /* Exact boundary through the real function: cap - manifest accepted, one over rejected. */
    CHECK(load(CAP, CAP - CK_SLOT_UNIT, &len, &bulk, buf) == 0);
    CHECK(len == CAP && bulk == 1 && g_max_sectors == CAP / 512);
    CHECK(load(CAP, CAP - CK_SLOT_UNIT + 1, &len, &bulk, buf) != 0);
    CHECK(len == 0 && bulk == 0);
    /* The old check let img_len == cap through; the read then ran 4096 bytes past cap. */
    CHECK(load(CAP, CAP, &len, &bulk, buf) != 0 && bulk == 0);
    CHECK(load(CAP, CAP + 1, &len, &bulk, buf) != 0 && bulk == 0);
    CHECK(load(CAP, 0, &len, &bulk, buf) == 0 && len == CK_SLOT_UNIT && g_max_sectors == 8);

    /* Buffer smaller than the manifest unit, including zero. */
    CHECK(load(0, 0, &len, &bulk, buf) != 0 && bulk == 0);
    CHECK(load(CK_SLOT_UNIT - 1, 0, &len, &bulk, buf) != 0 && bulk == 0);
    CHECK(load(CK_SLOT_UNIT, 0, &len, &bulk, buf) == 0 && len == CK_SLOT_UNIT);
    CHECK(load(CK_SLOT_UNIT, 1, &len, &bulk, buf) != 0 && bulk == 0);

    /* Partial-sector rounding: the object fits in bytes but its rounded read does not.
     * cap = manifest + 100 leaves a 100-byte image needing a 4608-byte (9-sector) read. */
    CHECK(load(CK_SLOT_UNIT + 100, 100, &len, &bulk, buf) != 0 && bulk == 0);
    CHECK(load(CK_SLOT_UNIT + 512, 512, &len, &bulk, buf) == 0 && len == CK_SLOT_UNIT + 512);
    CHECK(load(CK_SLOT_UNIT + 512, 513, &len, &bulk, buf) != 0 && bulk == 0);
    CHECK(load(CK_SLOT_UNIT + 511, 511, &len, &bulk, buf) != 0 && bulk == 0);
    /* Image shorter than a sector under a sector-aligned cap rounds up inside cap. */
    CHECK(load(CK_SLOT_UNIT + 512, 100, &len, &bulk, buf) == 0 &&
          len == CK_SLOT_UNIT + 100 && g_max_sectors == 9);

    /* Overflow-safe: lengths near the top of u64 and size_t are refused, never wrapped. */
    CHECK(load(CAP, UINT64_MAX, &len, &bulk, buf) != 0 && bulk == 0);
    CHECK(load(CAP, UINT64_MAX - CK_SLOT_UNIT + 1, &len, &bulk, buf) != 0 && bulk == 0);
    CHECK(load(CAP, (uint64_t)SIZE_MAX, &len, &bulk, buf) != 0 && bulk == 0);
    CHECK(ck_swap_disk_extent(SIZE_MAX, UINT64_MAX, &total, &ns) != 0);
    CHECK(ck_swap_disk_extent(SIZE_MAX, SIZE_MAX - CK_SLOT_UNIT, &total, &ns) != 0);
    CHECK(ck_swap_disk_extent((size_t)1 << 45, ((uint64_t)1 << 45) - CK_SLOT_UNIT, &total, &ns) != 0);   /* sector count over u32 */

    /* Helper contract: success fills total and a nonzero sector count. */
    CHECK(ck_swap_disk_extent(CAP, 0, &total, &ns) == 0 && total == CK_SLOT_UNIT && ns == 8);
    CHECK(ck_swap_disk_extent(CAP, 1, &total, &ns) == 0 && total == CK_SLOT_UNIT + 1 && ns == 9);

    /* Bad magic and an unreadable disk are refused before any bulk read. */
    g_bad_magic = 1;
    CHECK(load(CAP, 0, &len, &bulk, buf) != 0 && bulk == 0 && len == 0);
    g_bad_magic = 0; g_fail_bulk = 1;
    CHECK(load(CAP, 0, &len, &bulk, buf) != 0 && bulk == 1 && len == 0);
    free(buf);
    printf("SWAP_DISK_BOUND: PASS checks=%u\n", count);
    return 0;
}
