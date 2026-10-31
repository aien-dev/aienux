/* gpu_sim.h -- a simulated GB10 for host tests of gpu/ (cut D). NOT the chip.
 *
 * Memory: one array the tests own (the unified LPDDR window). Registers:
 * every read and write is logged in order. GPU-side accesses translate
 * through the page tables in that memory with a decoder written separately
 * from gmmu.c (from dev_mmu.h directly), through a small TLB that only a
 * completed invalidate empties, so a missing invalidate shows up as a stale
 * translation. A fault is recorded and the access does not touch memory.
 *
 * Simulator choices where the docs are silent are marked SIM-CHOICE.
 * Limitation: a completed invalidate matches TLB entries by PDB address
 * only; the PDB aperture bit is not checked here (the register-log test
 * in test_gpu_sim.c is the guard for its encoding). */
#ifndef CK_GPU_SIM_H
#define CK_GPU_SIM_H

#include <stdint.h>
#include "gmmu.h"
#include "gpu_chan.h"

#define CK_GSIM_LOG_MAX 512u
#define CK_GSIM_TLB 16u
#define CK_GSIM_FAULTS 16u
#define CK_GSIM_CHANS 4u

enum ck_gsim_fault_kind {
    CK_GSIM_F_UNMAPPED = 1, /* PDE aperture INVALID or PTE VALID=0 */
    CK_GSIM_F_BAD_PDE,      /* PTE bit set at PD4/PD3, where the format has no PTE */
    CK_GSIM_F_APERTURE,     /* VIDMEM or PEER: GB10 has neither (INFERRED, unified memory) */
    CK_GSIM_F_RO_WRITE,     /* write through a read-only PCF */
    CK_GSIM_F_OUTSIDE,      /* table or page physical address outside the memory array */
};

struct ck_gsim_reg_ev {
    uint32_t off;
    uint32_t val;
    uint8_t write;
};

struct ck_gsim_fault {
    uint64_t va;
    uint64_t pdb;
    uint32_t kind;
    uint8_t write;
};

struct ck_gsim_tlb {
    uint64_t pdb, va_page, pa_page, page;
    uint32_t pte_lo; /* low 12 bits of the PTE: aperture, PCF, kind */
    uint8_t valid;
};

struct ck_gsim_chan {
    struct ck_gpu_chan_desc d;
    uint32_t gp_get;
    uint32_t words_read;
    uint8_t bound, faulted, disabled;
};

struct ck_gsim {
    struct ck_gpu_window mem;
    struct ck_gsim_reg_ev log[CK_GSIM_LOG_MAX];
    uint32_t nlog, log_dropped;
    /* invalidate engine */
    uint32_t pdb_lo, pdb_hi;
    uint32_t inval_pending, inval_left, inval_latency, inval_stuck, inval_done, inval_all;
    /* TLB and faults */
    struct ck_gsim_tlb tlb[CK_GSIM_TLB];
    uint32_t tlb_next, tlb_hits;
    struct ck_gsim_fault faults[CK_GSIM_FAULTS];
    uint32_t nfaults;
    /* channels */
    struct ck_gsim_chan chan[CK_GSIM_CHANS];
    uint32_t doorbells, doorbell_unknown;
    uint32_t hang;      /* channels never go idle and never consume (teardown timeout tests) */
    uint32_t idle_polls;
};

void ck_gsim_init(struct ck_gsim *s, uint8_t *mem, uint64_t pa_base, uint64_t size);
/* Register file for the code under test. */
struct ck_gpu_regs ck_gsim_regs(struct ck_gsim *s);
/* Channel control the code under test calls (stands in for GSP RPC/CHRAM). */
struct ck_gpu_chan_ops ck_gsim_chan_ops(void);

/* GPU-side 32-bit access through the address space whose PD4 is at pdb.
 * Returns 0 or the fault kind (and records it). */
uint32_t ck_gsim_gpu_read32(struct ck_gsim *s, uint64_t pdb, uint64_t va, uint32_t *out);
uint32_t ck_gsim_gpu_write32(struct ck_gsim *s, uint64_t pdb, uint64_t va, uint32_t val);
/* Translate without touching the TLB or recording a fault (test oracle). */
uint32_t ck_gsim_walk(const struct ck_gsim *s, uint64_t pdb, uint64_t va, int write, uint64_t *pa, uint64_t *page,
                      uint32_t *pte_lo);

const struct ck_gsim_chan *ck_gsim_chan(const struct ck_gsim *s, uint32_t chid);

#endif
