/* gmmu.c -- GB10 GPU page tables over a CPU view of the GPU-visible window.
 * Format and register sources: see gmmu.h. Freestanding: no libc. */
#include "gmmu.h"

#define PDE_ADDR_MASK     0x000FFFFFFFFFF000ull /* NV_MMU_VER3_PDE_ADDRESS 51:12 */
#define DUAL_BIG_ADDR_MASK 0x000FFFFFFFFFFF00ull /* NV_MMU_VER3_DUAL_PDE_ADDRESS_BIG 51:8 */
#define PTE_ADDR_MASK     0x000FFFFFFFFFF000ull /* NV_MMU_VER3_PTE_ADDRESS 51:12 */

/* Level layout (kgmmuFmtInitLevels_GH10X / _GB10X). Index 5 is PT_BIG,
 * index 6 PT_SMALL; both hang off the PD0 dual PDE. */
static const uint8_t lv_lo[7] = {56, 47, 38, 29, 21, 16, 12};
static const uint8_t lv_hi[7] = {56, 55, 46, 37, 28, 20, 20};
#define LV_PD0 4u
#define LV_BIG 5u
#define LV_SMALL 6u

uint64_t ck_gmmu_pde(uint64_t pa, uint32_t aperture)
{
    return (pa & PDE_ADDR_MASK) | ((uint64_t)CK_GMMU_PDE_PCF_CACHED_NO_ATS << 3) | ((uint64_t)(aperture & 3u) << 1);
}

void ck_gmmu_dual_pde(uint64_t out[2], uint64_t big_pa, uint32_t big_ap, uint64_t small_pa, uint32_t small_ap)
{
    /* Big half: APERTURE_BIG 2:1, PCF_BIG 5:3, ADDRESS_BIG 51:8 (shift 8).
     * Small half: APERTURE_SMALL 66:65, PCF_SMALL 69:67, ADDRESS_SMALL 115:76 (shift 12). */
    out[0] = 0;
    out[1] = 0;
    if (big_ap != CK_GMMU_PDE_AP_INVALID)
        out[0] = (big_pa & DUAL_BIG_ADDR_MASK) | ((uint64_t)CK_GMMU_PDE_PCF_CACHED_NO_ATS << 3) | ((uint64_t)(big_ap & 3u) << 1);
    if (small_ap != CK_GMMU_PDE_AP_INVALID)
        out[1] = (small_pa & PTE_ADDR_MASK) | ((uint64_t)CK_GMMU_PDE_PCF_CACHED_NO_ATS << 3) | ((uint64_t)(small_ap & 3u) << 1);
}

uint64_t ck_gmmu_pte(uint64_t pa, uint32_t aperture, uint32_t pcf, uint32_t kind)
{
    /* VALID 0:0, APERTURE 2:1, PCF 7:3, KIND 11:8, ADDRESS 51:12, PEER_ID 63:61 = 0. */
    return 1ull | ((uint64_t)(aperture & 3u) << 1) | ((uint64_t)(pcf & 0x1Fu) << 3) | ((uint64_t)(kind & 0xFu) << 8) |
           (pa & PTE_ADDR_MASK);
}

static uint32_t lv_index(uint32_t lv, uint64_t va)
{
    uint32_t bits = (uint32_t)(lv_hi[lv] - lv_lo[lv] + 1u);
    return (uint32_t)((va >> lv_lo[lv]) & ((1ull << bits) - 1ull));
}

static volatile uint64_t *ent(const struct ck_gmmu_as *as, uint64_t table_pa, uint32_t idx, uint32_t esize)
{
    return (volatile uint64_t *)(as->win->cpu + (table_pa - as->win->pa_base) + (uint64_t)idx * esize);
}

static int pa_in_window(const struct ck_gpu_window *w, uint64_t pa, uint64_t size)
{
    return size != 0 && pa >= w->pa_base && pa - w->pa_base <= w->size && size <= w->size - (pa - w->pa_base);
}

static void zero_page(const struct ck_gmmu_as *as, uint64_t pa)
{
    volatile uint64_t *p = ent(as, pa, 0, 8);
    for (uint32_t i = 0; i < CK_GMMU_PT_BYTES / 8u; i++)
        p[i] = 0;
}

static int pt_alloc(struct ck_gmmu_as *as, uint64_t *pa)
{
    for (uint32_t i = 0; i < as->pt_count; i++) {
        if (!as->pt_used[i]) {
            as->pt_used[i] = 1;
            zero_page(as, as->pt_pa[i]);
            *pa = as->pt_pa[i];
            return CK_GMMU_OK;
        }
    }
    return CK_GMMU_ENOMEM;
}

static uint32_t pt_free_count(const struct ck_gmmu_as *as)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < as->pt_count; i++)
        n += as->pt_used[i] ? 0u : 1u;
    return n;
}

int ck_gmmu_as_init(struct ck_gmmu_as *as, const struct ck_gpu_window *win, uint64_t pool_pa, uint32_t pool_pages)
{
    if (!as || !win || !win->cpu || pool_pages < 2 || pool_pages > CK_GMMU_MAX_PT || (pool_pa & 0xFFFu))
        return CK_GMMU_EINVAL;
    if (!pa_in_window(win, pool_pa, (uint64_t)pool_pages * CK_GMMU_PT_BYTES))
        return CK_GMMU_ERANGE;
    as->win = win;
    as->pt_count = pool_pages;
    as->dead = 0;
    for (uint32_t i = 0; i < CK_GMMU_MAX_PT; i++) {
        as->pt_pa[i] = i < pool_pages ? pool_pa + (uint64_t)i * CK_GMMU_PT_BYTES : 0;
        as->pt_used[i] = 0;
    }
    return pt_alloc(as, &as->root_pa);
}

enum walk_r { W_FREE, W_MAPPED, W_MISSING, W_CONFLICT };

/* Walk to the leaf slot for `va` at page size `page`. With create=0 nothing
 * is written; *miss_lv gets the first level whose table does not exist yet
 * (W_MISSING). With create=1 missing tables are allocated (the caller has
 * already checked the pool). *leaf is the PTE slot on W_FREE / W_MAPPED. */
static enum walk_r walk(struct ck_gmmu_as *as, uint64_t va, uint64_t page, int create, uint32_t *miss_lv,
                        volatile uint64_t **leaf)
{
    uint64_t tbl = as->root_pa;
    for (uint32_t lv = 0; lv < LV_PD0; lv++) {
        volatile uint64_t *e = ent(as, tbl, lv_index(lv, va), 8);
        uint64_t v = *e;
        if (v & 1u)
            return W_CONFLICT; /* a 512 MiB / 256 GiB PTE covers this VA */
        if (((v >> 1) & 3u) == CK_GMMU_PDE_AP_INVALID) {
            if (!create) {
                *miss_lv = lv + 1u;
                return W_MISSING;
            }
            uint64_t npa;
            if (pt_alloc(as, &npa) != CK_GMMU_OK)
                return W_CONFLICT; /* unreachable: pool checked by the caller */
            *e = ck_gmmu_pde(npa, CK_GMMU_PDE_AP_SYS_COH);
            v = *e;
        }
        tbl = v & PDE_ADDR_MASK;
    }
    volatile uint64_t *d = ent(as, tbl, lv_index(LV_PD0, va), 16);
    uint64_t w0 = d[0], w1 = d[1];
    uint32_t big_ap = (uint32_t)((w0 >> 1) & 3u), small_ap = (uint32_t)((w1 >> 1) & 3u);
    if (page == CK_GMMU_SZ_2M) {
        if (w0 & 1u) {
            *leaf = &d[0];
            return W_MAPPED;
        }
        if (big_ap || small_ap)
            return W_CONFLICT; /* page tables already hang here */
        *leaf = &d[0];
        return W_FREE;
    }
    if (w0 & 1u)
        return W_CONFLICT; /* covered by a 2 MiB PTE */
    int big = page == CK_GMMU_SZ_64K;
    /* One page size per 2 MiB region: the dual PDE lets hardware walk both
     * halves in parallel, and which one wins on overlap is not something
     * this code wants to depend on (DOCS SILENT on overlap priority). */
#ifndef CK_GPU_MUTANT_NO_MIXED_PAGES
    if ((big && small_ap) || (!big && big_ap))
        return W_CONFLICT;
#endif
    uint32_t ap = big ? big_ap : small_ap;
    uint64_t pt;
    if (ap == CK_GMMU_PDE_AP_INVALID) {
        if (!create) {
            *miss_lv = big ? LV_BIG : LV_SMALL;
            return W_MISSING;
        }
        if (pt_alloc(as, &pt) != CK_GMMU_OK)
            return W_CONFLICT;
        uint64_t nd[2];
        if (big)
            ck_gmmu_dual_pde(nd, pt, CK_GMMU_PDE_AP_SYS_COH, 0, CK_GMMU_PDE_AP_INVALID);
        else
            ck_gmmu_dual_pde(nd, 0, CK_GMMU_PDE_AP_INVALID, pt, CK_GMMU_PDE_AP_SYS_COH);
        d[1] = nd[1];
        d[0] = nd[0];
    } else {
        pt = big ? (w0 & DUAL_BIG_ADDR_MASK) : (w1 & PTE_ADDR_MASK);
    }
    volatile uint64_t *p = ent(as, pt, lv_index(big ? LV_BIG : LV_SMALL, va), 8);
    *leaf = p;
    return (*p & 1u) ? W_MAPPED : W_FREE;
}

static int check_args(const struct ck_gmmu_as *as, uint64_t va, uint64_t size, uint64_t page)
{
    if (!as)
        return CK_GMMU_EINVAL;
    if (as->dead)
        return CK_GMMU_EDEAD;
    if (page != CK_GMMU_SZ_4K && page != CK_GMMU_SZ_64K && page != CK_GMMU_SZ_2M)
        return CK_GMMU_EINVAL;
    if (size == 0)
        return CK_GMMU_EINVAL;
    if ((va | size) & (page - 1u))
        return CK_GMMU_EALIGN;
#ifndef CK_GPU_MUTANT_NO_VA_BITS
    if (va >= (1ull << CK_GMMU_VA_BITS) || size > (1ull << CK_GMMU_VA_BITS) - va)
        return CK_GMMU_ERANGE;
#endif
    return CK_GMMU_OK;
}

int ck_gmmu_map(struct ck_gmmu_as *as, uint64_t va, uint64_t pa, uint64_t size, uint64_t page, uint32_t aperture,
                uint32_t pcf)
{
    int rc = check_args(as, va, size, page);
    if (rc != CK_GMMU_OK)
        return rc;
    if (aperture > 3u || pcf > 0x1Fu)
        return CK_GMMU_EINVAL;
    if (pa & (page - 1u))
        return CK_GMMU_EALIGN;
#ifndef CK_GPU_MUTANT_NO_PA_RANGE
    if (pa >= (1ull << CK_GMMU_PA_BITS) || !pa_in_window(as->win, pa, size))
        return CK_GMMU_ERANGE;
#endif

    /* Dry run: every page must be free, and count the page-table pages the
     * map would create. Pages run in VA order, so a table instance at level
     * j (key va >> (hi_j + 1)) is counted once. */
    uint64_t last_key[7];
    for (uint32_t j = 0; j < 7; j++)
        last_key[j] = ~0ull;
    uint32_t need = 0;
    for (uint64_t off = 0; off < size; off += page) {
        uint32_t miss = 0;
        volatile uint64_t *leaf = 0;
        enum walk_r w = walk(as, va + off, page, 0, &miss, &leaf);
#ifndef CK_GPU_MUTANT_NO_OVERLAP
        if (w == W_MAPPED || w == W_CONFLICT)
#else
        if (w == W_CONFLICT)
#endif
            return CK_GMMU_EBUSY;
        if (w == W_MISSING) {
            uint32_t last = page == CK_GMMU_SZ_2M ? LV_PD0 : (page == CK_GMMU_SZ_64K ? LV_BIG : LV_SMALL);
            for (uint32_t j = miss; j <= last; j++) {
                uint64_t key = (va + off) >> (lv_hi[j] + 1u);
                if (key != last_key[j]) {
                    last_key[j] = key;
                    need++;
                }
                if (j == LV_PD0 && last == LV_SMALL)
                    j = LV_BIG; /* skip PT_BIG on the small path */
            }
        }
    }
#ifndef CK_GPU_MUTANT_NO_POOL_COUNT
    if (need > pt_free_count(as))
        return CK_GMMU_ENOMEM;
#else
    (void)pt_free_count(as);
#endif

    for (uint64_t off = 0; off < size; off += page) {
        uint32_t miss = 0;
        volatile uint64_t *leaf = 0;
        enum walk_r w = walk(as, va + off, page, 1, &miss, &leaf);
#ifdef CK_GPU_MUTANT_NO_OVERLAP
        if (w == W_MAPPED)
            w = W_FREE;
#endif
        if (w != W_FREE)
            return CK_GMMU_EBUSY; /* unreachable after the dry run */
        *leaf = ck_gmmu_pte(pa + off, aperture, pcf, 0);
    }
    return CK_GMMU_OK;
}

int ck_gmmu_unmap(struct ck_gmmu_as *as, uint64_t va, uint64_t size, uint64_t page)
{
    int rc = check_args(as, va, size, page);
    if (rc != CK_GMMU_OK)
        return rc;
    for (uint64_t off = 0; off < size; off += page) {
        uint32_t miss = 0;
        volatile uint64_t *leaf = 0;
        if (walk(as, va + off, page, 0, &miss, &leaf) != W_MAPPED)
            return CK_GMMU_ENOENT;
    }
    for (uint64_t off = 0; off < size; off += page) {
        uint32_t miss = 0;
        volatile uint64_t *leaf = 0;
        (void)walk(as, va + off, page, 0, &miss, &leaf);
        *leaf = 0; /* VALID=0; page-table pages stay until the AS is torn down */
    }
    return CK_GMMU_OK;
}

int ck_gmmu_invalidate(struct ck_gmmu_as *as, const struct ck_gpu_regs *r, uint32_t poll_budget)
{
    if (!as || !r || !r->rd || !r->wr)
        return CK_GMMU_EINVAL;
    if (as->dead)
        return CK_GMMU_EDEAD;
    uint64_t pdb = as->root_pa >> 12; /* MMU_INVALIDATE_PDB_ADDR_ALIGNMENT 12 */
    r->wr(r->ctx, CK_GPU_REG_INVAL_PDB, (uint32_t)((pdb & 0x0FFFFFFFull) << 4) | CK_GPU_INVAL_PDB_AP_SYS);
    r->wr(r->ctx, CK_GPU_REG_INVAL_UPPER, (uint32_t)((pdb >> 28) & 0xFFFFFull));
    r->wr(r->ctx, CK_GPU_REG_INVAL, CK_GPU_INVAL_ALL_VA | CK_GPU_INVAL_TRIGGER);
#ifdef CK_GPU_MUTANT_NO_INVAL_POLL
    return CK_GMMU_OK;
#endif
    for (uint32_t i = 0; i < poll_budget; i++) {
        if (!(r->rd(r->ctx, CK_GPU_REG_INVAL) & CK_GPU_INVAL_TRIGGER))
            return CK_GMMU_OK;
    }
#ifndef CK_GPU_MUTANT_NO_TIMEOUT_LATCH
    as->dead = 1;
#endif
    return CK_GMMU_ETIMEDOUT;
}
