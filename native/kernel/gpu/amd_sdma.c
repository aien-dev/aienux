/* native/kernel/gpu/amd_sdma.c -- SDMA0 ring initialization and timed memory
 * operations for the 2017 iMac's AMD Radeon Pro 570/575/580 (Polaris 10, PCI 1002:67df).
 *
 * SDMA0 is the first DMA engine (OSS 3.0); it can fill or copy data without
 * the CP.  It has its own microcode loaded by the CP ME firmware initialisation;
 * if the CP is up, SDMA0 should also be running.
 *
 * Steps:
 *   1. Check SDMA0_STATUS_REG to see if the engine is idle (BIOS left it running).
 *   2. Disable and re-enable SDMA0 (sdma_v3_0.c:sdma_v3_0_gfx_stop /
 *      sdma_v3_0_gfx_resume).
 *   3. Allocate ring, RPTR and fence from DMA pool; programme
 *      SDMA0_GFX_RB_BASE, CNTL, RPTR_ADDR, WPTR.
 *   4. Submit a CONSTANT_FILL to a scratch VRAM range; verify the fill.
 *   5. Submit a COPY from system RAM to VRAM; time both against a CPU memset
 *      of the same buffer (write-combined vs UC for the GPU path, WB for CPU).
 *
 * Sources (MIT): drivers/gpu/drm/amd/amdgpu/sdma_v3_0.c, oss/oss_3_0_d.h.
 * No floats.  All waits TSC-bounded.  QEMU qualifies nothing physical. */
#include "amd_gpu.h"
#include <string.h>

/* SDMA packet opcodes (oss_3_0_d.h / sdma_v3_0.c) */
#define SDMA_OP_NOP               0x0u
#define SDMA_OP_COPY              0x1u
#define SDMA_OP_WRITE             0x2u
#define SDMA_OP_COPY_LINEAR       0x00u  /* sub-opcode */
#define SDMA_OP_WRITE_LINEAR      0x00u
#define SDMA_OP_CONST_FILL        0xbu

/* SDMA packet helper macros */
#define SDMA_PACKET(op,sub,e)     (((op)&0xffu) | (((sub)&0xffu)<<8u) | (((e)&0xffffu)<<16u))
#define SDMA_NOP_PKT              SDMA_PACKET(SDMA_OP_NOP,0,0)
#define SDMA_FILL_COUNT           512u  /* dwords to fill = 2 KiB */
#define SDMA_COPY_BYTES           (64u * 1024u)  /* 64 KiB benchmark copy */

/* SDMA0_GFX_RB_CNTL: ring size in log2(dwords).  For 2048 dwords: log2=11. */
#define SDMA_RB_SZ_LOG2  11u  /* 2048 dwords */

int amd_sdma_init(void)
{
    struct amd_gpu_state *g = &g_amd_gpu;

    if (!g->regs) { ck_puts("gpu: sdma: no register aperture\n"); return -1; }
    if (!(g->steps_done & AMD_STEP_UCODE)) {
        ck_puts("gpu: sdma: skipped (no firmware)\n");
        return 0;
    }

    /* Check engine status */
    uint32_t status = amd_rd(mmSDMA0_STATUS_REG);
    ck_printf("gpu: sdma: SDMA0_STATUS_REG=0x%08x (%s)\n",
              status, (status & SDMA0_STATUS_IDLE) ? "idle" : "active");

    /* Stop the GFX ring: write 0 to SDMA0_GFX_RB_CNTL (clear RB_ENABLE). */
    amd_wr(mmSDMA0_GFX_RB_CNTL, 0);
    amd_wait_ms(2);

    /* Allocate ring and rptr from DMA pool. */
    uint64_t ring_pa; void *ring_va;
    uint64_t rptr_pa; void *rptr_va;
    uint64_t fence_pa; void *fence_va;
    if (amd_dma_alloc(AMD_SDMA_RING_DW * 4u, &ring_pa, &ring_va) ||
        amd_dma_alloc(4u,                    &rptr_pa, &rptr_va) ||
        amd_dma_alloc(4u,                    &fence_pa, &fence_va)) {
        ck_puts("gpu: sdma: out of DMA memory\n");
        return -1;
    }

    struct amd_one_ring *r = &g->ring_sdma;
    r->ring     = (volatile uint32_t *)ring_va;
    r->ring_pa  = ring_pa;
    r->ring_dw  = AMD_SDMA_RING_DW;
    r->wptr     = 0;
    r->rptr_va  = (volatile uint32_t *)rptr_va;
    r->rptr_pa  = rptr_pa;
    r->fence_va = (volatile uint32_t *)fence_va;
    r->fence_pa = fence_pa;
    r->fence_seq = 0;
    *(r->rptr_va)  = 0;
    *(r->fence_va) = 0;

    /* Program ring registers (sdma_v3_0.c:sdma_v3_0_gfx_resume). */
    /* RPTR address: hardware writes rptr here after each packet */
    amd_wr(mmSDMA0_GFX_RB_RPTR_ADDR_LO, (uint32_t)(rptr_pa & 0xffffffffu));
    amd_wr(mmSDMA0_GFX_RB_RPTR_ADDR_HI, (uint32_t)(rptr_pa >> 32));
    /* Ring base address (byte address >> 8) */
    amd_wr(mmSDMA0_GFX_RB_BASE,    (uint32_t)(ring_pa >> 8));
    amd_wr(mmSDMA0_GFX_RB_BASE_HI, (uint32_t)(ring_pa >> 40));
    /* Reset write/read pointers to 0 */
    amd_wr(mmSDMA0_GFX_RB_WPTR, 0);
    amd_wr(mmSDMA0_GFX_RB_RPTR, 0);
    /* Enable GFX ring: RB_ENABLE=1, ring size */
    amd_wr(mmSDMA0_GFX_RB_CNTL,
           SDMA_RB_ENABLE | SDMA_RB_SIZE(SDMA_RB_SZ_LOG2));

    /* Disable IB processing (we use the ring directly). */
    amd_wr(mmSDMA0_GFX_IB_CNTL, 0);

    /* Wait for SDMA to come up: STATUS IDLE should stay asserted. */
    amd_wait_ms(5);
    status = amd_rd(mmSDMA0_STATUS_REG);
    ck_printf("gpu: sdma: after enable: STATUS=0x%08x ring_pa=0x%llx ring_dw=%u\n",
              status, (unsigned long long)ring_pa, AMD_SDMA_RING_DW);

    r->ready = 1;
    g->steps_done |= AMD_STEP_SDMA;
    ck_puts("gpu: sdma: ready\n");
    return 0;
}

/* ---- SDMA constant fill (benchmark entry point) -----------------------
 * Fills `count` dwords at VRAM physical address `vram_pa` with `fill_val`.
 * Returns AMD_RING_OK or error. */
int amd_sdma_fill(uint64_t dst_pa, uint32_t fill_val, uint32_t count_dw)
{
    /* SDMA CONSTANT_FILL packet (sdma_v3_0.c sdma_v3_0_emit_fill_buffer):
     *   dword 0: SDMA_PACKET(SDMA_OP_CONST_FILL, 0, 0)
     *   dword 1: dst_pa[31:0]
     *   dword 2: dst_pa[63:32]
     *   dword 3: fill_val
     *   dword 4: count (in bytes; bit 1:0 = element size, 2=DWORD)
     * Element size 2 = DWORD (4 bytes). */
    uint32_t pkt[5];
    pkt[0] = SDMA_PACKET(SDMA_OP_CONST_FILL, 0, 0);
    pkt[1] = (uint32_t)(dst_pa & 0xffffffffu);
    pkt[2] = (uint32_t)(dst_pa >> 32);
    pkt[3] = fill_val;
    pkt[4] = (count_dw * 4u) | (2u << 30u); /* bytes, DWORD element */

    return ck_gpu_ring_submit(AMD_RING_SDMA, pkt, 5);
}

/* ---- SDMA linear copy (RAM->VRAM or VRAM->VRAM) -----------------------*/
int amd_sdma_copy(uint64_t src_pa, uint64_t dst_pa, uint32_t bytes)
{
    /* SDMA LINEAR_COPY packet (sdma_v3_0.c sdma_v3_0_emit_copy_buffer):
     *   dword 0: SDMA_PACKET(SDMA_OP_COPY, SDMA_OP_COPY_LINEAR, 0)
     *   dword 1: count (bytes - 1)
     *   dword 2: reserved (0)
     *   dword 3: src_pa[31:0]
     *   dword 4: src_pa[63:32]
     *   dword 5: dst_pa[31:0]
     *   dword 6: dst_pa[63:32] */
    uint32_t pkt[7];
    pkt[0] = SDMA_PACKET(SDMA_OP_COPY, SDMA_OP_COPY_LINEAR, 0);
    pkt[1] = bytes - 1u;
    pkt[2] = 0;
    pkt[3] = (uint32_t)(src_pa & 0xffffffffu);
    pkt[4] = (uint32_t)(src_pa >> 32);
    pkt[5] = (uint32_t)(dst_pa & 0xffffffffu);
    pkt[6] = (uint32_t)(dst_pa >> 32);

    return ck_gpu_ring_submit(AMD_RING_SDMA, pkt, 7);
}

/* ---- Timed SDMA vs CPU benchmark (called from gpu_x86.c after ring up) --
 * Fills SDMA_COPY_BYTES bytes in VRAM via SDMA and via CPU memset.
 * Logs throughput in MiB/s (integer arithmetic, TSC-based). */
void amd_sdma_bench(void)
{
    struct amd_gpu_state *g = &g_amd_gpu;

    if (!(g->steps_done & AMD_STEP_SDMA) || !(g->steps_done & AMD_STEP_DISPLAY))
        return;

    /* Target: back buffer in VRAM (already mapped UC-). */
    uint64_t dst_pa  = g->bb_pa;
    volatile uint32_t *dst_cpu = g->bb_cpu;
    uint32_t count_dw = SDMA_COPY_BYTES / 4u;

    /* ---- SDMA fill benchmark ---- */
    struct amd_one_ring *r = &g->ring_sdma;
    uint32_t fence_seq = ++r->fence_seq;
    *r->fence_va = 0;

    uint64_t t0_sdma = ck_rdtsc();
    (void)amd_sdma_fill(dst_pa, 0xdeadbeef, count_dw);

    /* Follow with a fence (WRITE_DATA) to know when the fill is done. */
    uint32_t fence_pkt[5];
    fence_pkt[0] = SDMA_PACKET(SDMA_OP_WRITE, SDMA_OP_WRITE_LINEAR, 0);
    fence_pkt[1] = (uint32_t)(r->fence_pa & 0xffffffffu);
    fence_pkt[2] = (uint32_t)(r->fence_pa >> 32);
    fence_pkt[3] = 0;          /* count - 1: one dword */
    fence_pkt[4] = fence_seq;
    (void)ck_gpu_ring_submit(AMD_RING_SDMA, fence_pkt, 5);

    int frc = ck_gpu_fence_wait(r->fence_va, fence_seq, GPU_RING_WAIT_MS);
    uint64_t t1_sdma = ck_rdtsc();

    /* ---- CPU fill benchmark (UC- writes to same buffer) ---- */
    uint64_t t0_cpu = ck_rdtsc();
    for (uint32_t i = 0; i < count_dw; i++)
        dst_cpu[i] = 0xdeadbeef;
    __asm__ volatile("sfence" ::: "memory");
    uint64_t t1_cpu = ck_rdtsc();

    /* Compute throughput: bytes / tsc_ticks * tsc_hz = bytes/sec.
     * MiB/s = bytes * tsc_hz / (ticks * 1048576). */
    uint64_t hz = g->tsc_hz;
    uint64_t sdma_ticks = t1_sdma - t0_sdma;
    uint64_t cpu_ticks  = t1_cpu  - t0_cpu;
    /* MiB/s (avoid overflow: bytes=65536, hz~3e9, ticks~3e7 => product ~6e21 overflows)
     * Scale: MiB/s = (bytes/1024/1024) * hz / ticks = (bytes * hz) / (ticks * 1048576)
     * max: 65536 * 3e9 / 1e3 / 1048576 ~ 186 MiB/s, all fits in 64 bits. */
    uint64_t sdma_mbs = (sdma_ticks > 0)
        ? (uint64_t)SDMA_COPY_BYTES * hz / (sdma_ticks * 1048576ull) : 0;
    uint64_t cpu_mbs  = (cpu_ticks  > 0)
        ? (uint64_t)SDMA_COPY_BYTES * hz / (cpu_ticks  * 1048576ull) : 0;

    ck_printf("gpu: sdma_bench: fill %u bytes fence=%s sdma=%llu MiB/s cpu=%llu MiB/s "
              "(sdma_ticks=%llu cpu_ticks=%llu)\n",
              SDMA_COPY_BYTES,
              frc == AMD_RING_OK ? "ok" : "TIMEOUT",
              (unsigned long long)sdma_mbs,
              (unsigned long long)cpu_mbs,
              (unsigned long long)sdma_ticks,
              (unsigned long long)cpu_ticks);
}
