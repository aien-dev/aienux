/* gb10_gsp_boot.h -- GB10 (GB10B) GSP boot builder, cut C host side (aienos#286, L192).
 *
 * Freestanding C that lays out, in plain memory only, everything the pinned Linux driver
 * (open-gpu-kernel-modules tag 580.173.02) hands the chip to boot GSP-RM on the GB10B through
 * the SEC2-boots-GSP-FMC path, plus the RPC messages a native kernel sends afterwards.
 * Nothing here touches a device: no MMIO, no register write, no firmware load, no DMA start.
 * The caller decides where the arena lives and what physical address it has; this file only
 * writes bytes into it and reports the addresses the chip would be given.
 *
 * Blocks, in arena order (every one 4 KiB aligned), with the driver function that fills each:
 *   GspFwWprMeta page            kgspPopulateWprMeta_GH100 + _GB10B   (gsp_fw_wpr_meta.h)
 *   libos init-argument page     kgspSetupLibosInitArgs_IMPL          (libos_init_args.h)
 *   7 log buffers + PTE prefix   kgspInitLogging / log registration
 *   GSP_ARGUMENTS_CACHED page    kgspPopulateGspRmInitArgs_IMPL       (gsp_init_args.h)
 *   RPC shared buffer (129 pg)   GspMsgQueuesInit (message_queue_cpu.c), msgqTxCreate (msgq.c)
 *   radix-3 table + ELF copy     kgspCreateRadix3
 *   GspRmBoot bootloader copy    kgspPrepareForBootstrap_GH100
 *   GSP-RM signature copy        kgspPrepareForBootstrap_GH100 (256-aligned, NV_ALIGN_UP 256)
 *   GSP-FMC image copy           ksec2SetupGspImages_GB10B
 *   GSP_FMC_BOOT_PARAMS page     kgspSetupGspFmcArgs_GH100            (gspifpub.h)
 * and, outside the arena, the single SEC2 EMEM packet (MCTP + NVDM COT header + NVDM_PAYLOAD_COT)
 * ksec2PrepareBootCommands_GB10B / ksec2SendMessage would write. The two RPCs the driver queues
 * before boot (GSP_SET_SYSTEM_INFO 72, SET_REGISTRY 73) are placed in the command ring.
 *
 * docs/GB10_FIRMWARE_INVENTORY.md sections 10-14 cite the source line for every value. The hosted
 * tools native/kernel/tools/ck_gsp_*_encode.c are the earlier per-block encoders; this file is
 * the composed, freestanding form the kernel can link. */
#ifndef GB10_GSP_BOOT_H
#define GB10_GSP_BOOT_H

#include <stddef.h>
#include <stdint.h>

/* ---- driver constants (580.173.02) ---- */
#define GSP_PAGE                   4096u
#define GSP_WPR_META_MAGIC         0xdc3aae21371a60b3ULL
#define GSP_WPR_META_REVISION      1ULL
#define GSP_WPR_META_SIZE          256u
#define GSP_GB10B_BOOTLOADER_SIZE  196608u
#define GSP_GB10B_BL_CODE_OFF      0xb000u
#define GSP_GB10B_BL_DATA_OFF      0xa00u
#define GSP_GB10B_BL_MANIFEST_OFF  0x0u
#define GSP_GB10B_FW_HEAP_SIZE     (133ULL << 20)
#define GSP_GB10B_LOG_COUNT        7u
#define GSP_LIBOS_ENTRY_SIZE       32u
#define GSP_LIBOS_KIND_CONTIGUOUS  1u
#define GSP_LIBOS_LOC_SYSMEM       1u
#define GSP_ARGS_CACHED_SIZE       88u
#define GSP_MQ_QUEUE_SIZE          0x40000u
#define GSP_MQ_MSG_SIZE            4096u
#define GSP_MQ_NUM_PTES            129u
#define GSP_MQ_SHARED_SIZE         (GSP_MQ_NUM_PTES * GSP_PAGE)
#define GSP_MQ_CMDQ_OFF            GSP_PAGE
#define GSP_MQ_STATQ_OFF           (GSP_PAGE + GSP_MQ_QUEUE_SIZE)
#define GSP_MQ_RXHDR_OFF           32u
#define GSP_MQ_ENTRY_OFF           4096u
#define GSP_MQ_MSG_COUNT           63u
#define GSP_MQ_FLAGS_SWAP_RX       1u
#define GSP_ELEM_HDR_SIZE          48u
#define GSP_ELEM_MIN_LEN           80u          /* sizeof(GSP_MSG_QUEUE_ELEMENT) */
#define GSP_ELEM_SIZE_MAX          65536u
#define GSP_RPC_HDR_SIZE           32u
#define GSP_RPC_SIZE_MAX           (GSP_ELEM_SIZE_MAX - GSP_ELEM_HDR_SIZE)
#define GSP_RPC_HEADER_VERSION     0x03000000u
#define GSP_RPC_SIGNATURE          0x43505256u  /* "VRPC" */
#define GSP_RPC_PENDING            0xFFFFFFFFu
#define GSP_SYSTEM_INFO_SIZE       936u
#define GSP_FMC_PARAMS_SIZE        88u
#define GSP_COT_PAYLOAD_SIZE       860u
#define GSP_COT_VERSION            1u
#define GSP_COT_HASH_SIZE          48u
#define GSP_COT_KEY_SIZE           384u
#define GSP_COT_SIG_SIZE           384u
#define GSP_EMEM_CHANNEL_SIZE      1024u
#define GSP_DMA_TARGET_COHERENT    1u           /* GSP_DMA_TARGET_COHERENT_SYSTEM: cached sysmem */
#define GSP_DMA_TARGET_NONCOHERENT 2u           /* GSP_DMA_TARGET_NONCOHERENT_SYSTEM: uncached */

/* RPC function numbers (rpc_global_enums.h) and the GSP events a native kernel reads back. */
#define GSP_FN_FREE                10u
#define GSP_FN_GET_GSP_STATIC_INFO 65u
#define GSP_FN_CONTINUATION        71u
#define GSP_FN_SET_SYSTEM_INFO     72u
#define GSP_FN_SET_REGISTRY        73u
#define GSP_FN_GSP_RM_CONTROL      76u
#define GSP_FN_GSP_RM_ALLOC        103u
#define GSP_EV_INIT_DONE           0x1001u
#define GSP_EV_RUN_CPU_SEQUENCER   0x1002u

/* Fixed parts of the post-init RPC payloads (g_rpc-structures.h, g_sdk-structures.h). */
#define GSP_RM_ALLOC_FIXED         32u          /* rpc_gsp_rm_alloc_v03_00 before params[] */
#define GSP_RM_CONTROL_FIXED       40u          /* rpc_gsp_rm_control_v03_00 before params[] */
#define GSP_RM_FREE_SIZE           16u          /* rpc_free_v03_00 = NVOS00_PARAMETERS_v03_00 */

/* Registry value kinds (PACKED_REGISTRY_TABLE). */
#define GSP_REG_DWORD  1u
#define GSP_REG_BINARY 2u
#define GSP_REG_STRING 3u

/* ---- natural-layout mirrors (LP64, little endian) for size/offset asserts ---- */
typedef struct {
    uint64_t magic, revision;
    uint64_t sysmemAddrOfRadix3Elf, sizeOfRadix3Elf;
    uint64_t sysmemAddrOfBootloader, sizeOfBootloader;
    uint64_t bootloaderCodeOffset, bootloaderDataOffset, bootloaderManifestOffset;
    uint64_t sysmemAddrOfSignature, sizeOfSignature;
    uint64_t gspFwRsvdStart, nonWprHeapOffset, nonWprHeapSize, gspFwWprStart;
    uint64_t gspFwHeapOffset, gspFwHeapSize, gspFwOffset, bootBinOffset;
    uint64_t frtsOffset, frtsSize, gspFwWprEnd, fbSize;
    uint64_t vgaWorkspaceOffset, vgaWorkspaceSize, bootCount;
    uint64_t partitionRpcAddr;
    uint16_t partitionRpcRequestOffset, partitionRpcReplyOffset;
    uint32_t elfCodeOffset, elfDataOffset, elfCodeSize, elfDataSize, lsUcodeVersion;
    uint8_t  gspFwHeapVfPartitionCount, flags, padding[2];
    uint32_t pmuReservedSize;
    uint64_t verified;
} gsp_mirror_wpr_meta;

typedef struct { uint64_t id8, pa, size; uint8_t kind, loc; } gsp_mirror_libos_arg;

typedef struct {
    struct { uint64_t sharedMemPhysAddr; uint32_t pageTableEntryCount; uint64_t cmdQueueOffset, statQueueOffset; } mq;
    struct { uint32_t oldLevel, flags; uint8_t bInPMTransition; } sr;
    uint32_t gpuInstance;
    uint8_t  bDmemStack;
    struct { uint64_t pa, size; } profilerArgs;
    struct { uint64_t pa, size; } sysmemHeapArgs;
} gsp_mirror_args_cached;

typedef struct { uint32_t version, size, msgSize, msgCount, writePtr, flags, rxHdrOff, entryOff; } gsp_mirror_msgq_tx;
typedef struct { uint32_t header_version, signature, length, function, rpc_result, rpc_result_private, sequence, cpuRmGfid; } gsp_mirror_rpc_hdr;
typedef struct { uint8_t authTag[16], aad[16]; uint32_t checkSum, seqNum, elemCount; _Alignas(8) gsp_mirror_rpc_hdr rpc; } gsp_mirror_elem;
typedef struct { uint32_t hClient, hParent, hObject, hClass, status, paramsSize, flags; uint8_t reserved[4]; } gsp_mirror_rm_alloc;
typedef struct { uint32_t hClient, hObject, cmd, status, paramsSize, rmapiRpcFlags, rmctrlFlags, rmctrlAccessRight; _Alignas(8) uint64_t reserved0; } gsp_mirror_rm_control;
typedef struct { uint32_t hRoot, hObjectParent, hObjectOld, status; } gsp_mirror_rm_free;
typedef struct { uint32_t nameOffset; uint8_t type; uint32_t data, length; } gsp_mirror_reg_entry;

typedef struct {
    uint32_t regkeys;
    struct { uint32_t target, gspRmDescSize; uint64_t gspRmDescOffset, wprCarveoutOffset; uint32_t wprCarveoutSize; uint8_t bIsGspRmBoot; } boot;
    struct { uint32_t target; uint64_t bootArgsOffset; } rm;
    struct { uint32_t target; uint64_t payloadBufferOffset; uint32_t payloadBufferSize; } spdm;
    struct { uint32_t flushSysmemAddrValLo, flushSysmemAddrValHi; } mem;
} gsp_mirror_fmc_params;

typedef struct __attribute__((packed)) {
    uint16_t version, size;
    uint64_t gspFmcSysmemOffset, frtsSysmemOffset;
    uint32_t frtsSysmemSize;
    uint64_t frtsVidmemOffset;
    uint32_t frtsVidmemSize;
    uint8_t  hash384[48], publicKey[384], signature[384];
    uint64_t gspBootArgsSysmemOffset;
} gsp_mirror_cot;

/* ---- the arena: zeroed host memory whose first byte has physical address pa ---- */
typedef struct {
    uint8_t *base;
    uint64_t pa;
    uint64_t cap;
    uint64_t used;
} gsp_arena;

/* Fails (returns -1) on a NULL base, a base or pa that is not 4 KiB aligned, or pa + cap wrapping. */
int gsp_arena_init(gsp_arena *a, void *base, uint64_t pa, uint64_t cap);

typedef struct {
    const char    *name;     /* NUL-terminated, non-empty */
    uint8_t        type;     /* GSP_REG_DWORD / _BINARY / _STRING */
    uint32_t       dword;    /* DWORD value */
    const uint8_t *bytes;    /* BINARY / STRING value (STRING includes its NUL) */
    uint32_t       len;
} gsp_reg_entry;

typedef struct {
    /* GSP-RM firmware: the .fwimage section and this chip's signature section (container parsed
     * elsewhere, ck_gsp_fw_inspect). Copied into the arena; never modified. */
    const uint8_t *rm_elf;      uint64_t rm_elf_size;
    const uint8_t *rm_sig;      uint32_t rm_sig_size;
    /* GspRmBoot_GB10B image and its RM_RISCV_UCODE_DESC offsets (ck_gsp_bindata_extract). */
    const uint8_t *bootloader;  uint32_t bootloader_size;
    uint32_t bl_code_off, bl_data_off, bl_manifest_off;
    /* GspRmFmcGfwProdSigned_GB10B: image, hash 48, public key 384, signature 384. */
    const uint8_t *fmc;         uint32_t fmc_size;
    const uint8_t *fmc_hash, *fmc_pubkey, *fmc_sig;
    /* ZERO_FB sysmem heap (512 MiB on the driver), contiguous uncached; caller's carve-out. */
    uint64_t sysmem_heap_pa, sysmem_heap_size;
    /* GspSystemInfo bytes (936): content comes from the real PCI/BAR/ACPI state. */
    const uint8_t *system_info;
    /* Registry for SET_REGISTRY; nregs 0 = the driver's NV_WARN_NOTHING_TO_DO, RPC not queued. */
    const gsp_reg_entry *regs;  uint32_t nregs;
    uint32_t gpu_instance;
    uint32_t fmc_regkeys;       /* GSP_FMC_BOOT_PARAMS.initParams.regkeys (0 without CC/NVLink) */
    unsigned dma_bits;          /* every chip-visible address must be below 1 << dma_bits */
} gsp_boot_in;

typedef struct {
    uint64_t pa, size;
} gsp_region;

typedef struct {
    gsp_region wpr_meta, libos_args, log[GSP_GB10B_LOG_COUNT], args_cached;
    gsp_region rpc_shared;          /* page-table page + command queue + status queue */
    uint64_t   cmdq_pa, statq_pa;
    gsp_region radix3;              /* levels 0..2 then the ELF data pages */
    uint64_t   radix3_root_pa, elf_data_pa;
    uint32_t   radix3_level_pages[4];
    gsp_region bootloader, signature, fmc, fmc_params;
    /* command ring state after the pre-boot RPCs (the driver's txSeqNum / writePtr) */
    uint32_t   cmdq_write_ptr, tx_seq;
    /* the SEC2 EMEM packet: header dword 0 (MCTP), dword 1 (NVDM COT), then the COT payload */
    uint8_t    cot_packet[GSP_EMEM_CHANNEL_SIZE];
    uint32_t   cot_packet_len;      /* bytes written at EMEM offset 0 */
    uint32_t   emem_queue_tail;     /* QUEUE_TAIL value, QUEUE_HEAD = 0 */
} gsp_boot_plan;

/* Builds every block into the arena and the COT packet into *out. Returns 0, or -1 with *why set
 * to a fixed reason string (the test oracle). On failure the arena may hold partial bytes. */
int gsp_boot_build(gsp_arena *a, const gsp_boot_in *in, gsp_boot_plan *out, const char **why);

/* ---- message queue over the RPC shared buffer (post-boot RPCs and status reads) ---- */
typedef struct {
    uint8_t *cmdq;        /* host view of the command queue (producer: CPU) */
    uint8_t *statq;       /* host view of the status queue (producer: GSP) */
    uint32_t tx_seq;      /* next element seqNum on the command queue */
    uint32_t rx_seq;      /* next expected seqNum on the status queue */
    uint32_t rpc_seq;     /* next header sequence for synchronous RPCs */
} gsp_mq;

/* Attach to the queues of a built plan (arena must be the one passed to gsp_boot_build). */
int gsp_mq_attach(gsp_mq *q, const gsp_arena *a, const gsp_boot_plan *plan);

/* One piece of an RPC payload; p NULL means len zero bytes. */
typedef struct { const void *p; uint32_t len; } gsp_seg;
#define GSP_REG_MAX_ENTRIES 32u     /* builder bound: keeps the registry staging on a small stack */

/* Queue one message, streamed into the ring slot by slot (it may wrap the ring end, as
 * msgqTxGetWriteBuffer allows). The payload is the segments back to back. rpc_len is the header
 * length field minus 32 (what the driver passes to rpcWriteCommonHeader); the segments may be
 * longer than rpc_len (the driver's GSP_RM_ALLOC leaves params outside the length) but must fit
 * the elements the length covers. sync != 0 takes the next RPC sequence, 0 sends sequence 0 (the
 * driver's async RPCs). Refuses a full ring. */
int gsp_mq_send(gsp_mq *q, uint32_t function, const gsp_seg *segs, uint32_t nsegs, uint32_t rpc_len,
                int sync, uint32_t *sequence_out, const char **why);

/* Post-init RPCs, byte-for-byte as rpcRmApiAlloc_GSP / rpcRmApiControl_GSP / rpcRmApiFree_GSP /
 * rpcGetGspStaticInfo_v14_00 (rpc.c) build them; all synchronous. */
int gsp_rpc_rm_alloc(gsp_mq *q, uint32_t hClient, uint32_t hParent, uint32_t hObject, uint32_t hClass,
                     const void *params, uint32_t params_size, uint32_t *seq, const char **why);
int gsp_rpc_rm_control(gsp_mq *q, uint32_t hClient, uint32_t hObject, uint32_t cmd,
                       const void *params, uint32_t params_size, uint32_t *seq, const char **why);
int gsp_rpc_rm_free(gsp_mq *q, uint32_t hClient, uint32_t hObject, uint32_t *seq, const char **why);
int gsp_rpc_get_static_info(gsp_mq *q, uint32_t static_info_size, uint32_t *seq, const char **why);

/* Read one status-queue message if one is waiting: checks writePtr bounds, elemCount, the zero
 * checksum over 48 + rpc.length and seqNum == rx_seq, then copies 48 + rpc.length bytes to out
 * and advances the CPU's readPtr (kept in the command queue at rxHdrOff, SWAP_RX). Returns 1 on a
 * message, 0 when the queue is empty, -1 with *why on a corrupt queue or message. */
int gsp_mq_recv(gsp_mq *q, uint8_t *out, uint32_t cap, uint32_t *function, uint32_t *rpc_result,
                const char **why);

/* Shared helpers, also used by the tests. */
uint32_t gsp_checksum32(const uint8_t *p, uint32_t len);
uint64_t gsp_init_arg_id(const char *name);
uint32_t gsp_reg_table_size(const gsp_reg_entry *e, uint32_t n);

#endif
