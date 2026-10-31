/* memory_x86.c -- Allen's memory and notes on a hard disk of whatever
 * machine AIENOS boots on.
 *
 * Which disk, at boot, once:
 *   1. the SATA disk that already holds Allen's section (a GPT partition of
 *      the AIENOS type): on any machine, the section is what makes a disk
 *      Allen's. Two disks with a section: neither is written;
 *   2. with no section anywhere, one of ALLEN_PART_MIB is added only on a disk
 *      its owner approved (approved_disks, by IDENTIFY model; Drake approved
 *      his hard disk 2026-10-17), in unused space (dev/gpt_add.c): only then
 *      are the GPT blocks (LBA 1 up to the first usable LBA, and after the
 *      last usable LBA) opened for writing, and closed again right after.
 *      Existing partitions are never touched; no unused space, an invalid GPT
 *      or no GPT: nothing written. A new machine gets its section from the
 *      installer instead, on the disk its owner picks there;
 *   3. every partition of the chosen disk is listed ("ahci_part:"), a record
 *      of the layout as found;
 *   4. the write range is set to the units of the section Allen uses (the two
 *      memory copies and the note units, svc/allen_memory.h) and nothing
 *      else; the newest valid copy is loaded, then the notes it counts.
 * NVMe disks are never written (no write path exists). With no usable disk
 * Allen remembers only until the power goes off. The conversation itself
 * never goes on the console or the network report from here: only counts
 * ("3 facts", "2 notes"). */
#include "../core/ck_internal.h"
#include "../svc/allen_memory.h"
#include "disk_part.h"
#include "gpt_add.h"
#include "x86.h"

#define ALLEN_PART_MIB 1024u

/* Disks whose owner approved adding Allen's section to them. */
static const char *const approved_disks[] = {"APPLE HDD ST1000DM003", 0};

static ck_part part;
static ck_allen_store store;
static unsigned disk_index;
static int lasting;
static uint64_t hz;

static int same_text(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

static int approved(const char *model)
{
    for (const char *const *p = approved_disks; *p; p++)
        if (same_text(model, *p))
            return 1;
    return 0;
}

static void show_entry(void *ctx, const ck_gpt_entry *e)
{
    const disk_dev *d = ctx;
    const uint8_t *t = e->type;
    ck_printf("ahci_part: disk=%u entry=%u type=%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x "
              "first_lba=%llu last_lba=%llu mib=%llu name=\"%s\"%s\n",
              disk_index, e->index, t[3], t[2], t[1], t[0], t[5], t[4], t[7], t[6], t[8], t[9], t[10], t[11], t[12],
              t[13], t[14], t[15], (unsigned long long)e->first, (unsigned long long)e->last,
              (unsigned long long)((e->last - e->first + 1u) * d->block_size >> 20), e->name,
              memcmp(t, ck_aienos_part_type, 16) ? "" : " (AIENOS)");
}

static int no_memory(const char *why)
{
    ck_printf("allen_memory: %s; Allen remembers only until this computer turns off\n", why);
    return 0;
}

/* Adds Allen's section on the approved disk d (no section anywhere yet). */
static int add_section(const disk_dev *d, const ck_gpt_info *gi)
{
    uint64_t per_mib = (1024u * 1024u) / d->block_size;
    if (ck_x86_ahci_allow(disk_index, 1, gi->first_usable - 1u) ||
        ck_x86_ahci_allow(disk_index, gi->last_usable + 1u, gi->last_lba)) {
        ck_x86_ahci_allow_none(disk_index);
        return no_memory("could not open the GPT blocks for writing");
    }
    ck_gpt_added a;
    int rc = ck_gpt_add_aienos(d, (uint64_t)ALLEN_PART_MIB * per_mib, per_mib, &a);
    ck_x86_ahci_allow_none(disk_index);
    if (rc) {
        ck_printf("allen_memory: AIENOS partition not added: %s%s\n", ck_gpt_add_strerror(rc),
                  rc == CK_GPT_EWRITE || rc == CK_GPT_EVERIFY ? " (the GPT may need a look)" : "; nothing was written");
        return no_memory("no AIENOS partition");
    }
    ck_printf("allen_memory: AIENOS partition added, entry %u, first_lba=%llu last_lba=%llu (%u MiB); the existing "
              "partitions were not touched\n", a.index, (unsigned long long)a.first, (unsigned long long)a.last,
              ALLEN_PART_MIB);
    return 1;
}

int ck_x86_memory_open(ck_allen_memory *mem, ck_allen_notes *notes, uint64_t tsc_hz)
{
    hz = tsc_hz;
    const disk_dev *home = 0, *ok = 0;
    unsigned homes = 0, oks = 0, home_i = 0, ok_i = 0;
    ck_gpt_info gi, ok_gi;
    for (unsigned i = 0;; i++) {
        const char *model = 0;
        const disk_dev *x = ck_x86_ahci_dev(i, &model);
        if (!x)
            break;
        int rc = ck_gpt_find_aienos(x, &part, &gi);
        if (rc == CK_GPT_OK) {
            home = x;
            home_i = i;
            homes++;
        } else if (rc == CK_GPT_ENOPART && approved(model)) {
            ok = x;
            ok_i = i;
            ok_gi = gi;
            oks++;
        }
    }
    if (homes > 1)
        return no_memory("two disks hold Allen's section, writing to neither");
    const disk_dev *d = homes ? home : ok;
    disk_index = homes ? home_i : ok_i;
    if (!homes && oks != 1)
        return no_memory(oks ? "two approved disks and no section yet, writing to neither"
                             : "no disk holds Allen's section and none is approved for one");
    ck_printf("allen_memory: %s disk %u (%llu blocks of %u bytes)\n",
              homes ? "Allen's section is on" : "no section yet; the approved disk is", disk_index,
              (unsigned long long)d->block_count, d->block_size);
    int n = ck_gpt_list(d, show_entry, (void *)d);
    if (n < 0)
        return no_memory(ck_gpt_add_strerror(n)); /* no GPT or an invalid one: never written */
    ck_printf("allen_memory: the disk has %d partition%s\n", n, n == 1 ? "" : "s");
    if (!homes && !add_section(d, &ok_gi))
        return 0;
    int rc = ck_gpt_find_aienos(d, &part, &gi);
    if (rc)
        return no_memory(ck_gpt_add_strerror(rc));
    if (homes)
        ck_printf("allen_memory: AIENOS partition found, entry %u, first_lba=%llu last_lba=%llu\n", gi.part_index,
                  (unsigned long long)part.first_lba, (unsigned long long)(part.first_lba + part.blocks - 1u));

    uint64_t units = (uint64_t)CK_ALLEN_DISK_UNITS * CK_ALLEN_MEM_UNIT / d->block_size;
    if (part.blocks < units || ck_x86_ahci_allow(disk_index, part.first_lba, part.first_lba + units - 1u))
        return no_memory("could not open Allen's section for writing");
    rc = ck_allen_store_load(&store, &part.dev, mem);
    if (rc < 0) {
        ck_x86_ahci_allow_none(disk_index);
        return no_memory("reading the memory copies failed");
    }
    if (rc == CK_ALLEN_MEM_EMPTY)
        ck_puts("allen_memory: empty (a first start); Allen starts fresh\n");
    else
        ck_printf("allen_memory: loaded generation %llu (%u of 2 copies valid): %u fact%s, name %s, %u earlier start%s\n",
                  (unsigned long long)store.gen, store.valid, mem->nfacts, mem->nfacts == 1 ? "" : "s",
                  mem->name[0] ? "known" : "not told", mem->starts, mem->starts == 1 ? "" : "s");
    uint32_t damaged = 0;
    if (ck_allen_notes_load(&part.dev, mem->nnotes, notes, &damaged) != CK_ALLEN_MEM_OK) {
        ck_x86_ahci_allow_none(disk_index);
        return no_memory("reading the notes failed");
    }
    ck_printf("allen_memory: %u note%s loaded (%u damaged)\n", mem->nnotes, mem->nnotes == 1 ? "" : "s", damaged);
    lasting = 1;
    return 1;
}

static int failed(const char *what, int rc)
{
    lasting = 0;
    ck_x86_ahci_allow_none(disk_index);
    ck_printf("allen_memory: %s FAILED (rc=%d); nothing more is written this start\n", what, rc);
    return -1;
}

int ck_x86_memory_save(const ck_allen_memory *mem)
{
    if (!lasting)
        return -1;
    uint64_t t0 = ck_rdtsc();
    int rc = ck_allen_store_save(&store, mem);
    if (rc)
        return failed("save", rc);
    ck_printf("allen_memory: saved generation %llu to copy %c (%u fact%s, %u note%s) in %llu ms\n",
              (unsigned long long)store.gen, store.newest ? 'B' : 'A', mem->nfacts, mem->nfacts == 1 ? "" : "s",
              mem->nnotes, mem->nnotes == 1 ? "" : "s",
              hz ? (unsigned long long)((ck_rdtsc() - t0) * 1000 / hz) : 0ull);
    return 0;
}

int ck_x86_notes_write(const ck_allen_notes *notes, uint32_t index)
{
    if (!lasting)
        return -1;
    uint64_t t0 = ck_rdtsc();
    int rc = ck_allen_note_write(&part.dev, notes, index);
    if (rc)
        return failed("note write", rc);
    ck_printf("allen_memory: note %u written in %llu ms\n", index,
              hz ? (unsigned long long)((ck_rdtsc() - t0) * 1000 / hz) : 0ull);
    return 0;
}
