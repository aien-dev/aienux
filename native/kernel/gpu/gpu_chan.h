/* gpu_chan.h -- one GB10 compute channel's memory and doorbell (cut D, host
 * side), freestanding.
 *
 * DOCUMENTED (open-gpu-kernel-modules 580.173.02):
 * - GPFIFO entry: 8 bytes, NVC96F_GP_ENTRY0_GET 31:2, GP_ENTRY1_GET_HI 7:0,
 *   LEVEL 9:9, LENGTH 30:10 (class/clc96f.h). So the pushbuffer address
 *   field is 40 bits and the length 21 bits.
 * - USERD: Nvc96fControl, GPPut at byte 0x8c, 512 bytes (clc96f.h). It has
 *   no GPGet, so consumption is tracked by the caller (as physics does).
 * - Runlist entry USERD pointer on GB10B: NV_RAMRL_ENTRY_CHAN_USERD_PTR_LO
 *   31:8 of word 0, USERD_PTR_HI_HW 19:0 of word 1
 *   (swref/published/blackwell/gb10b/dev_ram.h, checked by
 *   kchannelIsUserdAddrSizeValid_GB10B).
 * - Work-submit token: kfifoGenerateWorkSubmitTokenHal_GB100 (the HAL GB10B
 *   gets): DOORBELL_RUNLIST_ID 22:16 = runlist id, DOORBELL_VECTOR 11:0 =
 *   channel id (blackwell/gb100/dev_vm.h). RUNLIST_ID 0x7F is INVALID.
 * - Doorbell register: NV_VIRTUAL_FUNCTION_DOORBELL at 0x30090 in the VF
 *   space (gb100/dev_vm.h); usermode class NVC361_NOTIFY_CHANNEL_PENDING is
 *   the same 0x90 inside the 0x30000 usermode page.
 * - Instance block size 4096 (NV_RAMIN_ALLOC_SIZE, ampere/ga100/dev_ram.h).
 * - Instance block page directory: for GB10B the driver uses
 *   kgmmuInstBlkPageDirBaseGet_GV100 -> _GP100 (kern_gmmu_gp100.c): word 128 =
 *   TARGET 1:0 | VOL 2 | FAULT_REPLAY_TEX 4 | FAULT_REPLAY_GCC 5 |
 *   USE_NEW_PT_FORMAT 10 (TRUE only for format VERSION_2, so FALSE on GB10B) |
 *   BIG_PAGE_SIZE 11 (1 = 64 KiB) | LO 31:12 = pa >> 12;
 *   word 129 = HI = pa >> 32 (pascal/gp100 and volta/gv100 dev_ram.h). Driver
 *   behaviour, DOCUMENTED; that the GB10 silicon reads it there is UNVERIFIED.
 *
 * UNKNOWN for GB10B (no public header): the subcontext PDB table
 * (NV_RAMIN_SC_* exists on GV100; which entries GB10B needs is not
 * checked here), the rest of the instance block, the other runlist-entry words, the USERD target/aperture bits,
 * and how the channel is made runnable. On GB10 RM asks GSP to do that
 * (channel allocation RPC); here it is the `bind` op, filled by the
 * simulator in tests and by the GSP lane (L192) on hardware.
 *
 * Encoders carried bit-identical from physics (aien-spark/physics
 * nvrm/nvrm.h:135 nvrm_mthd, nvrm/nvrm.c:809 nvrm_gp_entry): they live in a
 * Linux-only header there and Omega has not moved them to a shared header
 * yet (dependency map cut A). Keep them byte-for-byte equal. */
#ifndef CK_GPU_CHAN_H
#define CK_GPU_CHAN_H

#include <stdint.h>
#include "gmmu.h"
#include "gpu_mem.h"

/* nvrm_mthd: incrementing method header (bits 31:29 = 1, INC_METHOD). */
static inline uint32_t ck_gpu_mthd(uint32_t subc, uint32_t mthd, uint32_t count)
{
    return (2u << 28) | (count << 16) | (subc << 13) | (mthd >> 2);
}

/* nvrm_gp_entry: GET/GET_HI = va, LEVEL bit (41) set as physics does,
 * LENGTH = nwords. Callers must check va < 2^40 and nwords <= 0x1fffff
 * (ck_gpu_chan_submit does). */
static inline uint64_t ck_gpu_gp_entry(uint64_t va, uint32_t nwords)
{
    return (va & ~3ull) | ((uint64_t)(nwords & 0x1fffff) << 42) | (1ull << 41);
}

#define CK_GPU_GP_VA_LIMIT (1ull << 40)
#define CK_GPU_GP_LEN_MAX 0x1fffffu
#define CK_GPU_USERD_BYTES 512u
#define CK_GPU_USERD_GPPUT 0x8cu
#define CK_GPU_INST_BYTES 4096u
#define CK_GPU_RAMIN_PDB_WORD 128u
#define CK_GPU_RAMIN_TARGET_SYS_COH 2u
#define CK_GPU_RAMIN_NEW_PT_FORMAT (1u << 10)
#define CK_GPU_RAMIN_BIG_PAGE_64K (1u << 11)
#define CK_GPU_REG_DOORBELL (CK_GPU_VF_BASE + 0x30090u)
#define CK_GPU_CHID_MAX 0xFFFu
#define CK_GPU_RUNLIST_INVALID 0x7Fu

uint32_t ck_gpu_submit_token(uint32_t runlist_id, uint32_t chid);
/* Encode the USERD pointer words of a runlist channel entry. Refuses a
 * pointer that is not 256-byte aligned or does not fit 52 bits. Other bits
 * of the entry are UNKNOWN on GB10B and left zero. */
int ck_gpu_rl_userd(uint32_t out[2], uint64_t userd_pa);

/* What the device side needs to make the channel runnable (what a GSP
 * channel-allocation request would carry). */
struct ck_gpu_chan_desc {
    uint32_t chid, runlist_id, token, entries;
    uint64_t pdb;       /* PD4 physical address of the channel's address space */
    uint64_t gpfifo_va; /* GPU VA of the ring */
    uint64_t userd_pa, inst_pa;
    uint32_t rl_entry[2];
};

struct ck_gpu_chan_ops {
    int (*bind)(void *ctx, const struct ck_gpu_chan_desc *d);
    int (*disable)(void *ctx, uint32_t chid);
    int (*is_idle)(void *ctx, uint32_t chid);
};

enum ck_gpu_chan_status {
    CK_GCHAN_OK = 0,
    CK_GCHAN_EINVAL,
    CK_GCHAN_ENOMEM,
    CK_GCHAN_ESTATE,    /* not open, or faulted */
    CK_GCHAN_EFULL,     /* ring full: retire first */
    CK_GCHAN_EBIND,     /* device refused the channel */
    CK_GCHAN_ETIMEDOUT, /* teardown: never idle; memory quarantined */
};

struct ck_gpu_chan {
    struct ck_gmem *mem;
    const struct ck_gpu_regs *regs;
    struct ck_gpu_chan_ops ops;
    void *ops_ctx;
    struct ck_gpu_chan_desc d;
    struct ck_gmem_buf *gpfifo, *userd, *inst;
    uint32_t put, retired;
    uint32_t open;
};

/* entries: power of two, 2..65536. */
int ck_gpu_chan_create(struct ck_gpu_chan *c, struct ck_gmem *mem, const struct ck_gpu_regs *regs,
                       const struct ck_gpu_chan_ops *ops, void *ops_ctx, uint32_t chid, uint32_t runlist_id,
                       uint32_t entries);
/* Queue nwords of pb starting at byte off, publish GPPut, ring the doorbell.
 * pb must be a live buffer of the same allocator. */
int ck_gpu_chan_submit(struct ck_gpu_chan *c, const struct ck_gmem_buf *pb, uint32_t off, uint32_t nwords);
/* The caller saw entries up to `upto` complete (e.g. a semaphore it polled). */
void ck_gpu_chan_retire(struct ck_gpu_chan *c, uint32_t upto);
/* Disable, wait for idle at most idle_budget polls. Idle: free the ring,
 * USERD and instance block. Not idle: quarantine them and every buffer in
 * extra[] (pushbuffers, semaphores the GPU may still write). */
int ck_gpu_chan_teardown(struct ck_gpu_chan *c, uint32_t idle_budget, struct ck_gmem_buf **extra, uint32_t nextra);

#endif
