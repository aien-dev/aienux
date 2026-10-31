/* gb10_gsp_boot.c -- GB10 GSP boot builder (see gb10_gsp_boot.h). Freestanding: no libc, no
 * MMIO, no device access; writes little-endian bytes into a caller-owned arena only. */
#include "gb10_gsp_boot.h"

/* TEST-ONLY mutants (make gb10-gsp-mutants): each breaks one rule the host tests must catch. */
#if (defined(GSP_MUTANT_SWAPRX) || defined(GSP_MUTANT_CKSUM) || defined(GSP_MUTANT_RADIX) || \
     defined(GSP_MUTANT_ALLOCLEN)) && !defined(GSP_MUTANT_HOST)
#error "GSP mutants are host-test builds only"
#endif

/* ---- layout asserts: every offset the builder writes is taken from these mirrors, and the
 * host test compares the mirrors with the NVIDIA headers themselves (test_gb10_gsp_nvhdr). ---- */
_Static_assert(sizeof(gsp_mirror_wpr_meta) == GSP_WPR_META_SIZE, "GspFwWprMeta is 256 bytes");
_Static_assert(offsetof(gsp_mirror_wpr_meta, sysmemAddrOfSignature) == 72, "WprMeta signature at 72");
_Static_assert(offsetof(gsp_mirror_wpr_meta, gspFwHeapSize) == 128, "WprMeta heap size at 128");
_Static_assert(offsetof(gsp_mirror_wpr_meta, gspFwHeapVfPartitionCount) == 240, "WprMeta VF count at 240");
_Static_assert(offsetof(gsp_mirror_wpr_meta, verified) == 248, "WprMeta verified at 248");
_Static_assert(sizeof(gsp_mirror_libos_arg) == GSP_LIBOS_ENTRY_SIZE, "LibosMemoryRegionInitArgument is 32");
_Static_assert(offsetof(gsp_mirror_libos_arg, kind) == 24 && offsetof(gsp_mirror_libos_arg, loc) == 25, "libos kind/loc");
_Static_assert(sizeof(gsp_mirror_args_cached) == GSP_ARGS_CACHED_SIZE, "GSP_ARGUMENTS_CACHED is 88");
_Static_assert(offsetof(gsp_mirror_args_cached, sr) == 32 && offsetof(gsp_mirror_args_cached, gpuInstance) == 44 &&
               offsetof(gsp_mirror_args_cached, bDmemStack) == 48 && offsetof(gsp_mirror_args_cached, profilerArgs) == 56 &&
               offsetof(gsp_mirror_args_cached, sysmemHeapArgs) == 72, "GSP_ARGUMENTS_CACHED offsets");
_Static_assert(sizeof(gsp_mirror_msgq_tx) == 32, "msgqTxHeader is 32");
_Static_assert(sizeof(gsp_mirror_rpc_hdr) == GSP_RPC_HDR_SIZE, "rpc_message_header_v03_00 is 32");
_Static_assert(offsetof(gsp_mirror_elem, rpc) == GSP_ELEM_HDR_SIZE && sizeof(gsp_mirror_elem) == GSP_ELEM_MIN_LEN, "GSP_MSG_QUEUE_ELEMENT");
_Static_assert(sizeof(gsp_mirror_rm_alloc) == GSP_RM_ALLOC_FIXED, "rpc_gsp_rm_alloc_v03_00 fixed part");
_Static_assert(sizeof(gsp_mirror_rm_control) == GSP_RM_CONTROL_FIXED, "rpc_gsp_rm_control_v03_00 fixed part");
_Static_assert(sizeof(gsp_mirror_rm_free) == GSP_RM_FREE_SIZE, "rpc_free_v03_00");
_Static_assert(sizeof(gsp_mirror_reg_entry) == 16, "PACKED_REGISTRY_ENTRY is 16");
_Static_assert(sizeof(gsp_mirror_fmc_params) == GSP_FMC_PARAMS_SIZE, "GSP_FMC_BOOT_PARAMS is 88");
_Static_assert(offsetof(gsp_mirror_fmc_params, boot.bIsGspRmBoot) == 36 && offsetof(gsp_mirror_fmc_params, rm) == 40 &&
               offsetof(gsp_mirror_fmc_params, spdm) == 56 && offsetof(gsp_mirror_fmc_params, mem) == 80, "FMC params offsets");
_Static_assert(sizeof(gsp_mirror_cot) == GSP_COT_PAYLOAD_SIZE, "NVDM_PAYLOAD_COT is 860 packed");
_Static_assert(offsetof(gsp_mirror_cot, gspBootArgsSysmemOffset) == 852, "COT boot args at 852");

#define OFF(T, f) ((uint32_t)offsetof(T, f))
#define WM(f)  OFF(gsp_mirror_wpr_meta, f)

/* GB10B log set, registration order (kernel_gsp.c:3150-3240): seven buffers then RMARGS. */
static const struct { const char *id; uint32_t size; } gb10b_logs[GSP_GB10B_LOG_COUNT] = {
    {"LOGINIT", 0x10000}, {"LOGINTR", 0x10000}, {"LOGRM", 0x10000}, {"LOGMNOC", 0x10000},
    {"LOGDBG", 0x10000},  {"LOGROOT", 0x1000},  {"LOGKRNL", 0x10000},
};

/* ---- byte helpers ---- */
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) { p[i] = (uint8_t)v; v >>= 8; } }
static void wr64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) { p[i] = (uint8_t)v; v >>= 8; } }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }
static void bcopy_(uint8_t *d, const uint8_t *s, uint64_t n) { for (uint64_t i = 0; i < n; i++) d[i] = s[i]; }
static void bzero_(uint8_t *d, uint64_t n) { for (uint64_t i = 0; i < n; i++) d[i] = 0; }
static uint64_t up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }
static uint32_t slen(const char *s) { uint32_t n = 0; while (s[n]) n++; return n; }

uint32_t gsp_checksum32(const uint8_t *p, uint32_t len)
{
    uint64_t x = 0;
    for (uint32_t i = 0; i < len; i += 8)
        x ^= rd64(p + i);
    return (uint32_t)(x >> 32) ^ (uint32_t)x;
}

/* _kgspGenerateInitArgId: up to 8 ASCII bytes, first byte most significant. */
uint64_t gsp_init_arg_id(const char *name)
{
    uint64_t id = 0;
    for (int i = 0; i < 8 && name[i]; i++)
        id = (id << 8) | (uint8_t)name[i];
    return id;
}

/* ---- arena ---- */
int gsp_arena_init(gsp_arena *a, void *base, uint64_t pa, uint64_t cap)
{
    if (!a || !base || ((uintptr_t)base & (GSP_PAGE - 1)) || (pa & (GSP_PAGE - 1)) || pa + cap < pa)
        return -1;
    a->base = base;
    a->pa = pa;
    a->cap = cap;
    a->used = 0;
    return 0;
}

/* Reserve size bytes, 4 KiB aligned, zeroed. Returns the host pointer and the PA. */
static uint8_t *take(gsp_arena *a, uint64_t size, uint64_t *pa)
{
    uint64_t off = up(a->used, GSP_PAGE);
    uint64_t sz = up(size, GSP_PAGE);
    if (off < a->used || sz < size || off + sz < off || off + sz > a->cap)
        return NULL;
    a->used = off + sz;
    bzero_(a->base + off, sz);
    *pa = a->pa + off;
    return a->base + off;
}

static int fits_dma(uint64_t pa, uint64_t size, unsigned bits)
{
    if (bits >= 64) return 1;
    uint64_t lim = 1ULL << bits;
    return pa < lim && size <= lim - pa;
}

/* ---- registry table (PACKED_REGISTRY_TABLE, RmPackageRegistry) ---- */
uint32_t gsp_reg_table_size(const gsp_reg_entry *e, uint32_t n)
{
    if (n == 0 || n > GSP_REG_MAX_ENTRIES) return 0;
    uint64_t size = 8 + 16ULL * n;
    for (uint32_t i = 0; i < n; i++) {
        if (!e[i].name || !e[i].name[0]) return 0;
        if (e[i].type == GSP_REG_DWORD) { if (e[i].len) return 0; }
        else if (e[i].type == GSP_REG_BINARY || e[i].type == GSP_REG_STRING) { if (!e[i].len || !e[i].bytes) return 0; }
        else return 0;
        size += slen(e[i].name) + 1ULL + e[i].len;
    }
    return size > GSP_RPC_SIZE_MAX - GSP_RPC_HDR_SIZE ? 0 : (uint32_t)size;
}

/* Writes the table header and entry array into head (8 + 16 n bytes) and describes the whole
 * table as segments: head, then per entry its NUL-terminated name and, for BINARY / STRING, the
 * value bytes (the driver's string area order). Returns the segment count. */
static uint32_t reg_write(const gsp_reg_entry *e, uint32_t n, uint32_t size, uint8_t *head, gsp_seg *segs)
{
    uint32_t ns = 0;
    bzero_(head, 8 + 16 * n);
    wr32(head, size);
    wr32(head + 4, n);
    segs[ns++] = (gsp_seg){ head, 8 + 16 * n };
    uint32_t off = 8 + 16 * n;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t *ent = head + 8 + 16 * i;
        uint32_t nl = slen(e[i].name) + 1;
        wr32(ent + OFF(gsp_mirror_reg_entry, nameOffset), off);
        segs[ns++] = (gsp_seg){ e[i].name, nl };
        off += nl;
        ent[OFF(gsp_mirror_reg_entry, type)] = e[i].type;
        if (e[i].type == GSP_REG_DWORD) {
            wr32(ent + OFF(gsp_mirror_reg_entry, data), e[i].dword);
            wr32(ent + OFF(gsp_mirror_reg_entry, length), 4);
        } else {
            wr32(ent + OFF(gsp_mirror_reg_entry, data), off);
            wr32(ent + OFF(gsp_mirror_reg_entry, length), e[i].len);
            segs[ns++] = (gsp_seg){ e[i].bytes, e[i].len };
            off += e[i].len;
        }
    }
    return ns;
}

/* ---- message queue ---- */
#define TXH(f) OFF(gsp_mirror_msgq_tx, f)
#define RH(f)  (GSP_ELEM_HDR_SIZE + OFF(gsp_mirror_rpc_hdr, f))

static uint32_t ring_free(uint32_t rd, uint32_t wr) { return (rd + GSP_MQ_MSG_COUNT - wr - 1) % GSP_MQ_MSG_COUNT; }

int gsp_mq_attach(gsp_mq *q, const gsp_arena *a, const gsp_boot_plan *plan)
{
    if (!q || !a || !plan || plan->cmdq_pa < a->pa || plan->statq_pa < a->pa ||
        plan->statq_pa - a->pa + GSP_MQ_QUEUE_SIZE > a->used || plan->cmdq_pa - a->pa + GSP_MQ_QUEUE_SIZE > a->used)
        return -1;
    q->cmdq = a->base + (plan->cmdq_pa - a->pa);
    q->statq = a->base + (plan->statq_pa - a->pa);
    q->tx_seq = plan->tx_seq;
    q->rx_seq = 0;
    q->rpc_seq = 0;
    return 0;
}

/* Sequential reader over the 80-byte element+RPC header followed by the payload segments. */
typedef struct {
    const uint8_t *hdr;
    const gsp_seg *segs;
    uint32_t nsegs, seg, off, pos;
} cursor;

static uint8_t next_byte(cursor *c)
{
    if (c->pos < GSP_ELEM_HDR_SIZE + GSP_RPC_HDR_SIZE)
        return c->hdr[c->pos++];
    c->pos++;
    while (c->seg < c->nsegs && c->off >= c->segs[c->seg].len) { c->seg++; c->off = 0; }
    if (c->seg >= c->nsegs) return 0;
    const uint8_t *p = c->segs[c->seg].p;
    uint8_t v = p ? p[c->off] : 0;
    c->off++;
    return v;
}

int gsp_mq_send(gsp_mq *q, uint32_t function, const gsp_seg *segs, uint32_t nsegs, uint32_t rpc_len,
                int sync, uint32_t *sequence_out, const char **why)
{
    *why = 0;
    if (!q || (nsegs && !segs)) { *why = "null argument"; return -1; }
    uint64_t copy_len = 0;
    for (uint32_t i = 0; i < nsegs; i++) copy_len += segs[i].len;
    if (copy_len < rpc_len) { *why = "payload shorter than the RPC length"; return -1; }
    uint64_t msg_len = (uint64_t)GSP_ELEM_HDR_SIZE + GSP_RPC_HDR_SIZE + rpc_len;
    if (msg_len < GSP_ELEM_MIN_LEN || msg_len > GSP_ELEM_SIZE_MAX) { *why = "message length out of range"; return -1; }
    uint32_t elems = (uint32_t)((msg_len + GSP_MQ_MSG_SIZE - 1) / GSP_MQ_MSG_SIZE);
    uint64_t placed = (uint64_t)GSP_ELEM_HDR_SIZE + GSP_RPC_HDR_SIZE + copy_len;
    if (placed > (uint64_t)elems * GSP_MQ_MSG_SIZE) { *why = "payload beyond the elements the length covers"; return -1; }
    uint32_t wr = rd32(q->cmdq + TXH(writePtr));
#ifdef GSP_MUTANT_SWAPRX
    uint32_t rd = rd32(q->cmdq + GSP_MQ_RXHDR_OFF);
#else
    uint32_t rd = rd32(q->statq + GSP_MQ_RXHDR_OFF);   /* GSP's readPtr lives in GSP's own queue (SWAP_RX) */
#endif
    if (wr >= GSP_MQ_MSG_COUNT || rd >= GSP_MQ_MSG_COUNT) { *why = "ring pointer corrupt"; return -1; }
    if (ring_free(rd, wr) < elems) { *why = "command queue full"; return -1; }

    uint32_t seq = sync ? q->rpc_seq : 0;
    uint8_t hdr[GSP_ELEM_HDR_SIZE + GSP_RPC_HDR_SIZE];
    bzero_(hdr, sizeof hdr);
    wr32(hdr + OFF(gsp_mirror_elem, seqNum), q->tx_seq);
    wr32(hdr + OFF(gsp_mirror_elem, elemCount), elems);
    wr32(hdr + RH(header_version), GSP_RPC_HEADER_VERSION);
    wr32(hdr + RH(signature), GSP_RPC_SIGNATURE);
    wr32(hdr + RH(length), GSP_RPC_HDR_SIZE + rpc_len);
    wr32(hdr + RH(function), function);
    wr32(hdr + RH(rpc_result), GSP_RPC_PENDING);
    wr32(hdr + RH(rpc_result_private), GSP_RPC_PENDING);
    wr32(hdr + RH(sequence), seq);

    /* _checkSum32 over msg_len, zero-padded to 8 (message_queue_cpu.c), checkSum field 0. */
    cursor c = { hdr, segs, nsegs, 0, 0, 0 };
    uint64_t x = 0;
    for (uint64_t i = 0; i < up(msg_len, 8); i += 8) {
        uint8_t w[8];
        for (uint32_t k = 0; k < 8; k++) {
            uint8_t v = next_byte(&c);
            w[k] = i + k < msg_len ? v : 0;
        }
        x ^= rd64(w);
    }
#ifndef GSP_MUTANT_CKSUM
    wr32(hdr + OFF(gsp_mirror_elem, checkSum), (uint32_t)(x >> 32) ^ (uint32_t)x);
#endif

    /* Slot e of the message gets bytes [e*4096, e*4096 + 4096): header, payload, zeros. */
    c = (cursor){ hdr, segs, nsegs, 0, 0, 0 };
    for (uint32_t e = 0; e < elems; e++) {
        uint8_t *slot = q->cmdq + GSP_MQ_ENTRY_OFF + ((wr + e) % GSP_MQ_MSG_COUNT) * GSP_MQ_MSG_SIZE;
        for (uint32_t k = 0; k < GSP_MQ_MSG_SIZE; k++) {
            uint64_t b = (uint64_t)e * GSP_MQ_MSG_SIZE + k;
            slot[k] = b < placed ? next_byte(&c) : 0;
        }
    }
    /* message_queue_cpu.c:574-576: store fence before the writePtr publish (the doorbell after it is
     * the caller's; this file writes no register). */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    wr32(q->cmdq + TXH(writePtr), (wr + elems) % GSP_MQ_MSG_COUNT);
    q->tx_seq++;
    if (sync) q->rpc_seq++;
    if (sequence_out) *sequence_out = seq;
    return 0;
}

int gsp_rpc_rm_alloc(gsp_mq *q, uint32_t hClient, uint32_t hParent, uint32_t hObject, uint32_t hClass,
                     const void *params, uint32_t params_size, uint32_t *seq, const char **why)
{
    uint8_t fixed[GSP_RM_ALLOC_FIXED];
    if (params_size && !params) { *why = "null argument"; return -1; }
    bzero_(fixed, sizeof fixed);
    wr32(fixed + OFF(gsp_mirror_rm_alloc, hClient), hClient);
    wr32(fixed + OFF(gsp_mirror_rm_alloc, hParent), hParent);
    wr32(fixed + OFF(gsp_mirror_rm_alloc, hObject), hObject);
    wr32(fixed + OFF(gsp_mirror_rm_alloc, hClass), hClass);
    wr32(fixed + OFF(gsp_mirror_rm_alloc, paramsSize), params_size);
    gsp_seg s[2] = { { fixed, sizeof fixed }, { params, params_size } };
    /* rpcRmApiAlloc_GSP: rpcWriteCommonHeader(.., sizeof(rpc_gsp_rm_alloc_v03_00)); the params
     * follow outside the length. Kept as the driver does it (inventory section 14). */
#ifdef GSP_MUTANT_ALLOCLEN
    return gsp_mq_send(q, GSP_FN_GSP_RM_ALLOC, s, 2, GSP_RM_ALLOC_FIXED + params_size, 1, seq, why);
#else
    return gsp_mq_send(q, GSP_FN_GSP_RM_ALLOC, s, 2, GSP_RM_ALLOC_FIXED, 1, seq, why);
#endif
}

int gsp_rpc_rm_control(gsp_mq *q, uint32_t hClient, uint32_t hObject, uint32_t cmd,
                       const void *params, uint32_t params_size, uint32_t *seq, const char **why)
{
    uint8_t fixed[GSP_RM_CONTROL_FIXED];
    if (params_size && !params) { *why = "null argument"; return -1; }
    if (params_size > GSP_RPC_SIZE_MAX - GSP_RPC_HDR_SIZE - GSP_RM_CONTROL_FIXED) {
        *why = "control params need continuation records (not built)";
        return -1;
    }
    bzero_(fixed, sizeof fixed);
    wr32(fixed + OFF(gsp_mirror_rm_control, hClient), hClient);
    wr32(fixed + OFF(gsp_mirror_rm_control, hObject), hObject);
    wr32(fixed + OFF(gsp_mirror_rm_control, cmd), cmd);
    wr32(fixed + OFF(gsp_mirror_rm_control, paramsSize), params_size);
    gsp_seg s[2] = { { fixed, sizeof fixed }, { params, params_size } };
    return gsp_mq_send(q, GSP_FN_GSP_RM_CONTROL, s, 2, GSP_RM_CONTROL_FIXED + params_size, 1, seq, why);
}

int gsp_rpc_rm_free(gsp_mq *q, uint32_t hClient, uint32_t hObject, uint32_t *seq, const char **why)
{
    uint8_t buf[GSP_RM_FREE_SIZE];
    bzero_(buf, sizeof buf);
    wr32(buf + OFF(gsp_mirror_rm_free, hRoot), hClient);
    wr32(buf + OFF(gsp_mirror_rm_free, hObjectParent), 0);   /* NV01_NULL_OBJECT */
    wr32(buf + OFF(gsp_mirror_rm_free, hObjectOld), hObject);
    gsp_seg s = { buf, sizeof buf };
    return gsp_mq_send(q, GSP_FN_FREE, &s, 1, sizeof buf, 1, seq, why);
}

int gsp_rpc_get_static_info(gsp_mq *q, uint32_t static_info_size, uint32_t *seq, const char **why)
{
    /* rpcGetGspStaticInfo_v14_00: zeroed request, length = 32 + sizeof(GspStaticConfigInfo),
     * refused by the driver above maxRpcSize. The size is a parameter: the struct pulls in
     * RM-internal headers that do not compile standalone (inventory section 14). */
    if (static_info_size == 0 || static_info_size > GSP_RPC_SIZE_MAX - GSP_RPC_HDR_SIZE) {
        *why = "static info size out of range";
        return -1;
    }
    gsp_seg s = { 0, static_info_size };
    return gsp_mq_send(q, GSP_FN_GET_GSP_STATIC_INFO, &s, 1, static_info_size, 1, seq, why);
}

int gsp_mq_recv(gsp_mq *q, uint8_t *out, uint32_t cap, uint32_t *function, uint32_t *rpc_result,
                const char **why)
{
    *why = 0;
    if (!q || !out) { *why = "null argument"; return -1; }
    const uint8_t *tx = q->statq;
    if (rd32(tx + TXH(version)) != 0 || rd32(tx + TXH(size)) != GSP_MQ_QUEUE_SIZE ||
        rd32(tx + TXH(msgSize)) != GSP_MQ_MSG_SIZE || rd32(tx + TXH(msgCount)) != GSP_MQ_MSG_COUNT ||
        rd32(tx + TXH(rxHdrOff)) < 32 || rd32(tx + TXH(entryOff)) != GSP_MQ_ENTRY_OFF) {
        *why = "status queue header not the driver's shape";
        return -1;
    }
    uint32_t wr = rd32(tx + TXH(writePtr));
    __atomic_thread_fence(__ATOMIC_SEQ_CST);          /* read the slots only after the writePtr that covers them */
    uint8_t *rdp = q->cmdq + GSP_MQ_RXHDR_OFF;                   /* our readPtr lives in our own queue (SWAP_RX) */
    uint32_t rd = rd32(rdp);
    if (wr >= GSP_MQ_MSG_COUNT || rd >= GSP_MQ_MSG_COUNT) { *why = "ring pointer corrupt"; return -1; }
    uint32_t avail = (wr + GSP_MQ_MSG_COUNT - rd) % GSP_MQ_MSG_COUNT;
    if (avail == 0) return 0;
    const uint8_t *first = tx + GSP_MQ_ENTRY_OFF + rd * GSP_MQ_MSG_SIZE;
    uint32_t elems = rd32(first + OFF(gsp_mirror_elem, elemCount));
    uint32_t len = rd32(first + RH(length));
    if (elems == 0 || elems > avail || elems > GSP_ELEM_SIZE_MAX / GSP_MQ_MSG_SIZE) { *why = "element count bad"; return -1; }
    if (len < GSP_RPC_HDR_SIZE || GSP_ELEM_HDR_SIZE + len > elems * GSP_MQ_MSG_SIZE) { *why = "rpc length bad"; return -1; }
    uint32_t msg_len = GSP_ELEM_HDR_SIZE + len;
    if (cap < up(msg_len, 8)) { *why = "output too small"; return -1; }
    for (uint32_t b = 0; b < (uint32_t)up(msg_len, 8); b++) {
        uint32_t e = b / GSP_MQ_MSG_SIZE;
        out[b] = b < msg_len ? tx[GSP_MQ_ENTRY_OFF + ((rd + e) % GSP_MQ_MSG_COUNT) * GSP_MQ_MSG_SIZE + b % GSP_MQ_MSG_SIZE] : 0;
    }
    if (gsp_checksum32(out, (uint32_t)up(msg_len, 8)) != 0) { *why = "checksum not zero"; return -1; }
    if (rd32(out + OFF(gsp_mirror_elem, seqNum)) != q->rx_seq) { *why = "bad sequence number"; return -1; }
    if (rd32(out + RH(signature)) != GSP_RPC_SIGNATURE) { *why = "rpc signature bad"; return -1; }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);          /* finish reading before handing the slots back */
    wr32(rdp, (rd + elems) % GSP_MQ_MSG_COUNT);
    q->rx_seq++;
    if (function) *function = rd32(out + RH(function));
    if (rpc_result) *rpc_result = rd32(out + RH(rpc_result));
    return 1;
}

/* ---- the boot build ---- */
#define FAIL(s) do { *why = (s); return -1; } while (0)

int gsp_boot_build(gsp_arena *a, const gsp_boot_in *in, gsp_boot_plan *out, const char **why)
{
    *why = 0;
    if (!a || !in || !out) FAIL("null argument");
    if (!in->rm_elf || !in->rm_elf_size) FAIL("GSP-RM ELF missing");
    if (!in->rm_sig || !in->rm_sig_size) FAIL("GSP-RM signature missing");
    if (!in->bootloader || in->bootloader_size != GSP_GB10B_BOOTLOADER_SIZE) FAIL("bootloader is not the GB10B GspRmBoot image size");
    if (in->bl_code_off != GSP_GB10B_BL_CODE_OFF || in->bl_data_off != GSP_GB10B_BL_DATA_OFF ||
        in->bl_manifest_off != GSP_GB10B_BL_MANIFEST_OFF) FAIL("bootloader descriptor offsets are not the GB10B values");
    if (!in->fmc || !in->fmc_size) FAIL("GSP-FMC image missing");
    if (!in->fmc_hash || !in->fmc_pubkey || !in->fmc_sig) FAIL("GSP-FMC hash, key or signature missing");
    if (!in->sysmem_heap_pa || (in->sysmem_heap_pa & (GSP_PAGE - 1)) || !in->sysmem_heap_size ||
        (in->sysmem_heap_size & (GSP_PAGE - 1))) FAIL("sysmem heap zero or not 4 KiB aligned");
    if (!in->system_info) FAIL("GspSystemInfo missing");
    uint32_t reg_size = 0;
    if (in->nregs) {
        if (!in->regs || !(reg_size = gsp_reg_table_size(in->regs, in->nregs))) FAIL("registry entry invalid or too large");
    }
    if (in->dma_bits == 0 || in->dma_bits > 64) FAIL("dma_bits out of range");
    if (a->used) FAIL("arena not empty");

    bzero_((uint8_t *)out, sizeof *out);
    uint64_t pa;
    uint8_t *p;

    /* 1. WprMeta page (filled last, needs the other addresses). */
    uint8_t *wpr = take(a, GSP_PAGE, &pa);
    if (!wpr) FAIL("arena too small");
    out->wpr_meta = (gsp_region){pa, GSP_WPR_META_SIZE};

    /* 2. libos init-argument page. */
    uint8_t *libos = take(a, GSP_PAGE, &pa);
    if (!libos) FAIL("arena too small");
    out->libos_args = (gsp_region){pa, GSP_PAGE};

    /* 3. log buffers: u64 put pointer 0, then one u64 PA per 4 KiB page. */
    for (uint32_t i = 0; i < GSP_GB10B_LOG_COUNT; i++) {
        uint32_t sz = gb10b_logs[i].size;
        if (!(p = take(a, sz, &pa))) FAIL("arena too small");
        for (uint32_t k = 0; k < sz / GSP_PAGE; k++)
            wr64(p + 8 + 8 * k, pa + (uint64_t)k * GSP_PAGE);
        out->log[i] = (gsp_region){pa, sz};
    }

    /* 4. GSP_ARGUMENTS_CACHED page. */
    uint8_t *args = take(a, GSP_PAGE, &pa);
    if (!args) FAIL("arena too small");
    out->args_cached = (gsp_region){pa, GSP_PAGE};

    /* 5. RPC shared buffer: page table, command queue, status queue. */
    uint8_t *shared = take(a, GSP_MQ_SHARED_SIZE, &pa);
    if (!shared) FAIL("arena too small");
    out->rpc_shared = (gsp_region){pa, GSP_MQ_SHARED_SIZE};
    for (uint32_t k = 0; k < GSP_MQ_NUM_PTES; k++)
        wr64(shared + 8 * k, pa + (uint64_t)k * GSP_PAGE);
    out->cmdq_pa = pa + GSP_MQ_CMDQ_OFF;
    out->statq_pa = pa + GSP_MQ_STATQ_OFF;
    uint8_t *cmdq = shared + GSP_MQ_CMDQ_OFF;
    wr32(cmdq + TXH(version), 0);
    wr32(cmdq + TXH(size), GSP_MQ_QUEUE_SIZE);
    wr32(cmdq + TXH(msgSize), GSP_MQ_MSG_SIZE);
    wr32(cmdq + TXH(msgCount), GSP_MQ_MSG_COUNT);
    wr32(cmdq + TXH(writePtr), 0);
    wr32(cmdq + TXH(flags), GSP_MQ_FLAGS_SWAP_RX);
    wr32(cmdq + TXH(rxHdrOff), GSP_MQ_RXHDR_OFF);
    wr32(cmdq + TXH(entryOff), GSP_MQ_ENTRY_OFF);

    /* libos entries: logs in order, then RMARGS -> GSP_ARGUMENTS_CACHED. */
    for (uint32_t i = 0; i <= GSP_GB10B_LOG_COUNT; i++) {
        uint8_t *e = libos + i * GSP_LIBOS_ENTRY_SIZE;
        int rm = i == GSP_GB10B_LOG_COUNT;
        wr64(e + OFF(gsp_mirror_libos_arg, id8), gsp_init_arg_id(rm ? "RMARGS" : gb10b_logs[i].id));
        wr64(e + OFF(gsp_mirror_libos_arg, pa), rm ? out->args_cached.pa : out->log[i].pa);
        wr64(e + OFF(gsp_mirror_libos_arg, size), rm ? GSP_PAGE : out->log[i].size);
        e[OFF(gsp_mirror_libos_arg, kind)] = GSP_LIBOS_KIND_CONTIGUOUS;
        e[OFF(gsp_mirror_libos_arg, loc)] = GSP_LIBOS_LOC_SYSMEM;
    }

    /* GSP_ARGUMENTS_CACHED fill. */
    wr64(args + OFF(gsp_mirror_args_cached, mq.sharedMemPhysAddr), out->rpc_shared.pa);
    wr32(args + OFF(gsp_mirror_args_cached, mq.pageTableEntryCount), GSP_MQ_NUM_PTES);
    wr64(args + OFF(gsp_mirror_args_cached, mq.cmdQueueOffset), GSP_MQ_CMDQ_OFF);
    wr64(args + OFF(gsp_mirror_args_cached, mq.statQueueOffset), GSP_MQ_STATQ_OFF);
    wr32(args + OFF(gsp_mirror_args_cached, gpuInstance), in->gpu_instance);
    args[OFF(gsp_mirror_args_cached, bDmemStack)] = 1;
    wr64(args + OFF(gsp_mirror_args_cached, sysmemHeapArgs.pa), in->sysmem_heap_pa);
    wr64(args + OFF(gsp_mirror_args_cached, sysmemHeapArgs.size), in->sysmem_heap_size);

    /* 6. radix-3: levels 0..2 back to back, then the ELF data pages. */
    uint64_t np[4], lvl_off[4] = {0, 0, 0, 0}, n = 0;
    np[3] = (in->rm_elf_size + GSP_PAGE - 1) / GSP_PAGE;
    for (int i = 3; i > 0; i--) np[i - 1] = ((np[i] - 1) >> 9) + 1;
    if (np[0] != 1) FAIL("ELF too large for a 3-level radix table");
    for (int i = 1; i < 4; i++) { n += np[i - 1]; lvl_off[i] = n * GSP_PAGE; }
    uint64_t r3_size = n * GSP_PAGE + np[3] * GSP_PAGE;
    if (!(p = take(a, r3_size, &pa))) FAIL("arena too small");
    for (int i = 0; i < 3; i++)
        for (uint64_t k = 0; k < np[i + 1]; k++)
#ifdef GSP_MUTANT_RADIX
            wr64(p + lvl_off[i] + k * 8, pa + lvl_off[i + 1] + (k + 1) * GSP_PAGE);
#else
            wr64(p + lvl_off[i] + k * 8, pa + lvl_off[i + 1] + k * GSP_PAGE);
#endif
    bcopy_(p + lvl_off[3], in->rm_elf, in->rm_elf_size);
    out->radix3 = (gsp_region){pa, r3_size};
    out->radix3_root_pa = pa;
    out->elf_data_pa = pa + lvl_off[3];
    for (int i = 0; i < 4; i++) out->radix3_level_pages[i] = (uint32_t)np[i];

    /* 7. bootloader, 8. signature (size rounded up to 256), 9. FMC image. */
    if (!(p = take(a, in->bootloader_size, &pa))) FAIL("arena too small");
    bcopy_(p, in->bootloader, in->bootloader_size);
    out->bootloader = (gsp_region){pa, in->bootloader_size};
    uint64_t sig_sz = up(in->rm_sig_size, 256);
    if (!(p = take(a, sig_sz, &pa))) FAIL("arena too small");
    bcopy_(p, in->rm_sig, in->rm_sig_size);
    out->signature = (gsp_region){pa, sig_sz};
    if (!(p = take(a, in->fmc_size, &pa))) FAIL("arena too small");
    bcopy_(p, in->fmc, in->fmc_size);
    out->fmc = (gsp_region){pa, in->fmc_size};

    /* 10. GSP_FMC_BOOT_PARAMS. */
    uint8_t *fp = take(a, GSP_PAGE, &pa);
    if (!fp) FAIL("arena too small");
    out->fmc_params = (gsp_region){pa, GSP_FMC_PARAMS_SIZE};
#define FP(f) OFF(gsp_mirror_fmc_params, f)
    wr32(fp + FP(regkeys), in->fmc_regkeys);
    wr32(fp + FP(boot.target), GSP_DMA_TARGET_COHERENT);          /* WprMeta page is cached sysmem */
    wr32(fp + FP(boot.gspRmDescSize), GSP_WPR_META_SIZE);
    wr64(fp + FP(boot.gspRmDescOffset), out->wpr_meta.pa);
    fp[FP(boot.bIsGspRmBoot)] = 1;
    wr32(fp + FP(rm.target), GSP_DMA_TARGET_NONCOHERENT);         /* libos args page is uncached */
    wr64(fp + FP(rm.bootArgsOffset), out->libos_args.pa);

    /* WprMeta (gh100 populate + GB10B overrides; everything FMC-owned stays 0). */
    wr64(wpr + WM(magic), GSP_WPR_META_MAGIC);
    wr64(wpr + WM(revision), GSP_WPR_META_REVISION);
    wr64(wpr + WM(sysmemAddrOfRadix3Elf), out->radix3_root_pa);
    wr64(wpr + WM(sizeOfRadix3Elf), in->rm_elf_size);
    wr64(wpr + WM(sysmemAddrOfBootloader), out->bootloader.pa);
    wr64(wpr + WM(sizeOfBootloader), in->bootloader_size);
    wr64(wpr + WM(bootloaderCodeOffset), in->bl_code_off);
    wr64(wpr + WM(bootloaderDataOffset), in->bl_data_off);
    wr64(wpr + WM(bootloaderManifestOffset), in->bl_manifest_off);
    wr64(wpr + WM(sysmemAddrOfSignature), out->signature.pa);
    wr64(wpr + WM(sizeOfSignature), sig_sz);
    wr64(wpr + WM(gspFwHeapSize), GSP_GB10B_FW_HEAP_SIZE);

    /* Every chip-visible address must fit the DMA window. */
    if (!fits_dma(a->pa, a->used, in->dma_bits) || !fits_dma(in->sysmem_heap_pa, in->sysmem_heap_size, in->dma_bits))
        FAIL("address beyond the DMA window");

    /* Pre-boot RPCs, async (sequence 0), in kgspInitRm order. */
    gsp_mq q = { .cmdq = cmdq, .statq = shared + GSP_MQ_STATQ_OFF, .tx_seq = 0, .rx_seq = 0, .rpc_seq = 0 };
    /* The status queue is GSP's to create; until then its rx header (GSP's readPtr) is 0. */
    gsp_seg si = { in->system_info, GSP_SYSTEM_INFO_SIZE };
    if (gsp_mq_send(&q, GSP_FN_SET_SYSTEM_INFO, &si, 1, GSP_SYSTEM_INFO_SIZE, 0, 0, why))
        return -1;
    if (in->nregs) {
        uint8_t head[8 + 16 * GSP_REG_MAX_ENTRIES];
        gsp_seg segs[1 + 2 * GSP_REG_MAX_ENTRIES];
        uint32_t ns = reg_write(in->regs, in->nregs, reg_size, head, segs);
        if (gsp_mq_send(&q, GSP_FN_SET_REGISTRY, segs, ns, reg_size, 0, 0, why))
            return -1;
    }
    out->tx_seq = q.tx_seq;
    out->cmdq_write_ptr = rd32(cmdq + TXH(writePtr));

    /* SEC2 packet: MCTP dword, NVDM dword (type 0x7e, vendor 0x10de, NVDM type COT 0x14), then the
     * 860-byte payload; tail = padded length - 4. The MCTP dword on GB10B is the constant 1, not SOM|EOM:
     * kernel_sec2.c:320 via ksec2CreateMctpHeader_HAL; on GB10B the HAL is ksec2CreateMctpHeader_539ab4 = `return 1;` (g_kernel_sec2_nvoc.c:786-788, g_kernel_sec2_nvoc.h:853-855); only GB20B/GB20C build SOM|EOM (kernel_sec2_gb20b.c:206-219). */
    uint8_t *pk = out->cot_packet;
    wr32(pk, 1u);
    wr32(pk + 4, 0x7eu | (0x10deu << 8) | (0x14u << 24));
    uint8_t *cot = pk + 8;
#define CO(f) OFF(gsp_mirror_cot, f)
    wr16(cot + CO(version), GSP_COT_VERSION);
    wr16(cot + CO(size), GSP_COT_PAYLOAD_SIZE);
    wr64(cot + CO(gspFmcSysmemOffset), out->fmc.pa);
    wr32(cot + CO(frtsSysmemSize), in->fmc_size);                 /* sic: the driver stores the FMC size here */
    bcopy_(cot + CO(hash384), in->fmc_hash, GSP_COT_HASH_SIZE);
    bcopy_(cot + CO(publicKey), in->fmc_pubkey, GSP_COT_KEY_SIZE);
    bcopy_(cot + CO(signature), in->fmc_sig, GSP_COT_SIG_SIZE);
    wr64(cot + CO(gspBootArgsSysmemOffset), out->fmc_params.pa);
    out->cot_packet_len = 8 + GSP_COT_PAYLOAD_SIZE;
    out->emem_queue_tail = (uint32_t)up(out->cot_packet_len, 4) - 4;
    return 0;
}
