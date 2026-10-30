/* fat_boot.c -- the AIENOS boot section (FAT32); see fat_boot.h. Field names
 * and offsets are those of the Microsoft FAT32 File System Specification
 * 1.03 ("Boot Sector and BPB Structure", "FAT32 FSInfo Sector Structure",
 * "FAT 32 Byte Directory Entry Structure"). */
#include "fat_boot.h"

const uint8_t ck_esp_part_type[16] = {0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
                                      0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b};

#define SEC 512u
#define CHUNK 128u              /* sectors per buffered transfer */
#define PER_FAT_SEC (SEC / 4u)  /* FAT32 entries in one FAT sector */
#define EOC 0x0FFFFFFFu
#define MAN_CLUS (CK_FATB_MAN_BYTES / SEC)
#define ROOT 2u
#define EFI_DIR 3u
#define BOOT_DIR 4u
#define FMT_DATE 0x0021u /* 1980-01-01, the directories' stamp */

static uint8_t g_buf[CHUNK * SEC];
static uint8_t g_sec[SEC], g_want[SEC];

typedef struct {
    uint32_t total, rsv, fatsz, data0, clusters, slotc;
} geo;

static void put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static void put32(uint8_t *p, uint32_t v)
{
    put16(p, v);
    put16(p + 2, v >> 16);
}
static uint32_t get16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t get32(const uint8_t *p) { return get16(p) | get16(p + 2) << 16; }
static void fill(uint8_t *p, uint8_t v, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        p[i] = v;
}
static void copy(uint8_t *d, const uint8_t *s, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        d[i] = s[i];
}
static int same(const uint8_t *a, const uint8_t *b, uint32_t n)
{
    uint8_t x = 0;
    for (uint32_t i = 0; i < n; i++)
        x |= a[i] ^ b[i];
    return x == 0;
}

/* The spec's FAT32 sizing ("FAT Type Determination", BPB_FATSz32 formula
 * with TmpVal2 = (256 * BPB_SecPerClus + BPB_NumFATs) / 2), one sector per
 * cluster as its table gives up to 260 MB and fine above; the reserved
 * region padded to a 4 KiB boundary for the data region. */
static int geometry(const disk_dev *p, geo *g)
{
    if (p->block_size != SEC || p->block_count > 0xffffffffull ||
        p->block_count < (uint64_t)CK_FATB_MIN_MIB * (1024u * 1024u / SEC))
        return CK_FATB_EGEOM;
    g->total = (uint32_t)p->block_count;
    uint32_t t1 = g->total - 32u, t2 = (256u * 1u + 2u) / 2u;
    g->fatsz = (t1 + t2 - 1u) / t2;
    g->rsv = 32u + (8u - (32u + 2u * g->fatsz) % 8u) % 8u;
    g->data0 = g->rsv + 2u * g->fatsz;
    g->clusters = g->total - g->data0;
    if (g->clusters < 65525u || (uint64_t)g->fatsz * PER_FAT_SEC < (uint64_t)g->clusters + 2u)
        return CK_FATB_EGEOM;
    g->slotc = ((g->clusters + 2u - 128u) / 2u) & ~127u;
    return g->slotc >= 256u ? CK_FATB_OK : CK_FATB_EGEOM;
}

static uint32_t slot_base(const geo *g, int k) { return 128u + (uint32_t)k * g->slotc; }
static uint32_t clus_sec(const geo *g, uint32_t c) { return g->data0 + (c - 2u); }
static uint32_t slot_max(const geo *g) { return (g->slotc - 128u) * SEC; }

static int rd(const disk_dev *p, uint32_t lba, uint32_t n, uint8_t *b)
{
    return disk_read(p, lba, n, b) == DISK_OK ? CK_FATB_OK : CK_FATB_EIO;
}
static int wr(const disk_dev *p, uint32_t lba, uint32_t n, const uint8_t *b, uint32_t *count)
{
    if (disk_write(p, lba, n, b) != DISK_OK)
        return CK_FATB_EIO;
    if (count)
        *count += n;
    return CK_FATB_OK;
}
static int flush(const disk_dev *p) { return disk_flush(p) == DISK_OK ? CK_FATB_OK : CK_FATB_EIO; }

/* ---- the fixed sectors ---------------------------------------------------- */
static void boot_sector(const geo *g, uint32_t serial, uint8_t *b)
{
    fill(b, 0, SEC);
    b[0] = 0xeb, b[1] = 0x58, b[2] = 0x90;
    copy(b + 3, (const uint8_t *)"AIENOS  ", 8);
    put16(b + 11, SEC);
    b[13] = 1;             /* BPB_SecPerClus */
    put16(b + 14, g->rsv); /* BPB_RsvdSecCnt */
    b[16] = 2;             /* BPB_NumFATs; RootEntCnt, TotSec16, FATSz16 stay 0 */
    b[21] = 0xf8;          /* BPB_Media: fixed */
    put16(b + 24, 63);
    put16(b + 26, 255);
    put32(b + 32, g->total);
    put32(b + 36, g->fatsz);
    put32(b + 44, ROOT); /* BPB_RootClus; ExtFlags 0 = mirrored, FSVer 0:0 */
    put16(b + 48, 1);    /* BPB_FSInfo */
    put16(b + 50, 6);    /* BPB_BkBootSec */
    b[64] = 0x80;
    b[66] = 0x29;
    put32(b + 67, serial);
    copy(b + 71, (const uint8_t *)"AIENOS     ", 11);
    copy(b + 82, (const uint8_t *)"FAT32   ", 8);
    b[90] = 0xcd, b[91] = 0x18; /* not a BIOS boot disk: int 18h */
    b[510] = 0x55, b[511] = 0xaa;
}

static void fsinfo(uint8_t *b)
{
    fill(b, 0, SEC);
    put32(b, 0x41615252u);
    put32(b + 484, 0x61417272u);
    put32(b + 488, 0xffffffffu); /* free count unknown */
    put32(b + 492, 0xffffffffu); /* no hint */
    put32(b + 508, 0xaa550000u);
}

static void dirent(uint8_t *e, const char *name11, uint8_t attr, uint32_t clus, uint32_t size, uint16_t date,
                   uint16_t time)
{
    fill(e, 0, 32);
    copy(e, (const uint8_t *)name11, 11);
    e[11] = attr;
    put16(e + 14, time);
    put16(e + 16, date);
    put16(e + 18, date);
    put16(e + 20, clus >> 16);
    put16(e + 22, time);
    put16(e + 24, date);
    put16(e + 26, clus & 0xffffu);
    put32(e + 28, size);
}

/* Directory sector for cluster 2, 3 or 4 as ck_fatb_format writes it. */
static void dir_sector(uint32_t c, uint8_t *b)
{
    fill(b, 0, SEC);
    if (c == ROOT) {
        dirent(b, "AIENOS     ", 0x08, 0, 0, FMT_DATE, 0);
        dirent(b + 32, "EFI        ", 0x10, EFI_DIR, 0, FMT_DATE, 0);
    } else if (c == EFI_DIR) {
        dirent(b, ".          ", 0x10, EFI_DIR, 0, FMT_DATE, 0);
        dirent(b + 32, "..         ", 0x10, 0, 0, FMT_DATE, 0); /* parent is the root: 0 */
        dirent(b + 64, "BOOT       ", 0x10, BOOT_DIR, 0, FMT_DATE, 0);
    } else {
        dirent(b, ".          ", 0x10, BOOT_DIR, 0, FMT_DATE, 0);
        dirent(b + 32, "..         ", 0x10, EFI_DIR, 0, FMT_DATE, 0);
    }
}

/* FAT sector 0: FAT[0] media, FAT[1] end of chain (clean, no error), and
 * the three one-cluster directories. */
static void fat_sector0(uint8_t *b)
{
    fill(b, 0, SEC);
    put32(b, 0x0FFFFFF8u);
    for (uint32_t c = 1; c <= BOOT_DIR; c++)
        put32(b + 4 * c, EOC);
}

/* ---- format --------------------------------------------------------------- */
int ck_fatb_format(const disk_dev *p, uint32_t serial)
{
    geo g;
    int rc = geometry(p, &g);
    if (rc)
        return rc;
    /* the old boot sector goes first, so a cut format is never taken for ours */
    fill(g_sec, 0, SEC);
    if ((rc = wr(p, 0, 1, g_sec, 0)) || (rc = flush(p)))
        return rc;
    fill(g_buf, 0, sizeof g_buf);
    for (uint32_t s = g.rsv; s < g.data0; s += CHUNK) {
        uint32_t n = g.data0 - s < CHUNK ? g.data0 - s : CHUNK;
        if ((rc = wr(p, s, n, g_buf, 0)))
            return rc;
    }
    fat_sector0(g_sec);
    if ((rc = wr(p, g.rsv, 1, g_sec, 0)) || (rc = wr(p, g.rsv + g.fatsz, 1, g_sec, 0)))
        return rc;
    for (uint32_t c = ROOT; c <= BOOT_DIR; c++) {
        dir_sector(c, g_sec);
        if ((rc = wr(p, clus_sec(&g, c), 1, g_sec, 0)))
            return rc;
    }
    if ((rc = flush(p)))
        return rc;
    for (uint32_t s = 1; s < g.rsv; s++) {
        if (s == 1 || s == 7)
            fsinfo(g_sec);
        else if (s == 6)
            boot_sector(&g, serial, g_sec);
        else
            fill(g_sec, 0, SEC);
        if ((rc = wr(p, s, 1, g_sec, 0)))
            return rc;
    }
    if ((rc = flush(p)))
        return rc;
    boot_sector(&g, serial, g_sec);
    if ((rc = wr(p, 0, 1, g_sec, 0)))
        return rc;
    return flush(p);
}

/* ---- reading -------------------------------------------------------------- */
/* 1 when FAT `fat` (0 or 1) holds the chain first, first+1, .., first+n-1,
 * end of chain; 0 when not; or CK_FATB_EIO. */
static int chain_ok(const disk_dev *p, const geo *g, int fat, uint32_t first, uint32_t n)
{
    uint32_t loaded = 0xffffffffu, base = g->rsv + (uint32_t)fat * g->fatsz;
    for (uint32_t c = first; c < first + n; c++) {
        uint32_t s = c / PER_FAT_SEC;
        if (s != loaded) {
            if (rd(p, base + s, 1, g_sec))
                return CK_FATB_EIO;
            loaded = s;
        }
        uint32_t v = get32(g_sec + 4u * (c % PER_FAT_SEC)) & 0x0FFFFFFFu;
        if (c + 1u < first + n ? v != c + 1u : v < 0x0FFFFFF8u)
            return 0;
    }
    return 1;
}

static int find(const uint8_t *dir, const char *name11, uint32_t *clus, uint32_t *size)
{
    for (uint32_t i = 2; i < SEC / 32u; i++) {
        const uint8_t *e = dir + 32u * i;
        if (e[0] == 0)
            break;
        if (same(e, (const uint8_t *)name11, 11) && !(e[11] & 0x18)) {
            *clus = get16(e + 20) << 16 | get16(e + 26);
            *size = get32(e + 28);
            return 1;
        }
    }
    return 0;
}

/* Compares n bytes read (got) with what ck_fatb_format wrote (want), after
 * taking over from want the fields FAT drivers change in normal use (see
 * fat_boot.h). Records the first difference in st; 1 when none. */
enum { BOOT_SEC, FAT_SEC, DIR_SEC };
static int as_written(uint8_t *got, const uint8_t *want, uint32_t n, int kind, const char *where, ck_fatb_state *st)
{
    if (kind == BOOT_SEC) {
        got[65] = want[65]; /* BS_Reserved1 */
    } else if (kind == FAT_SEC) {
        got[7] = (uint8_t)((got[7] & ~0x0cu) | (want[7] & 0x0cu)); /* FAT[1] bits 27, 26 */
    } else {
        for (uint32_t e = 0; e + 32u <= n; e += 32u) {
            got[e + 18] = want[e + 18], got[e + 19] = want[e + 19]; /* DIR_LstAccDate */
            /* Firmware may mark an existing directory for backup and stamp
             * it as written (Apple's does both to \EFI when it starts the
             * kernel). Only normalize ARCHIVE, DIR_WrtTime and DIR_WrtDate
             * on expected directories, never their type, volume labels, or
             * unused entries (FAT32 spec DIR_Attr). */
            if (want[e] && (want[e + 11] & 0x10)) {
                got[e + 11] = (uint8_t)((got[e + 11] & ~0x20u) | (want[e + 11] & 0x20u));
                copy(got + e + 22, want + e + 22, 4);
            }
        }
    }
    for (uint32_t i = 0; i < n; i++)
        if (got[i] != want[i]) {
            st->where = where, st->at = i, st->got = got[i], st->want = want[i];
            return 0;
        }
    return 1;
}

int ck_fatb_state_read(const disk_dev *p, uint32_t serial, ck_fatb_state *st)
{
    static const char *const dir_name[] = {"root", "EFI", "BOOT"};
    geo g;
    int rc = geometry(p, &g);
    st->ours = 0;
    st->slot = -1;
    st->image_len = 0;
    st->slot_max = 0;
    st->where = 0;
    if (rc)
        return rc;
    st->slot_max = slot_max(&g);
    boot_sector(&g, serial, g_want);
    if (rd(p, 0, 1, g_sec))
        return CK_FATB_EIO;
    if (!as_written(g_sec, g_want, SEC, BOOT_SEC, "boot sector", st))
        return CK_FATB_OK;
    fat_sector0(g_want);
    for (int f = 0; f < 2; f++) {
        if (rd(p, g.rsv + (uint32_t)f * g.fatsz, 1, g_sec))
            return CK_FATB_EIO;
        if (!as_written(g_sec, g_want, 4u * (BOOT_DIR + 1u), FAT_SEC, f ? "FAT 2" : "FAT 1", st))
            return CK_FATB_OK;
    }
    for (uint32_t c = ROOT; c <= BOOT_DIR; c++) {
        dir_sector(c, g_want);
        if (rd(p, clus_sec(&g, c), 1, g_sec))
            return CK_FATB_EIO;
        if (!as_written(g_sec, g_want, c == BOOT_DIR ? 64u : SEC, DIR_SEC, dir_name[c - ROOT], st))
            return CK_FATB_OK;
    }
    st->ours = 1;
    uint32_t ic, is, mc, ms;
    if (!find(g_sec, "BOOTX64 EFI", &ic, &is) || !find(g_sec, "BOOTX64 MAN", &mc, &ms))
        return CK_FATB_OK;
    for (int k = 0; k < 2; k++) {
        uint32_t base = slot_base(&g, k);
        if (mc != base || ms != CK_FATB_MAN_BYTES || ic != base + 128u || is == 0 || is > st->slot_max)
            continue;
        for (int f = 0; f < 2; f++) {
            int a = chain_ok(p, &g, f, mc, MAN_CLUS), b = a == 1 ? chain_ok(p, &g, f, ic, (is + SEC - 1u) / SEC) : a;
            if (a < 0 || b < 0)
                return CK_FATB_EIO;
            if (!a || !b)
                return CK_FATB_OK;
        }
        st->slot = k;
        st->image_len = is;
    }
    return CK_FATB_OK;
}

/* 1 when the n bytes at sector lba equal want; 0 when not; or CK_FATB_EIO. */
static int same_on_disk(const disk_dev *p, uint32_t lba, const uint8_t *want, uint32_t n)
{
    for (uint32_t done = 0; done < n;) {
        uint32_t secs = (n - done + SEC - 1u) / SEC;
        if (secs > CHUNK)
            secs = CHUNK;
        if (rd(p, lba + done / SEC, secs, g_buf))
            return CK_FATB_EIO;
        uint32_t m = secs * SEC < n - done ? secs * SEC : n - done;
        if (!same(g_buf, want + done, m))
            return 0;
        for (uint32_t i = m; i < secs * SEC; i++) /* the tail of the last sector: zero */
            if (g_buf[i])
                return 0;
        done += m;
    }
    return 1;
}

static int slot_holds(const disk_dev *p, const geo *g, int k, const uint8_t *man, const uint8_t *img, uint32_t len)
{
    uint32_t base = slot_base(g, k);
    int a = same_on_disk(p, clus_sec(g, base), man, CK_FATB_MAN_BYTES);
    return a == 1 ? same_on_disk(p, clus_sec(g, base + 128u), img, len) : a;
}

/* Writes slot k's FAT sectors in both FATs: the manifest and image chains
 * when len > 0, all free when len == 0. */
static int slot_fat(const disk_dev *p, const geo *g, int k, uint32_t len, uint32_t *count)
{
    uint32_t base = slot_base(g, k), img0 = base + 128u, n = (len + SEC - 1u) / SEC;
    uint32_t s0 = base / PER_FAT_SEC, s1 = (base + g->slotc) / PER_FAT_SEC;
    for (uint32_t s = s0; s < s1; s += CHUNK) {
        uint32_t secs = s1 - s < CHUNK ? s1 - s : CHUNK;
        for (uint32_t i = 0; i < secs * PER_FAT_SEC; i++) {
            uint32_t c = s * PER_FAT_SEC + i, v = 0;
            if (len && c >= base && c < base + MAN_CLUS)
                v = c + 1u < base + MAN_CLUS ? c + 1u : EOC;
            else if (len && c >= img0 && c < img0 + n)
                v = c + 1u < img0 + n ? c + 1u : EOC;
            put32(g_buf + 4u * i, v);
        }
        int rc;
        if ((rc = wr(p, g->rsv + s, secs, g_buf, count)) || (rc = wr(p, g->rsv + g->fatsz + s, secs, g_buf, count)))
            return rc;
    }
    return CK_FATB_OK;
}

static int write_bytes(const disk_dev *p, uint32_t lba, const uint8_t *b, uint32_t n, uint32_t *count)
{
    int rc;
    if (n / SEC && (rc = wr(p, lba, n / SEC, b, count)))
        return rc;
    if (n % SEC) {
        fill(g_sec, 0, SEC);
        copy(g_sec, b + n / SEC * SEC, n % SEC);
        if ((rc = wr(p, lba + n / SEC, 1, g_sec, count)))
            return rc;
    }
    return CK_FATB_OK;
}

int ck_fatb_install(const disk_dev *p, uint32_t serial, int format_ok, const uint8_t *man, const uint8_t *img,
                    uint32_t img_len, uint16_t dos_date, uint16_t dos_time, ck_fatb_result *out)
{
    geo g;
    ck_fatb_state st;
    out->formatted = out->written = 0;
    out->slot = -1;
    out->sectors_written = 0;
    int rc = geometry(p, &g);
    if (rc)
        return rc;
    if (!man || !img || img_len == 0 || img_len > slot_max(&g))
        return CK_FATB_EBIG;
    if ((rc = ck_fatb_state_read(p, serial, &st)))
        return rc;
    if (!st.ours) {
        if (!format_ok)
            return CK_FATB_ENOTOURS;
        if ((rc = ck_fatb_format(p, serial)) || (rc = ck_fatb_state_read(p, serial, &st)))
            return rc;
        if (!st.ours)
            return CK_FATB_EVERIFY;
        out->formatted = 1;
        out->sectors_written = g.data0 + 4u;
    }
    if (st.slot >= 0 && st.image_len == img_len) {
        int h = slot_holds(p, &g, st.slot, man, img, img_len);
        if (h < 0)
            return h;
        if (h) {
            out->slot = st.slot;
            return CK_FATB_OK;
        }
    }
    int k = st.slot == 0 ? 1 : 0;
    uint32_t base = slot_base(&g, k);
    if ((rc = slot_fat(p, &g, k, img_len, &out->sectors_written)) ||
        (rc = write_bytes(p, clus_sec(&g, base), man, CK_FATB_MAN_BYTES, &out->sectors_written)) ||
        (rc = write_bytes(p, clus_sec(&g, base + 128u), img, img_len, &out->sectors_written)) || (rc = flush(p)))
        return rc;
    dir_sector(BOOT_DIR, g_sec);
    dirent(g_sec + 64, "BOOTX64 EFI", 0x20, base + 128u, img_len, dos_date, dos_time);
    dirent(g_sec + 96, "BOOTX64 MAN", 0x20, base, CK_FATB_MAN_BYTES, dos_date, dos_time);
    if ((rc = wr(p, clus_sec(&g, BOOT_DIR), 1, g_sec, &out->sectors_written)) || (rc = flush(p)))
        return rc;
    if (st.slot >= 0 && ((rc = slot_fat(p, &g, st.slot, 0, &out->sectors_written)) || (rc = flush(p))))
        return rc;
    if ((rc = ck_fatb_state_read(p, serial, &st)))
        return rc;
    int h = st.slot == k && st.image_len == img_len ? slot_holds(p, &g, k, man, img, img_len) : 0;
    if (h < 0)
        return h;
    if (!h)
        return CK_FATB_EVERIFY;
    out->written = 1;
    out->slot = k;
    return CK_FATB_OK;
}

void ck_fatb_dos_time(uint64_t unix_s, uint16_t *date, uint16_t *time)
{
    *date = FMT_DATE;
    *time = 0;
    if (unix_s < 315532800ull) /* 1980-01-01 */
        return;
    uint64_t days = unix_s / 86400u, rem = unix_s % 86400u;
    /* civil from days (proleptic Gregorian), era-based */
    uint64_t z = days + 719468u, era = z / 146097u, doe = z - era * 146097u;
    uint64_t yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    uint64_t doy = doe - (365u * yoe + yoe / 4u - yoe / 100u), mp = (5u * doy + 2u) / 153u;
    uint64_t d = doy - (153u * mp + 2u) / 5u + 1u, m = mp < 10u ? mp + 3u : mp - 9u, y = yoe + era * 400u + (m <= 2u);
    if (y > 2107u)
        return;
    *date = (uint16_t)((y - 1980u) << 9 | m << 5 | d);
    *time = (uint16_t)((rem / 3600u) << 11 | (rem / 60u % 60u) << 5 | (rem % 60u) / 2u);
}

const char *ck_fatb_strerror(int rc)
{
    switch (rc) {
    case CK_FATB_OK: return "ok";
    case CK_FATB_EGEOM: return "the section is not 512-byte sectors or too small";
    case CK_FATB_EIO: return "a read, write or flush failed";
    case CK_FATB_ENOTOURS: return "the section holds a volume AIENOS did not write";
    case CK_FATB_EBIG: return "the image is larger than a slot";
    case CK_FATB_EVERIFY: return "read back, the files are not what was written";
    default: return "unknown";
    }
}
