/* gpt_add.h -- the one GPT change the AIENOS kernel may make: add an AIENOS
 * partition (disk_part.h type GUID) in space no partition uses, on a disk
 * whose GPT already passes ck_gpt_find_aienos's strict check with "no AIENOS
 * partition". Existing entries are never moved, resized or rewritten; the
 * protective MBR (LBA 0) is never written. Writes, in this order: the backup
 * entry array and backup header, flush, then the primary entry array and
 * primary header, flush; then the whole GPT is checked again and must show
 * the new partition exactly. A power cut part way leaves one copy old and
 * one new; the strict check then refuses both until a person looks, so
 * nothing further is written (the firmware keeps reading the primary).
 *
 * Also ck_gpt_list: every used entry, read only, for the boot report.
 * Freestanding: no libc, no heap. Host tested in dev/tests/gpt_add_test.c. */
#ifndef AIENOS_CK_GPT_ADD_H
#define AIENOS_CK_GPT_ADD_H
#include <stdint.h>
#include "disk.h"

enum {
    CK_GPT_ENOSPACE = -420, /* no unused gap holds the partition */
    CK_GPT_ENOSLOT = -421,  /* no free entry in the array */
    CK_GPT_EBIG = -422,     /* entry array larger than this file handles (64 KiB) */
    CK_GPT_EWRITE = -423,   /* a write or flush failed */
    CK_GPT_EVERIFY = -424,  /* after writing, the GPT does not show the partition exactly */
};

typedef struct {
    uint32_t index;
    uint8_t type[16];      /* on-disk byte order */
    uint64_t first, last;  /* inclusive LBAs */
    char name[37];         /* the UTF-16 name as ASCII, '?' for anything else */
} ck_gpt_entry;

/* Calls fn for every used entry when both GPT copies are valid (any
 * ck_gpt_find_aienos answer that got that far). The number of used entries,
 * or the CK_GPT_* refusal. */
int ck_gpt_list(const disk_dev *d, void (*fn)(void *ctx, const ck_gpt_entry *e), void *ctx);

typedef struct {
    uint64_t first, last; /* the AIENOS partition, inclusive */
    uint32_t index;       /* its entry */
    int created;          /* 1: added now; 0: it was already there */
} ck_gpt_added;

/* Makes sure the disk has an AIENOS partition: an existing one is reported
 * (nothing written); otherwise one of `blocks` blocks is added at the start
 * of the first unused gap that holds it, its first LBA rounded up to a
 * multiple of `align` blocks. CK_GPT_OK or the refusal (nothing written
 * unless the code is CK_GPT_EWRITE or CK_GPT_EVERIFY). */
int ck_gpt_add_aienos(const disk_dev *d, uint64_t blocks, uint64_t align, ck_gpt_added *out);

/* Text for the codes here and those of disk_part.h. */
const char *ck_gpt_add_strerror(int rc);
#endif
