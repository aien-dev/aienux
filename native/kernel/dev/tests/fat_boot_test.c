/* fat_boot_test.c -- host tests for the AIENOS boot section:
 *   1. the EFI System Partition added in unused space next to an existing
 *      partition full of sentinel bytes (dev/gpt_add.c ck_gpt_add), known as
 *      ours afterwards; a foreign one is not;
 *   2. a volume that is not ours is refused unless formatting is allowed;
 *      installed: only blocks inside the partition change; the same image
 *      again writes nothing; a new image goes to the other slot;
 *   3. a power cut after every possible number of written blocks (sampled,
 *      plus the exact switch point) leaves exactly the old or the new
 *      kernel, old before one point and new after it, and the next install
 *      completes;
 *   4. FAT time stamps.
 * Writes part1.img/img1.bin, part2.img/img2.bin and part3.img into the
 * folder given, for scripts/ck_fat_boot_test.sh to check with fsck.fat and
 * mtools (outside readers). Hosted test code only (libc). */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "disk_file.h"
#include "disk_part.h"
#include "fat_boot.h"
#include "gpt_add.h"
#include "gpt_write.h"

static int checks, failures;
#define CHECK(c)                                                                 \
    do {                                                                         \
        checks++;                                                                \
        if (!(c)) {                                                              \
            failures++;                                                          \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                \
        }                                                                        \
    } while (0)

#define BS 512u
#define MIB (1024u * 1024u / BS)
static const ck_gpt_new esp = {ck_esp_part_type, "AIENOS boot", "aienos.boot.v1"};
static const uint32_t serial = 0x1234abcdu;
static char g_dir[400], g_path[512];

static uint8_t *slurp(const char *path, size_t *len)
{
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return NULL;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n);
    if (b && fread(b, 1, (size_t)n, fp) != (size_t)n) {
        free(b);
        b = NULL;
    }
    fclose(fp);
    *len = (size_t)n;
    return b;
}

static int spill(const char *name, const uint8_t *b, size_t n)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", g_dir, name);
    /* 0644, not fopen's 0666 & ~umask: the images must not be world-writable. */
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    FILE *fp = fd < 0 ? NULL : fdopen(fd, "wb");
    if (fd >= 0 && !fp)
        close(fd);
    if (!fp)
        return -1;
    size_t w = fwrite(b, 1, n, fp);
    return fclose(fp) == 0 && w == n ? 0 : -1;
}

static void pattern(uint8_t *b, size_t n, uint32_t seed)
{
    for (size_t i = 0; i < n; i++) {
        seed = seed * 1103515245u + 12345u;
        b[i] = (uint8_t)(seed >> 16);
    }
}

/* the partition's bytes from the image file, written to name */
static int export_part(const ck_part *p, const char *name)
{
    size_t n = 0;
    uint8_t *all = slurp(g_path, &n);
    int rc = all ? spill(name, all + p->first_lba * BS, p->blocks * BS) : -1;
    free(all);
    return rc;
}

/* xors byte at of the partition's sector lba with x, through the disk itself */
static int poke(const ck_part *p, uint32_t lba, uint32_t at, uint8_t x)
{
    uint8_t s[BS];
    if (disk_read(&p->dev, lba, 1, s) != DISK_OK)
        return -1;
    s[at] ^= x;
    return disk_write(&p->dev, lba, 1, s) == DISK_OK ? 0 : -1;
}

/* the changes FAT drivers make in normal use: BS_Reserved1, FAT[1]'s two
 * flags in both FATs, DIR_LstAccDate in the root and in BOOT */
static int driver_marks(const ck_part *p, uint32_t rsv, uint32_t fatsz)
{
    uint32_t root = rsv + 2u * fatsz;
    return poke(p, 0, 65, 0x01) || poke(p, rsv, 7, 0x0c) || poke(p, rsv + fatsz, 7, 0x08) ||
           poke(p, root, 32 + 18, 0x5a) || poke(p, root + 2, 19, 0x01);
}

static int install(disk_file *f, const ck_part *p, int fmt, const uint8_t *man, const uint8_t *img, uint32_t len,
                   ck_fatb_result *r, uint64_t *blocks)
{
    uint64_t b0 = f->blocks_written;
    int rc = ck_fatb_install(&p->dev, serial, fmt, man, img, len, 23890, 12149, r);
    if (blocks)
        *blocks = f->blocks_written - b0;
    return rc;
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return 2;
    snprintf(g_dir, sizeof g_dir, "%s", argv[1]);
    snprintf(g_path, sizeof g_path, "%s/disk.img", g_dir);
    const uint64_t disk = 96 * MIB;

    /* 1. the partition */
    gw_part sentinel[1] = {{gw_linux_type, 1 * MIB, 9 * MIB - 1u, "old system", 0xa5}};
    CHECK(gw_write(g_path, BS, disk, sentinel, 1, 0) == 0);
    disk_file f;
    disk_dev d;
    CHECK(disk_file_open(&f, &d, g_path, BS, 0, 0) == 0);
    ck_gpt_added a;
    CHECK(ck_gpt_add(&d, &esp, 64 * MIB, MIB, &a) == CK_GPT_OK);
    CHECK(a.created == 1 && a.first == 9 * MIB && a.last == 73 * MIB - 1u && a.index == 1);
    CHECK(ck_gpt_is_ours(&d, &esp, a.index) == 1);
    CHECK(ck_gpt_add(&d, &esp, 64 * MIB, MIB, &a) == CK_GPT_OK && a.created == 0 && a.first == 9 * MIB);
    ck_part p, q;
    ck_gpt_info gi;
    CHECK(ck_gpt_find_type(&d, ck_esp_part_type, &p, &gi) == CK_GPT_OK && p.first_lba == 9 * MIB &&
          p.blocks == 64 * MIB);
    CHECK(ck_gpt_find_aienos(&d, &q, &gi) == CK_GPT_ENOPART); /* GPT still valid; no Allen section yet */
    /* Allen's section can still be added beside it */
    CHECK(ck_gpt_add_aienos(&d, 8 * MIB, MIB, &a) == CK_GPT_OK && a.created == 1 && a.first == 73 * MIB);
    CHECK(ck_gpt_find_type(&d, ck_esp_part_type, &p, &gi) == CK_GPT_OK && ck_gpt_is_ours(&d, &esp, gi.part_index) == 1);

    /* 2. install */
    ck_fatb_state st;
    CHECK(ck_fatb_state_read(&p.dev, serial, &st) == CK_FATB_OK && st.ours == 0 && st.slot == -1);
    static uint8_t man1[CK_FATB_MAN_BYTES], man2[CK_FATB_MAN_BYTES];
    const uint32_t len1 = 300001, len2 = 123457;
    uint8_t *img1 = malloc(len1), *img2 = malloc(len2);
    if (!img1 || !img2)
        return 2;
    pattern(man1, sizeof man1, 1);
    pattern(man2, sizeof man2, 2);
    pattern(img1, len1, 3);
    pattern(img2, len2, 4);
    ck_fatb_result r;
    uint64_t wrote = 0;
    size_t n0, n1;
    uint8_t *before = slurp(g_path, &n0), *after;
    CHECK(install(&f, &p, 0, man1, img1, len1, &r, &wrote) == CK_FATB_ENOTOURS && wrote == 0);
    CHECK(install(&f, &p, 1, man1, img1, len1, &r, &wrote) == CK_FATB_OK && r.formatted && r.written && r.slot == 0);
    after = slurp(g_path, &n1);
    CHECK(before && after && n0 == n1);
    if (before && after && n0 == n1) {
        CHECK(!memcmp(before, after, p.first_lba * BS)); /* MBR, GPT, the sentinel partition */
        size_t end = (p.first_lba + p.blocks) * BS;
        CHECK(!memcmp(before + end, after + end, n0 - end)); /* Allen's section, the backup GPT */
    }
    free(before);
    free(after);
    CHECK(ck_fatb_state_read(&p.dev, serial, &st) == CK_FATB_OK && st.ours && st.slot == 0 && st.image_len == len1);
    CHECK(ck_fatb_state_read(&p.dev, serial + 1u, &st) == CK_FATB_OK && st.ours == 0); /* another serial: not ours */
    {
        /* what FAT drivers change in normal use leaves it ours; any other change is named */
        uint8_t s0[BS];
        CHECK(disk_read(&p.dev, 0, 1, s0) == DISK_OK);
        uint32_t rsv = (uint32_t)s0[14] | (uint32_t)s0[15] << 8, fatsz;
        memcpy(&fatsz, s0 + 36, 4);
        uint32_t efi = rsv + 2u * fatsz + 1u, boot_entry = 64u + 13u; /* EFI dir, BOOT's DIR_CrtTimeTenth */
        CHECK(driver_marks(&p, rsv, fatsz) == 0);
        CHECK(ck_fatb_state_read(&p.dev, serial, &st) == CK_FATB_OK && st.ours && st.slot == 0 && st.image_len == len1);
        CHECK(poke(&p, efi, boot_entry, 0x01) == 0);
        CHECK(ck_fatb_state_read(&p.dev, serial, &st) == CK_FATB_OK && !st.ours && st.where &&
              !strcmp(st.where, "EFI") && st.at == boot_entry && st.got == 1 && st.want == 0);
        CHECK(poke(&p, efi, boot_entry, 0x01) == 0 && driver_marks(&p, rsv, fatsz) == 0); /* back as written */
        CHECK(ck_fatb_state_read(&p.dev, serial, &st) == CK_FATB_OK && st.ours && st.slot == 0);
        /* Apple sets ATTR_ARCHIVE on the root's EFI directory during boot. */
        CHECK(poke(&p, efi - 1u, 32u + 11u, 0x20) == 0);
        CHECK(ck_fatb_state_read(&p.dev, serial, &st) == CK_FATB_OK && st.ours && st.slot == 0);
        /* ... and stamps it written (real machine: root byte 54 = 0x82); a
         * file entry's stamp is not taken over */
        CHECK(poke(&p, efi - 1u, 32u + 22u, 0x82) == 0 && poke(&p, efi - 1u, 32u + 25u, 0x5a) == 0);
        CHECK(ck_fatb_state_read(&p.dev, serial, &st) == CK_FATB_OK && st.ours && st.slot == 0);
        CHECK(poke(&p, efi - 1u, 32u + 22u, 0x82) == 0 && poke(&p, efi - 1u, 32u + 25u, 0x5a) == 0);
        CHECK(poke(&p, efi - 1u, 22u, 0x01) == 0); /* the volume label entry: strict */
        CHECK(ck_fatb_state_read(&p.dev, serial, &st) == CK_FATB_OK && !st.ours && st.at == 22u);
        CHECK(poke(&p, efi - 1u, 22u, 0x01) == 0);
        /* The directory type is still structural, even with ARCHIVE set. */
        CHECK(poke(&p, efi - 1u, 32u + 11u, 0x10) == 0);
        CHECK(ck_fatb_state_read(&p.dev, serial, &st) == CK_FATB_OK && !st.ours && st.where &&
              !strcmp(st.where, "root") && st.at == 43u);
        CHECK(poke(&p, efi - 1u, 32u + 11u, 0x10) == 0);
        /* Do not ignore archive bits in the volume label or unused entries. */
        CHECK(poke(&p, efi - 1u, 11u, 0x20) == 0);
        CHECK(ck_fatb_state_read(&p.dev, serial, &st) == CK_FATB_OK && !st.ours);
        CHECK(poke(&p, efi - 1u, 11u, 0x20) == 0);
        CHECK(poke(&p, efi - 1u, 64u + 11u, 0x20) == 0);
        CHECK(ck_fatb_state_read(&p.dev, serial, &st) == CK_FATB_OK && !st.ours);
        CHECK(poke(&p, efi - 1u, 64u + 11u, 0x20) == 0);
        CHECK(ck_fatb_state_read(&p.dev, serial, &st) == CK_FATB_OK && st.ours && st.slot == 0);
    }
    CHECK(export_part(&p, "part1.img") == 0 && spill("img1.bin", img1, len1) == 0);
    CHECK(install(&f, &p, 0, man1, img1, len1, &r, &wrote) == CK_FATB_OK && !r.written && r.slot == 0 && wrote == 0);
    static uint8_t big[1];
    CHECK(install(&f, &p, 0, man1, big, st.slot_max + 1u, &r, &wrote) == CK_FATB_EBIG && wrote == 0);

    /* 3. power cuts while img2 replaces img1 */
    uint8_t *base = malloc(p.blocks * BS);
    if (!base)
        return 2;
    CHECK(pread(f.fd, base, p.blocks * BS, (off_t)(p.first_lba * BS)) == (ssize_t)(p.blocks * BS));
    uint64_t total = 0;
    CHECK(install(&f, &p, 0, man2, img2, len2, &r, &total) == CK_FATB_OK && r.written && r.slot == 1);
    int last = 0, bad = 0, switches = 0;
    uint64_t first_new = 0;
    for (uint64_t cut = 0; cut <= total; cut += cut < 16 || total - cut < 16 ? 1 : total / 97 + 1) {
        if (pwrite(f.fd, base, p.blocks * BS, (off_t)(p.first_lba * BS)) != (ssize_t)(p.blocks * BS))
            bad++;
        disk_file_arm_cut(&f, cut);
        int rc = install(&f, &p, 0, man2, img2, len2, &r, 0);
        disk_file_disarm(&f);
        if (rc == CK_FATB_OK && cut < total) /* a cut is always reported (at the very end only the flush is lost) */
            bad++;
        int now = -1;
        if (ck_fatb_state_read(&p.dev, serial, &st) == CK_FATB_OK && st.ours) {
            uint64_t w;
            if (st.slot == 0 && install(&f, &p, 0, man1, img1, len1, &r, &w) == CK_FATB_OK && !r.written && !w)
                now = 0;
            else if (st.slot == 1 && install(&f, &p, 0, man2, img2, len2, &r, &w) == CK_FATB_OK && !r.written && !w)
                now = 1;
        }
        if (now < 0 || now < last) {
            printf("  cut after %llu of %llu blocks: %s\n", (unsigned long long)cut, (unsigned long long)total,
                   now < 0 ? "neither kernel whole" : "back to the old kernel");
            bad++;
        }
        if (now == 1 && last == 0) {
            switches++;
            first_new = cut;
        }
        last = now < 0 ? last : now;
        if (install(&f, &p, 0, man2, img2, len2, &r, 0) != CK_FATB_OK)
            bad++; /* the next start installs it whole */
    }
    /* the exact switch: one block before it the old kernel, at it the new */
    uint64_t lo = 0, hi = first_new;
    while (lo + 1 < hi) {
        uint64_t mid = (lo + hi) / 2;
        if (pwrite(f.fd, base, p.blocks * BS, (off_t)(p.first_lba * BS)) != (ssize_t)(p.blocks * BS))
            bad++;
        disk_file_arm_cut(&f, mid);
        install(&f, &p, 0, man2, img2, len2, &r, 0);
        disk_file_disarm(&f);
        if (ck_fatb_state_read(&p.dev, serial, &st) == CK_FATB_OK && st.slot == 1)
            hi = mid;
        else
            lo = mid;
    }
    printf("note: replacing the kernel writes %llu blocks; the firmware's view switches after block %llu "
           "(one directory sector); cut points tried with neither kernel whole: %d\n",
           (unsigned long long)total, (unsigned long long)hi, bad);
    CHECK(bad == 0 && switches == 1 && hi > 0);
    CHECK(pwrite(f.fd, base, p.blocks * BS, (off_t)(p.first_lba * BS)) == (ssize_t)(p.blocks * BS));
    CHECK(install(&f, &p, 0, man2, img2, len2, &r, 0) == CK_FATB_OK && r.written && r.slot == 1);
    CHECK(export_part(&p, "part2.img") == 0 && spill("img2.bin", img2, len2) == 0);
    CHECK(install(&f, &p, 0, man1, img1, len1, &r, 0) == CK_FATB_OK && r.written && r.slot == 0);
    CHECK(export_part(&p, "part3.img") == 0);
    free(base);
    disk_file_close(&f);

    /* someone else's EFI System Partition is not ours */
    gw_part foreign[1] = {{gw_esp_type, 1 * MIB, 65 * MIB - 1u, "AIENOS boot", 0}};
    CHECK(gw_write(g_path, BS, disk, foreign, 1, 0) == 0);
    CHECK(disk_file_open(&f, &d, g_path, BS, 0, 0) == 0);
    CHECK(ck_gpt_find_type(&d, ck_esp_part_type, &p, &gi) == CK_GPT_OK && ck_gpt_is_ours(&d, &esp, gi.part_index) == 0);
    CHECK(ck_gpt_add(&d, &esp, 64 * MIB, MIB, &a) == CK_GPT_OK && a.created == 0); /* reported, never a second */
    disk_file_close(&f);

    /* 4. time stamps */
    uint16_t dd, tt;
    ck_fatb_dos_time(1792303183u, &dd, &tt); /* 2026-10-18 05:59:43 UTC */
    CHECK(dd == 23890 && tt == 12149);
    ck_fatb_dos_time(1709251199u, &dd, &tt); /* 2024-02-29 23:59:59 UTC */
    CHECK(dd == 22621 && tt == 49021);
    ck_fatb_dos_time(0, &dd, &tt);
    CHECK(dd == 0x21 && tt == 0);

    free(img1);
    free(img2);
    printf("fat boot: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
