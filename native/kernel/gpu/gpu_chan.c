/* gpu_chan.c -- see gpu_chan.h. Freestanding: no libc. */
#include "gpu_chan.h"

uint32_t ck_gpu_submit_token(uint32_t runlist_id, uint32_t chid)
{
    return ((runlist_id & 0x7Fu) << 16) | (chid & CK_GPU_CHID_MAX);
}

int ck_gpu_rl_userd(uint32_t out[2], uint64_t userd_pa)
{
#ifndef CK_GPU_MUTANT_NO_USERD_WIDTH
    if ((userd_pa & 0xFFu) || userd_pa >= (1ull << 52))
        return CK_GCHAN_EINVAL;
#endif
    out[0] = (uint32_t)userd_pa & 0xFFFFFF00u;
    out[1] = (uint32_t)(userd_pa >> 32) & 0xFFFFFu;
    return CK_GCHAN_OK;
}

static void fence(void)
{
    /* physics nvrm_enqueue uses "dsb sy" around the GPPut store; on the
     * kernel target this is the same full barrier. */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

static void zero(uint8_t *p, uint64_t n)
{
    for (uint64_t i = 0; i < n; i++)
        ((volatile uint8_t *)p)[i] = 0;
}

int ck_gpu_chan_create(struct ck_gpu_chan *c, struct ck_gmem *mem, const struct ck_gpu_regs *regs,
                       const struct ck_gpu_chan_ops *ops, void *ops_ctx, uint32_t chid, uint32_t runlist_id,
                       uint32_t entries)
{
    if (!c || !mem || !regs || !ops || !ops->bind || !ops->disable || !ops->is_idle)
        return CK_GCHAN_EINVAL;
#ifndef CK_GPU_MUTANT_NO_CHID_WIDTH
    if (chid > CK_GPU_CHID_MAX || runlist_id >= CK_GPU_RUNLIST_INVALID)
        return CK_GCHAN_EINVAL;
#endif
    if (entries < 2 || entries > 65536u || (entries & (entries - 1u)))
        return CK_GCHAN_EINVAL;
    c->open = 0;
    c->mem = mem;
    c->regs = regs;
    c->ops = *ops;
    c->ops_ctx = ops_ctx;
    c->put = c->retired = 0;
    c->gpfifo = c->userd = c->inst = 0;

    /* Ring and USERD are written by the CPU while the GPU reads them, so
     * GPU-uncached (spark-hardware-manual 4.2); USERD is also host-polled
     * state in general. */
    if (ck_gmem_alloc(mem, (uint64_t)entries * 8u, CK_GMEM_UNCACHED, CK_GMEM_GPU_RO, &c->gpfifo) != CK_GMEM_OK)
        goto nomem;
    if (ck_gmem_alloc(mem, CK_GPU_USERD_BYTES, CK_GMEM_UNCACHED, CK_GMEM_HOST_POLLED, &c->userd) != CK_GMEM_OK)
        goto nomem;
    if (ck_gmem_alloc(mem, CK_GPU_INST_BYTES, CK_GMEM_UNCACHED, 0, &c->inst) != CK_GMEM_OK)
        goto nomem;
    zero(c->gpfifo->cpu, c->gpfifo->size);
    zero(c->userd->cpu, c->userd->size);
    zero(c->inst->cpu, c->inst->size);
#ifndef CK_GPU_MUTANT_NO_INST_PDB
    /* The channel's address space, as kgmmuInstBlkPageDirBaseGet_GP100 writes it.
     * FAULT_REPLAY_* and VOL stay 0 (replay disabled, non-volatile). */
    volatile uint32_t *ramin = (volatile uint32_t *)c->inst->cpu;
    uint64_t pdb = mem->as->root_pa;
    ramin[CK_GPU_RAMIN_PDB_WORD] = ((uint32_t)pdb & 0xFFFFF000u) | CK_GPU_RAMIN_BIG_PAGE_64K | CK_GPU_RAMIN_TARGET_SYS_COH;
                                   /* USE_NEW_PT_FORMAT (bit 10) stays FALSE: GP100 sets it only for GMMU_FMT_VERSION_2
                                    * and GB10B uses VERSION_3 (kern_gmmu_gp100.c:165, kern_gmmu_fmt_gh10x.c). */
    ramin[CK_GPU_RAMIN_PDB_WORD + 1u] = (uint32_t)(pdb >> 32);
#endif

    c->d.chid = chid;
    c->d.runlist_id = runlist_id;
    c->d.token = ck_gpu_submit_token(runlist_id, chid);
    c->d.entries = entries;
    c->d.pdb = mem->as->root_pa;
    c->d.gpfifo_va = c->gpfifo->gpu_va;
    c->d.userd_pa = c->userd->pa;
    c->d.inst_pa = c->inst->pa;
    if (ck_gpu_rl_userd(c->d.rl_entry, c->userd->pa) != CK_GCHAN_OK)
        goto nomem;
    fence();
    if (c->ops.bind(c->ops_ctx, &c->d) != 0) {
        (void)ck_gmem_free(mem, c->inst);
        (void)ck_gmem_free(mem, c->userd);
        (void)ck_gmem_free(mem, c->gpfifo);
        return CK_GCHAN_EBIND;
    }
    c->open = 1;
    return CK_GCHAN_OK;
nomem:
    if (c->inst)
        (void)ck_gmem_free(mem, c->inst);
    if (c->userd)
        (void)ck_gmem_free(mem, c->userd);
    if (c->gpfifo)
        (void)ck_gmem_free(mem, c->gpfifo);
    c->gpfifo = c->userd = c->inst = 0;
    return CK_GCHAN_ENOMEM;
}

static int buf_of(const struct ck_gmem *m, const struct ck_gmem_buf *b)
{
    for (uint32_t i = 0; i < CK_GMEM_MAX_BUF; i++)
        if (&m->buf[i] == b)
            return b->state == CK_GMEM_LIVE;
    return 0;
}

int ck_gpu_chan_submit(struct ck_gpu_chan *c, const struct ck_gmem_buf *pb, uint32_t off, uint32_t nwords)
{
    if (!c || !c->open)
        return CK_GCHAN_ESTATE;
    if (!buf_of(c->mem, pb))
        return CK_GCHAN_EINVAL;
    if ((off & 3u) || nwords == 0 || nwords > CK_GPU_GP_LEN_MAX)
        return CK_GCHAN_EINVAL;
    uint64_t span = (uint64_t)nwords * 4u;
    if ((uint64_t)off > pb->size || span > pb->size - off)
        return CK_GCHAN_EINVAL;
    uint64_t va = pb->gpu_va + off;
#ifndef CK_GPU_MUTANT_NO_GP_VA40
    if (va >= CK_GPU_GP_VA_LIMIT || span > CK_GPU_GP_VA_LIMIT - va)
        return CK_GCHAN_EINVAL; /* GP_ENTRY1_GET_HI is 8 bits: 40-bit pushbuffer address */
#endif
#ifndef CK_GPU_MUTANT_NO_RING_FULL
    if (c->put - c->retired >= c->d.entries - 1u)
        return CK_GCHAN_EFULL;
#endif

    volatile uint64_t *ring = (volatile uint64_t *)c->gpfifo->cpu;
    ring[c->put % c->d.entries] = ck_gpu_gp_entry(va, nwords);
    c->put++;
    fence();
    *(volatile uint32_t *)(c->userd->cpu + CK_GPU_USERD_GPPUT) = c->put % c->d.entries;
    fence();
    c->regs->wr(c->regs->ctx, CK_GPU_REG_DOORBELL, c->d.token);
    return CK_GCHAN_OK;
}

void ck_gpu_chan_retire(struct ck_gpu_chan *c, uint32_t upto)
{
    /* Same wrap-safe rule as physics nvrm_retire: only move forward, never past put. */
    if (c && upto - c->retired <= c->put - c->retired)
        c->retired = upto;
}

int ck_gpu_chan_teardown(struct ck_gpu_chan *c, uint32_t idle_budget, struct ck_gmem_buf **extra, uint32_t nextra)
{
    if (!c || !c->open)
        return CK_GCHAN_ESTATE;
    c->open = 0;
#ifndef CK_GPU_MUTANT_NO_DISABLE
    (void)c->ops.disable(c->ops_ctx, c->d.chid);
#endif
    int idle = 0;
    for (uint32_t i = 0; i < idle_budget && !idle; i++)
        idle = c->ops.is_idle(c->ops_ctx, c->d.chid) != 0;
#ifdef CK_GPU_MUTANT_NO_IDLE_QUARANTINE
    idle = 1;
#endif
    struct ck_gmem_buf *own[3] = {c->inst, c->userd, c->gpfifo};
    int rc = CK_GCHAN_OK;
    for (uint32_t i = 0; i < 3; i++) {
        if (!idle)
            (void)ck_gmem_quarantine(c->mem, own[i]);
        else if (ck_gmem_free(c->mem, own[i]) != CK_GMEM_OK)
            rc = CK_GCHAN_ETIMEDOUT; /* free failed (invalidate timeout or dead address space): ck_gmem quarantined it */
    }
    if (!idle) {
        for (uint32_t i = 0; extra && i < nextra; i++)
            if (extra[i] && extra[i]->state == CK_GMEM_LIVE)
                (void)ck_gmem_quarantine(c->mem, extra[i]);
        rc = CK_GCHAN_ETIMEDOUT;
    }
    c->gpfifo = c->userd = c->inst = 0;
    return rc;
}
