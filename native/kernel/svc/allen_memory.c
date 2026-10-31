/* allen_memory.c -- Allen's memory on disk in two copies; see allen_memory.h. */
#include "allen_memory.h"

#include "sha256.h"

#define HDR 64u
#define FACT_BYTES (CK_ALLEN_KEY_MAX + CK_ALLEN_VAL_MAX + 1u)
#define PAYLOAD_FIXED (4u + 4u + CK_ALLEN_NAME_MAX + 4u + 4u)
#define VERSION 2u

_Static_assert(HDR + PAYLOAD_FIXED + CK_ALLEN_FACTS_MAX * FACT_BYTES <= CK_ALLEN_MEM_UNIT, "memory fits one unit");

static const uint8_t magic[8] = {'A', 'I', 'E', 'N', 'A', 'L', 'M', '1'};
static uint8_t g_unit[CK_ALLEN_MEM_UNIT] __attribute__((aligned(64)));
static uint8_t g_back[CK_ALLEN_MEM_UNIT] __attribute__((aligned(64)));

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
static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t get64(const uint8_t *p) { return (uint64_t)get32(p) | (uint64_t)get32(p + 4) << 32; }
static void zero(void *d, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        ((uint8_t *)d)[i] = 0;
}
static void copy(void *d, const void *s, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        ((uint8_t *)d)[i] = ((const uint8_t *)s)[i];
}
static int same(const uint8_t *a, const uint8_t *b, uint32_t n)
{
    uint8_t d = 0;
    for (uint32_t i = 0; i < n; i++)
        d |= (uint8_t)(a[i] ^ b[i]);
    return d == 0;
}

static void seal(uint8_t *unit, uint32_t plen)
{
    sha256_ctx h;
    sha256_init(&h);
    sha256_update(&h, unit, 32);
    sha256_update(&h, unit + HDR, plen);
    sha256_final(&h, unit + 32);
}

/* Folded text in a field of cap bytes: NUL-terminated, a-z 0-9 and single
 * inner spaces only, the rest of the field zero. letters: a name (A-Z first,
 * then a-z, or empty). */
static int text_ok(const uint8_t *t, uint32_t cap, int name)
{
    uint32_t n = 0;
    while (n < cap && t[n])
        n++;
    if (n == cap)
        return 0;
    for (uint32_t i = n; i < cap; i++)
        if (t[i])
            return 0;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t c = t[i];
        int lower = c >= 'a' && c <= 'z', digit = c >= '0' && c <= '9';
        if (name) {
            /* learn_name keeps one folded word, first letter capitalised */
            if (!(i == 0 ? (c >= 'A' && c <= 'Z') || digit : lower || digit))
                return 0;
        } else if (c == ' ') {
            if (i == 0 || i == n - 1 || t[i - 1] == ' ')
                return 0;
        } else if (!lower && !digit) {
            return 0;
        }
    }
    return 1;
}

void ck_allen_mem_encode(const ck_allen_memory *mem, uint64_t gen, uint8_t unit[CK_ALLEN_MEM_UNIT])
{
    uint32_t n = mem->nfacts > CK_ALLEN_FACTS_MAX ? CK_ALLEN_FACTS_MAX : mem->nfacts;
    uint32_t plen = PAYLOAD_FIXED + n * FACT_BYTES;
    zero(unit, CK_ALLEN_MEM_UNIT);
    copy(unit, magic, 8);
    put32(unit + 8, VERSION);
    put32(unit + 12, plen);
    put64(unit + 16, gen);
    uint8_t *p = unit + HDR;
    put32(p, mem->starts);
    put32(p + 4, mem->lines);
    copy(p + 8, mem->name, CK_ALLEN_NAME_MAX);
    p[8 + CK_ALLEN_NAME_MAX - 1] = 0;
    put32(p + 8 + CK_ALLEN_NAME_MAX, n);
    put32(p + 12 + CK_ALLEN_NAME_MAX, mem->nnotes);
    p += PAYLOAD_FIXED;
    for (uint32_t i = 0; i < n; i++, p += FACT_BYTES) {
        copy(p, mem->fact[i].key, CK_ALLEN_KEY_MAX);
        p[CK_ALLEN_KEY_MAX - 1] = 0;
        copy(p + CK_ALLEN_KEY_MAX, mem->fact[i].val, CK_ALLEN_VAL_MAX);
        p[CK_ALLEN_KEY_MAX + CK_ALLEN_VAL_MAX - 1] = 0;
        p[CK_ALLEN_KEY_MAX + CK_ALLEN_VAL_MAX] = mem->fact[i].are ? 1 : 0;
    }
    seal(unit, plen);
}

int ck_allen_mem_decode(const uint8_t unit[CK_ALLEN_MEM_UNIT], ck_allen_memory *mem, uint64_t *gen)
{
    if (!same(unit, magic, 8) || get32(unit + 8) != VERSION)
        return -1;
    const uint8_t *p = unit + HDR;
    uint32_t plen = get32(unit + 12), n = get32(p + 8 + CK_ALLEN_NAME_MAX);
    if (n > CK_ALLEN_FACTS_MAX || get32(p + 12 + CK_ALLEN_NAME_MAX) > CK_ALLEN_NOTES_MAX ||
        plen != PAYLOAD_FIXED + n * FACT_BYTES || get64(unit + 16) == 0)
        return -1;
    for (uint32_t i = 24; i < 32; i++)
        if (unit[i])
            return -1;
    for (uint32_t i = HDR + plen; i < CK_ALLEN_MEM_UNIT; i++)
        if (unit[i])
            return -1;
    uint8_t want[32];
    sha256_ctx h;
    sha256_init(&h);
    sha256_update(&h, unit, 32);
    sha256_update(&h, unit + HDR, plen);
    sha256_final(&h, want);
    if (!same(want, unit + 32, 32))
        return -1;
    if (!text_ok(p + 8, CK_ALLEN_NAME_MAX, 1))
        return -1;
    const uint8_t *f = p + PAYLOAD_FIXED;
    for (uint32_t i = 0; i < n; i++, f += FACT_BYTES)
        if (!text_ok(f, CK_ALLEN_KEY_MAX, 0) || !f[0] || !text_ok(f + CK_ALLEN_KEY_MAX, CK_ALLEN_VAL_MAX, 0) ||
            !f[CK_ALLEN_KEY_MAX] || f[CK_ALLEN_KEY_MAX + CK_ALLEN_VAL_MAX] > 1)
            return -1;
    zero(mem, sizeof *mem);
    mem->starts = get32(p);
    mem->lines = get32(p + 4);
    copy(mem->name, p + 8, CK_ALLEN_NAME_MAX);
    mem->nfacts = n;
    mem->nnotes = get32(p + 12 + CK_ALLEN_NAME_MAX);
    f = p + PAYLOAD_FIXED;
    for (uint32_t i = 0; i < n; i++, f += FACT_BYTES) {
        copy(mem->fact[i].key, f, CK_ALLEN_KEY_MAX);
        copy(mem->fact[i].val, f + CK_ALLEN_KEY_MAX, CK_ALLEN_VAL_MAX);
        mem->fact[i].are = f[CK_ALLEN_KEY_MAX + CK_ALLEN_VAL_MAX];
    }
    *gen = get64(unit + 16);
    return 0;
}

static int geom(const disk_dev *d, uint32_t *per)
{
    if (disk_check(d) != DISK_OK || (d->block_size != 512 && d->block_size != 4096))
        return CK_ALLEN_MEM_EGEOM;
    *per = CK_ALLEN_MEM_UNIT / d->block_size;
    if (d->block_count < (uint64_t)*per * CK_ALLEN_MEM_UNITS)
        return CK_ALLEN_MEM_EGEOM;
    return CK_ALLEN_MEM_OK;
}

int ck_allen_store_load(ck_allen_store *st, const disk_dev *part, ck_allen_memory *mem)
{
    uint32_t per = 0;
    zero(st, sizeof *st);
    zero(mem, sizeof *mem);
    st->dev = part;
    int rc = geom(part, &per);
    if (rc)
        return rc;
    for (unsigned c = 0; c < 2; c++) {
        ck_allen_memory m;
        uint64_t g = 0;
        if (disk_read(part, (uint64_t)c * per, per, g_unit) != DISK_OK)
            return CK_ALLEN_MEM_EIO;
        if (ck_allen_mem_decode(g_unit, &m, &g))
            continue;
        st->valid++;
        if (g > st->gen) {
            st->gen = g;
            st->newest = c;
            *mem = m;
        }
    }
    return st->gen ? CK_ALLEN_MEM_OK : CK_ALLEN_MEM_EMPTY;
}

int ck_allen_store_save(ck_allen_store *st, const ck_allen_memory *mem)
{
    uint32_t per = 0;
    int rc = geom(st->dev, &per);
    if (rc)
        return rc;
    unsigned target = st->gen ? 1u - st->newest : 0u; /* the older copy (A on a first save) */
    uint64_t lba = (uint64_t)target * per;
    ck_allen_mem_encode(mem, st->gen + 1, g_unit);
    if (disk_write(st->dev, lba, per, g_unit) != DISK_OK || disk_flush(st->dev) != DISK_OK)
        return CK_ALLEN_MEM_EIO;
    if (disk_read(st->dev, lba, per, g_back) != DISK_OK)
        return CK_ALLEN_MEM_EIO;
    if (!same(g_unit, g_back, CK_ALLEN_MEM_UNIT))
        return CK_ALLEN_MEM_EVERIFY;
    st->gen++;
    st->newest = target;
    if (st->valid < 2)
        st->valid++;
    return CK_ALLEN_MEM_OK;
}

/* ---- notes: one unit each, from unit CK_ALLEN_NOTES_FIRST ---- */

static const uint8_t note_magic[8] = {'A', 'I', 'E', 'N', 'N', 'O', 'T', '1'};

void ck_allen_note_encode(const ck_allen_note *n, uint32_t index, uint8_t unit[CK_ALLEN_MEM_UNIT])
{
    uint32_t len = n->len < CK_ALLEN_NOTE_MAX ? n->len : CK_ALLEN_NOTE_MAX - 1u;
    zero(unit, CK_ALLEN_MEM_UNIT);
    copy(unit, note_magic, 8);
    put32(unit + 8, 1);
    put32(unit + 12, len);
    put32(unit + 16, index);
    put32(unit + 20, n->start);
    put32(unit + 24, n->gone ? 1u : 0u);
    copy(unit + HDR, n->text, len);
    seal(unit, len);
}

int ck_allen_note_decode(const uint8_t unit[CK_ALLEN_MEM_UNIT], uint32_t index, ck_allen_note *n)
{
    uint32_t len = get32(unit + 12);
    if (!same(unit, note_magic, 8) || get32(unit + 8) != 1 || len == 0 || len >= CK_ALLEN_NOTE_MAX ||
        get32(unit + 16) != index || get32(unit + 24) > 1 || get32(unit + 28))
        return -1;
    for (uint32_t i = HDR + len; i < CK_ALLEN_MEM_UNIT; i++)
        if (unit[i])
            return -1;
    for (uint32_t i = 0; i < len; i++)
        if (unit[HDR + i] < 0x20 || unit[HDR + i] > 0x7e)
            return -1;
    uint8_t want[32];
    sha256_ctx h;
    sha256_init(&h);
    sha256_update(&h, unit, 32);
    sha256_update(&h, unit + HDR, len);
    sha256_final(&h, want);
    if (!same(want, unit + 32, 32))
        return -1;
    zero(n, sizeof *n);
    copy(n->text, unit + HDR, len);
    n->len = (uint16_t)len;
    n->start = get32(unit + 20);
    n->gone = (uint8_t)get32(unit + 24);
    ck_allen_note_index(n);
    return 0;
}

static int notes_geom(const disk_dev *d, uint32_t count, uint32_t *per)
{
    int rc = geom(d, per);
    if (rc)
        return rc;
    if (count > CK_ALLEN_NOTES_MAX || d->block_count < (uint64_t)*per * (CK_ALLEN_NOTES_FIRST + count))
        return CK_ALLEN_MEM_EGEOM;
    return CK_ALLEN_MEM_OK;
}

int ck_allen_notes_load(const disk_dev *part, uint32_t count, ck_allen_notes *ns, uint32_t *damaged)
{
    uint32_t per = 0;
    zero(ns, sizeof *ns);
    *damaged = 0;
    int rc = notes_geom(part, count, &per);
    if (rc)
        return rc;
    for (uint32_t i = 0; i < count; i++) {
        if (disk_read(part, (uint64_t)(CK_ALLEN_NOTES_FIRST + i) * per, per, g_unit) != DISK_OK)
            return CK_ALLEN_MEM_EIO;
        if (ck_allen_note_decode(g_unit, i, &ns->note[i])) {
            zero(&ns->note[i], sizeof ns->note[i]);
            ns->note[i].gone = 1;
            (*damaged)++;
        }
    }
    ns->n = count;
    return CK_ALLEN_MEM_OK;
}

int ck_allen_note_write(const disk_dev *part, const ck_allen_notes *ns, uint32_t index)
{
    uint32_t per = 0;
    int rc = notes_geom(part, index + 1u, &per);
    if (rc)
        return rc;
    uint64_t lba = (uint64_t)(CK_ALLEN_NOTES_FIRST + index) * per;
    ck_allen_note_encode(&ns->note[index], index, g_unit);
    if (disk_write(part, lba, per, g_unit) != DISK_OK || disk_flush(part) != DISK_OK)
        return CK_ALLEN_MEM_EIO;
    if (disk_read(part, lba, per, g_back) != DISK_OK)
        return CK_ALLEN_MEM_EIO;
    return same(g_unit, g_back, CK_ALLEN_MEM_UNIT) ? CK_ALLEN_MEM_OK : CK_ALLEN_MEM_EVERIFY;
}
