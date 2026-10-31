/* gmmu.h -- GB10 GPU page tables (cut D, host side), freestanding.
 *
 * Format source (DOCUMENTED, open-gpu-kernel-modules tag 580.173.02): for
 * GB10B the driver HAL picks kgmmuFmtInitLevels_GB10X and
 * kgmmuFmtInitPte/Pde_GH10X (src/nvidia/generated/g_kern_gmmu_nvoc.c, the
 * "else" branch after T234D/T264D, TU/GA/AD and GH100); field layouts are
 * NV_MMU_VER3_* in src/common/inc/swref/published/hopper/gh100/dev_mmu.h.
 *
 *   57-bit VA.  PD4 [56]  PD3 [55:47]  PD2 [46:38]  PD1 [37:29]
 *   PD0 [28:21] (dual 16-byte PDE: big half, small half; or a 2 MiB PTE)
 *   PT_BIG [20:16] (64 KiB pages)   PT_SMALL [20:12] (4 KiB pages)
 *
 * PD1 may hold a 512 MiB PTE and PD2 a 256 GiB PTE on GB10X; this code never
 * writes those (it maps 4 KiB, 64 KiB and 2 MiB pages only) but the walker
 * in gpu_sim.c decodes them so a stray big PTE is still translated the way
 * the format says.
 *
 * Names: this code calls the bit-56 level PD4 and the dual level PD0, as the
 * ASCII diagram in kern_gmmu_fmt_gh10x.c does. The driver's pageLevelIdTag
 * runs the other way (bit 56 is MMU_FMT_PT_SURF_ID_PD0, the dual level
 * PD4); tests assert bit ranges, not names.
 *
 * A 2 MiB PTE sits in the low 8 bytes of the 16-byte PD0 slot, high 8 bytes
 * zero: the driver's VER3 PTE descriptor (kgmmuFmtInitPte_GH10X) places its
 * fields at bits 0..63 of the entry. Driver-sourced; the chip must confirm.
 *
 * Whether the GB10 silicon walks exactly this format is UNKNOWN until a chip
 * run; it is what the pinned driver programs for it.
 */
#ifndef CK_GMMU_H
#define CK_GMMU_H

#include <stdint.h>

#define CK_GMMU_VA_BITS 57u
#define CK_GMMU_PA_BITS 52u /* NV_MMU_VER3_PTE_ADDRESS 51:12 */
#define CK_GMMU_SZ_4K  0x1000ull
#define CK_GMMU_SZ_64K 0x10000ull
#define CK_GMMU_SZ_2M  0x200000ull
#define CK_GMMU_PT_BYTES 4096u /* every level instance this code allocates */

/* PDE aperture (NV_MMU_VER3_PDE_APERTURE 2:1). */
#define CK_GMMU_PDE_AP_INVALID 0u
#define CK_GMMU_PDE_AP_VIDMEM  1u
#define CK_GMMU_PDE_AP_SYS_COH 2u
#define CK_GMMU_PDE_AP_SYS_NC  3u
/* PTE aperture (NV_MMU_VER3_PTE_APERTURE 2:1); note the different numbering. */
#define CK_GMMU_PTE_AP_VIDMEM  0u
#define CK_GMMU_PTE_AP_PEER    1u
#define CK_GMMU_PTE_AP_SYS_COH 2u
#define CK_GMMU_PTE_AP_SYS_NC  3u
/* PTE PCF (NV_MMU_VER3_PTE_PCF 7:3), the values this code uses. */
#define CK_GMMU_PCF_RW_CACHED   0x00u /* REGULAR_RW_ATOMIC_CACHED_ACE */
#define CK_GMMU_PCF_RW_UNCACHED 0x01u /* REGULAR_RW_ATOMIC_UNCACHED_ACE */
#define CK_GMMU_PCF_RO_CACHED   0x04u /* REGULAR_RO_ATOMIC_CACHED_ACE */
#define CK_GMMU_PCF_RO_UNCACHED 0x05u /* REGULAR_RO_ATOMIC_UNCACHED_ACE */
/* PDE PCF (NV_MMU_VER3_PDE_PCF 5:3): VALID_CACHED_ATS_NOT_ALLOWED. */
#define CK_GMMU_PDE_PCF_CACHED_NO_ATS 0x2u

/* Entry encoders: pure bit packing, exactly the dev_mmu.h fields. */
uint64_t ck_gmmu_pde(uint64_t pa, uint32_t aperture);
void ck_gmmu_dual_pde(uint64_t out[2], uint64_t big_pa, uint32_t big_ap, uint64_t small_pa, uint32_t small_ap);
uint64_t ck_gmmu_pte(uint64_t pa, uint32_t aperture, uint32_t pcf, uint32_t kind);

/* The CPU's view of the GPU-visible physical window. GB10 memory is unified
 * (one LPDDR5x pool); `cpu` is where the CPU sees the bytes at `pa_base`.
 * The CPU address and the GPU physical address are carried as distinct
 * values; nothing here assumes they are equal. */
struct ck_gpu_window {
    uint8_t *cpu;
    uint64_t pa_base;
    uint64_t size;
};

/* Register access. Offsets are absolute BAR0 offsets (the driver writes
 * them window-relative through GPU_VREG_WR32; this code adds the window
 * base itself). A kernel would back this with
 * ck_mmio_map; the host tests back it with the simulator in gpu_sim.c. */
struct ck_gpu_regs {
    uint32_t (*rd)(void *ctx, uint32_t off);
    void (*wr)(void *ctx, uint32_t off, uint32_t val);
    void *ctx;
};

/* TLB invalidate registers as the driver programs them on GB10B
 * (kgmmuCommitTlbInvalidate_GB100 + kgmmuSetPdbToInvalidate_TU102 +
 * kgmmuCheckPendingInvalidates_TU102; gb100/dev_vm.h; window base
 * gpuGetVirtRegPhysOffset_TU102 = DRF_BASE(NV_VIRTUAL_FUNCTION_FULL_PHYS_OFFSET)). */
#define CK_GPU_VF_BASE            0x00B80000u
#define CK_GPU_REG_INVAL_PDB      (CK_GPU_VF_BASE + 0x30A0u)
#define CK_GPU_REG_INVAL_UPPER    (CK_GPU_VF_BASE + 0x30A4u)
#define CK_GPU_REG_INVAL          (CK_GPU_VF_BASE + 0x30B0u)
#define CK_GPU_INVAL_ALL_VA       (1u << 0)
#define CK_GPU_INVAL_ALL_PDB      (1u << 1)
#define CK_GPU_INVAL_SYS_MEMBAR   (1u << 6)
#define CK_GPU_INVAL_TRIGGER      (1u << 31)
#define CK_GPU_INVAL_PDB_AP_SYS   (1u << 1) /* MMU_INVALIDATE_PDB_APERTURE 1:1 SYS_MEM */
/* Fields left 0 on purpose, matching the driver's plain invalidate:
 * HUBTLB_ONLY, REPLAY (NONE), ACK (NONE_REQUIRED), CACHE_LEVEL (ALL),
 * ALL_PDB (one PDB), INVAL_SCOPE 16:15 (ALL_TLBS; GB10B sets it through
 * kgmmuSetTlbInvalidationScope_GA100), SYS_MEMBAR (set only by the Turing
 * membar workaround; a stub on GB10B: g_kern_gmmu_nvoc.c). The PDB aperture for GB10 unified memory is taken as
 * SYS_MEM: UNKNOWN on the chip. */

enum ck_gmmu_status {
    CK_GMMU_OK = 0,
    CK_GMMU_EINVAL,     /* bad argument: size, page size, flags */
    CK_GMMU_EALIGN,     /* VA, PA or size not aligned to the page size */
    CK_GMMU_ERANGE,     /* VA beyond 57 bits, or PA outside the window */
    CK_GMMU_EBUSY,      /* overlaps an existing mapping or a different page size */
    CK_GMMU_ENOMEM,     /* page-table pool exhausted */
    CK_GMMU_ENOENT,     /* unmap of a range that is not fully mapped */
    CK_GMMU_ETIMEDOUT,  /* invalidate never completed */
    CK_GMMU_EDEAD,      /* the address space latched a failure; refuses everything */
};

#define CK_GMMU_MAX_PT 64u /* page-table pages per address space (bounded) */

struct ck_gmmu_as {
    const struct ck_gpu_window *win;
    uint64_t root_pa;               /* PD4 page */
    uint64_t pt_pa[CK_GMMU_MAX_PT]; /* page-table pool, carved by the caller */
    uint8_t pt_used[CK_GMMU_MAX_PT];
    uint32_t pt_count;
    uint32_t dead;                  /* latched after an invalidate timeout */
};

/* pool_pa: CK_GMMU_MAX_PT * 4 KiB inside the window, 4 KiB aligned; pages
 * are zeroed here. The first page becomes the PD4 root. */
int ck_gmmu_as_init(struct ck_gmmu_as *as, const struct ck_gpu_window *win, uint64_t pool_pa, uint32_t pool_pages);

/* Map [va, va+size) -> [pa, pa+size) with one page size (4K, 64K or 2M).
 * Refuses: zero size, wrap, misalignment, VA >= 2^57, PA outside the window,
 * any page already mapped (any size), a 2 MiB page over an existing page
 * table, small/big pages under an existing 2 MiB PTE, pool exhaustion.
 * All-or-nothing: on refusal no entry is left changed. */
int ck_gmmu_map(struct ck_gmmu_as *as, uint64_t va, uint64_t pa, uint64_t size, uint64_t page, uint32_t aperture, uint32_t pcf);
/* Unmap a range mapped with `page`; refuses (no change) unless every page
 * in it is mapped with that page size. Caller must invalidate after. */
int ck_gmmu_unmap(struct ck_gmmu_as *as, uint64_t va, uint64_t size, uint64_t page);

/* Invalidate this address space's TLB entries (ALL_VA, one PDB),
 * then poll TRIGGER until clear for at most `poll_budget` reads. On timeout
 * the address space latches dead (UNKNOWN whether the GPU still uses stale
 * translations, so nothing mapped through it may be reused). */
int ck_gmmu_invalidate(struct ck_gmmu_as *as, const struct ck_gpu_regs *r, uint32_t poll_budget);

#endif
