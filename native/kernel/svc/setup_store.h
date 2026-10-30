/* setup_store.h -- the setup wizard's records on Allen's disk section, just
 * after his memory and notes (allen_memory.h): two copies, so a power cut in
 * the middle of a save spoils only the copy being written.
 *
 *   unit CK_SETUP_FIRST       copy A      unit CK_SETUP_FIRST + 1   copy B
 *
 * One copy is one unit (4096 bytes):
 *   0   "AIENSET1"   8 bytes
 *   8   version (1)  u32 little endian
 *   12  text length  u32 (at most CK_SETUP_TEXT)
 *   16  generation   u64, +1 on every save
 *   24  zero         8 bytes
 *   32  SHA-256 of bytes 0..31 and the text
 *   64  the text: printable ASCII and newlines (svc/setup_wizard.h: the saved
 *       step, then llm.conf and the consent record, each starting with its
 *       own first line); never a key: sw_save and sw_llm_conf hold none
 * A save writes generation g+1 over the older copy, flushes, reads it back
 * and checks it. Loading keeps the valid copy with the highest generation.
 * Freestanding; host tested in svc/tests/setup_wizard_test.c. */
#ifndef AIENOS_SETUP_STORE_H
#define AIENOS_SETUP_STORE_H
#include <stdint.h>
#include "disk.h"
#include "allen_memory.h"

#define CK_SETUP_FIRST CK_ALLEN_DISK_UNITS
#define CK_SETUP_UNITS 2u
#define CK_SETUP_TEXT (CK_ALLEN_MEM_UNIT - 64u)

typedef struct {
    const disk_dev *dev; /* the AIENOS partition view */
    uint64_t gen;        /* newest valid copy's generation, 0 none */
    unsigned newest;
} ck_setup_store;

/* 0 and text (NUL-terminated, *n bytes), 1 when no valid copy (a first start), or <0 (CK_ALLEN_MEM_E*). */
int ck_setup_load(ck_setup_store *st, const disk_dev *part, char *text, unsigned cap, unsigned *n);
/* 0, or <0; on error the previous newest copy is untouched. */
int ck_setup_save(ck_setup_store *st, const char *text, unsigned n);
#endif
