/* Intel VT-d firmware inventory only. No register access or DMA authority.
 * Source: Intel VT-d Architecture Specification rev. 5.0, sections 8.1-8.4
 * https://cdrdv2-public.intel.com/831418/vt-directed-io-spec.pdf */
#ifndef AIENOS_CK_DMAR_H
#define AIENOS_CK_DMAR_H
#include <stddef.h>
#include <stdint.h>

#define CK_DMAR_MAX_BYTES 65536u
#define CK_DMAR_MAX_UNITS 8u
#define CK_DMAR_MAX_REGIONS 16u
#define CK_DMAR_MAX_SCOPES 64u
#define CK_DMAR_MAX_PATH 8u
#define CK_DMAR_BAD_TABLE (-1)
#define CK_DMAR_CAPACITY (-2)
#define CK_DMAR_UNSUPPORTED (-3)

struct ck_dmar_unit {
    uint64_t base;
    uint32_t register_bytes;
    uint16_t segment;
    uint8_t include_all;
};
struct ck_dmar_region {
    uint64_t base, limit; /* inclusive, not an allocation or a granted window */
    uint16_t segment;
};
struct ck_dmar_scope {
    uint16_t owner; /* index into unit[] (kind 0) or region[] (kind 1) */
    uint8_t kind, type, enumeration_id, start_bus, npath;
    struct { uint8_t device, function; } path[CK_DMAR_MAX_PATH];
};
struct ck_dmar {
    uint32_t bytes, nunits, nregions, nscopes, nother;
    uint8_t address_bits, flags;
    struct ck_dmar_unit unit[CK_DMAR_MAX_UNITS];
    struct ck_dmar_region region[CK_DMAR_MAX_REGIONS];
    struct ck_dmar_scope scope[CK_DMAR_MAX_SCOPES];
};
/* Bounded parse of exactly one DMAR table. Checks the supplied buffer span,
 * table checksum, nested lengths, reserved fields and storage capacities.
 * Success is an inventory, never proof that protection is enabled. Other
 * structure types are counted and skipped as required for forward compatibility.
 * Unknown scope types or revisions are UNSUPPORTED. Any failure clears out.
 * Multi-hop paths remain paths: resolving their final requester ID requires
 * reading each bridge's secondary bus, not guessing it from the last hop. */
int ck_dmar_parse(const void *table, size_t available, struct ck_dmar *out);
#endif
