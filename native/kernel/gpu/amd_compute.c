/* native/kernel/gpu/amd_compute.c -- one MEC HQD compute queue for the 2017
 * iMac's AMD Radeon Pro 570/575/580 (Polaris 10, PCI 1002:67df, GFX 8.0).
 *
 * Sets up pipe 0 queue 0 of MEC1 as a user-mode dispatch queue backed by a
 * physical queue (HQD).  The shader stream (feat/imac-gpu-shader) targets this
 * queue with PM4 DISPATCH_DIRECT packets submitted via ck_gpu_ring_submit().
 *
 * Steps:
 *   1. Allocate MQD (256 bytes), ring, RPTR and EOP buffer from DMA pool.
 *   2. Select the HQD via GRBM_GFX_INDEX (SE=0, SH=0, INSTANCE=0).
 *   3. Deactivate the HQD (CP_HQD_ACTIVE = 0) and wait for drain.
 *   4. Programme HQD registers: PQ_BASE, PQ_RPTR, PQ_WPTR_POLL, EOP_BASE,
 *      PQ_CNTL (ring size log2).
 *   5. Activate: CP_HQD_ACTIVE = 1.
 *   6. Restore GRBM_GFX_INDEX to broadcast.
 *
 * Sources (MIT): drivers/gpu/drm/amd/amdgpu/gfx_v8_0.c (gfx_v8_0_mqd_init,
 *   gfx_v8_0_hqd_load), include/asic_reg/gca/gfx_8_0_d.h.
 * No floats.  All waits TSC-bounded.  QEMU qualifies nothing physical. */
#include "amd_gpu.h"

/* MEC ring size: 256 dwords (1 KiB) is the minimum for a compute queue. */
#define MEC_RING_DW  256u
#define MEC_RB_BLKSZ  7u   /* log2(256) - 1 = 7 */

/* EOP buffer size: 4096 bytes (required by HW). */
#define MEC_EOP_BYTES  4096u

int amd_compute_init(void)
{
    struct amd_gpu_state *g = &g_amd_gpu;

    if (!g->regs) return -1;
    if (!(g->steps_done & AMD_STEP_UCODE)) {
        ck_puts("gpu: compute: skipped (no firmware)\n");
        return 0;
    }

    /* Allocate ring, rptr, fence, EOP and MQD from DMA pool. */
    uint64_t ring_pa;  void *ring_va;
    uint64_t rptr_pa;  void *rptr_va;
    uint64_t fence_pa; void *fence_va;
    uint64_t eop_pa;   void *eop_va;
    uint64_t mqd_pa;   void *mqd_va;
    if (amd_dma_alloc(MEC_RING_DW * 4u, &ring_pa, &ring_va) ||
        amd_dma_alloc(4u,              &rptr_pa, &rptr_va)  ||
        amd_dma_alloc(4u,              &fence_pa, &fence_va)||
        amd_dma_alloc(MEC_EOP_BYTES,   &eop_pa, &eop_va)   ||
        amd_dma_alloc(AMD_MQD_SIZE,    &mqd_pa, &mqd_va)) {
        ck_puts("gpu: compute: out of DMA memory\n");
        return -1;
    }
    *(volatile uint32_t *)rptr_va  = 0;
    *(volatile uint32_t *)fence_va = 0;

    struct amd_one_ring *r = &g->ring_mec;
    r->ring     = (volatile uint32_t *)ring_va;
    r->ring_pa  = ring_pa;
    r->ring_dw  = MEC_RING_DW;
    r->wptr     = 0;
    r->rptr_va  = (volatile uint32_t *)rptr_va;
    r->rptr_pa  = rptr_pa;
    r->fence_va = (volatile uint32_t *)fence_va;
    r->fence_pa = fence_pa;
    r->fence_seq = 0;

    /* Select SE=0, SH=0, INSTANCE=0 for HQD programming. */
    amd_wr(mmGRBM_GFX_INDEX,
           GRBM_GFX_INDEX_SE_INDEX(0) |
           GRBM_GFX_INDEX_SH_INDEX(0) |
           GRBM_GFX_INDEX_INSTANCE_INDEX(0));
    (void)amd_rd(mmGRBM_GFX_INDEX); /* flush */

    /* Deactivate the HQD first. */
    uint32_t active_prev = amd_rd(mmCP_HQD_ACTIVE);
    if (active_prev & 1u) {
        amd_wr(mmCP_HQD_DEQUEUE_REQUEST, 2u); /* DEQUEUE_REQUEST_HARD */
        /* Wait for dequeue (gfx_v8_0.c:gfx_v8_0_hqd_is_occupied) */
        uint64_t t0 = ck_rdtsc(), ticks = g->tsc_hz / 1000ull * 200u;
        while ((amd_rd(mmCP_HQD_ACTIVE) & 1u) && ck_rdtsc() - t0 < ticks)
            ck_pause();
    }
    amd_wr(mmCP_HQD_ACTIVE, 0);

    /* Programme the HQD: ring base, rptr report, wptr poll, EOP.
     * Follows gfx_v8_0.c:gfx_v8_0_hqd_load. */
    amd_wr(mmCP_HQD_PQ_BASE,    (uint32_t)(ring_pa >> 8));   /* PA >> 8 */
    amd_wr(mmCP_HQD_PQ_BASE_HI, (uint32_t)(ring_pa >> 40));
    amd_wr(mmCP_HQD_PQ_RPTR,    0);
    amd_wr(mmCP_HQD_PQ_RPTR_REPORT_ADDR,    (uint32_t)(rptr_pa & 0xffffffffu));
    amd_wr(mmCP_HQD_PQ_RPTR_REPORT_ADDR_HI, (uint32_t)(rptr_pa >> 32));
    amd_wr(mmCP_HQD_PQ_WPTR_POLL_ADDR,    (uint32_t)(rptr_pa & 0xffffffffu));
    amd_wr(mmCP_HQD_PQ_WPTR_POLL_ADDR_HI, (uint32_t)(rptr_pa >> 32));
    amd_wr(mmCP_HQD_PQ_CONTROL, MEC_RB_BLKSZ); /* ring size log2 - 1 in bits [4:0] */
    amd_wr(mmCP_HQD_EOP_BASE_ADDR,    (uint32_t)(eop_pa >> 8));
    amd_wr(mmCP_HQD_EOP_BASE_ADDR_HI, (uint32_t)(eop_pa >> 40));
    amd_wr(mmCP_HQD_EOP_CONTROL, 0x3u | (5u << 2u)); /* EOP_SIZE=5 (1<<5=32 entries), MTYPE=3 */
    amd_wr(mmCP_HQD_PIPE_PRIORITY,  0u);
    amd_wr(mmCP_HQD_QUEUE_PRIORITY, 1u);
    amd_wr(mmCP_HQD_QUANTUM, 0u);
    amd_wr(mmCP_HQD_PQ_WPTR, 0);

    /* Activate HQD. */
    amd_wr(mmCP_HQD_ACTIVE, 1u);
    (void)amd_rd(mmCP_HQD_ACTIVE); /* flush */

    /* Restore GRBM_GFX_INDEX to broadcast. */
    amd_wr(mmGRBM_GFX_INDEX,
           GRBM_GFX_INDEX_SE_BROADCAST | GRBM_GFX_INDEX_SH_BROADCAST |
           GRBM_GFX_INDEX_INSTANCE_BROADCAST);

    uint32_t active_now = amd_rd(mmCP_HQD_ACTIVE);
    ck_printf("gpu: compute: HQD SE=0 SH=0 INSTANCE=0 active=%u ring_pa=0x%llx ring_dw=%u\n",
              active_now & 1u, (unsigned long long)ring_pa, MEC_RING_DW);

    r->ready = 1;
    g->steps_done |= AMD_STEP_COMPUTE;
    ck_puts("gpu: compute: ready (pipe 0 queue 0, MEC1)\n");
    return 0;
}
