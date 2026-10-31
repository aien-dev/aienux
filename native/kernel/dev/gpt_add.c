/* gpt_add.c -- add the AIENOS partition in unused space; see gpt_add.h. */
#include "gpt_add.h"

#include "disk_part.h"
#include "sha256.h"

#define ENT_MAX (64u << 10)
static uint8_t g_ent[ENT_MAX];
static uint8_t g_ph[4096]; /* primary header block */
static uint8_t g_bh[4096]; /* backup header block */
static uint64_t g_f[CK_GPT_MAX_USED], g_l[CK_GPT_MAX_USED];

/* UEFI 2.10 table 5-5 offsets, as in disk_part.c */
enum { H_CRC = 16, H_SIZE = 12, H_GUID = 56, H_ENT_LBA = 72, H_ECRC = 88 };
static const ck_gpt_new allen_memory = {ck_aienos_part_type, "AIENOS Allen memory", "aienos.allen.memory.v1"};

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t get64(const uint8_t *p) { return (uint64_t)get32(p) | (uint64_t)get32(p + 4) << 32; }
static void put32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}
static void put64(uint8_t *p, uint64_t v)
{
    put32(p, (uint32_t)v);
    put32(p + 4, (uint32_t)(v >> 32));
}
static int zero16(const uint8_t *p)
{
    uint8_t d = 0;
    for (int i = 0; i < 16; i++)
        d |= p[i];
    return d == 0;
}

/* Reads both header blocks and the primary entry array (after the strict
 * check passed far enough to fill info). */
static int load(const disk_dev *d, const ck_gpt_info *gi, uint32_t *bytes, uint32_t *blocks)
{
    *bytes = gi->entries * gi->entry_size;
    if (*bytes == 0 || *bytes > ENT_MAX || d->block_size > sizeof g_ph)
        return CK_GPT_EBIG;
    *blocks = (*bytes + d->block_size - 1u) / d->block_size;
    if (disk_read(d, 1, 1, g_ph) != DISK_OK || disk_read(d, gi->last_lba, 1, g_bh) != DISK_OK ||
        disk_read(d, get64(g_ph + H_ENT_LBA), *blocks, g_ent) != DISK_OK)
        return CK_GPT_EIO;
    return CK_GPT_OK;
}

int ck_gpt_list(const disk_dev *d, void (*fn)(void *ctx, const ck_gpt_entry *e), void *ctx)
{
    ck_part p;
    ck_gpt_info gi;
    int rc = ck_gpt_find_aienos(d, &p, &gi);
    if (rc != CK_GPT_OK && gi.entries == 0)
        return rc;
    uint32_t bytes, blocks, used = 0;
    if ((rc = load(d, &gi, &bytes, &blocks)))
        return rc;
    for (uint32_t i = 0; i < gi.entries; i++) {
        const uint8_t *e = g_ent + (uint64_t)i * gi.entry_size;
        if (zero16(e))
            continue;
        ck_gpt_entry out;
        out.index = i;
        for (int k = 0; k < 16; k++) {
            out.type[k] = e[k];
            out.uguid[k] = e[16 + k];
        }
        out.first = get64(e + 32);
        out.last = get64(e + 40);
        unsigned n = 0;
        for (; n < 36; n++) {
            uint16_t c = (uint16_t)(e[56 + 2 * n] | e[57 + 2 * n] << 8);
            if (!c)
                break;
            out.name[n] = c >= 0x20 && c < 0x7f && c != '"' ? (char)c : '?';
        }
        out.name[n] = 0;
        used++;
        if (fn)
            fn(ctx, &out);
    }
    return (int)used;
}

static void header_crc(uint8_t *h, uint32_t ecrc)
{
    put32(h + H_ECRC, ecrc);
    put32(h + H_CRC, 0);
    put32(h + H_CRC, ck_crc32(0, h, get32(h + H_SIZE)));
}

/* The unique GUID of a partition this file adds: SHA-256 over the label
 * (with its terminator), the disk GUID and the first LBA, cut to 16 bytes and
 * marked version 4 / variant 1. */
static void unique_guid(const char *label, const uint8_t *disk_guid, uint64_t first, uint8_t out[16])
{
    uint8_t dig[32], lba8[8];
    size_t n = 0;
    while (label[n])
        n++;
    put64(lba8, first);
    sha256_ctx h;
    sha256_init(&h);
    sha256_update(&h, (const uint8_t *)label, n + 1u);
    sha256_update(&h, disk_guid, 16);
    sha256_update(&h, lba8, 8);
    sha256_final(&h, dig);
    for (int i = 0; i < 16; i++)
        out[i] = dig[i];
    out[7] = (uint8_t)((out[7] & 0x0f) | 0x40);
    out[8] = (uint8_t)((out[8] & 0x3f) | 0x80);
}

int ck_gpt_is_ours(const disk_dev *d, const ck_gpt_new *spec, uint32_t index)
{
    ck_part p;
    ck_gpt_info gi;
    int rc = ck_gpt_find_type(d, spec->type, &p, &gi);
    if (rc != CK_GPT_OK)
        return rc;
    uint32_t bytes, blocks;
    if (index != gi.part_index || (rc = load(d, &gi, &bytes, &blocks)))
        return rc ? rc : 0;
    const uint8_t *e = g_ent + (uint64_t)index * gi.entry_size;
    uint8_t want[16];
    unique_guid(spec->label, g_ph + H_GUID, get64(e + 32), want);
    for (int i = 0; i < 16; i++)
        if (e[16 + i] != want[i])
            return 0;
    for (unsigned i = 0; i < 36; i++) {
        uint16_t c = (uint16_t)(e[56 + 2 * i] | e[57 + 2 * i] << 8);
        if (c != (uint8_t)spec->name[i])
            return 0;
        if (!c)
            break;
    }
    return 1;
}

int ck_gpt_add_aienos(const disk_dev *d, uint64_t blocks, uint64_t align, ck_gpt_added *out)
{
    return ck_gpt_add(d, &allen_memory, blocks, align, out);
}

int ck_gpt_add(const disk_dev *d, const ck_gpt_new *spec, uint64_t blocks, uint64_t align, ck_gpt_added *out)
{
    ck_part p;
    ck_gpt_info gi;
    if (!out || !blocks || !align || !spec || !spec->type || !spec->name || !spec->label)
        return CK_GPT_EGEOM;
    size_t name_len = 0;
    while (spec->name[name_len])
        name_len++;
    if (name_len > 36)
        return CK_GPT_EGEOM;
    *out = (ck_gpt_added){0, 0, 0, 0};
    int rc = ck_gpt_find_type(d, spec->type, &p, &gi);
    if (rc == CK_GPT_OK) {
        out->first = p.first_lba;
        out->last = p.first_lba + p.blocks - 1u;
        out->index = gi.part_index;
        return CK_GPT_OK;
    }
    if (rc != CK_GPT_ENOPART)
        return rc;
    uint32_t bytes, eblocks;
    if ((rc = load(d, &gi, &bytes, &eblocks)))
        return rc;

    /* Used ranges, sorted by first LBA (the strict check proved they lie in
     * the usable range and do not overlap). */
    uint32_t n = 0, slot = gi.entries;
    for (uint32_t i = 0; i < gi.entries; i++) {
        const uint8_t *e = g_ent + (uint64_t)i * gi.entry_size;
        if (zero16(e)) {
            if (slot == gi.entries)
                slot = i;
            continue;
        }
        if (n == CK_GPT_MAX_USED)
            return CK_GPT_ETOOMANY;
        uint64_t f = get64(e + 32), l = get64(e + 40);
        uint32_t k = n++;
        for (; k && g_f[k - 1] > f; k--) {
            g_f[k] = g_f[k - 1];
            g_l[k] = g_l[k - 1];
        }
        g_f[k] = f;
        g_l[k] = l;
    }
    if (slot == gi.entries)
        return CK_GPT_ENOSLOT;
    uint64_t at = 0, cursor = gi.first_usable;
    int found = 0;
    for (uint32_t k = 0; k <= n && !found; k++) {
        uint64_t gap_end = k < n ? g_f[k] - 1u : gi.last_usable; /* g_f[k] >= first_usable >= 34 */
        uint64_t start = (cursor + align - 1u) / align * align;
        if (cursor <= gap_end && start >= cursor && start <= gap_end && gap_end - start >= blocks - 1u) {
            at = start;
            found = 1;
        }
        if (k < n && g_l[k] + 1u > cursor)
            cursor = g_l[k] + 1u;
    }
    if (!found)
        return CK_GPT_ENOSPACE;

    /* The new entry, its unique GUID from unique_guid. */
    uint8_t *e = g_ent + (uint64_t)slot * gi.entry_size;
    for (uint32_t i = 0; i < gi.entry_size; i++)
        e[i] = 0;
    for (int i = 0; i < 16; i++)
        e[i] = spec->type[i];
    unique_guid(spec->label, g_ph + H_GUID, at, e + 16);
    put64(e + 32, at);
    put64(e + 40, at + blocks - 1u);
    for (unsigned i = 0; i < name_len; i++)
        e[56 + 2 * i] = (uint8_t)spec->name[i];

    uint32_t ecrc = ck_crc32(0, g_ent, bytes);
    header_crc(g_ph, ecrc);
    header_crc(g_bh, ecrc);
    uint64_t bent = get64(g_bh + H_ENT_LBA), pent = get64(g_ph + H_ENT_LBA);
    if (disk_write(d, bent, eblocks, g_ent) != DISK_OK || disk_write(d, gi.last_lba, 1, g_bh) != DISK_OK ||
        disk_flush(d) != DISK_OK || disk_write(d, pent, eblocks, g_ent) != DISK_OK ||
        disk_write(d, 1, 1, g_ph) != DISK_OK || disk_flush(d) != DISK_OK)
        return CK_GPT_EWRITE;

    rc = ck_gpt_find_type(d, spec->type, &p, &gi);
    if (rc != CK_GPT_OK || p.first_lba != at || p.blocks != blocks || gi.part_index != slot)
        return CK_GPT_EVERIFY;
    out->first = at;
    out->last = at + blocks - 1u;
    out->index = slot;
    out->created = 1;
    return CK_GPT_OK;
}

const char *ck_gpt_add_strerror(int rc)
{
    switch (rc) {
    case CK_GPT_ENOSPACE: return "no unused space large enough";
    case CK_GPT_ENOSLOT: return "no free partition entry";
    case CK_GPT_EBIG: return "partition entry array too large";
    case CK_GPT_EWRITE: return "write failed";
    case CK_GPT_EVERIFY: return "partition not as written after the check";
    default: return ck_gpt_strerror(rc);
    }
}
