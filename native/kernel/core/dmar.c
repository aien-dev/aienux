/* Byte-based reads: ACPI fields are little-endian and may be unaligned.
 * Inventory only. This file never reads or writes a device register. */
#include "dmar.h"
#include <string.h>

static uint16_t r16(const uint8_t *p) { return (uint16_t)p[0] | (uint16_t)p[1] << 8; }
static uint32_t r32(const uint8_t *p) { return (uint32_t)r16(p) | (uint32_t)r16(p + 2) << 16; }
static uint64_t r64(const uint8_t *p) { return (uint64_t)r32(p) | (uint64_t)r32(p + 4) << 32; }

static int scopes(const uint8_t *p, unsigned bytes, uint8_t kind, uint16_t owner,
                  int include_all, struct ck_dmar *d)
{
    while (bytes) {
        if (bytes < 2 || p[1] < 8 || p[1] > bytes || ((p[1] - 6) & 1))
            return CK_DMAR_BAD_TABLE;
        if (p[0] < 1 || p[0] > 5)
            return CK_DMAR_UNSUPPORTED;
        if (p[2] || p[3] || ((p[0] == 1 || p[0] == 2) && p[4]) ||
            (include_all && (p[0] == 1 || p[0] == 2)))
            return CK_DMAR_BAD_TABLE;
        unsigned npath = (p[1] - 6) / 2;
        if (npath > CK_DMAR_MAX_PATH || d->nscopes == CK_DMAR_MAX_SCOPES)
            return CK_DMAR_CAPACITY;
        struct ck_dmar_scope *s = &d->scope[d->nscopes++];
        s->kind = kind;
        s->owner = owner;
        s->type = p[0];
        s->enumeration_id = p[4];
        s->start_bus = p[5];
        s->npath = (uint8_t)npath;
        for (unsigned i = 0; i < npath; i++) {
            if (p[6 + 2 * i] > 31 || p[7 + 2 * i] > 7)
                return CK_DMAR_BAD_TABLE;
            s->path[i].device = p[6 + 2 * i];
            s->path[i].function = p[7 + 2 * i];
        }
        bytes -= p[1];
        p += p[1];
    }
    return 0;
}

int ck_dmar_parse(const void *table, size_t available, struct ck_dmar *out)
{
    if (!out)
        return CK_DMAR_BAD_TABLE;
    memset(out, 0, sizeof *out);
    if (!table || available < 48)
        return CK_DMAR_BAD_TABLE;
    const uint8_t *p = table;
    uint32_t bytes = r32(p + 4);
    if (memcmp(p, "DMAR", 4) || bytes < 48 || bytes > available)
        return CK_DMAR_BAD_TABLE;
    if (bytes > CK_DMAR_MAX_BYTES)
        return CK_DMAR_CAPACITY;
    uint8_t sum = 0;
    for (uint32_t i = 0; i < bytes; i++)
        sum = (uint8_t)(sum + p[i]);
    if (sum || p[36] > 63 || (p[37] & ~7u))
        return CK_DMAR_BAD_TABLE;
    if (p[8] != 1)
        return CK_DMAR_UNSUPPORTED;
    for (unsigned i = 38; i < 48; i++)
        if (p[i])
            return CK_DMAR_BAD_TABLE;
    struct ck_dmar d = {0};
    d.bytes = bytes;
    d.address_bits = p[36] + 1;
    d.flags = p[37];
    uint16_t previous_type = 0;
    for (uint32_t at = 48; at < bytes;) {
        if (bytes - at < 4)
            return CK_DMAR_BAD_TABLE;
        const uint8_t *entry = p + at;
        uint16_t type = r16(entry), size = r16(entry + 2);
        if (size < 4 || size > bytes - at || type < previous_type)
            return CK_DMAR_BAD_TABLE;
        previous_type = type;
        int rc = 0;
        if (type == 0) {
            if (size < 16 || (entry[4] & ~1u) || (entry[5] & 0xf0))
                return CK_DMAR_BAD_TABLE;
            if (d.nunits == CK_DMAR_MAX_UNITS)
                return CK_DMAR_CAPACITY;
            struct ck_dmar_unit *u = &d.unit[d.nunits];
            u->base = r64(entry + 8);
            u->register_bytes = 1u << (12 + entry[5]);
            u->segment = r16(entry + 6);
            u->include_all = entry[4] & 1;
            if (!u->base || (u->base & (u->register_bytes - 1)))
                return CK_DMAR_BAD_TABLE;
            rc = scopes(entry + 16, size - 16, 0, (uint16_t)d.nunits, u->include_all, &d);
            d.nunits++;
        } else if (type == 1) {
            if (size < 32 || r16(entry + 4))
                return CK_DMAR_BAD_TABLE;
            if (d.nregions == CK_DMAR_MAX_REGIONS)
                return CK_DMAR_CAPACITY;
            struct ck_dmar_region *r = &d.region[d.nregions];
            r->base = r64(entry + 8);
            r->limit = r64(entry + 16);
            r->segment = r16(entry + 6);
            if ((r->base & 4095) || (r->limit & 4095) != 4095 || r->base >= r->limit ||
                (d.address_bits < 64 && (r->limit >> d.address_bits)))
                return CK_DMAR_BAD_TABLE;
            rc = scopes(entry + 24, size - 24, 1, (uint16_t)d.nregions, 0, &d);
            d.nregions++;
        } else {
            d.nother++;
        }
        if (rc)
            return rc;
        at += size;
    }
    if (!d.nunits)
        return CK_DMAR_BAD_TABLE;
    *out = d;
    return 0;
}
