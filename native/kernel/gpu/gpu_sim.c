/* gpu_sim.c -- see gpu_sim.h. A model for host tests, not the chip.
 * The page walker below is written from dev_mmu.h (NV_MMU_VER3_*) and the
 * GB10X level table on its own, so it cross-checks gmmu.c instead of
 * sharing its code. */
#include "gpu_sim.h"

static void logev(struct ck_gsim *s, uint32_t off, uint32_t val, uint8_t write)
{
    if (s->nlog < CK_GSIM_LOG_MAX) {
        s->log[s->nlog].off = off;
        s->log[s->nlog].val = val;
        s->log[s->nlog].write = write;
        s->nlog++;
    } else {
        s->log_dropped++;
    }
}

void ck_gsim_init(struct ck_gsim *s, uint8_t *mem, uint64_t pa_base, uint64_t size)
{
    uint8_t *p = (uint8_t *)s;
    for (uint64_t i = 0; i < sizeof(*s); i++)
        p[i] = 0;
    s->mem.cpu = mem;
    s->mem.pa_base = pa_base;
    s->mem.size = size;
    s->inval_latency = 3; /* SIM-CHOICE: TRIGGER reads back set for 3 polls */
}

static int in_mem(const struct ck_gsim *s, uint64_t pa, uint64_t n)
{
    return pa >= s->mem.pa_base && pa - s->mem.pa_base <= s->mem.size && n <= s->mem.size - (pa - s->mem.pa_base);
}

static uint64_t rd64(const struct ck_gsim *s, uint64_t pa)
{
    const uint8_t *p = s->mem.cpu + (pa - s->mem.pa_base);
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = (v << 8) | p[i];
    return v;
}

/* GB10X levels: PD4[56] PD3[55:47] PD2[46:38] PD1[37:29] PD0[28:21]. */
static const uint8_t lo[5] = {56, 47, 38, 29, 21};
static const uint8_t hi[5] = {56, 55, 46, 37, 28};

static uint32_t pte_check(uint64_t pte, int write)
{
    uint32_t ap = (uint32_t)(pte >> 1) & 3u, pcf = (uint32_t)(pte >> 3) & 0x1Fu;
    if (ap == 0u || ap == 1u) /* PTE VIDEO_MEMORY / PEER_MEMORY */
        return CK_GSIM_F_APERTURE;
    if (write && (pcf & 0x4u)) /* the RO PCF values all have bit 2 set (dev_mmu.h 0x4-0x7, 0xC-0xF, ...) */
        return CK_GSIM_F_RO_WRITE;
    return 0;
}

uint32_t ck_gsim_walk(const struct ck_gsim *s, uint64_t pdb, uint64_t va, int write, uint64_t *pa, uint64_t *page,
                      uint32_t *pte_lo)
{
    if (va >> 57)
        return CK_GSIM_F_UNMAPPED;
    uint64_t tbl = pdb;
    for (uint32_t lv = 0; lv < 5; lv++) {
        uint32_t esz = lv == 4 ? 16u : 8u;
        uint64_t idx = (va >> lo[lv]) & ((1ull << (hi[lv] - lo[lv] + 1u)) - 1ull);
        if (!in_mem(s, tbl + idx * esz, esz))
            return CK_GSIM_F_OUTSIDE;
        uint64_t e = rd64(s, tbl + idx * esz);
        if (e & 1u) {
            if (lv < 2)
                return CK_GSIM_F_BAD_PDE; /* PD4/PD3 cannot hold a PTE */
            uint64_t psz = 1ull << lo[lv]; /* PD2 256 GiB, PD1 512 MiB, PD0 2 MiB */
            uint32_t f = pte_check(e, write);
            if (f)
                return f;
            *pa = (e & 0x000FFFFFFFFFF000ull & ~(psz - 1u)) + (va & (psz - 1u));
            *page = psz;
            *pte_lo = (uint32_t)e & 0xFFFu;
            return in_mem(s, *pa, 4) ? 0u : CK_GSIM_F_OUTSIDE;
        }
        if (lv < 4) {
            uint32_t ap = (uint32_t)(e >> 1) & 3u;
            if (ap == 0)
                return CK_GSIM_F_UNMAPPED;
            if (ap == 1)
                return CK_GSIM_F_APERTURE;
            tbl = e & 0x000FFFFFFFFFF000ull;
            continue;
        }
        /* PD0 dual PDE: small half in the second word, big half in the first.
         * SIM-CHOICE (DOCS SILENT on overlap priority): a valid small PTE wins,
         * then a valid big PTE. gmmu.c never builds both for one 2 MiB region. */
        uint64_t e1 = rd64(s, tbl + idx * 16u + 8u);
        uint32_t sap = (uint32_t)(e1 >> 1) & 3u, bap = (uint32_t)(e >> 1) & 3u;
        for (int half = 0; half < 2; half++) {
            uint32_t ap = half == 0 ? sap : bap;
            if (ap == 0)
                continue;
            if (ap == 1)
                return CK_GSIM_F_APERTURE;
            uint64_t pt = half == 0 ? (e1 & 0x000FFFFFFFFFF000ull) : (e & 0x000FFFFFFFFFFF00ull);
            uint32_t shift = half == 0 ? 12u : 16u;
            uint64_t pidx = (va >> shift) & ((1ull << (21u - shift)) - 1ull);
            if (!in_mem(s, pt + pidx * 8u, 8))
                return CK_GSIM_F_OUTSIDE;
            uint64_t pte = rd64(s, pt + pidx * 8u);
            if (!(pte & 1u))
                continue;
            uint32_t f = pte_check(pte, write);
            if (f)
                return f;
            uint64_t psz = 1ull << shift;
            *pa = (pte & 0x000FFFFFFFFFF000ull & ~(psz - 1u)) + (va & (psz - 1u));
            *page = psz;
            *pte_lo = (uint32_t)pte & 0xFFFu;
            return in_mem(s, *pa, 4) ? 0u : CK_GSIM_F_OUTSIDE;
        }
        return CK_GSIM_F_UNMAPPED;
    }
    return CK_GSIM_F_UNMAPPED;
}

static void fault(struct ck_gsim *s, uint64_t pdb, uint64_t va, uint32_t kind, int write)
{
    if (s->nfaults < CK_GSIM_FAULTS) {
        s->faults[s->nfaults].va = va;
        s->faults[s->nfaults].pdb = pdb;
        s->faults[s->nfaults].kind = kind;
        s->faults[s->nfaults].write = (uint8_t)write;
    }
    s->nfaults++;
}

static uint32_t xlate(struct ck_gsim *s, uint64_t pdb, uint64_t va, int write, uint64_t *pa)
{
    for (uint32_t i = 0; i < CK_GSIM_TLB; i++) {
        struct ck_gsim_tlb *t = &s->tlb[i];
        if (t->valid && t->pdb == pdb && va - t->va_page < t->page) {
            /* A cached translation keeps working until an invalidate,
             * even after the PTE in memory was cleared. */
            uint32_t f = pte_check(t->pte_lo, write);
            if (f) {
                fault(s, pdb, va, f, write);
                return f;
            }
            s->tlb_hits++;
            *pa = t->pa_page + (va - t->va_page);
            return 0;
        }
    }
    uint64_t page = 0;
    uint32_t pte_lo = 0;
    uint32_t f = ck_gsim_walk(s, pdb, va, write, pa, &page, &pte_lo);
    if (f) {
        fault(s, pdb, va, f, write);
        return f;
    }
    struct ck_gsim_tlb *t = &s->tlb[s->tlb_next++ % CK_GSIM_TLB];
    t->valid = 1;
    t->pdb = pdb;
    t->page = page;
    t->va_page = va & ~(page - 1u);
    t->pa_page = *pa - (va - t->va_page);
    t->pte_lo = pte_lo;
    return 0;
}

uint32_t ck_gsim_gpu_read32(struct ck_gsim *s, uint64_t pdb, uint64_t va, uint32_t *out)
{
    uint64_t pa = 0;
    uint32_t f = xlate(s, pdb, va, 0, &pa);
    if (f)
        return f;
    const uint8_t *p = s->mem.cpu + (pa - s->mem.pa_base);
    *out = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
    return 0;
}

uint32_t ck_gsim_gpu_write32(struct ck_gsim *s, uint64_t pdb, uint64_t va, uint32_t val)
{
    uint64_t pa = 0;
    uint32_t f = xlate(s, pdb, va, 1, &pa);
    if (f)
        return f;
    uint8_t *p = s->mem.cpu + (pa - s->mem.pa_base);
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(val >> (8 * i));
    return 0;
}

static struct ck_gsim_chan *chan_by_id(struct ck_gsim *s, uint32_t chid)
{
    for (uint32_t i = 0; i < CK_GSIM_CHANS; i++)
        if (s->chan[i].bound && s->chan[i].d.chid == chid)
            return &s->chan[i];
    return 0;
}

const struct ck_gsim_chan *ck_gsim_chan(const struct ck_gsim *s, uint32_t chid)
{
    return chan_by_id((struct ck_gsim *)s, chid);
}

/* Fetch GP entries get..put through the GMMU and read every pushbuffer word.
 * SIM-CHOICE: methods are not executed; the model proves the GPU could
 * reach every byte the CPU queued, and faults the channel if not. */
static void consume(struct ck_gsim *s, struct ck_gsim_chan *c)
{
    if (c->faulted || c->disabled || s->hang)
        return;
    uint64_t upa = c->d.userd_pa + CK_GPU_USERD_GPPUT;
    if (!in_mem(s, upa, 4)) {
        c->faulted = 1;
        return;
    }
    const uint8_t *u = s->mem.cpu + (upa - s->mem.pa_base);
    uint32_t put = (uint32_t)u[0] | (uint32_t)u[1] << 8 | (uint32_t)u[2] << 16 | (uint32_t)u[3] << 24;
    if (put >= c->d.entries) {
        c->faulted = 1;
        return;
    }
    while (c->gp_get != put) {
        uint64_t eva = c->d.gpfifo_va + (uint64_t)c->gp_get * 8u;
        uint32_t lo32 = 0, hi32 = 0;
        if (ck_gsim_gpu_read32(s, c->d.pdb, eva, &lo32) || ck_gsim_gpu_read32(s, c->d.pdb, eva + 4u, &hi32)) {
            c->faulted = 1;
            return;
        }
        uint64_t pbva = (uint64_t)(lo32 & ~3u) | ((uint64_t)(hi32 & 0xFFu) << 32); /* GET 31:2, GET_HI 7:0 */
        uint32_t len = (hi32 >> 10) & 0x1FFFFFu;                                    /* LENGTH 30:10 */
        for (uint32_t w = 0; w < len; w++) {
            uint32_t word = 0;
            if (ck_gsim_gpu_read32(s, c->d.pdb, pbva + (uint64_t)w * 4u, &word)) {
                c->faulted = 1;
                return;
            }
            c->words_read++;
        }
        c->gp_get = (c->gp_get + 1u) % c->d.entries;
    }
}

static uint32_t sim_rd(void *ctx, uint32_t off)
{
    struct ck_gsim *s = ctx;
    uint32_t v = 0;
    if (off == CK_GPU_REG_INVAL) {
        if (s->inval_pending && !s->inval_stuck) {
            if (s->inval_left == 0) {
                /* ALL_VA for the latched PDB: drop that PDB's TLB entries. */
                uint64_t pdb = (((uint64_t)(s->pdb_lo >> 4) & 0x0FFFFFFFull) | ((uint64_t)(s->pdb_hi & 0xFFFFFu) << 28)) << 12;
                for (uint32_t i = 0; i < CK_GSIM_TLB; i++)
                    if (s->inval_all || s->tlb[i].pdb == pdb)
                        s->tlb[i].valid = 0;
                s->inval_pending = 0;
                s->inval_done++;
            } else {
                s->inval_left--;
            }
        }
        v = s->inval_pending ? CK_GPU_INVAL_TRIGGER : 0u;
    }
    logev(s, off, v, 0);
    return v;
}

static void sim_wr(void *ctx, uint32_t off, uint32_t val)
{
    struct ck_gsim *s = ctx;
    logev(s, off, val, 1);
    if (off == CK_GPU_REG_INVAL_PDB) {
        s->pdb_lo = val;
    } else if (off == CK_GPU_REG_INVAL_UPPER) {
        s->pdb_hi = val;
    } else if (off == CK_GPU_REG_INVAL && (val & CK_GPU_INVAL_TRIGGER)) {
        s->inval_pending = 1;
        s->inval_left = s->inval_latency;
        s->inval_all = (val & CK_GPU_INVAL_ALL_PDB) != 0;
    } else if (off == CK_GPU_REG_DOORBELL) {
        s->doorbells++;
        struct ck_gsim_chan *c = 0;
        for (uint32_t i = 0; i < CK_GSIM_CHANS && !c; i++)
            if (s->chan[i].bound && s->chan[i].d.token == val)
                c = &s->chan[i];
        if (!c)
            s->doorbell_unknown++;
        else
            consume(s, c);
    }
}

struct ck_gpu_regs ck_gsim_regs(struct ck_gsim *s)
{
    struct ck_gpu_regs r = {sim_rd, sim_wr, s};
    return r;
}

static int sim_bind(void *ctx, const struct ck_gpu_chan_desc *d)
{
    struct ck_gsim *s = ctx;
    if (chan_by_id(s, d->chid))
        return -1; /* channel id already live */
    /* Take the page directory from the instance block, the way the hardware
     * would, not from the descriptor: proves the channel code wrote it. */
    uint64_t ipa = d->inst_pa + CK_GPU_RAMIN_PDB_WORD * 4u;
    if (!in_mem(s, ipa, 8))
        return -1;
    uint64_t w = rd64(s, ipa);
    uint32_t lo32 = (uint32_t)w, target = lo32 & 3u;
    if ((target != CK_GPU_RAMIN_TARGET_SYS_COH && target != 3u) || (lo32 & CK_GPU_RAMIN_NEW_PT_FORMAT))
        return -1; /* GB10 has only system memory; SIM-CHOICE: bit 10 set means VERSION_2 tables, not GB10X */
    uint64_t pdb = ((w >> 32) << 32) | (lo32 & 0xFFFFF000u);
    for (uint32_t i = 0; i < CK_GSIM_CHANS; i++) {
        if (!s->chan[i].bound) {
            struct ck_gsim_chan *c = &s->chan[i];
            c->d = *d;
            c->d.pdb = pdb; /* from the instance block */
            c->gp_get = 0;
            c->words_read = 0;
            c->faulted = c->disabled = 0;
            c->bound = 1;
            return 0;
        }
    }
    return -1;
}

static int sim_disable(void *ctx, uint32_t chid)
{
    struct ck_gsim_chan *c = chan_by_id(ctx, chid);
    if (!c)
        return -1;
    c->disabled = 1;
    return 0;
}

static int sim_is_idle(void *ctx, uint32_t chid)
{
    struct ck_gsim *s = ctx;
    s->idle_polls++;
    struct ck_gsim_chan *c = chan_by_id(s, chid);
    if (!c)
        return 1;
    if (s->hang || !c->disabled)
        return 0; /* never idle before a disable */
    c->bound = 0; /* idle and disabled: the id may be reused */
    return 1;
}

struct ck_gpu_chan_ops ck_gsim_chan_ops(void)
{
    struct ck_gpu_chan_ops o = {sim_bind, sim_disable, sim_is_idle};
    return o;
}
