/* allen_memory.h -- Allen's memory on disk: ck_allen_memory (allen_reply.h)
 * kept in two copies at the start of the AIENOS partition, so a power cut in
 * the middle of a save can only spoil the copy being written.
 *
 *   unit 0   copy A      unit 1   copy B      (a unit is 4096 bytes)
 *   unit 2 + i   note i, for i below the note count held in the copies
 *
 * One copy is one unit:
 *   0   "AIENALM1"                        8 bytes
 *   8   version (2)                       u32 little endian
 *   12  payload length                    u32
 *   16  generation                        u64, +1 on every save
 *   24  zero                              8 bytes
 *   32  SHA-256 of bytes 0..31 and the payload
 *   64  payload: starts u32, lines u32, name[24], fact count u32, note
 *       count u32, then per fact key[48] val[64] are(u8); the rest zero
 * A save writes generation g+1 over the OLDER copy, flushes the disk's write
 * cache, then reads it back and checks it; the newer copy is never touched
 * until the older one holds the newer state. Loading reads both and keeps
 * the valid copy with the highest generation. A copy is valid only when every
 * field checks out: magic, version, exact length, fingerprint, fact count,
 * NUL-terminated folded text (a-z, 0-9, single spaces), a name of letters.
 * Freestanding (no libc, no heap): every disk access goes through the
 * partition view handed in. Host tested in svc/tests/allen_memory_test.c,
 * power cuts included. */
#ifndef AIENOS_ALLEN_MEMORY_H
#define AIENOS_ALLEN_MEMORY_H
#include <stdint.h>
#include "disk.h"
#include "allen_notes.h"
#include "allen_reply.h"

#define CK_ALLEN_MEM_UNIT 4096u
#define CK_ALLEN_MEM_UNITS 2u   /* the two copies */
#define CK_ALLEN_NOTES_FIRST 2u /* the unit of note 0 */
/* Units of the partition this file ever touches. */
#define CK_ALLEN_DISK_UNITS (CK_ALLEN_NOTES_FIRST + CK_ALLEN_NOTES_MAX)

enum {
    CK_ALLEN_MEM_OK = 0,      /* loaded the newest valid copy */
    CK_ALLEN_MEM_EMPTY = 1,   /* no valid copy: a first start (or both spoiled) */
    CK_ALLEN_MEM_EIO = -501,  /* a read, write or flush failed */
    CK_ALLEN_MEM_EGEOM = -502, /* partition view too small or odd block size */
    CK_ALLEN_MEM_EVERIFY = -503, /* the copy read back differs from the copy written */
};

typedef struct {
    const disk_dev *dev; /* the AIENOS partition view */
    uint64_t gen;        /* generation of the newest valid copy, 0 = none */
    unsigned newest;     /* which copy holds it (0 = A, 1 = B) */
    unsigned valid;      /* copies that were valid at load (0..2) */
} ck_allen_store;

/* Encode / decode one copy (exposed for the tests). decode: 0 and *mem,
 * *gen filled, or -1 when the copy is not valid. */
void ck_allen_mem_encode(const ck_allen_memory *mem, uint64_t gen, uint8_t unit[CK_ALLEN_MEM_UNIT]);
int ck_allen_mem_decode(const uint8_t unit[CK_ALLEN_MEM_UNIT], ck_allen_memory *mem, uint64_t *gen);

/* Read both copies. OK (mem = newest), EMPTY (mem zeroed) or an error. */
int ck_allen_store_load(ck_allen_store *st, const disk_dev *part, ck_allen_memory *mem);
/* Save mem as the next generation (see above). OK or an error; on error
 * the previous newest copy is still on disk and st is unchanged. */
int ck_allen_store_save(ck_allen_store *st, const ck_allen_memory *mem);

/* A note is one unit, written in place (a deleted note is rewritten with
 * the gone flag): "AIENNOT1", version 1, text length u32, index u32, start
 * u32, gone u32 (0|1), zero u32, SHA-256 of bytes 0..31 and the text at
 * 32, the text (printable ASCII) at 64, the rest zero. A note counts only
 * once the copies hold a note count past it: a save writes the note, then
 * the copies, so a power cut leaves either no note or a whole one. A note
 * that fails any check loads as gone and is counted in *damaged. */
void ck_allen_note_encode(const ck_allen_note *n, uint32_t index, uint8_t unit[CK_ALLEN_MEM_UNIT]);
int ck_allen_note_decode(const uint8_t unit[CK_ALLEN_MEM_UNIT], uint32_t index, ck_allen_note *n);
int ck_allen_notes_load(const disk_dev *part, uint32_t count, ck_allen_notes *ns, uint32_t *damaged);
/* Writes note index, flushes, reads it back. OK, EIO, EGEOM or EVERIFY. */
int ck_allen_note_write(const disk_dev *part, const ck_allen_notes *ns, uint32_t index);

#endif
