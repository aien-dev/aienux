/* Crafted byte tables from Intel VT-d rev. 5.0 sections 8.1-8.4.
 * These tests inventory firmware; they do not exercise an IOMMU. */
#include <stdint.h>
#include <string.h>
#include "ck_test.h"
#include "dmar.h"

static uint8_t table[4096];
static void w16(unsigned at, uint16_t v) { table[at] = (uint8_t)v; table[at + 1] = (uint8_t)(v >> 8); }
static void w32(unsigned at, uint32_t v) { for (unsigned i = 0; i < 4; i++) table[at + i] = (uint8_t)(v >> (8 * i)); }
static void w64(unsigned at, uint64_t v) { for (unsigned i = 0; i < 8; i++) table[at + i] = (uint8_t)(v >> (8 * i)); }
static void seal(unsigned bytes)
{
    w32(4, bytes);
    table[9] = 0;
    uint8_t sum = 0;
    for (unsigned i = 0; i < bytes; i++) sum = (uint8_t)(sum + table[i]);
    table[9] = (uint8_t)-sum;
}
static void header(void)
{
    memset(table, 0, sizeof table);
    memcpy(table, "DMAR", 4);
    table[8] = 1;
    table[36] = 38; /* encoded width: 39 physical address bits */
    table[37] = 1;
}
static unsigned fixture(void)
{
    header();
    /* Explicit unit with a two-hop PCI path. */
    w16(48, 0); w16(50, 26); w64(56, 0xfed90000ull);
    table[64] = 1; table[65] = 10;
    table[70] = 28; table[71] = 0; table[72] = 0; table[73] = 1;
    /* Include-all unit with an IOAPIC scope, in another segment. */
    w16(74, 0); w16(76, 24); table[78] = 1; w16(80, 2); w64(82, 0xfed91000ull);
    table[90] = 3; table[91] = 8; table[94] = 7; table[95] = 4;
    table[96] = 31; table[97] = 7;
    /* Reserved DMA region with one directly attached endpoint. */
    w16(98, 1); w16(100, 32); w16(104, 2);
    w64(106, 0x200000); w64(114, 0x203fff);
    table[122] = 1; table[123] = 8; table[127] = 3; table[128] = 20;
    /* Unknown future structure: skip by its declared length. */
    w16(130, 7); w16(132, 4);
    seal(134);
    return 134;
}
static void rejected(unsigned bytes, int expected)
{
    struct ck_dmar out, zero = {0};
    memset(&out, 0xa5, sizeof out);
    seal(bytes);
    CHECK(ck_dmar_parse(table, bytes, &out) == expected);
    CHECK(memcmp(&out, &zero, sizeof out) == 0);
}
int main(void)
{
    struct ck_dmar out;
    unsigned bytes = fixture();
    CHECK(ck_dmar_parse(table, bytes, &out) == 0);
    CHECK(out.bytes == bytes && out.address_bits == 39 && out.flags == 1);
    CHECK(out.nunits == 2 && out.nregions == 1 && out.nscopes == 3 && out.nother == 1);
    CHECK(out.unit[0].base == 0xfed90000ull && out.unit[0].register_bytes == 4096);
    CHECK(out.unit[1].include_all == 1 && out.unit[1].segment == 2);
    CHECK(out.scope[0].npath == 2 && out.scope[0].path[0].device == 28 && out.scope[0].path[1].function == 1);
    CHECK(out.scope[1].type == 3 && out.scope[1].enumeration_id == 7 && out.scope[1].start_bus == 4);
    CHECK(out.scope[2].kind == 1 && out.scope[2].owner == 0 && out.scope[2].start_bus == 3);
    CHECK(out.region[0].base == 0x200000 && out.region[0].limit == 0x203fff && out.region[0].segment == 2);
    /* Every prefix, even one byte short of a complete table, must refuse. */
    for (unsigned n = 0; n < bytes; n++) {
        memset(&out, 0xa5, sizeof out);
        CHECK(ck_dmar_parse(table, n, &out) == CK_DMAR_BAD_TABLE);
        CHECK(out.nunits == 0 && out.nregions == 0 && out.nscopes == 0);
    }
    CHECK(ck_dmar_parse(0, bytes, &out) == CK_DMAR_BAD_TABLE);
    CHECK(ck_dmar_parse(table, bytes, 0) == CK_DMAR_BAD_TABLE);
    table[9]++; CHECK(ck_dmar_parse(table, bytes, &out) == CK_DMAR_BAD_TABLE);
    fixture(); table[0] = 'X'; rejected(bytes, CK_DMAR_BAD_TABLE);
    fixture(); table[8] = 2; rejected(bytes, CK_DMAR_UNSUPPORTED);
    fixture(); table[36] = 64; rejected(bytes, CK_DMAR_BAD_TABLE);
    fixture(); table[38] = 1; rejected(bytes, CK_DMAR_BAD_TABLE);
    fixture(); w16(50, 0); rejected(bytes, CK_DMAR_BAD_TABLE);
    fixture(); w16(50, 3); rejected(bytes, CK_DMAR_BAD_TABLE);
    fixture(); w16(50, 200); rejected(bytes, CK_DMAR_BAD_TABLE);
    fixture(); w16(50, 15); rejected(bytes, CK_DMAR_BAD_TABLE);
    fixture(); w64(56, 0); rejected(bytes, CK_DMAR_BAD_TABLE);
    fixture(); w64(56, 0xfed90001ull); rejected(bytes, CK_DMAR_BAD_TABLE);
    fixture(); table[53] = 1; /* 8 KiB register set, base not 8 KiB aligned */
    w64(56, 0xfed91000ull); rejected(bytes, CK_DMAR_BAD_TABLE);
    fixture(); table[65] = 9; rejected(bytes, CK_DMAR_BAD_TABLE);
    fixture(); table[65] = 7; rejected(bytes, CK_DMAR_BAD_TABLE);
    fixture(); table[66] = 1; rejected(bytes, CK_DMAR_BAD_TABLE);
    fixture(); table[70] = 32; rejected(bytes, CK_DMAR_BAD_TABLE);
    fixture(); table[71] = 8; rejected(bytes, CK_DMAR_BAD_TABLE);
    fixture(); table[64] = 6; rejected(bytes, CK_DMAR_UNSUPPORTED);
    fixture(); table[52] = 1; rejected(bytes, CK_DMAR_BAD_TABLE); /* PCI scope under include-all */
    fixture(); w64(106, 0x200001); rejected(bytes, CK_DMAR_BAD_TABLE);
    fixture(); w64(114, 0x203ffe); rejected(bytes, CK_DMAR_BAD_TABLE);
    fixture(); w64(114, 0x1fffff); rejected(bytes, CK_DMAR_BAD_TABLE);
    fixture(); w16(100, 24); rejected(122, CK_DMAR_BAD_TABLE); /* RMRR needs a scope */
    header(); seal(48); rejected(48, CK_DMAR_BAD_TABLE); /* no remapping unit */
    /* Capacity exhaustion clears all of the earlier valid entries. */
    header();
    unsigned at = 48;
    for (unsigned n = 0; n <= CK_DMAR_MAX_UNITS; n++, at += 16) {
        w16(at, 0); w16(at + 2, 16); w64(at + 8, 0xfed90000ull + 4096 * n);
    }
    rejected(at, CK_DMAR_CAPACITY);
    /* A long path and either shared scope pool or region pool must stop
     * before writing past their fixed storage, with no partial inventory. */
    header();
    w16(50, 16 + 6 + 2 * (CK_DMAR_MAX_PATH + 1));
    w64(56, 0xfed90000ull); table[64] = 1;
    table[65] = 6 + 2 * (CK_DMAR_MAX_PATH + 1);
    rejected(48 + 16 + table[65], CK_DMAR_CAPACITY);
    header();
    w16(50, 16 + 8 * CK_DMAR_MAX_SCOPES); w64(56, 0xfed90000ull);
    at = 64;
    for (unsigned n = 0; n < CK_DMAR_MAX_SCOPES; n++, at += 8) {
        table[at] = 1; table[at + 1] = 8;
    }
    /* The last scope is in an RMRR: capacities apply across both owners. */
    w16(at, 1); w16(at + 2, 32); w64(at + 16, 4095);
    table[at + 24] = 1; table[at + 25] = 8;
    rejected(at + 32, CK_DMAR_CAPACITY);
    header(); w16(50, 16); w64(56, 0xfed90000ull);
    at = 64;
    for (unsigned n = 0; n <= CK_DMAR_MAX_REGIONS; n++, at += 32) {
        w16(at, 1); w16(at + 2, 32);
        w64(at + 8, 4096 * n); w64(at + 16, 4096 * n + 4095);
        table[at + 24] = 1; table[at + 25] = 8;
    }
    rejected(at, CK_DMAR_CAPACITY);
    fixture(); w64(114, (1ull << 39) | 4095); rejected(bytes, CK_DMAR_BAD_TABLE);
    fixture(); table[36] = 63; w64(114, UINT64_MAX); seal(bytes);
    CHECK(ck_dmar_parse(table, bytes, &out) == 0 && out.address_bits == 64);
    /* Extra supplied bytes are not silently added to the declared table. */
    bytes = fixture(); table[bytes] = 0xff;
    CHECK(ck_dmar_parse(table, sizeof table, &out) == 0 && out.bytes == bytes);
    return ck_t_verdict("CK_DMAR");
}
