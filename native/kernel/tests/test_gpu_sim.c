/* test_gpu_sim.c -- cut D host tests: GB10 GPU page tables, allocator and
 * channel over the simulated device in gpu/gpu_sim.c. Proves the logic and
 * the encodings against the driver-source layouts, NOT the chip. Run under
 * ASan/UBSan and with each CK_GPU_MUTANT_* (make gpu-sim-mutants), where
 * every mutant must make this test FAIL. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ck_test.h"
#include "gmmu.h"
#include "gpu_mem.h"
#include "gpu_chan.h"
#include "gpu_sim.h"

#define MEM_BYTES (8u << 20) /* 8 MiB simulated unified memory */
#define PA_BASE 0x80000000ull
#define POOL_PA PA_BASE                           /* 64 page-table pages */
#define DATA_PA (PA_BASE + 64ull * 4096u)          /* data pages after the pool */
#define VA_BASE 0x0000000200000000ull              /* 8 GiB, below 2^40 */

static uint8_t *mem;
static struct ck_gsim sim;
static struct ck_gpu_regs regs;
static struct ck_gmmu_as as;
static struct ck_gmem gm;

static void fresh(uint32_t pt_pages)
{
    memset(mem, 0xA5, MEM_BYTES); /* garbage: gmmu must zero what it uses */
    ck_gsim_init(&sim, mem, PA_BASE, MEM_BYTES);
    regs = ck_gsim_regs(&sim);
    CHECK(ck_gmmu_as_init(&as, &sim.mem, POOL_PA, pt_pages) == CK_GMMU_OK);
}

static uint32_t gread(uint64_t va, uint32_t *v) { return ck_gsim_gpu_read32(&sim, as.root_pa, va, v); }
static uint32_t gwrite(uint64_t va, uint32_t v) { return ck_gsim_gpu_write32(&sim, as.root_pa, va, v); }

/* 1. Entry encodings against hand-computed dev_mmu.h bit positions. */
static void test_encodings(void)
{
    /* PTE: VALID 0, APERTURE 2:1 = SYS_COH(2), PCF 7:3 = 1, KIND 11:8 = 0, ADDR 51:12. */
    CHECK(ck_gmmu_pte(0x123456789000ull, CK_GMMU_PTE_AP_SYS_COH, CK_GMMU_PCF_RW_UNCACHED, 0) ==
          (0x123456789000ull | (1u << 3) | (2u << 1) | 1u));
    CHECK(ck_gmmu_pte(0xFFF0000000001FFFull, 3, 0x1F, 0xF) == (0x0000000000001000ull | 0xFFFull)); /* masks */
    /* PDE: IS_PTE 0 = 0, APERTURE 2:1, PCF 5:3 = 2 (cached, no ATS), ADDR 51:12. */
    CHECK(ck_gmmu_pde(0xABCDE000ull, CK_GMMU_PDE_AP_SYS_COH) == (0xABCDE000ull | (2u << 3) | (2u << 1)));
    CHECK((ck_gmmu_pde(0x1000, CK_GMMU_PDE_AP_SYS_COH) & 1u) == 0);
    /* Dual PDE: small half lands in bits 66:65 (aperture), 69:67 (PCF), 115:76 (addr >> 12). */
    uint64_t d[2];
    ck_gmmu_dual_pde(d, 0, CK_GMMU_PDE_AP_INVALID, 0x77777000ull, CK_GMMU_PDE_AP_SYS_COH);
    CHECK(d[0] == 0);
    CHECK(d[1] == (0x77777000ull | (2u << 3) | (2u << 1)));
    ck_gmmu_dual_pde(d, 0x55555500ull, CK_GMMU_PDE_AP_SYS_NC, 0, CK_GMMU_PDE_AP_INVALID);
    CHECK(d[0] == (0x55555500ull | (2u << 3) | (3u << 1))); /* ADDRESS_BIG 51:8, shift 8 */
    CHECK(d[1] == 0);
    /* Work-submit token: RUNLIST_ID 22:16, VECTOR 11:0. */
    CHECK(ck_gpu_submit_token(3, 0x2A) == 0x0003002Au);
    CHECK(ck_gpu_submit_token(0x7E, 0xFFF) == 0x007E0FFFu);
    /* Runlist USERD pointer: LO 31:8 of word 0, HI_HW 19:0 of word 1. */
    uint32_t rl[2] = {0, 0};
    CHECK(ck_gpu_rl_userd(rl, 0x000ABCDE12345600ull) == CK_GCHAN_OK);
    CHECK(rl[0] == 0x12345600u && rl[1] == 0xABCDEu);
    CHECK(ck_gpu_rl_userd(rl, 0x1000000000080ull) == CK_GCHAN_EINVAL);  /* not 256-byte aligned */
    CHECK(ck_gpu_rl_userd(rl, 1ull << 52) == CK_GCHAN_EINVAL);           /* does not fit 20+32 bits */
    /* Carried encoders, bit-identical to physics nvrm_mthd / nvrm_gp_entry. */
    CHECK(ck_gpu_mthd(1, 0x0100, 1) == ((2u << 28) | (1u << 16) | (1u << 13) | 0x40u));
    CHECK(ck_gpu_gp_entry(0x12345678ull, 10) == (0x12345678ull | (10ull << 42) | (1ull << 41)));
    /* clc96f view of the same entry: GET 31:2 / GET_HI 7:0 / LENGTH 30:10 of word 1. */
    uint64_t e = ck_gpu_gp_entry(0xAB12345674ull, 0x1FFFFF);
    CHECK((((uint32_t)e) & ~3u) == 0x12345674u);
    CHECK(((uint32_t)(e >> 32) & 0xFFu) == 0xABu);
    CHECK((((uint32_t)(e >> 32)) >> 10 & 0x1FFFFFu) == 0x1FFFFFu);
    /* LEVEL 9:9 of word 1 is set (SUBROUTINE): what physics emits on the chip today. */
    CHECK(((uint32_t)(e >> 32) >> 9 & 1u) == 1u);
}

/* 2. Map, walk through the simulator, unmap, refusals. */
static void test_map_unmap(void)
{
    fresh(64);
    uint64_t va = 0x0100000000000000ull + 0x5000; /* bit 56 set: PD4 entry 1 */
    uint64_t pa = DATA_PA + 0x3000;
    CHECK(ck_gmmu_map(&as, va, pa, 0x2000, CK_GMMU_SZ_4K, CK_GMMU_PTE_AP_SYS_COH, CK_GMMU_PCF_RW_CACHED) == CK_GMMU_OK);
    uint64_t wpa = 0, page = 0;
    uint32_t lo = 0;
    CHECK(ck_gsim_walk(&sim, as.root_pa, va + 0x1234, 0, &wpa, &page, &lo) == 0);
    CHECK(wpa == pa + 0x1234 && page == CK_GMMU_SZ_4K);
    CHECK(gwrite(va + 0x10, 0xC0FFEE01u) == 0);
    CHECK(memcmp(mem + (pa - PA_BASE) + 0x10, "\x01\xEE\xFF\xC0", 4) == 0); /* landed at the PA */
    uint32_t v = 0;
    CHECK(gread(va + 0x10, &v) == 0 && v == 0xC0FFEE01u);
    /* Unmapped neighbour faults and the access does not touch memory. */
    uint32_t nf = sim.nfaults;
    CHECK(gread(va + 0x2000, &v) == CK_GSIM_F_UNMAPPED);
    CHECK(sim.nfaults == nf + 1 && sim.faults[nf].va == va + 0x2000);
    /* Refusals leave everything unchanged. */
    CHECK(ck_gmmu_map(&as, va + 0x1000, pa, 0x1000, CK_GMMU_SZ_4K, 2, 0) == CK_GMMU_EBUSY);   /* overlap */
    CHECK(ck_gmmu_map(&as, va + 0x800, pa, 0x1000, CK_GMMU_SZ_4K, 2, 0) == CK_GMMU_EALIGN);   /* VA */
    CHECK(ck_gmmu_map(&as, va + 0x4000, pa + 1, 0x1000, CK_GMMU_SZ_4K, 2, 0) == CK_GMMU_EALIGN); /* PA */
    CHECK(ck_gmmu_map(&as, va + 0x4000, pa, 0x1800, CK_GMMU_SZ_4K, 2, 0) == CK_GMMU_EALIGN);  /* size */
    CHECK(ck_gmmu_map(&as, va + 0x4000, pa, 0, CK_GMMU_SZ_4K, 2, 0) == CK_GMMU_EINVAL);       /* empty */
    CHECK(ck_gmmu_map(&as, va + 0x4000, pa, 0x1000, 0x2000, 2, 0) == CK_GMMU_EINVAL);         /* page size */
    CHECK(ck_gmmu_map(&as, 1ull << 57, pa, 0x1000, CK_GMMU_SZ_4K, 2, 0) == CK_GMMU_ERANGE);   /* VA bits */
    CHECK(ck_gmmu_map(&as, (1ull << 57) - 0x1000, pa, 0x2000, CK_GMMU_SZ_4K, 2, 0) == CK_GMMU_ERANGE); /* VA end */
    CHECK(ck_gmmu_map(&as, va + 0x4000, PA_BASE - 0x1000, 0x1000, CK_GMMU_SZ_4K, 2, 0) == CK_GMMU_ERANGE); /* PA below */
    CHECK(ck_gmmu_map(&as, va + 0x4000, PA_BASE + MEM_BYTES, 0x1000, CK_GMMU_SZ_4K, 2, 0) == CK_GMMU_ERANGE); /* PA above */
    CHECK(ck_gmmu_map(&as, va + 0x4000, PA_BASE + MEM_BYTES - 0x1000, 0x2000, CK_GMMU_SZ_4K, 2, 0) == CK_GMMU_ERANGE); /* PA end */
    CHECK(ck_gmmu_map(&as, va + 0x4000, pa, 0x1000, CK_GMMU_SZ_4K, 4, 0) == CK_GMMU_EINVAL);  /* aperture */
    CHECK(ck_gmmu_map(&as, va + 0x4000, pa, 0x1000, CK_GMMU_SZ_4K, 2, 0x20) == CK_GMMU_EINVAL); /* PCF */
    /* A 64 KiB page in a 2 MiB region that already has a 4 KiB table: refused. */
    CHECK(ck_gmmu_map(&as, va & ~0xFFFFull, DATA_PA + 0x10000, 0x10000, CK_GMMU_SZ_64K, 2, 0) == CK_GMMU_EBUSY);
    /* A 2 MiB page over that region: refused. */
    CHECK(ck_gmmu_map(&as, va & ~0x1FFFFFull, PA_BASE + 0x200000, CK_GMMU_SZ_2M, CK_GMMU_SZ_2M, 2, 0) == CK_GMMU_EBUSY);
    CHECK(gread(va + 0x10, &v) == 0 && v == 0xC0FFEE01u);
    /* Unmap: partial or wrong-size requests refused with no change. */
    CHECK(ck_gmmu_unmap(&as, va, 0x3000, CK_GMMU_SZ_4K) == CK_GMMU_ENOENT);
    CHECK(ck_gmmu_unmap(&as, va & ~0xFFFFull, 0x10000, CK_GMMU_SZ_64K) == CK_GMMU_ENOENT);
    CHECK(ck_gsim_walk(&sim, as.root_pa, va + 0x1000, 0, &wpa, &page, &lo) == 0);
    CHECK(ck_gmmu_unmap(&as, va, 0x2000, CK_GMMU_SZ_4K) == CK_GMMU_OK);
    CHECK(ck_gsim_walk(&sim, as.root_pa, va, 0, &wpa, &page, &lo) == CK_GSIM_F_UNMAPPED);
    CHECK(ck_gmmu_unmap(&as, va, 0x1000, CK_GMMU_SZ_4K) == CK_GMMU_ENOENT); /* double unmap */
}

/* 3. Page sizes 64 KiB and 2 MiB, and the read-only PCF. */
static void test_page_sizes(void)
{
    fresh(64);
    uint64_t v64 = 0x40000000ull, v2m = 0x80000000ull;
    CHECK(ck_gmmu_map(&as, v64, DATA_PA + 0x10000 - (DATA_PA & 0xFFFF), 0x20000, CK_GMMU_SZ_64K, 2,
                      CK_GMMU_PCF_RO_CACHED) == CK_GMMU_OK);
    uint64_t pa2m = (PA_BASE + 0x200000ull);
    CHECK(ck_gmmu_map(&as, v2m, pa2m, CK_GMMU_SZ_2M, CK_GMMU_SZ_2M, 2, CK_GMMU_PCF_RW_CACHED) == CK_GMMU_OK);
    uint64_t wpa = 0, page = 0;
    uint32_t lo = 0, v = 0;
    CHECK(ck_gsim_walk(&sim, as.root_pa, v64 + 0x1ABCD, 0, &wpa, &page, &lo) == 0);
    CHECK(page == CK_GMMU_SZ_64K && wpa == DATA_PA + 0x10000 - (DATA_PA & 0xFFFF) + 0x1ABCD);
    CHECK(ck_gsim_walk(&sim, as.root_pa, v2m + 0x1F0004, 0, &wpa, &page, &lo) == 0);
    CHECK(page == CK_GMMU_SZ_2M && wpa == pa2m + 0x1F0004);
    CHECK(gwrite(v64 + 8, 1) == CK_GSIM_F_RO_WRITE); /* read-only page */
    CHECK(gread(v64 + 8, &v) == 0);
    CHECK(gwrite(v2m + 8, 7) == 0);
    /* Small page inside the 2 MiB page: refused. */
    CHECK(ck_gmmu_map(&as, v2m + 0x1000, DATA_PA, 0x1000, CK_GMMU_SZ_4K, 2, 0) == CK_GMMU_EBUSY);
    CHECK(ck_gmmu_map(&as, v2m, pa2m, CK_GMMU_SZ_2M, CK_GMMU_SZ_2M, 2, 0) == CK_GMMU_EBUSY);
    CHECK(ck_gmmu_unmap(&as, v2m, CK_GMMU_SZ_2M, CK_GMMU_SZ_4K) == CK_GMMU_ENOENT);
    CHECK(ck_gmmu_unmap(&as, v2m, CK_GMMU_SZ_2M, CK_GMMU_SZ_2M) == CK_GMMU_OK);
    CHECK(ck_gmmu_unmap(&as, v64, 0x20000, CK_GMMU_SZ_64K) == CK_GMMU_OK);
}

/* 4. Page-table pool exhaustion: the map is refused whole, nothing written. */
static void test_pool_exhaustion(void)
{
    fresh(5); /* root + 4: exactly one 4K path (PD3, PD2, PD1, PD0) and no PT */
    uint32_t before = 0;
    for (uint32_t i = 0; i < as.pt_count; i++)
        before += as.pt_used[i];
    CHECK(ck_gmmu_map(&as, 0x1000, DATA_PA, 0x1000, CK_GMMU_SZ_4K, 2, 0) == CK_GMMU_ENOMEM);
    uint32_t after = 0;
    for (uint32_t i = 0; i < as.pt_count; i++)
        after += as.pt_used[i];
    CHECK(before == after);
    uint64_t wpa, page;
    uint32_t lo;
    CHECK(ck_gsim_walk(&sim, as.root_pa, 0x1000, 0, &wpa, &page, &lo) == CK_GSIM_F_UNMAPPED);
    /* 2 MiB needs no PT: the same pool is enough. */
    CHECK(ck_gmmu_map(&as, 0x200000, PA_BASE + 0x200000, CK_GMMU_SZ_2M, CK_GMMU_SZ_2M, 2, 0) == CK_GMMU_OK);
    /* Exact count across a 2 MiB boundary: two PTs needed, pool has none left. */
    fresh(7); /* root + PD3 + PD2 + PD1 + PD0 + 2 PT */
    CHECK(ck_gmmu_map(&as, 0x1FF000, DATA_PA, 0x2000, CK_GMMU_SZ_4K, 2, 0) == CK_GMMU_OK);
    fresh(6);
    CHECK(ck_gmmu_map(&as, 0x1FF000, DATA_PA, 0x2000, CK_GMMU_SZ_4K, 2, 0) == CK_GMMU_ENOMEM);
    CHECK(ck_gmmu_as_init(&as, &sim.mem, POOL_PA + 1, 8) == CK_GMMU_EINVAL);
    CHECK(ck_gmmu_as_init(&as, &sim.mem, PA_BASE + MEM_BYTES - 4096, 2) == CK_GMMU_ERANGE);
    CHECK(ck_gmmu_as_init(&as, &sim.mem, POOL_PA, CK_GMMU_MAX_PT + 1) == CK_GMMU_EINVAL);
}

/* 5. The invalidate sequence, the stale TLB without it, and the timeout latch. */
static void test_invalidate(void)
{
    fresh(64);
    uint64_t va = 0x10000;
    CHECK(ck_gmmu_map(&as, va, DATA_PA, 0x1000, CK_GMMU_SZ_4K, 2, 0) == CK_GMMU_OK);
    uint32_t v = 0;
    CHECK(gwrite(va, 0x11) == 0); /* fills the TLB */
    CHECK(ck_gmmu_unmap(&as, va, 0x1000, CK_GMMU_SZ_4K) == CK_GMMU_OK);
    CHECK(gread(va, &v) == 0 && v == 0x11); /* stale: no invalidate yet */
    /* Prove it is the old translation: change the bytes at the old PA from the CPU. */
    *(volatile uint32_t *)(mem + (DATA_PA - PA_BASE)) = 0x5E5E5E5Eu;
    CHECK(gread(va, &v) == 0 && v == 0x5E5E5E5Eu);
    sim.nlog = 0;
    CHECK(ck_gmmu_invalidate(&as, &regs, 10) == CK_GMMU_OK);
    /* Register order: PDB, UPPER_PDB, INVALIDATE(ALL_VA|TRIGGER), then polls. */
    CHECK(sim.nlog >= 4);
    uint64_t pdb = as.root_pa >> 12;
    CHECK(sim.log[0].write && sim.log[0].off == 0x00B830A0u && sim.log[0].val == ((uint32_t)((pdb & 0xFFFFFFF) << 4) | 2u));
    CHECK(sim.log[1].write && sim.log[1].off == 0x00B830A4u && sim.log[1].val == (uint32_t)(pdb >> 28));
    CHECK(sim.log[2].write && sim.log[2].off == 0x00B830B0u && sim.log[2].val == 0x80000001u);
    CHECK(!sim.log[3].write && sim.log[3].off == 0x00B830B0u);
    CHECK(sim.inval_done == 1);
    CHECK(gread(va, &v) == CK_GSIM_F_UNMAPPED); /* translation gone */
    /* Budget smaller than the device latency: timeout, then the AS refuses everything. */
    fresh(64);
    sim.inval_latency = 5;
    CHECK(ck_gmmu_invalidate(&as, &regs, 3) == CK_GMMU_ETIMEDOUT);
    CHECK(as.dead == 1);
    CHECK(ck_gmmu_map(&as, va, DATA_PA, 0x1000, CK_GMMU_SZ_4K, 2, 0) == CK_GMMU_EDEAD);
    CHECK(ck_gmmu_unmap(&as, va, 0x1000, CK_GMMU_SZ_4K) == CK_GMMU_EDEAD);
    CHECK(ck_gmmu_invalidate(&as, &regs, 100) == CK_GMMU_EDEAD);
    /* Exact poll budget: latency 3 needs 4 reads (3 with TRIGGER set, then clear). */
    fresh(64);
    CHECK(ck_gmmu_invalidate(&as, &regs, 3) == CK_GMMU_ETIMEDOUT);
    fresh(64);
    CHECK(ck_gmmu_invalidate(&as, &regs, 4) == CK_GMMU_OK);
    /* Invalidate-all drops every PDB; one-PDB leaves other PDBs cached. */
    CHECK(ck_gmmu_map(&as, va, DATA_PA, 0x1000, CK_GMMU_SZ_4K, 2, 0) == CK_GMMU_OK);
    CHECK(gread(va, &v) == 0);
    sim.tlb[0].pdb ^= 0x1000; /* pretend the entry belongs to another address space */
    CHECK(ck_gmmu_invalidate(&as, &regs, 4) == CK_GMMU_OK && sim.tlb[0].valid);
    regs.wr(regs.ctx, CK_GPU_REG_INVAL, CK_GPU_INVAL_ALL_PDB | CK_GPU_INVAL_ALL_VA | CK_GPU_INVAL_TRIGGER);
    for (int i = 0; i < 4; i++)
        (void)regs.rd(regs.ctx, CK_GPU_REG_INVAL);
    CHECK(!sim.tlb[0].valid);
}

/* 6. Allocator: distinct addresses, cache policy, reclaim, bounds, quarantine. */
static void test_allocator(void)
{
    fresh(64);
    CHECK(ck_gmem_init(&gm, &as, &regs, 10, DATA_PA, 16, VA_BASE) == CK_GMEM_OK);
    CHECK(ck_gmem_init(&gm, &as, &regs, 10, POOL_PA, 16, VA_BASE) == CK_GMEM_EINVAL);   /* overlaps the PT pool */
    CHECK(ck_gmem_init(&gm, &as, &regs, 10, DATA_PA, 16, VA_BASE + 0x1000) == CK_GMEM_EINVAL); /* VA alignment */
    CHECK(ck_gmem_init(&gm, &as, &regs, 10, DATA_PA, 16, VA_BASE) == CK_GMEM_OK);
    struct ck_gmem_buf *a = 0, *b = 0, *flag = 0, *x = 0;
    CHECK(ck_gmem_alloc(&gm, 5000, CK_GMEM_CACHED, 0, &a) == CK_GMEM_OK);
    CHECK(a && a->size == 8192 && a->pa == DATA_PA && a->gpu_va == VA_BASE);
    CHECK((uint64_t)(uintptr_t)a->cpu != a->gpu_va && a->cpu == mem + (DATA_PA - PA_BASE));
    {
        int dirty = 0;
        for (uint64_t i = 0; i < a->size; i++)
            dirty |= a->cpu[i];
        CHECK(!dirty); /* alloc zeros, including a window poisoned with 0xA5 */
    }
    /* Host-polled must be uncached: refused when asked cached. */
    CHECK(ck_gmem_alloc(&gm, 4, CK_GMEM_CACHED, CK_GMEM_HOST_POLLED, &flag) == CK_GMEM_EINVAL);
    CHECK(ck_gmem_alloc(&gm, 4, CK_GMEM_UNCACHED, CK_GMEM_HOST_POLLED, &flag) == CK_GMEM_OK);
    CHECK(ck_gmem_alloc(&gm, 4, 7, 0, &x) == CK_GMEM_EINVAL);
    CHECK(ck_gmem_alloc(&gm, 4, CK_GMEM_CACHED, 0x80, &x) == CK_GMEM_EINVAL);
    CHECK(ck_gmem_alloc(&gm, 0, CK_GMEM_CACHED, 0, &x) == CK_GMEM_EINVAL);
    uint64_t wpa, page;
    uint32_t lo = 0;
    CHECK(ck_gsim_walk(&sim, as.root_pa, flag->gpu_va, 0, &wpa, &page, &lo) == 0);
    CHECK(((lo >> 3) & 0x1F) == CK_GMMU_PCF_RW_UNCACHED && ((lo >> 1) & 3) == CK_GMMU_PTE_AP_SYS_COH);
    CHECK(ck_gsim_walk(&sim, as.root_pa, a->gpu_va + 4096, 0, &wpa, &page, &lo) == 0);
    CHECK(((lo >> 3) & 0x1F) == CK_GMMU_PCF_RW_CACHED);
    /* GPU writes the flag through its VA; the CPU sees it through its own pointer. */
    CHECK(gwrite(flag->gpu_va, 0xD0E) == 0);
    CHECK(*(volatile uint32_t *)flag->cpu == 0xD0E);
    /* Bounds: 16 pages total, 3 used. 14 more is too many; 13 fits exactly. */
    CHECK(ck_gmem_alloc(&gm, 14 * 4096, CK_GMEM_CACHED, 0, &b) == CK_GMEM_ENOMEM);
    CHECK(ck_gmem_alloc(&gm, 13 * 4096, CK_GMEM_CACHED, 0, &b) == CK_GMEM_OK);
    CHECK(ck_gmem_count(&gm, CK_GMEM_FREE) == 0);
    /* Free reclaims after a completed invalidate; GPU access then faults. */
    uint32_t inv = sim.inval_done;
    CHECK(gwrite(a->gpu_va, 1) == 0);
    uint64_t a_va = a->gpu_va;
    CHECK(ck_gmem_free(&gm, a) == CK_GMEM_OK);
    CHECK(sim.inval_done == inv + 1);
    CHECK(gwrite(a_va, 2) == CK_GSIM_F_UNMAPPED);
    CHECK(ck_gmem_free(&gm, a) == CK_GMEM_ESTATE); /* double free */
    CHECK(ck_gmem_free(&gm, (struct ck_gmem_buf *)((uint8_t *)&gm.buf[0] + 1)) == CK_GMEM_ESTATE); /* not a slot */
    CHECK(ck_gmem_count(&gm, CK_GMEM_FREE) == 2);
    CHECK(ck_gmem_alloc(&gm, 8192, CK_GMEM_CACHED, 0, &a) == CK_GMEM_OK && a->pa == DATA_PA); /* reused */
    {
        int dirty = 0;
        for (uint64_t i = 0; i < a->size; i++)
            dirty |= a->cpu[i];
        CHECK(!dirty); /* reused pages do not keep the previous buffer */
    }
    /* Invalidate that never completes: the buffer is quarantined, not reused. */
    sim.inval_stuck = 1;
    CHECK(ck_gmem_free(&gm, b) == CK_GMEM_ETIMEDOUT);
    CHECK(ck_gmem_count(&gm, CK_GMEM_QUARANTINED) == 13);
    sim.inval_stuck = 0;
    CHECK(ck_gmem_alloc(&gm, 4096, CK_GMEM_CACHED, 0, &x) != CK_GMEM_OK); /* AS dead + no free pages */
    CHECK(ck_gmem_count(&gm, CK_GMEM_QUARANTINED) == 13);
    /* Slot table bound. */
    fresh(64);
    CHECK(ck_gmem_init(&gm, &as, &regs, 10, DATA_PA, 128, VA_BASE) == CK_GMEM_OK);
    int ok = 1;
    for (uint32_t i = 0; i < CK_GMEM_MAX_BUF; i++)
        ok &= ck_gmem_alloc(&gm, 1, CK_GMEM_CACHED, 0, &x) == CK_GMEM_OK;
    CHECK(ok);
    CHECK(ck_gmem_alloc(&gm, 1, CK_GMEM_CACHED, 0, &x) == CK_GMEM_ENOMEM);
}

/* 7. Channel: layout, token, submit through the GMMU, refusals, teardown. */
static void test_channel(void)
{
    fresh(64);
    CHECK(ck_gmem_init(&gm, &as, &regs, 10, DATA_PA, 64, VA_BASE) == CK_GMEM_OK);
    struct ck_gpu_chan_ops ops = ck_gsim_chan_ops();
    struct ck_gpu_chan ch;
    CHECK(ck_gpu_chan_create(&ch, &gm, &regs, &ops, &sim, 0x1000, 0, 8) == CK_GCHAN_EINVAL); /* chid > 11 bits */
    CHECK(ck_gpu_chan_create(&ch, &gm, &regs, &ops, &sim, 1, 0x7F, 8) == CK_GCHAN_EINVAL);   /* INVALID_RUNLIST */
    CHECK(ck_gpu_chan_create(&ch, &gm, &regs, &ops, &sim, 1, 0, 6) == CK_GCHAN_EINVAL);      /* not a power of 2 */
    CHECK(ck_gpu_chan_create(&ch, &gm, &regs, &ops, &sim, 1, 0, 1) == CK_GCHAN_EINVAL);
    CHECK(ck_gmem_count(&gm, CK_GMEM_LIVE) == 0);
    CHECK(ck_gpu_chan_create(&ch, &gm, &regs, &ops, &sim, 5, 2, 8) == CK_GCHAN_OK);
    if (!ch.open)
        return; /* the rest needs a live channel */
    CHECK(ch.d.token == 0x00020005u);
    CHECK(ch.d.rl_entry[0] == ((uint32_t)ch.userd->pa & 0xFFFFFF00u) && ch.d.rl_entry[1] == (uint32_t)(ch.userd->pa >> 32));
    CHECK(ch.d.pdb == as.root_pa && ch.d.gpfifo_va == ch.gpfifo->gpu_va);
    CHECK(ch.gpfifo->cache == CK_GMEM_UNCACHED && ch.userd->cache == CK_GMEM_UNCACHED);
    /* Instance block word 128/129 as the driver writes it: TARGET SYS_COH, BIG_PAGE 64K, NEW_PT_FORMAT 0. */
    const uint32_t *ramin = (const uint32_t *)ch.inst->cpu;
    CHECK(ramin[128] == (((uint32_t)as.root_pa & 0xFFFFF000u) | (1u << 11) | 2u));
    CHECK(ramin[129] == (uint32_t)(as.root_pa >> 32));
    struct ck_gpu_chan dup;
    CHECK(ck_gpu_chan_create(&dup, &gm, &regs, &ops, &sim, 5, 2, 8) == CK_GCHAN_EBIND); /* same chid live */
    CHECK(ck_gmem_count(&gm, CK_GMEM_LIVE) == 3);                                      /* dup's memory returned */

    struct ck_gmem_buf *pb = 0, *sem = 0;
    CHECK(ck_gmem_alloc(&gm, 4096, CK_GMEM_CACHED, CK_GMEM_GPU_RO, &pb) == CK_GMEM_OK);
    CHECK(ck_gmem_alloc(&gm, 16, CK_GMEM_UNCACHED, CK_GMEM_HOST_POLLED, &sem) == CK_GMEM_OK);
    uint32_t *w = (uint32_t *)pb->cpu;
    w[0] = ck_gpu_mthd(0, 0x5c, 2);
    w[1] = (uint32_t)sem->gpu_va;
    w[2] = (uint32_t)(sem->gpu_va >> 32);
    sim.nlog = 0;
    CHECK(ck_gpu_chan_submit(&ch, pb, 0, 3) == CK_GCHAN_OK);
    /* Ring entry (CPU view), GPPut in USERD at 0x8c, doorbell write with the token. */
    uint64_t ent = ((uint64_t *)ch.gpfifo->cpu)[0];
    CHECK(ent == ck_gpu_gp_entry(pb->gpu_va, 3));
    CHECK(*(uint32_t *)(ch.userd->cpu + 0x8c) == 1);
    CHECK(sim.nlog == 1 && sim.log[0].write && sim.log[0].off == 0x00BB0090u && sim.log[0].val == 0x00020005u);
    const struct ck_gsim_chan *sc = ck_gsim_chan(&sim, 5);
    CHECK(sc && sc->gp_get == 1 && sc->words_read == 3 && !sc->faulted);
    /* Refusals. */
    CHECK(ck_gpu_chan_submit(&ch, pb, 2, 1) == CK_GCHAN_EINVAL);              /* misaligned */
    CHECK(ck_gpu_chan_submit(&ch, pb, 0, 0) == CK_GCHAN_EINVAL);              /* empty */
    CHECK(ck_gpu_chan_submit(&ch, pb, 4092, 2) == CK_GCHAN_EINVAL);           /* past the buffer */
    CHECK(ck_gpu_chan_submit(&ch, pb, 0, 0x200000) == CK_GCHAN_EINVAL);       /* LENGTH is 21 bits */
    struct ck_gmem_buf fake = *pb;
    CHECK(ck_gpu_chan_submit(&ch, &fake, 0, 1) == CK_GCHAN_EINVAL);           /* not an allocator buffer */
    /* Ring full: entries - 1 outstanding at most. */
    int okc = 1;
    for (uint32_t i = 1; i < 7; i++)
        okc &= ck_gpu_chan_submit(&ch, pb, 0, 1) == CK_GCHAN_OK;
    CHECK(okc);
    CHECK(ck_gpu_chan_submit(&ch, pb, 0, 1) == CK_GCHAN_EFULL);
    ck_gpu_chan_retire(&ch, 100); /* beyond put: ignored */
    CHECK(ch.retired == 0);
    ck_gpu_chan_retire(&ch, 7);
    CHECK(ck_gpu_chan_submit(&ch, pb, 0, 1) == CK_GCHAN_OK); /* wraps the ring */
    CHECK(sc->gp_get == 0 && !sc->faulted);
    /* A pushbuffer that was freed: the GPU faults on it and the channel stops. */
    struct ck_gmem_buf *gone = 0;
    CHECK(ck_gmem_alloc(&gm, 4096, CK_GMEM_CACHED, 0, &gone) == CK_GMEM_OK);
    ck_gpu_chan_retire(&ch, 8);
    uint64_t gone_va = gone->gpu_va;
    /* Queue it by hand after freeing (submit itself would refuse a dead buffer). */
    CHECK(ck_gmem_free(&gm, gone) == CK_GMEM_OK);
    CHECK(ck_gpu_chan_submit(&ch, gone, 0, 1) == CK_GCHAN_EINVAL);
    ((uint64_t *)ch.gpfifo->cpu)[ch.put % 8] = ck_gpu_gp_entry(gone_va, 1);
    ch.put++;
    *(uint32_t *)(ch.userd->cpu + 0x8c) = ch.put % 8;
    uint32_t nf = sim.nfaults;
    regs.wr(regs.ctx, CK_GPU_REG_DOORBELL, ch.d.token);
    CHECK(sc->faulted && sim.nfaults == nf + 1 && sim.faults[nf].va == gone_va);
    /* Unknown token is counted, not acted on. */
    regs.wr(regs.ctx, CK_GPU_REG_DOORBELL, 0x00010001u);
    CHECK(sim.doorbell_unknown == 1);
    /* Clean teardown: everything returns to FREE except pb/sem (caller's). */
    CHECK(ck_gpu_chan_teardown(&ch, 4, 0, 0) == CK_GCHAN_OK);
    CHECK(ck_gmem_count(&gm, CK_GMEM_QUARANTINED) == 0);
    CHECK(ck_gmem_count(&gm, CK_GMEM_LIVE) == 2);
    CHECK(ck_gpu_chan_submit(&ch, pb, 0, 1) == CK_GCHAN_ESTATE);
    CHECK(ck_gpu_chan_teardown(&ch, 4, 0, 0) == CK_GCHAN_ESTATE);
    /* The channel id is free again: reopen, then a hung teardown quarantines. */
    CHECK(ck_gpu_chan_create(&ch, &gm, &regs, &ops, &sim, 5, 2, 8) == CK_GCHAN_OK);
    sim.hang = 1;
    struct ck_gmem_buf *extra[2] = {pb, sem};
    uint64_t pb_pa = pb->pa, sem_pa = sem->pa; /* slots are reused after quarantine; the pages are not */
    uint32_t polls = sim.idle_polls;
    CHECK(ck_gpu_chan_teardown(&ch, 6, extra, 2) == CK_GCHAN_ETIMEDOUT);
    CHECK(sim.idle_polls == polls + 6); /* bounded wait */
    CHECK(ck_gmem_count(&gm, CK_GMEM_LIVE) == 0);
    CHECK(ck_gmem_count(&gm, CK_GMEM_QUARANTINED) == 3 + 1 + 1); /* ring 1 page, USERD 1, inst 1, pb 1, sem 1 */
    uint32_t freep = ck_gmem_count(&gm, CK_GMEM_FREE);
    struct ck_gmem_buf *y = 0;
    int never = 1;
    while (ck_gmem_alloc(&gm, 4096, CK_GMEM_CACHED, 0, &y) == CK_GMEM_OK)
        never &= gm.page_state[(y->pa - DATA_PA) / 4096] == CK_GMEM_LIVE && y->pa != pb_pa && y->pa != sem_pa;
    CHECK(never);
    CHECK(ck_gmem_count(&gm, CK_GMEM_QUARANTINED) == 5 && ck_gmem_count(&gm, CK_GMEM_LIVE) <= freep);
}

/* 8. Reopen loop: repeated create/submit/teardown leaks nothing. */
static void test_reopen(void)
{
    fresh(64);
    CHECK(ck_gmem_init(&gm, &as, &regs, 10, DATA_PA, 32, VA_BASE) == CK_GMEM_OK);
    struct ck_gpu_chan_ops ops = ck_gsim_chan_ops();
    struct ck_gpu_chan ch;
    struct ck_gmem_buf *pb = 0;
    CHECK(ck_gmem_alloc(&gm, 4096, CK_GMEM_CACHED, CK_GMEM_GPU_RO, &pb) == CK_GMEM_OK);
    int ok = 1;
    for (uint32_t i = 0; i < 200; i++) {
        ok &= ck_gpu_chan_create(&ch, &gm, &regs, &ops, &sim, i & 0xFFF, 1, 4) == CK_GCHAN_OK;
        ok &= ck_gpu_chan_submit(&ch, pb, 0, 4) == CK_GCHAN_OK;
        ok &= ck_gpu_chan_teardown(&ch, 2, 0, 0) == CK_GCHAN_OK;
    }
    CHECK(ok);
    CHECK(ck_gmem_count(&gm, CK_GMEM_LIVE) == 1 && ck_gmem_count(&gm, CK_GMEM_FREE) == 31);
    CHECK(sim.nfaults == 0 && sim.log_dropped > 0); /* the log is bounded too */
}

/* 9. Levels with a PTE the mapper never writes: PD1 (512 MiB) and PD2 (256 GiB)
 * decode as huge pages (kgmmuFmtInitLevels_GH10X / _GB10X bPageTable), PD3 and
 * PD4 have no PTE form (BAD_PDE). Entries are planted by hand. */
static uint64_t table_at(uint64_t tbl, uint32_t idx)
{
    uint64_t e;
    memcpy(&e, mem + (tbl - PA_BASE) + idx * 8u, 8);
    return e & 0x000FFFFFFFFFF000ull;
}

static void plant(uint64_t tbl, uint32_t idx, uint64_t val)
{
    memcpy(mem + (tbl - PA_BASE) + idx * 8u, &val, 8);
}

static void test_huge_levels(void)
{
    fresh(64);
    CHECK(ck_gmmu_map(&as, 0x1000, DATA_PA, 0x1000, CK_GMMU_SZ_4K, 2, 0) == CK_GMMU_OK);
    uint64_t pd3 = table_at(as.root_pa, 0), pd2 = table_at(pd3, 0), pd1 = table_at(pd2, 0);
    uint64_t wpa = 0, page = 0;
    uint32_t lo = 0;
    plant(pd1, 1, ck_gmmu_pte(PA_BASE, CK_GMMU_PTE_AP_SYS_COH, 0, 0)); /* VA [512 MiB, 1 GiB) */
    CHECK(ck_gsim_walk(&sim, as.root_pa, (1ull << 29) + 0x4321, 0, &wpa, &page, &lo) == 0);
    CHECK(page == (1ull << 29) && wpa == PA_BASE + 0x4321);
    plant(pd2, 1, ck_gmmu_pte(0, CK_GMMU_PTE_AP_SYS_COH, 0, 0)); /* VA [256 GiB, 512 GiB) */
    CHECK(ck_gsim_walk(&sim, as.root_pa, (1ull << 38) + PA_BASE + 8, 0, &wpa, &page, &lo) == 0);
    CHECK(page == (1ull << 38) && wpa == PA_BASE + 8);
    plant(pd3, 1, ck_gmmu_pte(PA_BASE, CK_GMMU_PTE_AP_SYS_COH, 0, 0));
    CHECK(ck_gsim_walk(&sim, as.root_pa, 1ull << 47, 0, &wpa, &page, &lo) == CK_GSIM_F_BAD_PDE);
    plant(as.root_pa, 1, ck_gmmu_pte(PA_BASE, CK_GMMU_PTE_AP_SYS_COH, 0, 0));
    CHECK(ck_gsim_walk(&sim, as.root_pa, 1ull << 56, 0, &wpa, &page, &lo) == CK_GSIM_F_BAD_PDE);
    /* The mapper refuses to map under a huge PTE instead of overwriting it. */
    CHECK(ck_gmmu_map(&as, 1ull << 29, DATA_PA, 0x1000, CK_GMMU_SZ_4K, 2, 0) == CK_GMMU_EBUSY);
    CHECK(ck_gmmu_map(&as, 1ull << 38, PA_BASE + 0x200000, CK_GMMU_SZ_2M, CK_GMMU_SZ_2M, 2, 0) == CK_GMMU_EBUSY);
    /* Video and peer apertures do not exist on GB10: fault, never translate. */
    plant(pd1, 2, ck_gmmu_pte(PA_BASE, CK_GMMU_PTE_AP_VIDMEM, 0, 0));
    CHECK(ck_gsim_walk(&sim, as.root_pa, 2ull << 29, 0, &wpa, &page, &lo) == CK_GSIM_F_APERTURE);
    plant(pd1, 3, ck_gmmu_pde(PA_BASE, CK_GMMU_PDE_AP_VIDMEM));
    CHECK(ck_gsim_walk(&sim, as.root_pa, 3ull << 29, 0, &wpa, &page, &lo) == CK_GSIM_F_APERTURE);
}

/* 10. The 40-bit pushbuffer address limit of the GPFIFO entry. */
static void test_gp_va40(void)
{
    struct ck_gpu_chan_ops ops = ck_gsim_chan_ops();
    struct ck_gpu_chan ch;
    struct ck_gmem_buf *big = 0, *pb = 0;
    /* Last page ends exactly at 2^40: allowed. */
    fresh(64);
    CHECK(ck_gmem_init(&gm, &as, &regs, 10, DATA_PA, 520, (1ull << 40) - 0x200000) == CK_GMEM_OK);
    CHECK(ck_gpu_chan_create(&ch, &gm, &regs, &ops, &sim, 9, 0, 4) == CK_GCHAN_OK);
    CHECK(ck_gmem_alloc(&gm, 0x200000 - 4u * 4096u, CK_GMEM_CACHED, 0, &big) == CK_GMEM_OK);
    CHECK(ck_gmem_alloc(&gm, 4096, CK_GMEM_CACHED, CK_GMEM_GPU_RO, &pb) == CK_GMEM_OK);
    CHECK(pb->gpu_va == (1ull << 40) - 4096);
    CHECK(ck_gpu_chan_submit(&ch, pb, 0, 1024) == CK_GCHAN_OK);
    const struct ck_gsim_chan *sc = ck_gsim_chan(&sim, 9);
    CHECK(sc && sc->words_read == 1024 && !sc->faulted);
    /* At or beyond 2^40: refused before anything reaches the ring. */
    struct ck_gmem_buf *hi = 0;
    CHECK(ck_gmem_alloc(&gm, 4096, CK_GMEM_CACHED, CK_GMEM_GPU_RO, &hi) == CK_GMEM_OK);
    CHECK(hi->gpu_va == (1ull << 40));
    uint32_t put = ch.put, bells = sim.doorbells;
    CHECK(ck_gpu_chan_submit(&ch, hi, 0, 1) == CK_GCHAN_EINVAL);
    CHECK(ch.put == put && sim.doorbells == bells);
    CHECK(ck_gpu_chan_teardown(&ch, 2, 0, 0) == CK_GCHAN_OK);
}

int main(void)
{
    mem = aligned_alloc(4096, MEM_BYTES);
    if (!mem)
        return 2;
    test_encodings();
    test_map_unmap();
    test_page_sizes();
    test_pool_exhaustion();
    test_invalidate();
    test_allocator();
    test_channel();
    test_reopen();
    test_huge_levels();
    test_gp_va40();
    free(mem);
    return ck_t_verdict("test_gpu_sim");
}
