/* gpu_mem.h -- bounded, reclaiming GPU-visible buffer allocator (cut D, host
 * side), freestanding. Pages come from one physical window; each buffer
 * gets a CPU address and a separate GPU virtual address, mapped through
 * gmmu with the buffer's cache policy.
 *
 * Cache policy (spark-hardware-manual section 4, nvos.h NVOS32_ATTR2_
 * GPU_CACHEABLE comment): a word the host polls must be GPU-uncached, so
 * CK_GMEM_HOST_POLLED forces CK_GMEM_UNCACHED and refuses CACHED. The CPU
 * side stays coherent (PTE aperture SYS_COH: kern_gmmu.c picks SYS_COH for
 * CPU-cached system memory).
 *
 * Free = unmap, invalidate, then the pages are reusable. If the invalidate
 * does not complete, the pages are QUARANTINED: never handed out again for
 * the life of this allocator, because the GPU may still hold translations
 * to them. Only a device reset (operator step) can clear that. */
#ifndef CK_GPU_MEM_H
#define CK_GPU_MEM_H

#include <stdint.h>
#include "gmmu.h"

#define CK_GMEM_MAX_BUF 64u
#define CK_GMEM_MAX_PAGES 1024u /* 4 KiB pages: 4 MiB window max */
#define CK_GMEM_PAGE CK_GMMU_SZ_4K

enum ck_gmem_cache { CK_GMEM_CACHED = 0, CK_GMEM_UNCACHED = 1 };
#define CK_GMEM_HOST_POLLED 0x1u /* the CPU polls a word in it */
#define CK_GMEM_GPU_RO 0x2u      /* GPU may only read */

enum ck_gmem_state { CK_GMEM_FREE = 0, CK_GMEM_LIVE = 1, CK_GMEM_QUARANTINED = 2 };

enum ck_gmem_status {
    CK_GMEM_OK = 0,
    CK_GMEM_EINVAL,
    CK_GMEM_ENOMEM,    /* no buffer slot or no run of free pages */
    CK_GMEM_ESTATE,    /* not a live buffer of this allocator (double free, stale) */
    CK_GMEM_EMAP,      /* gmmu refused; nothing changed */
    CK_GMEM_ETIMEDOUT, /* invalidate timed out; buffer quarantined */
};

struct ck_gmem_buf {
    uint64_t pa;     /* GPU physical address */
    uint64_t gpu_va; /* what the GPU uses */
    uint8_t *cpu;    /* what the CPU uses; never equal to gpu_va by construction */
    uint64_t size;   /* rounded up to 4 KiB */
    uint32_t cache, flags, state, gen;
};

struct ck_gmem {
    struct ck_gmmu_as *as;
    const struct ck_gpu_regs *regs;
    uint32_t poll_budget;
    uint64_t pa_base, va_base;
    uint32_t pages;
    uint8_t page_state[CK_GMEM_MAX_PAGES]; /* ck_gmem_state per page */
    struct ck_gmem_buf buf[CK_GMEM_MAX_BUF];
};

/* pa_base: start of the data pages (inside as->win, 4 KiB aligned, not
 * overlapping the page-table pool); va_base: GPU VA of page 0 (2 MiB
 * aligned, so a page's VA never equals its CPU address by accident: the
 * tests also check that directly). */
int ck_gmem_init(struct ck_gmem *m, struct ck_gmmu_as *as, const struct ck_gpu_regs *regs, uint32_t poll_budget,
                 uint64_t pa_base, uint32_t pages, uint64_t va_base);
int ck_gmem_alloc(struct ck_gmem *m, uint64_t size, uint32_t cache, uint32_t flags, struct ck_gmem_buf **out);
int ck_gmem_free(struct ck_gmem *m, struct ck_gmem_buf *b);
/* Retire without reuse: unmap and invalidate best-effort, pages stay QUARANTINED. */
int ck_gmem_quarantine(struct ck_gmem *m, struct ck_gmem_buf *b);
uint32_t ck_gmem_count(const struct ck_gmem *m, uint32_t page_state);

#endif
