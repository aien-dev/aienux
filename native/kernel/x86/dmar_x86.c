/* Firmware inventory before PCI drivers start. No remapping register access. */
#include "../core/ck_internal.h"
#include "../core/dmar.h"
#include "x86.h"

void ck_x86_dmar_inventory(uint64_t rsdp)
{
    static struct ck_dmar d;
    const uint8_t *table = rsdp ? ck_acpi_lookup(rsdp, "DMAR") : 0;
    if (!table) {
        ck_puts("dmar: absent-or-invalid; DMA confinement NOT QUALIFIED\n");
        return;
    }
    uint32_t bytes;
    memcpy(&bytes, table + 4, sizeof bytes);
    int rc = ck_dmar_parse(table, bytes, &d);
    if (rc) {
        ck_printf("dmar: inventory refused rc=%d; DMA confinement NOT QUALIFIED\n", rc);
        return;
    }
    ck_printf("dmar: inventory bytes=%u address_bits=%u flags=%u units=%u regions=%u scopes=%u other=%u\n",
              d.bytes, d.address_bits, d.flags, d.nunits, d.nregions, d.nscopes, d.nother);
    for (unsigned i = 0; i < d.nunits; i++) {
        const struct ck_dmar_unit *u = &d.unit[i];
        ck_printf("dmar: unit=%u segment=%u base=0x%llx register_bytes=%u include_all=%u\n",
                  i, u->segment, (unsigned long long)u->base, u->register_bytes, u->include_all);
    }
    for (unsigned i = 0; i < d.nregions; i++) {
        const struct ck_dmar_region *r = &d.region[i];
        ck_printf("dmar: region=%u segment=%u base=0x%llx inclusive_limit=0x%llx\n",
                  i, r->segment, (unsigned long long)r->base, (unsigned long long)r->limit);
    }
    for (unsigned i = 0; i < d.nscopes; i++) {
        const struct ck_dmar_scope *s = &d.scope[i];
        unsigned segment = s->kind ? d.region[s->owner].segment : d.unit[s->owner].segment;
        char path[CK_DMAR_MAX_PATH * 6 + 1];
        unsigned n = 0;
        for (unsigned j = 0; j < s->npath; j++)
            n += (unsigned)ck_snprintf(path + n, sizeof path - n, j ? "/%02x.%u" : "%02x.%u",
                                      s->path[j].device, s->path[j].function);
        ck_printf("dmar: scope=%u owner=%s:%u segment=%u type=%u enum=%u start_bus=%u path=%s\n",
                  i, s->kind ? "region" : "unit", s->owner, segment, s->type,
                  s->enumeration_id, s->start_bus, path);
        if (s->npath == 1) {
            unsigned rid = (unsigned)s->start_bus << 8 | (unsigned)s->path[0].device << 3 |
                           s->path[0].function;
            ck_printf("dmar: scope=%u direct_source_id=%04x downstream=%s\n", i, rid,
                      s->type == 2 ? "requires-bridge-walk" : "none");
        } else {
            ck_printf("dmar: scope=%u source_id=unresolved requires-bridge-walk\n", i);
        }
    }
    ck_puts("dmar: inventory-only register_access=none; DMA confinement NOT QUALIFIED\n");
}
