/* test_gb10_gsp_nvhdr -- compares the builder's mirror structs (dev/gb10_gsp_boot.h) with the
 * NVIDIA headers themselves, field by field, and decodes a built plan through the NVIDIA types.
 * Needs open-gpu-kernel-modules at tag 580.173.02 (MIT) on the include path: the Makefile target
 * gb10-gsp-nvhdr-test passes OGKM_DIR (default ~/workspace/_ext/open-gpu-kernel-modules-580.173.02)
 * and fails, never skips, when the tree is missing. Host only; nothing NVIDIA is linked or shipped.
 *
 * Covered from the real headers: GspFwWprMeta, LibosMemoryRegionInitArgument,
 * GSP_ARGUMENTS_CACHED (+ MESSAGE_QUEUE_INIT_ARGUMENTS, GSP_SR_INIT_ARGUMENTS), msgqTxHeader,
 * msgqRxHeader, rpc_message_header_v03_00, rpc_gsp_rm_alloc_v03_00, rpc_gsp_rm_control_v03_00,
 * rpc_free_v03_00, GSP_FMC_BOOT_PARAMS, NVDM_PAYLOAD_COT, the function and event numbers.
 * GSP_MSG_QUEUE_ELEMENT (message_queue_priv.h) pulls RM-internal headers (nvassert.h needs an
 * NV_PORT_HEADER); its five fields are re-declared here verbatim around NVIDIA's own
 * rpc_message_header_v. PACKED_REGISTRY_TABLE (g_os_nvoc.h) and GspSystemInfo /
 * GspStaticConfigInfo (gsp_static_config.h) are in the same position and stay checked by the
 * hosted tool ck_gsp_rpc_encode (inventory section 13). */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "nvtypes.h"
#include "nvstatus.h"
#include "gsp/gsp_fw_wpr_meta.h"
#include "libos_init_args.h"
#include "gpu/gsp/gsp_init_args.h"
#include "msgq/msgq_priv.h"
#include "gsp/gspifpub.h"
#include "fsp/fsp_nvdm_format.h"
#include "gpu/fsp/kern_fsp_cot_payload.h"
#include "vgpu/rpc_headers.h"
#define RPC_STRUCTURES
#define RPC_GENERIC_UNION
#include "g_rpc-structures.h"
#define RPC_MESSAGE_STRUCTURES
#include "g_rpc-message-header.h"
#include "ck_test.h"
#include "gb10_gsp_boot.h"

typedef struct {                                   /* message_queue_priv.h:43-51, verbatim fields */
    NvU8  authTagBuffer[16];
    NvU8  aadBuffer[16];
    NvU32 checkSum;
    NvU32 seqNum;
    NvU32 elemCount;
    NV_DECLARE_ALIGNED(rpc_message_header_v rpc, 8);
} NV_GSP_MSG_QUEUE_ELEMENT;

#define SAME(M, N, fm, fn) CHECK(offsetof(M, fm) == offsetof(N, fn) && sizeof(((M *)0)->fm) == sizeof(((N *)0)->fn))

static void test_layouts(void)
{
    CHECK(sizeof(gsp_mirror_wpr_meta) == sizeof(GspFwWprMeta));
#define W(f) SAME(gsp_mirror_wpr_meta, GspFwWprMeta, f, f)
    W(magic); W(revision); W(sysmemAddrOfRadix3Elf); W(sizeOfRadix3Elf); W(sysmemAddrOfBootloader); W(sizeOfBootloader);
    W(bootloaderCodeOffset); W(bootloaderDataOffset); W(bootloaderManifestOffset); W(sysmemAddrOfSignature); W(sizeOfSignature);
    W(gspFwRsvdStart); W(nonWprHeapOffset); W(nonWprHeapSize); W(gspFwWprStart); W(gspFwHeapOffset); W(gspFwHeapSize);
    W(gspFwOffset); W(bootBinOffset); W(frtsOffset); W(frtsSize); W(gspFwWprEnd); W(fbSize); W(vgaWorkspaceOffset);
    W(vgaWorkspaceSize); W(bootCount); W(partitionRpcAddr); W(partitionRpcRequestOffset); W(partitionRpcReplyOffset);
    W(elfCodeOffset); W(elfDataOffset); W(elfCodeSize); W(elfDataSize); W(lsUcodeVersion); W(gspFwHeapVfPartitionCount);
    W(flags); W(padding); W(pmuReservedSize); W(verified);
    CHECK(GSP_WPR_META_MAGIC == GSP_FW_WPR_META_MAGIC && GSP_WPR_META_REVISION == GSP_FW_WPR_META_REVISION);

    CHECK(sizeof(gsp_mirror_libos_arg) == sizeof(LibosMemoryRegionInitArgument));
#define L(f) SAME(gsp_mirror_libos_arg, LibosMemoryRegionInitArgument, f, f)
    L(id8); L(pa); L(size); L(kind); L(loc);
    CHECK(GSP_LIBOS_KIND_CONTIGUOUS == LIBOS_MEMORY_REGION_CONTIGUOUS && GSP_LIBOS_LOC_SYSMEM == LIBOS_MEMORY_REGION_LOC_SYSMEM);

    CHECK(sizeof(gsp_mirror_args_cached) == sizeof(GSP_ARGUMENTS_CACHED));
#define A(fm, fn) SAME(gsp_mirror_args_cached, GSP_ARGUMENTS_CACHED, fm, fn)
    A(mq.sharedMemPhysAddr, messageQueueInitArguments.sharedMemPhysAddr);
    A(mq.pageTableEntryCount, messageQueueInitArguments.pageTableEntryCount);
    A(mq.cmdQueueOffset, messageQueueInitArguments.cmdQueueOffset);
    A(mq.statQueueOffset, messageQueueInitArguments.statQueueOffset);
    A(sr.oldLevel, srInitArguments.oldLevel); A(sr.flags, srInitArguments.flags);
    A(sr.bInPMTransition, srInitArguments.bInPMTransition);
    A(gpuInstance, gpuInstance); A(bDmemStack, bDmemStack);
    A(profilerArgs.pa, profilerArgs.pa); A(profilerArgs.size, profilerArgs.size);
    A(sysmemHeapArgs.pa, sysmemHeapArgs.pa); A(sysmemHeapArgs.size, sysmemHeapArgs.size);

    CHECK(sizeof(gsp_mirror_msgq_tx) == sizeof(msgqTxHeader) && sizeof(msgqRxHeader) == 4);
#define T(f) SAME(gsp_mirror_msgq_tx, msgqTxHeader, f, f)
    T(version); T(size); T(msgSize); T(msgCount); T(writePtr); T(flags); T(rxHdrOff); T(entryOff);
    CHECK(GSP_MQ_FLAGS_SWAP_RX == MSGQ_FLAGS_SWAP_RX && MSGQ_VERSION == 0);

    CHECK(sizeof(gsp_mirror_rpc_hdr) == sizeof(rpc_message_header_v03_00));
#define R(f) SAME(gsp_mirror_rpc_hdr, rpc_message_header_v03_00, f, f)
    R(header_version); R(signature); R(length); R(function); R(rpc_result); R(rpc_result_private); R(sequence);
    SAME(gsp_mirror_rpc_hdr, rpc_message_header_v03_00, cpuRmGfid, u.cpuRmGfid);
    CHECK(GSP_RPC_SIGNATURE == NV_VGPU_MSG_SIGNATURE_VALID && GSP_RPC_PENDING == NV_VGPU_MSG_RESULT_RPC_PENDING);
    CHECK(GSP_RPC_HEADER_VERSION == ((NV_VGPU_MSG_HEADER_VERSION_MAJOR_TOT << 24) | (NV_VGPU_MSG_HEADER_VERSION_MINOR_TOT << 16)));

    CHECK(sizeof(gsp_mirror_elem) == sizeof(NV_GSP_MSG_QUEUE_ELEMENT));
    SAME(gsp_mirror_elem, NV_GSP_MSG_QUEUE_ELEMENT, checkSum, checkSum);
    SAME(gsp_mirror_elem, NV_GSP_MSG_QUEUE_ELEMENT, seqNum, seqNum);
    SAME(gsp_mirror_elem, NV_GSP_MSG_QUEUE_ELEMENT, elemCount, elemCount);
    SAME(gsp_mirror_elem, NV_GSP_MSG_QUEUE_ELEMENT, rpc, rpc);

#define RA(f) SAME(gsp_mirror_rm_alloc, rpc_gsp_rm_alloc_v03_00, f, f)
    RA(hClient); RA(hParent); RA(hObject); RA(hClass); RA(status); RA(paramsSize); RA(flags); RA(reserved);
    CHECK(offsetof(rpc_gsp_rm_alloc_v03_00, params) == GSP_RM_ALLOC_FIXED);
#define RC(f) SAME(gsp_mirror_rm_control, rpc_gsp_rm_control_v03_00, f, f)
    RC(hClient); RC(hObject); RC(cmd); RC(status); RC(paramsSize); RC(rmapiRpcFlags); RC(rmctrlFlags);
    RC(rmctrlAccessRight); RC(reserved0);
    CHECK(offsetof(rpc_gsp_rm_control_v03_00, params) == GSP_RM_CONTROL_FIXED);
    CHECK(sizeof(rpc_free_v03_00) == GSP_RM_FREE_SIZE);
#define RF(f) SAME(gsp_mirror_rm_free, NVOS00_PARAMETERS_v03_00, f, f)
    RF(hRoot); RF(hObjectParent); RF(hObjectOld); RF(status);

    CHECK(GSP_FN_FREE == NV_VGPU_MSG_FUNCTION_FREE && GSP_FN_GET_GSP_STATIC_INFO == NV_VGPU_MSG_FUNCTION_GET_GSP_STATIC_INFO &&
          GSP_FN_CONTINUATION == NV_VGPU_MSG_FUNCTION_CONTINUATION_RECORD && GSP_FN_SET_SYSTEM_INFO == NV_VGPU_MSG_FUNCTION_GSP_SET_SYSTEM_INFO &&
          GSP_FN_SET_REGISTRY == NV_VGPU_MSG_FUNCTION_SET_REGISTRY && GSP_FN_GSP_RM_CONTROL == NV_VGPU_MSG_FUNCTION_GSP_RM_CONTROL &&
          GSP_FN_GSP_RM_ALLOC == NV_VGPU_MSG_FUNCTION_GSP_RM_ALLOC);
    CHECK(GSP_EV_INIT_DONE == NV_VGPU_MSG_EVENT_GSP_INIT_DONE && GSP_EV_RUN_CPU_SEQUENCER == NV_VGPU_MSG_EVENT_GSP_RUN_CPU_SEQUENCER);

    CHECK(sizeof(gsp_mirror_fmc_params) == sizeof(GSP_FMC_BOOT_PARAMS));
#define F(fm, fn) SAME(gsp_mirror_fmc_params, GSP_FMC_BOOT_PARAMS, fm, fn)
    F(regkeys, initParams.regkeys);
    F(boot.target, bootGspRmParams.target); F(boot.gspRmDescSize, bootGspRmParams.gspRmDescSize);
    F(boot.gspRmDescOffset, bootGspRmParams.gspRmDescOffset); F(boot.wprCarveoutOffset, bootGspRmParams.wprCarveoutOffset);
    F(boot.wprCarveoutSize, bootGspRmParams.wprCarveoutSize); F(boot.bIsGspRmBoot, bootGspRmParams.bIsGspRmBoot);
    F(rm.target, gspRmParams.target); F(rm.bootArgsOffset, gspRmParams.bootArgsOffset);
    F(spdm.target, gspSpdmParams.target); F(spdm.payloadBufferOffset, gspSpdmParams.payloadBufferOffset);
    F(spdm.payloadBufferSize, gspSpdmParams.payloadBufferSize);
    F(mem.flushSysmemAddrValLo, gspRmMemParams.flushSysmemAddrValLo);
    F(mem.flushSysmemAddrValHi, gspRmMemParams.flushSysmemAddrValHi);
    CHECK(GSP_DMA_TARGET_COHERENT == GSP_DMA_TARGET_COHERENT_SYSTEM && GSP_DMA_TARGET_NONCOHERENT == GSP_DMA_TARGET_NONCOHERENT_SYSTEM);

    CHECK(sizeof(gsp_mirror_cot) == sizeof(NVDM_PAYLOAD_COT));
#define C(f) SAME(gsp_mirror_cot, NVDM_PAYLOAD_COT, f, f)
    C(version); C(size); C(gspFmcSysmemOffset); C(frtsSysmemOffset); C(frtsSysmemSize); C(frtsVidmemOffset);
    C(frtsVidmemSize); C(hash384); C(publicKey); C(signature); C(gspBootArgsSysmemOffset);
    CHECK(NVDM_TYPE_COT == 0x14);
}

/* Build a plan with synthetic bytes and read it back through the NVIDIA types. */
static void test_decode(void)
{
    enum { CAP = 4u << 20, ELF = 300000 };
    uint8_t *mem = aligned_alloc(4096, CAP), *elf = calloc(1, ELF), *bl = calloc(1, GSP_GB10B_BOOTLOADER_SIZE);
    static uint8_t sig[512], fmc[9000], h[48], k[384], s[384], si[GSP_SYSTEM_INFO_SIZE];
    gsp_boot_in in = { .rm_elf = elf, .rm_elf_size = ELF, .rm_sig = sig, .rm_sig_size = 300,
        .bootloader = bl, .bootloader_size = GSP_GB10B_BOOTLOADER_SIZE, .bl_code_off = 0xb000, .bl_data_off = 0xa00,
        .fmc = fmc, .fmc_size = sizeof fmc, .fmc_hash = h, .fmc_pubkey = k, .fmc_sig = s,
        .sysmem_heap_pa = 0x40000000ULL, .sysmem_heap_size = 512u << 20, .system_info = si, .dma_bits = 41 };
    gsp_arena a; gsp_boot_plan p; const char *why = 0;
    CHECK(gsp_arena_init(&a, mem, 0x10000000ULL, CAP) == 0 && gsp_boot_build(&a, &in, &p, &why) == 0);
#define AT(pa) (mem + ((pa) - 0x10000000ULL))
    GspFwWprMeta w; memcpy(&w, AT(p.wpr_meta.pa), sizeof w);
    CHECK(w.magic == GSP_FW_WPR_META_MAGIC && w.revision == GSP_FW_WPR_META_REVISION);
    CHECK(w.sysmemAddrOfRadix3Elf == p.radix3_root_pa && w.sizeOfRadix3Elf == ELF);
    CHECK(w.sysmemAddrOfBootloader == p.bootloader.pa && w.sizeOfBootloader == GSP_GB10B_BOOTLOADER_SIZE);
    CHECK(w.sysmemAddrOfSignature == p.signature.pa && w.sizeOfSignature == 512 && w.gspFwHeapSize == (133ULL << 20));
    CHECK(w.frtsSize == 0 && w.nonWprHeapSize == 0 && w.vgaWorkspaceSize == 0 && w.pmuReservedSize == 0 && w.verified == 0);
    LibosMemoryRegionInitArgument la[8]; memcpy(la, AT(p.libos_args.pa), sizeof la);
    CHECK(la[7].pa == p.args_cached.pa && la[7].size == 4096 && la[7].kind == LIBOS_MEMORY_REGION_CONTIGUOUS);
    CHECK(la[5].size == 0x1000 && la[0].loc == LIBOS_MEMORY_REGION_LOC_SYSMEM);
    GSP_ARGUMENTS_CACHED g; memcpy(&g, AT(p.args_cached.pa), sizeof g);
    CHECK(g.messageQueueInitArguments.sharedMemPhysAddr == p.rpc_shared.pa && g.messageQueueInitArguments.pageTableEntryCount == 129);
    CHECK(g.messageQueueInitArguments.cmdQueueOffset == 4096 && g.messageQueueInitArguments.statQueueOffset == 4096 + 0x40000);
    CHECK(g.bDmemStack == NV_TRUE && g.sysmemHeapArgs.pa == 0x40000000ULL && g.sysmemHeapArgs.size == (512ULL << 20));
    msgqTxHeader tx; memcpy(&tx, AT(p.cmdq_pa), sizeof tx);
    CHECK(tx.version == 0 && tx.size == 0x40000 && tx.msgSize == 4096 && tx.msgCount == 63 && tx.writePtr == 1);
    CHECK(tx.flags == MSGQ_FLAGS_SWAP_RX && tx.rxHdrOff == 32 && tx.entryOff == 4096);
    NV_GSP_MSG_QUEUE_ELEMENT el; memcpy(&el, AT(p.cmdq_pa) + 4096, sizeof el);
    CHECK(el.seqNum == 0 && el.elemCount == 1 && el.rpc.function == NV_VGPU_MSG_FUNCTION_GSP_SET_SYSTEM_INFO);
    CHECK(el.rpc.length == sizeof(rpc_message_header_v) + GSP_SYSTEM_INFO_SIZE && el.rpc.signature == NV_VGPU_MSG_SIGNATURE_VALID);
    GSP_FMC_BOOT_PARAMS f; memcpy(&f, AT(p.fmc_params.pa), sizeof f);
    CHECK(f.bootGspRmParams.target == GSP_DMA_TARGET_COHERENT_SYSTEM && f.bootGspRmParams.gspRmDescSize == sizeof(GspFwWprMeta));
    CHECK(f.bootGspRmParams.gspRmDescOffset == p.wpr_meta.pa && f.bootGspRmParams.bIsGspRmBoot == NV_TRUE);
    CHECK(f.gspRmParams.target == GSP_DMA_TARGET_NONCOHERENT_SYSTEM && f.gspRmParams.bootArgsOffset == p.libos_args.pa);
    NVDM_PAYLOAD_COT c; memcpy(&c, p.cot_packet + 8, sizeof c);
    CHECK(c.version == 1 && c.size == sizeof(NVDM_PAYLOAD_COT) && c.gspFmcSysmemOffset == p.fmc.pa);
    CHECK(c.frtsSysmemSize == sizeof fmc && c.gspBootArgsSysmemOffset == p.fmc_params.pa);

    gsp_mq q;
    CHECK(gsp_mq_attach(&q, &a, &p) == 0);
    uint32_t prm[4] = {1, 2, 3, 4}, seq;
    CHECK(gsp_rpc_rm_alloc(&q, 0xc1d00001, 0xc1d00001, 0xcaf00010, 0x0080, prm, sizeof prm, &seq, &why) == 0);
    uint8_t raw[4096]; memcpy(raw, AT(p.cmdq_pa) + 4096 + 4096, sizeof raw);
    NV_GSP_MSG_QUEUE_ELEMENT *e = (NV_GSP_MSG_QUEUE_ELEMENT *)raw;
    rpc_gsp_rm_alloc_v03_00 *ra = (rpc_gsp_rm_alloc_v03_00 *)(raw + sizeof *e);
    CHECK(e->rpc.function == NV_VGPU_MSG_FUNCTION_GSP_RM_ALLOC && e->rpc.length == sizeof(rpc_message_header_v) + sizeof(rpc_gsp_rm_alloc_v03_00));
    CHECK(ra->hObject == 0xcaf00010 && ra->hClass == 0x0080 && ra->paramsSize == sizeof prm && ra->flags == 0 &&
          memcmp(ra->params, prm, sizeof prm) == 0);
    free(mem); free(elf); free(bl);
}

int main(void)
{
    test_layouts();
    test_decode();
    return ck_t_verdict("test_gb10_gsp_nvhdr");
}
