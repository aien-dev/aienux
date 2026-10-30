/* setup_store.c -- the setup wizard's records on disk; see setup_store.h. */
#include "setup_store.h"
#include "sha256.h"

static uint8_t g_unit[CK_ALLEN_MEM_UNIT], g_back[CK_ALLEN_MEM_UNIT];
static const uint8_t magic[8] = {'A', 'I', 'E', 'N', 'S', 'E', 'T', '1'};

static void put32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void put64(uint8_t *p, uint64_t v)
{
    put32(p, (uint32_t)v);
    put32(p + 4, (uint32_t)(v >> 32));
}
static uint64_t get64(const uint8_t *p) { return (uint64_t)get32(p) | (uint64_t)get32(p + 4) << 32; }

static int geom(const disk_dev *d, uint32_t *per)
{
    if (!d || !d->block_size || CK_ALLEN_MEM_UNIT % d->block_size)
        return CK_ALLEN_MEM_EGEOM;
    *per = CK_ALLEN_MEM_UNIT / d->block_size;
    if (d->block_count < (uint64_t)(CK_SETUP_FIRST + CK_SETUP_UNITS) * *per)
        return CK_ALLEN_MEM_EGEOM;
    return 0;
}

static void fingerprint(const uint8_t *unit, uint32_t len, uint8_t out[32])
{
    sha256_ctx h;
    sha256_init(&h);
    sha256_update(&h, unit, 32);
    sha256_update(&h, unit + 64, len);
    sha256_final(&h, out);
}

/* 1 when the unit is a valid copy */
static int valid(const uint8_t *u, uint64_t *gen, uint32_t *len)
{
    for (int i = 0; i < 8; i++)
        if (u[i] != magic[i])
            return 0;
    *len = get32(u + 12);
    *gen = get64(u + 16);
    if (get32(u + 8) != 1 || *len > CK_SETUP_TEXT || !*gen || get64(u + 24))
        return 0;
    for (uint32_t i = 0; i < *len; i++)
        if ((u[64 + i] < ' ' || u[64 + i] > '~') && u[64 + i] != '\n')
            return 0;
    uint8_t want[32];
    fingerprint(u, *len, want);
    uint8_t diff = 0;
    for (int i = 0; i < 32; i++)
        diff |= (uint8_t)(want[i] ^ u[32 + i]);
    return diff == 0;
}

int ck_setup_load(ck_setup_store *st, const disk_dev *part, char *text, unsigned cap, unsigned *n)
{
    uint32_t per = 0;
    st->dev = part, st->gen = 0, st->newest = 0;
    *n = 0;
    if (cap)
        text[0] = 0;
    int rc = geom(part, &per);
    if (rc)
        return rc;
    for (unsigned c = 0; c < CK_SETUP_UNITS; c++) {
        uint64_t g;
        uint32_t len;
        if (disk_read(part, (uint64_t)(CK_SETUP_FIRST + c) * per, per, g_unit) != DISK_OK)
            return CK_ALLEN_MEM_EIO;
        if (!valid(g_unit, &g, &len) || g <= st->gen)
            continue;
        st->gen = g, st->newest = c;
        uint32_t m = cap && len >= cap ? cap - 1 : len;
        for (uint32_t i = 0; cap && i < m; i++)
            text[i] = (char)g_unit[64 + i];
        if (cap)
            text[m] = 0;
        *n = m;
    }
    return st->gen ? 0 : 1;
}

int ck_setup_save(ck_setup_store *st, const char *text, unsigned n)
{
    uint32_t per = 0;
    int rc = geom(st->dev, &per);
    if (rc)
        return rc;
    if (n > CK_SETUP_TEXT)
        return CK_ALLEN_MEM_EGEOM;
    unsigned target = st->gen ? 1u - st->newest : 0u;
    for (unsigned i = 0; i < CK_ALLEN_MEM_UNIT; i++)
        g_unit[i] = 0;
    for (int i = 0; i < 8; i++)
        g_unit[i] = magic[i];
    put32(g_unit + 8, 1);
    put32(g_unit + 12, n);
    put64(g_unit + 16, st->gen + 1);
    for (unsigned i = 0; i < n; i++)
        g_unit[64 + i] = (uint8_t)text[i];
    fingerprint(g_unit, n, g_unit + 32);
    uint64_t lba = (uint64_t)(CK_SETUP_FIRST + target) * per;
    if (disk_write(st->dev, lba, per, g_unit) != DISK_OK || disk_flush(st->dev) != DISK_OK)
        return CK_ALLEN_MEM_EIO;
    if (disk_read(st->dev, lba, per, g_back) != DISK_OK)
        return CK_ALLEN_MEM_EIO;
    for (unsigned i = 0; i < CK_ALLEN_MEM_UNIT; i++)
        if (g_unit[i] != g_back[i])
            return CK_ALLEN_MEM_EVERIFY;
    st->gen++;
    st->newest = target;
    return 0;
}
