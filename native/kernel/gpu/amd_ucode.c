/* native/kernel/gpu/amd_ucode.c -- RLC and CP microcode load for the 2017 iMac's
 * AMD Radeon Pro 570/575/580 (Polaris 10, PCI 1002:67df, GFX 8.0).
 *
 * Steps:
 *   1. Soft-reset CP and RLC via GRBM_SOFT_RESET (gfx_v8_0.c:gfx_v8_0_cp_reset).
 *   2. Load RLC firmware: write index 0 to mmRLC_GPM_UCODE_ADDR, stream dwords to
 *      mmRLC_GPM_UCODE_DATA, enable via mmRLC_CNTL.
 *   3. Load CP PFP, ME, CE, MEC1, MEC2 firmware via their respective
 *      UCODE_ADDR / UCODE_DATA registers.
 *   4. Configure GFX ring 0 (mmCP_RB0_BASE, CP_RB0_CNTL, CP_RB0_RPTR_ADDR).
 *   5. Start the CP; wait for CP_STAT.PFP_BUSY to deassert (bounded).
 *   6. Submit a NOP + WRITE_DATA fence packet; poll the fence slot in memory
 *      to confirm the CP is executing.
 *
 * Microcode blobs are fetched at build time by scripts/fetch_gpu_ucode.sh and
 * embedded as C arrays in target/native-kernel/gpu_ucode.inc.  The build
 * computes and verifies sha256 against the pinned digests in
 * scripts/gpu_ucode_sha256.txt before embedding.  Blobs are never committed.
 * Source: linux-firmware amdgpu/polaris11_*.bin (AMD redistributable licence).
 * KNOWN MISMATCH (L213): the iMac chip (1002:67df) is Polaris 10, and Linux v6.12 loads
 * amdgpu/polaris10_*.bin for it (gfx_v8_0.c:967-968, MODULE_FIRMWARE at gfx_v8_0.c:134-144).
 * These pinned polaris11 blobs have not been proven to run on it; see ~/handoffs/fixer-imac-gpu-id.md.
 *
 * If the include is absent (CK_X86_GPU_UCODE=0, default for now), the step
 * logs "not available" and skips to Step 5, leaving the CP unfueled.  In that
 * mode the GFX ring and fence test are skipped and amd_sdma_init / amd_compute_init
 * will not run.
 *
 * Sources (MIT): drivers/gpu/drm/amd/amdgpu/gfx_v8_0.c, gca/gfx_8_0_d.h.
 * No floats.  All waits TSC-bounded.  QEMU qualifies nothing physical. */
#include "amd_gpu.h"

/* PM4 packet words (gfx_v8_0.c:891-892 and 4165; PACKET3/PACKET2 layout as
 * amdgpu_ring.h: type in bits 31:30, opcode 15:8, body count-1 in 29:16). */
#define PM4_PACKET3(op, n)            (0xc0000000u | (((uint32_t)(op) & 0xffu) << 8) | (((uint32_t)(n) & 0x3fffu) << 16))
#define PM4_FILLER2                   0x80000000u   /* type-2 filler: one dword, no body */
#define PM4_IT_WRITE_DATA             0x37u
#define PM4_DST_SEL_MEM               5u            /* WRITE_DATA_DST_SEL(5): memory */
#define PM4_WRITE_DATA_DST_SEL(v)     ((uint32_t)(v) << 8)
#define PM4_WR_CONFIRM                (1u << 20)

#ifndef CK_X86_GPU_UCODE
#define CK_X86_GPU_UCODE 0
#endif

#if CK_X86_GPU_UCODE
/* Embedded firmware arrays.  The build generates this file from the blobs.
 * Found through the -I target/native-kernel that the Makefile adds for
 * CK_X86_GPU_UCODE=1 (a relative ../ path here would land outside the repo). */
#include "gpu_ucode.inc"
#else
/* Stubs: the step will log its absence and skip */
static const uint32_t g_rlc_ucode[]  __attribute__((unused)) = {0};
static const uint32_t g_pfp_ucode[]  __attribute__((unused)) = {0};
static const uint32_t g_me_ucode[]   __attribute__((unused)) = {0};
static const uint32_t g_ce_ucode[]   __attribute__((unused)) = {0};
static const uint32_t g_mec_ucode[]  __attribute__((unused)) = {0};
static const uint32_t g_mec2_ucode[] __attribute__((unused)) = {0};
#define RLC_UCODE_SIZE   0u
#define PFP_UCODE_SIZE   0u
#define ME_UCODE_SIZE    0u
#define CE_UCODE_SIZE    0u
#define MEC_UCODE_SIZE   0u
#define MEC2_UCODE_SIZE  0u
#endif

/* Ring buffer is 4096 dwords = 16 KiB.  Allocated from DMA pool. */
#define GFX_RING_DW    AMD_RING_DWORDS  /* 4096 */

/* CP_RB0_CNTL: RB_BUFSZ = log2(ring bytes / 8), RB_BLKSZ = RB_BUFSZ - 2 (gfx_v8_0.c:4248-4250).
 * 4096 dwords = 16 KiB = 2^11 qwords: BUFSZ = 11. */
#define GFX_RB_BUFSZ    11u  /* 8 << 11 = 16 KiB = 4096 dwords */

/* Soft-reset CP and RLC, wait for reset to land, then clear. */
static int __attribute__((unused)) gfx_soft_reset(void)
{
    amd_wr(mmGRBM_SOFT_RESET,
           GRBM_SOFT_RESET_CP | GRBM_SOFT_RESET_GFX | GRBM_SOFT_RESET_RLC);
    (void)amd_rd(mmGRBM_SOFT_RESET); /* flush */
    amd_wait_ms(1);
    amd_wr(mmGRBM_SOFT_RESET, 0);
    (void)amd_rd(mmGRBM_SOFT_RESET);
    amd_wait_ms(2);
    return 0;
}

/* Load a block of dwords into an upload register pair (addr=0 clears address). */
static void __attribute__((unused)) ucode_stream(unsigned addr_idx, unsigned data_idx,
                         const uint32_t *dw, unsigned count)
{
    amd_wr(addr_idx, 0);
    for (unsigned i = 0; i < count; i++)
        amd_wr(data_idx, dw[i]);
}

int amd_ucode_load(void)
{
    struct amd_gpu_state *g = &g_amd_gpu;

    if (!g->regs) {
        ck_puts("gpu: ucode: no register aperture\n");
        return -1;
    }

#if !CK_X86_GPU_UCODE
    ck_puts("gpu: ucode: CK_X86_GPU_UCODE=0; skipping firmware load "
            "(build with scripts/fetch_gpu_ucode.sh first)\n");
    /* Allow display / MC / SMU steps that are already done; just don't mark
     * UCODE done so ring init will be skipped. */
    return 0;
#else
    ck_puts("gpu: ucode: loading RLC + CP (PFP/ME/CE/MEC1/MEC2) firmware\n");

    /* 1. Soft-reset */
    gfx_soft_reset();
    ck_puts("gpu: ucode: soft-reset done\n");

    /* 2. Load RLC */
    if (RLC_UCODE_SIZE) {
        /* Disable RLC before upload (gfx_v8_0.c:gfx_v8_0_rlc_fini) */
        amd_wr(mmRLC_CNTL, 0);
        ucode_stream(mmRLC_GPM_UCODE_ADDR, mmRLC_GPM_UCODE_DATA, g_rlc_ucode, RLC_UCODE_SIZE);
        ck_printf("gpu: ucode: RLC loaded (%u dwords)\n", RLC_UCODE_SIZE);
    }

    /* 3. Load CP: PFP then ME then CE */
    if (PFP_UCODE_SIZE) {
        ucode_stream(mmCP_PFP_UCODE_ADDR, mmCP_PFP_UCODE_DATA, g_pfp_ucode, PFP_UCODE_SIZE);
        ck_printf("gpu: ucode: PFP loaded (%u dwords)\n", PFP_UCODE_SIZE);
    }
    if (ME_UCODE_SIZE) {
        ucode_stream(mmCP_ME_RAM_WADDR, mmCP_ME_RAM_DATA, g_me_ucode, ME_UCODE_SIZE);
        ck_printf("gpu: ucode: ME loaded (%u dwords)\n", ME_UCODE_SIZE);
    }
    if (CE_UCODE_SIZE) {
        ucode_stream(mmCP_CE_UCODE_ADDR, mmCP_CE_UCODE_DATA, g_ce_ucode, CE_UCODE_SIZE);
        ck_printf("gpu: ucode: CE loaded (%u dwords)\n", CE_UCODE_SIZE);
    }
    if (MEC_UCODE_SIZE) {
        ucode_stream(mmCP_MEC_ME1_UCODE_ADDR, mmCP_MEC_ME1_UCODE_DATA, g_mec_ucode, MEC_UCODE_SIZE);
        ck_printf("gpu: ucode: MEC1 loaded (%u dwords)\n", MEC_UCODE_SIZE);
    }
    if (MEC2_UCODE_SIZE) {
        ucode_stream(mmCP_MEC_ME2_UCODE_ADDR, mmCP_MEC_ME2_UCODE_DATA, g_mec2_ucode, MEC2_UCODE_SIZE);
        ck_printf("gpu: ucode: MEC2 loaded (%u dwords)\n", MEC2_UCODE_SIZE);
    }

    /* Enable RLC */
    amd_wr(mmRLC_CNTL, RLC_ENABLE);
    amd_wait_ms(2);

    g->steps_done |= AMD_STEP_UCODE;
    ck_puts("gpu: ucode: done\n");
    return 0;
#endif /* CK_X86_GPU_UCODE */
}

/* ---- GFX ring initialisation ------------------------------------------*/
int amd_gfx_ring_init(void)
{
    struct amd_gpu_state *g = &g_amd_gpu;

    if (!g->regs) return -1;
    if (!(g->steps_done & AMD_STEP_UCODE)) {
        ck_puts("gpu: gfx_ring: skipped (no firmware)\n");
        return 0;
    }

    /* Allocate ring dwords, rptr slot, and fence slot from DMA pool. */
    uint64_t ring_pa;  void *ring_va;
    uint64_t rptr_pa;  void *rptr_va;
    uint64_t fence_pa; void *fence_va;
    if (amd_dma_alloc(GFX_RING_DW * 4u, &ring_pa, &ring_va)  ||
        amd_dma_alloc(4u,              &rptr_pa, &rptr_va)   ||
        amd_dma_alloc(4u,              &fence_pa, &fence_va)) {
        ck_puts("gpu: gfx_ring: out of DMA memory\n");
        return -1;
    }

    struct amd_one_ring *r = &g->ring_gfx;
    r->ring    = (volatile uint32_t *)ring_va;
    r->ring_pa = ring_pa;
    r->ring_dw = GFX_RING_DW;
    r->wptr    = 0;
    r->rptr_va = (volatile uint32_t *)rptr_va;
    r->rptr_pa = rptr_pa;
    r->fence_va = (volatile uint32_t *)fence_va;
    r->fence_pa = fence_pa;
    r->fence_seq = 0;
    *(r->rptr_va)  = 0;
    *(r->fence_va) = 0;

    /* Program ring into CP_RB0.  Sequence from gfx_v8_0.c:gfx_v8_0_cp_gfx_resume. */
    /* Write rptr report address (hardware writes RPTR here on retire). */
    amd_wr(mmCP_RB0_RPTR_ADDR,    (uint32_t)(rptr_pa & 0xffffffffu));
    amd_wr(mmCP_RB0_RPTR_ADDR_HI, (uint32_t)(rptr_pa >> 32));

    /* Set ring buffer size (BUFSZ=11: 4096 dwords, BLKSZ=BUFSZ-2) and NO_UPDATE=0. */
    amd_wr(mmCP_RB0_CNTL, CP_RB_BUFSZ(GFX_RB_BUFSZ) | CP_RB_BLKSZ(GFX_RB_BUFSZ - 2u));

    /* Set ring base address. */
    amd_wr(mmCP_RB0_BASE,    (uint32_t)(ring_pa >> 8));  /* base in 256-byte units */
    amd_wr(mmCP_RB0_BASE_HI, (uint32_t)(ring_pa >> 40));

    /* Reset write pointer. */
    amd_wr(mmCP_RB0_WPTR, 0);

    /* Start the CP: clear ME_HALT. */
    uint32_t cp_cntl = amd_rd(mmCP_RB0_CNTL);
    amd_wr(mmCP_RB0_CNTL, cp_cntl & ~(1u << 28)); /* clear CP_RB0_CNTL__RB_NO_UPDATE_MASK */

    /* Enable RLC */
    amd_wr(mmRLC_CNTL, RLC_ENABLE);

    /* Wait for CP to be ready (CP_STAT.PFP_BUSY = 0). */
    uint64_t t0 = ck_rdtsc(), ticks = g->tsc_hz / 1000ull * GPU_RING_WAIT_MS;
    uint32_t stat;
    int cp_up = 0;
    do {
        stat = amd_rd(mmCP_STAT);
        if (!(stat & CP_STAT_PFP_BUSY)) { cp_up = 1; break; }
        amd_wait_us(100);
    } while (ck_rdtsc() - t0 < ticks);

    ck_printf("gpu: gfx_ring: CP_STAT=0x%08x (%s) ring_pa=0x%llx ring_dw=%u\n",
              stat, cp_up ? "idle" : "BUSY after timeout",
              (unsigned long long)ring_pa, GFX_RING_DW);

    if (!cp_up) {
        ck_puts("gpu: gfx_ring: CP did not come up; GFX ring not ready\n");
        return -1;
    }

    r->ready = 1;
    g->steps_done |= AMD_STEP_GFX_RING;
    ck_puts("gpu: gfx_ring: ready\n");
    return 0;
}

/* ---- Ring submit / fence (implements amd_ring.h public API) -----------*/

int ck_gpu_ring_submit(amd_ring_id_t id, const uint32_t *dw, unsigned count)
{
    struct amd_gpu_state *g = &g_amd_gpu;
    struct amd_one_ring  *r;

    switch (id) {
    case AMD_RING_GFX:  r = &g->ring_gfx;  break;
    case AMD_RING_SDMA: r = &g->ring_sdma; break;
    case AMD_RING_MEC:  r = &g->ring_mec;  break;
    default: return AMD_RING_ENODEV;
    }
    if (!r->ready)       return AMD_RING_ENODEV;
    if (count == 0 || count > 256) return AMD_RING_EFULL;

    /* Check for space: need count dwords; ring is full if wptr would lap rptr. */
    uint32_t free = r->ring_dw - ((r->wptr - *r->rptr_va) & (r->ring_dw - 1u));
    if (count > free) return AMD_RING_EFULL;

    /* Write dwords into ring. */
    for (unsigned i = 0; i < count; i++) {
        r->ring[r->wptr & (r->ring_dw - 1u)] = dw[i];
        r->wptr++;
    }
    __asm__ volatile("" ::: "memory");
#if defined(__x86_64__)
    __asm__ volatile("sfence" ::: "memory");  /* WC/UC write ordering (kernel build, x86-64) */
#else
    __atomic_thread_fence(__ATOMIC_RELEASE);  /* host test on other hosts */
#endif

    /* Advance write pointer (hardware polls WPTR). */
    if (id == AMD_RING_GFX)
        amd_wr(mmCP_RB0_WPTR, r->wptr & (r->ring_dw - 1u));
    else if (id == AMD_RING_SDMA)
        amd_wr(mmSDMA0_GFX_RB_WPTR, r->wptr * 4u);  /* SDMA uses byte offset */

    return AMD_RING_OK;
}

int ck_gpu_fence_wait(const volatile uint32_t *fence_va, uint32_t seq, unsigned timeout_ms)
{
    uint64_t t0    = ck_rdtsc();
    uint64_t ticks = g_amd_gpu.tsc_hz / 1000ull * (timeout_ms ? timeout_ms : 1u);
    do {
        if (*fence_va == seq) return AMD_RING_OK;
        ck_pause();
    } while (ck_rdtsc() - t0 < ticks);
    return AMD_RING_ETIMEOUT;
}

/* Submit a NOP + WRITE_DATA fence and wait for it to land.
 * This is the "GFX ring self-test" described in the task brief. */
int amd_gfx_ring_test(void)
{
    struct amd_gpu_state *g = &g_amd_gpu;
    struct amd_one_ring  *r = &g->ring_gfx;

    if (!r->ready) {
        ck_puts("gpu: gfx_test: ring not ready; skipped\n");
        return 0;
    }

    uint32_t seq = ++r->fence_seq;
    *r->fence_va = 0; /* clear */

    /* Packet stream, as gfx_v8_0_ring_test_ib emits it (gfx_v8_0.c:891-892):
     * a one-dword PACKET2 filler (0x80000000, the padding gfx_v8_0.c:4165
     * writes), then PACKET3(WRITE_DATA, 3): header, info (DST_SEL=5 memory,
     * WR_CONFIRM), address low, address high, data. Six dwords in all. */
    uint32_t pkt[6];
    pkt[0] = PM4_FILLER2;
    pkt[1] = PM4_PACKET3(PM4_IT_WRITE_DATA, 3);
    pkt[2] = PM4_WRITE_DATA_DST_SEL(PM4_DST_SEL_MEM) | PM4_WR_CONFIRM;
    pkt[3] = (uint32_t)(r->fence_pa & 0xffffffffu);
    pkt[4] = (uint32_t)(r->fence_pa >> 32);
    pkt[5] = seq;

    int rc = ck_gpu_ring_submit(AMD_RING_GFX, pkt, 6);
    if (rc != AMD_RING_OK) {
        ck_printf("gpu: gfx_test: submit failed rc=%d\n", rc);
        return rc;
    }

    rc = ck_gpu_fence_wait(r->fence_va, seq, GPU_RING_WAIT_MS);
    if (rc != AMD_RING_OK) {
        ck_printf("gpu: gfx_test: fence timeout after %u ms (fence_va=0x%08x wanted %u)\n",
                  GPU_RING_WAIT_MS, *r->fence_va, seq);
        return rc;
    }
    ck_printf("gpu: gfx_test: PASS (fence seq=%u landed at 0x%llx)\n",
              seq, (unsigned long long)r->fence_pa);
    return AMD_RING_OK;
}
