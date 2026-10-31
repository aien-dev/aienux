/* gpu_chan_trace: the differential trace for the two channel twins (L332 rung 1).
 *
 * Runs one fixed script of channel operations on the simulated GB10 (gpu_sim)
 * and prints, after every operation, its return code, every register access the
 * simulator logged since the last line, and a hash of the whole simulated memory
 * (page tables, ring, USERD, instance block, data). Built once with the hand
 * channel (gpu/gpu_chan.c, L193) and once with the generated one
 * (gpu/contract/gen/gch_contract.c from gpu/contract/gb10_chan.devc); the two
 * outputs must be byte-identical (`make gpu-contract-trace`).
 * The script covers: refused arguments, create, 300 submits across ring wraps
 * with retires, a full ring, a bad span and address, teardown idle, and teardown
 * of a hung channel (quarantine). */
#include "gmmu.h"
#include "gpu_chan.h"
#include "gpu_mem.h"
#include "gpu_sim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MEM_BYTES (8u << 20)
#define PA_BASE 0x80000000ull
#define POOL_PA PA_BASE
#define DATA_PA (PA_BASE + 64ull * 4096u)
#define VA_BASE 0x0000000200000000ull

static uint8_t *mem;
static struct ck_gsim sim;
static struct ck_gpu_regs regs;
static struct ck_gmmu_as as;
static struct ck_gmem gm;
static uint32_t seen;
static unsigned step;

static uint64_t fnv(const uint8_t *p, uint64_t n)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint64_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

static void line(const char *what, int rc)
{
    printf("%04u %-24s rc=%d", step++, what, rc);
    for (; seen < sim.nlog; seen++)
        printf(" %c%06x=%08x", sim.log[seen].write ? 'W' : 'R', sim.log[seen].off, sim.log[seen].val);
    printf(" doorbells=%u mem=%016llx\n", sim.doorbells, (unsigned long long)fnv(mem, MEM_BYTES));
}

static void fresh(void)
{
    memset(mem, 0xA5, MEM_BYTES);
    ck_gsim_init(&sim, mem, PA_BASE, MEM_BYTES);
    regs = ck_gsim_regs(&sim);
    seen = 0;
    if (ck_gmmu_as_init(&as, &sim.mem, POOL_PA, 64) != CK_GMMU_OK || ck_gmem_init(&gm, &as, &regs, 10, DATA_PA, 128, VA_BASE) != CK_GMEM_OK) {
        printf("setup failed\n");
        exit(1);
    }
    line("setup", 0);
}

int main(void)
{
    mem = malloc(MEM_BYTES);
    if (!mem)
        return 1;
    struct ck_gpu_chan_ops ops = ck_gsim_chan_ops();
    struct ck_gpu_chan ch;
    struct ck_gmem_buf *pb = 0, *sem = 0;
    char what[64];

    fresh();
    line("create chid>max", ck_gpu_chan_create(&ch, &gm, &regs, &ops, &sim, 0x1000, 1, 16));
    line("create runlist=0x7f", ck_gpu_chan_create(&ch, &gm, &regs, &ops, &sim, 3, 0x7F, 16));
    line("create entries=24", ck_gpu_chan_create(&ch, &gm, &regs, &ops, &sim, 3, 1, 24));
    line("create entries=1", ck_gpu_chan_create(&ch, &gm, &regs, &ops, &sim, 3, 1, 1));
    line("create", ck_gpu_chan_create(&ch, &gm, &regs, &ops, &sim, 3, 1, 16));
    printf("token=%08x rl=%08x,%08x\n", ch.d.token, ch.d.rl_entry[0], ch.d.rl_entry[1]);
    line("alloc pb", ck_gmem_alloc(&gm, 8192, CK_GMEM_CACHED, CK_GMEM_GPU_RO, &pb));
    line("alloc sem", ck_gmem_alloc(&gm, 4096, CK_GMEM_UNCACHED, CK_GMEM_HOST_POLLED, &sem));
    for (uint32_t i = 0; i < 4096; i++)
        pb->cpu[i] = (uint8_t)(i * 7u);
    line("submit off=2", ck_gpu_chan_submit(&ch, pb, 2, 4));
    line("submit nwords=0", ck_gpu_chan_submit(&ch, pb, 0, 0));
    line("submit past end", ck_gpu_chan_submit(&ch, pb, 8188, 2));
    line("submit foreign buf", ck_gpu_chan_submit(&ch, (const struct ck_gmem_buf *)&ch, 0, 4));
    for (uint32_t i = 0; i < 300; i++) {
        int rc = ck_gpu_chan_submit(&ch, pb, (i % 64u) * 64u, 1u + i % 16u);
        snprintf(what, sizeof what, "submit %u", i);
        line(what, rc);
        if (rc == CK_GCHAN_EFULL || i % 5u == 4u) {
            ck_gpu_chan_retire(&ch, ch.put - (i % 3u));
            snprintf(what, sizeof what, "retire put=%u ret=%u", ch.put, ch.retired);
            line(what, 0);
        }
    }
    ck_gpu_chan_retire(&ch, ch.put + 5u); /* past put: ignored */
    snprintf(what, sizeof what, "retire past put ret=%u", ch.retired);
    line(what, 0);
    struct ck_gmem_buf *extra[2] = {pb, sem};
    line("teardown idle", ck_gpu_chan_teardown(&ch, 4, extra, 2));
    line("teardown again", ck_gpu_chan_teardown(&ch, 4, extra, 2));
    line("submit after close", ck_gpu_chan_submit(&ch, pb, 0, 4));

    fresh();
    line("create (hung run)", ck_gpu_chan_create(&ch, &gm, &regs, &ops, &sim, 7, 2, 8));
    pb = sem = 0;
    line("alloc pb", ck_gmem_alloc(&gm, 4096, CK_GMEM_CACHED, CK_GMEM_GPU_RO, &pb));
    for (uint32_t i = 0; i < 9; i++) {
        snprintf(what, sizeof what, "submit %u", i);
        line(what, ck_gpu_chan_submit(&ch, pb, 0, 8));
    }
    sim.hang = 1;
    struct ck_gmem_buf *extra2[1] = {pb};
    line("teardown hung", ck_gpu_chan_teardown(&ch, 6, extra2, 1));
    printf("quarantined_pages=%u free_pages=%u\n", ck_gmem_count(&gm, CK_GMEM_QUARANTINED), ck_gmem_count(&gm, CK_GMEM_FREE));
    free(mem);
    return 0;
}
