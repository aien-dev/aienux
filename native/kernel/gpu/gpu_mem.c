/* gpu_mem.c -- see gpu_mem.h. Freestanding: no libc. */
#include "gpu_mem.h"

int ck_gmem_init(struct ck_gmem *m, struct ck_gmmu_as *as, const struct ck_gpu_regs *regs, uint32_t poll_budget,
                 uint64_t pa_base, uint32_t pages, uint64_t va_base)
{
    if (!m || !as || !as->win || !regs || pages == 0 || pages > CK_GMEM_MAX_PAGES || (pa_base & (CK_GMEM_PAGE - 1u)) ||
        (va_base & (CK_GMMU_SZ_2M - 1u)) || poll_budget == 0)
        return CK_GMEM_EINVAL;
    const struct ck_gpu_window *w = as->win;
    uint64_t bytes = (uint64_t)pages * CK_GMEM_PAGE;
    if (pa_base < w->pa_base || pa_base - w->pa_base > w->size || bytes > w->size - (pa_base - w->pa_base))
        return CK_GMEM_EINVAL;
    if (va_base >= (1ull << CK_GMMU_VA_BITS) || bytes > (1ull << CK_GMMU_VA_BITS) - va_base)
        return CK_GMEM_EINVAL;
    /* The page-table pool must not overlap the data pages. */
    for (uint32_t i = 0; i < as->pt_count; i++)
        if (as->pt_pa[i] + CK_GMMU_PT_BYTES > pa_base && as->pt_pa[i] < pa_base + bytes)
            return CK_GMEM_EINVAL;
    m->as = as;
    m->regs = regs;
    m->poll_budget = poll_budget;
    m->pa_base = pa_base;
    m->va_base = va_base;
    m->pages = pages;
    for (uint32_t i = 0; i < CK_GMEM_MAX_PAGES; i++)
        m->page_state[i] = CK_GMEM_FREE;
    for (uint32_t i = 0; i < CK_GMEM_MAX_BUF; i++) {
        m->buf[i].state = CK_GMEM_FREE;
        m->buf[i].gen = 0;
    }
    return CK_GMEM_OK;
}

static int live_buf(const struct ck_gmem *m, const struct ck_gmem_buf *b)
{
    if (!m || !b || b < &m->buf[0] || b >= &m->buf[CK_GMEM_MAX_BUF])
        return 0;
    return (uint64_t)((const uint8_t *)b - (const uint8_t *)&m->buf[0]) % sizeof(m->buf[0]) == 0 &&
           b->state == CK_GMEM_LIVE;
}

static void set_pages(struct ck_gmem *m, const struct ck_gmem_buf *b, uint8_t st)
{
    uint32_t first = (uint32_t)((b->pa - m->pa_base) / CK_GMEM_PAGE);
    uint32_t n = (uint32_t)(b->size / CK_GMEM_PAGE);
    for (uint32_t i = 0; i < n; i++)
        m->page_state[first + i] = st;
}

int ck_gmem_alloc(struct ck_gmem *m, uint64_t size, uint32_t cache, uint32_t flags, struct ck_gmem_buf **out)
{
    if (!m || !out || size == 0 || size > (uint64_t)CK_GMEM_MAX_PAGES * CK_GMEM_PAGE || cache > CK_GMEM_UNCACHED ||
        (flags & ~(CK_GMEM_HOST_POLLED | CK_GMEM_GPU_RO)))
        return CK_GMEM_EINVAL;
#ifndef CK_GPU_MUTANT_NO_POLLED_UNCACHED
    if ((flags & CK_GMEM_HOST_POLLED) && cache != CK_GMEM_UNCACHED)
        return CK_GMEM_EINVAL; /* host-polled words must be GPU-uncached */
#endif
    *out = 0;
    uint32_t n = (uint32_t)((size + CK_GMEM_PAGE - 1u) / CK_GMEM_PAGE);

    struct ck_gmem_buf *b = 0;
    for (uint32_t i = 0; i < CK_GMEM_MAX_BUF && !b; i++)
        if (m->buf[i].state == CK_GMEM_FREE)
            b = &m->buf[i];
    if (!b)
        return CK_GMEM_ENOMEM;

    /* First fit over FREE pages only; QUARANTINED pages are never taken. */
    uint32_t run = 0, start = 0;
    int found = 0;
    for (uint32_t i = 0; i < m->pages && !found; i++) {
        if (m->page_state[i] != CK_GMEM_FREE) {
            run = 0;
            continue;
        }
        if (run == 0)
            start = i;
        if (++run == n)
            found = 1;
    }
    if (!found)
        return CK_GMEM_ENOMEM;

    uint64_t off = (uint64_t)start * CK_GMEM_PAGE;
    uint64_t bytes = (uint64_t)n * CK_GMEM_PAGE;
    uint32_t ro = (flags & CK_GMEM_GPU_RO) ? 1u : 0u;
    uint32_t pcf = cache == CK_GMEM_UNCACHED ? (ro ? CK_GMMU_PCF_RO_UNCACHED : CK_GMMU_PCF_RW_UNCACHED)
                                             : (ro ? CK_GMMU_PCF_RO_CACHED : CK_GMMU_PCF_RW_CACHED);
    /* Zero before the GPU mapping exists, so a reused page cannot show the previous buffer. */
    {
        volatile uint8_t *cpu = m->as->win->cpu + (m->pa_base + off - m->as->win->pa_base);
        for (uint64_t i = 0; i < bytes; i++)
            cpu[i] = 0;
    }
    if (ck_gmmu_map(m->as, m->va_base + off, m->pa_base + off, bytes, CK_GMMU_SZ_4K,
                    CK_GMMU_PTE_AP_SYS_COH, pcf) != CK_GMMU_OK)
        return CK_GMEM_EMAP;

    b->pa = m->pa_base + off;
    b->gpu_va = m->va_base + off;
    b->cpu = m->as->win->cpu + (b->pa - m->as->win->pa_base);
    b->size = bytes;
    b->cache = cache;
    b->flags = flags;
    b->state = CK_GMEM_LIVE;
    b->gen++;
    set_pages(m, b, CK_GMEM_LIVE);
    *out = b;
    return CK_GMEM_OK;
}

static int retire(struct ck_gmem *m, struct ck_gmem_buf *b, int reuse)
{
    if (!live_buf(m, b))
        return CK_GMEM_ESTATE;
    int um = ck_gmmu_unmap(m->as, b->gpu_va, b->size, CK_GMMU_SZ_4K);
    int iv = um == CK_GMMU_OK ? ck_gmmu_invalidate(m->as, m->regs, m->poll_budget) : um;
    if (reuse && iv == CK_GMMU_OK) {
        set_pages(m, b, CK_GMEM_FREE);
        b->state = CK_GMEM_FREE;
        return CK_GMEM_OK;
    }
#ifndef CK_GPU_MUTANT_NO_QUARANTINE
    set_pages(m, b, CK_GMEM_QUARANTINED);
#else
    set_pages(m, b, CK_GMEM_FREE);
#endif
    b->state = CK_GMEM_FREE; /* the slot is reusable; the pages are not */
    if (!reuse)
        return CK_GMEM_OK;
    return iv == CK_GMMU_ETIMEDOUT || iv == CK_GMMU_EDEAD ? CK_GMEM_ETIMEDOUT : CK_GMEM_EMAP;
}

int ck_gmem_free(struct ck_gmem *m, struct ck_gmem_buf *b)
{
    return retire(m, b, 1);
}

int ck_gmem_quarantine(struct ck_gmem *m, struct ck_gmem_buf *b)
{
    return retire(m, b, 0);
}

uint32_t ck_gmem_count(const struct ck_gmem *m, uint32_t page_state)
{
    uint32_t n = 0;
    for (uint32_t i = 0; m && i < m->pages; i++)
        n += m->page_state[i] == page_state ? 1u : 0u;
    return n;
}
