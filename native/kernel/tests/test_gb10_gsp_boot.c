/* test_gb10_gsp_boot -- host test for dev/gb10_gsp_boot.c (L192, aienos#286 cut C host side).
 * Synthetic firmware bytes only; no device, no real firmware. Every decode below reads fixed byte
 * offsets written out by hand from the NVIDIA headers (inventory sections 10-14), not the builder's
 * mirror structs, so a wrong mirror offset fails here as well as in test_gb10_gsp_nvhdr. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "ck_test.h"
#include "gb10_gsp_boot.h"

static uint32_t r32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t r64(const uint8_t *p) { return (uint64_t)r32(p) | ((uint64_t)r32(p + 4) << 32); }
static uint16_t r16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static void w32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) { p[i] = (uint8_t)v; v >>= 8; } }

#define ARENA_PA   0x80000000ULL
#define ARENA_CAP  (8u << 20)
#define ELF_SIZE   (3u * 1024 * 1024 + 123)     /* 769 data pages: radix levels 1, 1, 2, 769 */
#define SIG_SIZE   4097u
#define FMC_SIZE   70000u

static uint8_t *arena_mem, *elf, *bl, *sig, *fmc, hash[48], key[384], fsig[384], sysinfo[GSP_SYSTEM_INFO_SIZE];
static const uint8_t bin_val[5] = {9, 8, 7, 6, 5};
static const gsp_reg_entry regs[3] = {
    {"RMSecBusResetEnable", GSP_REG_DWORD, 1, 0, 0},
    {"RmTestString", GSP_REG_STRING, 0, (const uint8_t *)"abc", 4},
    {"RmTestBin", GSP_REG_BINARY, 0, bin_val, 5},
};

static void fill(uint8_t *p, size_t n, uint32_t seed) { for (size_t i = 0; i < n; i++) p[i] = (uint8_t)((i * 131u + seed) >> 3); }

static gsp_boot_in good_input(void)
{
    gsp_boot_in in;
    memset(&in, 0, sizeof in);
    in.rm_elf = elf; in.rm_elf_size = ELF_SIZE;
    in.rm_sig = sig; in.rm_sig_size = SIG_SIZE;
    in.bootloader = bl; in.bootloader_size = GSP_GB10B_BOOTLOADER_SIZE;
    in.bl_code_off = 0xb000; in.bl_data_off = 0xa00; in.bl_manifest_off = 0;
    in.fmc = fmc; in.fmc_size = FMC_SIZE; in.fmc_hash = hash; in.fmc_pubkey = key; in.fmc_sig = fsig;
    in.sysmem_heap_pa = 0x100000000ULL; in.sysmem_heap_size = 512u << 20;
    in.system_info = sysinfo;
    in.regs = regs; in.nregs = 3;
    in.gpu_instance = 0;
    in.dma_bits = 41;
    return in;
}

static uint8_t *at(uint64_t pa) { return arena_mem + (pa - ARENA_PA); }

static int build(gsp_arena *a, const gsp_boot_in *in, gsp_boot_plan *plan, const char **why)
{
    memset(arena_mem, 0xA5, ARENA_CAP);          /* the builder must zero what it takes */
    gsp_arena_init(a, arena_mem, ARENA_PA, ARENA_CAP);
    return gsp_boot_build(a, in, plan, why);
}

/* Check one element at slot ptr: checksum, header fields. Returns rpc length. */
static uint32_t check_elem(const uint8_t *e, uint32_t seq_num, uint32_t fn, uint32_t rpc_seq, uint32_t elems)
{
    uint32_t len = r32(e + 48 + 8);
    uint32_t msg = 48 + len, padded = (msg + 7) & ~7u;
    uint8_t tmp[65536];
    memcpy(tmp, e, padded);
    for (uint32_t i = msg; i < padded; i++) tmp[i] = 0;
    CHECK(gsp_checksum32(tmp, padded) == 0);
    CHECK(r32(e + 36) == seq_num);
    CHECK(r32(e + 40) == elems);
    CHECK(r32(e + 48) == 0x03000000u);
    CHECK(r32(e + 52) == 0x43505256u);
    CHECK(r32(e + 60) == fn);
    CHECK(r32(e + 64) == 0xFFFFFFFFu && r32(e + 68) == 0xFFFFFFFFu);
    CHECK(r32(e + 72) == rpc_seq);
    return len;
}

static void test_layout(void)
{
    gsp_arena a; gsp_boot_plan p; const char *why = 0;
    gsp_boot_in in = good_input();
    CHECK(build(&a, &in, &p, &why) == 0);
    if (why) printf("  build: %s\n", why);

    /* every region 4 KiB aligned, inside the arena, in order, not overlapping */
    gsp_region rs[] = { p.wpr_meta, p.libos_args, p.log[0], p.log[1], p.log[2], p.log[3], p.log[4], p.log[5], p.log[6],
                        p.args_cached, p.rpc_shared, p.radix3, p.bootloader, p.signature, p.fmc, p.fmc_params };
    uint64_t end = ARENA_PA;
    for (size_t i = 0; i < sizeof rs / sizeof rs[0]; i++) {
        CHECK((rs[i].pa & 4095) == 0);
        CHECK(rs[i].pa >= end);
        end = rs[i].pa + rs[i].size;
    }
    CHECK(end <= ARENA_PA + a.used && a.used <= ARENA_CAP);

    /* GspFwWprMeta (gsp_fw_wpr_meta.h, natural layout) */
    const uint8_t *w = at(p.wpr_meta.pa);
    CHECK(r64(w + 0) == 0xdc3aae21371a60b3ULL && r64(w + 8) == 1);
    CHECK(r64(w + 16) == p.radix3_root_pa && r64(w + 24) == ELF_SIZE);
    CHECK(r64(w + 32) == p.bootloader.pa && r64(w + 40) == 196608);
    CHECK(r64(w + 48) == 0xb000 && r64(w + 56) == 0xa00 && r64(w + 64) == 0);
    CHECK(r64(w + 72) == p.signature.pa && r64(w + 80) == 4352 && (p.signature.pa & 255) == 0);
    CHECK(r64(w + 128) == (133ULL << 20));
    int fmc_owned_zero = 1;
    for (int off = 88; off < 256; off += 8)
        if (off != 128 && r64(w + off) != 0) fmc_owned_zero = 0;
    CHECK(fmc_owned_zero);                           /* FB layout, bootCount, flags, verified: FMC's */

    /* libos init args: seven logs then RMARGS, CONTIGUOUS / SYSMEM */
    static const char *ids[8] = {"LOGINIT", "LOGINTR", "LOGRM", "LOGMNOC", "LOGDBG", "LOGROOT", "LOGKRNL", "RMARGS"};
    static const uint32_t sizes[8] = {0x10000, 0x10000, 0x10000, 0x10000, 0x10000, 0x1000, 0x10000, 0x1000};
    const uint8_t *l = at(p.libos_args.pa);
    for (int i = 0; i < 8; i++) {
        const uint8_t *e = l + 32 * i;
        uint64_t want = 0;
        for (const char *c = ids[i]; *c; c++) want = (want << 8) | (uint8_t)*c;
        CHECK(r64(e) == want);
        CHECK(r64(e + 8) == (i < 7 ? p.log[i].pa : p.args_cached.pa));
        CHECK(r64(e + 16) == sizes[i]);
        CHECK(e[24] == 1 && e[25] == 1);
    }
    CHECK(r64(l) == 0x004C4F47494E4954ULL);          /* "LOGINIT" big-endian packed */
    CHECK(r64(l + 32 * 8) == 0 && r64(l + 32 * 8 + 8) == 0);

    /* log buffers: put pointer 0, then one PA per page */
    for (int i = 0; i < 7; i++) {
        const uint8_t *b = at(p.log[i].pa);
        CHECK(r64(b) == 0);
        CHECK(r64(b + 8) == p.log[i].pa);
        CHECK(r64(b + 8 * (p.log[i].size / 4096)) == p.log[i].pa + p.log[i].size - 4096);
    }

    /* GSP_ARGUMENTS_CACHED */
    const uint8_t *g = at(p.args_cached.pa);
    CHECK(r64(g + 0) == p.rpc_shared.pa && r32(g + 8) == 129 && r64(g + 16) == 4096 && r64(g + 24) == 4096 + 0x40000);
    CHECK(r32(g + 32) == 0 && r32(g + 36) == 0 && g[40] == 0);
    CHECK(r32(g + 44) == 0 && g[48] == 1);
    CHECK(r64(g + 56) == 0 && r64(g + 64) == 0);
    CHECK(r64(g + 72) == 0x100000000ULL && r64(g + 80) == (512ULL << 20));

    /* RPC shared buffer page table and command queue header */
    const uint8_t *s = at(p.rpc_shared.pa);
    CHECK(p.rpc_shared.size == 129 * 4096);
    CHECK(r64(s) == p.rpc_shared.pa && r64(s + 8 * 128) == p.rpc_shared.pa + 128 * 4096 && r64(s + 8 * 129) == 0);
    CHECK(p.cmdq_pa == p.rpc_shared.pa + 4096 && p.statq_pa == p.rpc_shared.pa + 4096 + 0x40000);
    const uint8_t *cq = at(p.cmdq_pa);
    CHECK(r32(cq) == 0 && r32(cq + 4) == 0x40000 && r32(cq + 8) == 4096 && r32(cq + 12) == 63);
    CHECK(r32(cq + 16) == 2 && r32(cq + 20) == 1 && r32(cq + 24) == 32 && r32(cq + 28) == 4096);
    CHECK(p.cmdq_write_ptr == 2 && p.tx_seq == 2);
    const uint8_t *sq = at(p.statq_pa);
    int sq_zero = 1;
    for (int i = 0; i < 4096; i++) if (sq[i]) sq_zero = 0;
    CHECK(sq_zero);                                   /* GSP creates the status queue */

    /* element 0: GSP_SET_SYSTEM_INFO, async */
    const uint8_t *e0 = cq + 4096;
    CHECK(check_elem(e0, 0, 72, 0, 1) == 32 + 936);
    CHECK(memcmp(e0 + 80, sysinfo, 936) == 0);
    /* element 1: SET_REGISTRY, async, PACKED_REGISTRY_TABLE */
    const uint8_t *e1 = cq + 2 * 4096;
    uint32_t rl = check_elem(e1, 1, 73, 0, 1);
    const uint8_t *t = e1 + 80;
    uint32_t tsize = 8 + 16 * 3 + 20 + 13 + 4 + 10 + 5;
    CHECK(rl == 32 + tsize && r32(t) == tsize && r32(t + 4) == 3);
    CHECK(r32(t + 8) == 56 && t[12] == 1 && r32(t + 16) == 1 && r32(t + 20) == 4);
    CHECK(strcmp((const char *)t + 56, "RMSecBusResetEnable") == 0);
    CHECK(r32(t + 24) == 76 && t[28] == 3 && r32(t + 32) == 89 && r32(t + 36) == 4);
    CHECK(strcmp((const char *)t + 76, "RmTestString") == 0 && strcmp((const char *)t + 89, "abc") == 0);
    CHECK(r32(t + 40) == 93 && t[44] == 2 && r32(t + 48) == 103 && r32(t + 52) == 5);
    CHECK(memcmp(t + 103, bin_val, 5) == 0);
    CHECK(t[13] == 0 && t[14] == 0 && t[15] == 0);

    /* radix-3: level pages 1, 1, 2, 769; each level points at the next; data is the ELF */
    CHECK(p.radix3_level_pages[0] == 1 && p.radix3_level_pages[1] == 1 && p.radix3_level_pages[2] == 2 && p.radix3_level_pages[3] == 769);
    CHECK(p.radix3.size == (4 + 769) * 4096ULL && p.elf_data_pa == p.radix3.pa + 4 * 4096);
    const uint8_t *r0 = at(p.radix3.pa), *r1 = r0 + 4096, *r2 = r0 + 2 * 4096;
    CHECK(r64(r0) == p.radix3.pa + 4096 && r64(r0 + 8) == 0);
    CHECK(r64(r1) == p.radix3.pa + 2 * 4096 && r64(r1 + 8) == p.radix3.pa + 3 * 4096 && r64(r1 + 16) == 0);
    CHECK(r64(r2) == p.elf_data_pa && r64(r2 + 8 * 511) == p.elf_data_pa + 511 * 4096ULL);
    CHECK(r64(r2 + 4096) == p.elf_data_pa + 512 * 4096ULL && r64(r2 + 4096 + 8 * 256) == p.elf_data_pa + 768 * 4096ULL);
    CHECK(r64(r2 + 4096 + 8 * 257) == 0);
    CHECK(memcmp(at(p.elf_data_pa), elf, ELF_SIZE) == 0 && at(p.elf_data_pa)[ELF_SIZE] == 0);

    /* firmware copies */
    CHECK(memcmp(at(p.bootloader.pa), bl, 196608) == 0);
    CHECK(memcmp(at(p.signature.pa), sig, SIG_SIZE) == 0 && at(p.signature.pa)[SIG_SIZE] == 0);
    CHECK(memcmp(at(p.fmc.pa), fmc, FMC_SIZE) == 0);

    /* GSP_FMC_BOOT_PARAMS (gspifpub.h, natural layout) */
    const uint8_t *f = at(p.fmc_params.pa);
    CHECK(r32(f + 0) == 0 && r32(f + 8) == 1 && r32(f + 12) == 256 && r64(f + 16) == p.wpr_meta.pa);
    CHECK(r64(f + 24) == 0 && r32(f + 32) == 0 && f[36] == 1);
    CHECK(r32(f + 40) == 2 && r64(f + 48) == p.libos_args.pa);
    CHECK(r32(f + 56) == 0 && r64(f + 64) == 0 && r32(f + 72) == 0 && r32(f + 80) == 0 && r32(f + 84) == 0);

    /* SEC2 packet: MCTP dword 1 (GB10B HAL constant), NVDM 0x7e / 0x10de / COT 0x14, NVDM_PAYLOAD_COT packed */
    const uint8_t *k = p.cot_packet;
    CHECK(r32(k) == 0x00000001u && r32(k + 4) == 0x1410de7eu);
    CHECK(p.cot_packet_len == 868 && p.emem_queue_tail == 864);
    const uint8_t *c = k + 8;
    CHECK(r16(c) == 1 && r16(c + 2) == 860);
    CHECK(r64(c + 4) == p.fmc.pa && r64(c + 12) == 0 && r32(c + 20) == FMC_SIZE && r64(c + 24) == 0 && r32(c + 32) == 0);
    CHECK(memcmp(c + 36, hash, 48) == 0 && memcmp(c + 84, key, 384) == 0 && memcmp(c + 468, fsig, 384) == 0);
    CHECK(r64(c + 852) == p.fmc_params.pa);
    int tail_zero = 1;
    for (int i = 868; i < 1024; i++) if (k[i]) tail_zero = 0;
    CHECK(tail_zero);

    /* deterministic: same input, same bytes */
    uint8_t *copy = malloc(a.used);
    memcpy(copy, arena_mem, a.used);
    gsp_boot_plan p2; gsp_arena a2;
    CHECK(build(&a2, &in, &p2, &why) == 0 && a2.used == a.used && memcmp(copy, arena_mem, a.used) == 0 &&
          memcmp(&p, &p2, sizeof p) == 0);
    free(copy);

    /* no registry: only SET_SYSTEM_INFO queued */
    gsp_boot_in nr = in; nr.nregs = 0; nr.regs = 0;
    CHECK(build(&a, &nr, &p, &why) == 0 && p.cmdq_write_ptr == 1 && p.tx_seq == 1);
}

static void expect_refusal(gsp_boot_in in, const char *want)
{
    gsp_arena a; gsp_boot_plan p; const char *why = 0;
    int rc = build(&a, &in, &p, &why);
    CHECK(rc == -1 && why && strcmp(why, want) == 0);
    if (rc != -1 || !why || strcmp(why, want)) printf("  wanted \"%s\", got \"%s\"\n", want, why ? why : "(accepted)");
}

static void test_refusals(void)
{
    gsp_boot_in in;
    in = good_input(); in.rm_elf = 0; expect_refusal(in, "GSP-RM ELF missing");
    in = good_input(); in.rm_elf_size = 0; expect_refusal(in, "GSP-RM ELF missing");
    in = good_input(); in.rm_sig_size = 0; expect_refusal(in, "GSP-RM signature missing");
    in = good_input(); in.bootloader_size = 196608 + 4096; expect_refusal(in, "bootloader is not the GB10B GspRmBoot image size");
    in = good_input(); in.bl_code_off = 0xa000; expect_refusal(in, "bootloader descriptor offsets are not the GB10B values");
    in = good_input(); in.bl_manifest_off = 0x100; expect_refusal(in, "bootloader descriptor offsets are not the GB10B values");
    in = good_input(); in.fmc_size = 0; expect_refusal(in, "GSP-FMC image missing");
    in = good_input(); in.fmc_hash = 0; expect_refusal(in, "GSP-FMC hash, key or signature missing");
    in = good_input(); in.sysmem_heap_pa = 0x100000800ULL; expect_refusal(in, "sysmem heap zero or not 4 KiB aligned");
    in = good_input(); in.sysmem_heap_size = 0; expect_refusal(in, "sysmem heap zero or not 4 KiB aligned");
    in = good_input(); in.system_info = 0; expect_refusal(in, "GspSystemInfo missing");
    in = good_input(); in.dma_bits = 0; expect_refusal(in, "dma_bits out of range");
    in = good_input(); in.dma_bits = 31; expect_refusal(in, "address beyond the DMA window");     /* arena at 2 GiB */
    in = good_input(); in.dma_bits = 32; expect_refusal(in, "address beyond the DMA window");     /* heap at 4 GiB */
    in = good_input(); in.rm_elf_size = (1ULL << 39) + 1; expect_refusal(in, "ELF too large for a 3-level radix table");
    in = good_input(); in.rm_elf_size = 8u << 20; expect_refusal(in, "arena too small");
    gsp_reg_entry bad[2] = { {"", GSP_REG_DWORD, 1, 0, 0}, {"x", GSP_REG_STRING, 0, 0, 0} };
    in = good_input(); in.regs = bad; in.nregs = 1; expect_refusal(in, "registry entry invalid or too large");
    in = good_input(); in.regs = bad + 1; in.nregs = 1; expect_refusal(in, "registry entry invalid or too large");
    gsp_reg_entry many[GSP_REG_MAX_ENTRIES + 1];
    for (unsigned i = 0; i <= GSP_REG_MAX_ENTRIES; i++) many[i] = (gsp_reg_entry){"k", GSP_REG_DWORD, i, 0, 0};
    in = good_input(); in.regs = many; in.nregs = GSP_REG_MAX_ENTRIES + 1; expect_refusal(in, "registry entry invalid or too large");
    in = good_input(); in.regs = many; in.nregs = GSP_REG_MAX_ENTRIES;
    { gsp_arena a; gsp_boot_plan p; const char *why = 0; CHECK(build(&a, &in, &p, &why) == 0); }
    /* builder on a used arena */
    { gsp_arena a; gsp_boot_plan p; const char *why = 0; in = good_input();
      CHECK(build(&a, &in, &p, &why) == 0); CHECK(gsp_boot_build(&a, &in, &p, &why) == -1 && strcmp(why, "arena not empty") == 0); }
    /* arena init */
    gsp_arena a;
    CHECK(gsp_arena_init(&a, arena_mem, ARENA_PA + 8, ARENA_CAP) == -1);
    CHECK(gsp_arena_init(&a, arena_mem + 8, ARENA_PA, ARENA_CAP) == -1);
    CHECK(gsp_arena_init(&a, arena_mem, ~0ULL - 4095, ARENA_CAP) == -1);
    CHECK(gsp_arena_init(&a, 0, ARENA_PA, ARENA_CAP) == -1);
}

/* Simulated GSP side: create the status queue the way msgqTxCreate would and post one message. */
static void gsp_post(uint8_t *sq, uint32_t slot, uint32_t seq_num, uint32_t fn, uint32_t result, uint32_t payload_len)
{
    w32(sq + 0, 0); w32(sq + 4, 0x40000); w32(sq + 8, 4096); w32(sq + 12, 63);
    w32(sq + 20, 1); w32(sq + 24, 32); w32(sq + 28, 4096);
    uint8_t *e = sq + 4096 + slot * 4096;
    memset(e, 0, 4096);
    w32(e + 36, seq_num); w32(e + 40, 1);
    w32(e + 48, 0x03000000u); w32(e + 52, 0x43505256u); w32(e + 56, 32 + payload_len); w32(e + 60, fn);
    w32(e + 64, result); w32(e + 68, result);
    for (uint32_t i = 0; i < payload_len; i++) e[80 + i] = (uint8_t)(i + 1);
    uint32_t padded = (48 + 32 + payload_len + 7) & ~7u;
    w32(e + 32, gsp_checksum32(e, padded));
    w32(sq + 16, (slot + 1) % 63);
}

static void test_rpcs(void)
{
    gsp_arena a; gsp_boot_plan p; const char *why = 0;
    gsp_boot_in in = good_input();
    CHECK(build(&a, &in, &p, &why) == 0);
    gsp_mq q;
    CHECK(gsp_mq_attach(&q, &a, &p) == 0 && q.tx_seq == 2);
    uint8_t *cq = at(p.cmdq_pa), *sq = at(p.statq_pa);
    uint32_t seq = 99;

    /* GSP_RM_ALLOC: length covers the fixed part only, params follow in the same element */
    uint8_t params[300];
    fill(params, sizeof params, 7);
    CHECK(gsp_rpc_rm_alloc(&q, 0xc1d00001, 0xc1d00001, 0xcaf00001, 0x0080, params, sizeof params, &seq, &why) == 0 && seq == 0);
    const uint8_t *e = cq + 4096 + 2 * 4096;
    CHECK(check_elem(e, 2, 103, 0, 1) == 64);
    CHECK(r32(e + 80) == 0xc1d00001 && r32(e + 84) == 0xc1d00001 && r32(e + 88) == 0xcaf00001 && r32(e + 92) == 0x80);
    CHECK(r32(e + 96) == 0 && r32(e + 100) == 300 && r32(e + 104) == 0 && r32(e + 108) == 0);
    CHECK(memcmp(e + 112, params, 300) == 0 && e[412] == 0);

    /* GSP_RM_CONTROL spanning two elements */
    uint8_t *big = malloc(5000);
    fill(big, 5000, 3);
    CHECK(gsp_rpc_rm_control(&q, 0xc1d00001, 0xcaf00002, 0x20800110, big, 5000, &seq, &why) == 0 && seq == 1);
    e = cq + 4096 + 3 * 4096;
    CHECK(r32(e + 40) == 2 && r32(e + 56) == 32 + 40 + 5000);
    uint8_t *flat = malloc(8192);
    memcpy(flat, e, 8192);                            /* slots 3 and 4 are adjacent */
    CHECK(check_elem(flat, 3, 76, 1, 2) == 32 + 40 + 5000);
    CHECK(r32(flat + 80) == 0xc1d00001 && r32(flat + 84) == 0xcaf00002 && r32(flat + 88) == 0x20800110 && r32(flat + 96) == 5000);
    CHECK(r64(flat + 112) == 0 && memcmp(flat + 120, big, 5000) == 0);

    /* free and static-info (3 elements, zero payload) */
    CHECK(gsp_rpc_rm_free(&q, 0xc1d00001, 0xcaf00001, &seq, &why) == 0 && seq == 2);
    e = cq + 4096 + 5 * 4096;
    CHECK(check_elem(e, 4, 10, 2, 1) == 48);
    CHECK(r32(e + 80) == 0xc1d00001 && r32(e + 84) == 0 && r32(e + 88) == 0xcaf00001 && r32(e + 92) == 0);
    CHECK(gsp_rpc_get_static_info(&q, 9000, &seq, &why) == 0 && seq == 3);
    e = cq + 4096 + 6 * 4096;
    CHECK(r32(e + 40) == 3 && r32(e + 56) == 9032 && r32(e + 60) == 65);
    CHECK(r32(cq + 16) == 9 && q.tx_seq == 6);
    CHECK(gsp_rpc_get_static_info(&q, 0, &seq, &why) == -1);
    CHECK(gsp_rpc_get_static_info(&q, 65457, &seq, &why) == -1);
    CHECK(gsp_rpc_rm_control(&q, 1, 2, 3, big, 65488 - 32 - 40 + 1, &seq, &why) == -1);

    /* ring full: 62 usable slots, GSP has read none */
    int sent = 0;
    while (gsp_rpc_rm_free(&q, 1, 2, &seq, &why) == 0) sent++;
    CHECK(sent == 62 - 9 && strcmp(why, "command queue full") == 0);
    /* GSP consumes everything (its readPtr is in the status queue at rxHdrOff), then a 2-element
     * control wraps the ring end: slot 62 then slot 0 */
    w32(sq + 32, r32(cq + 16));
    CHECK(r32(cq + 16) == 62);
    CHECK(gsp_rpc_rm_control(&q, 5, 6, 7, big, 5000, &seq, &why) == 0);
    CHECK(r32(cq + 16) == 1);
    memcpy(flat, cq + 4096 + 62 * 4096, 4096);
    memcpy(flat + 4096, cq + 4096, 4096);
    CHECK(check_elem(flat, q.tx_seq - 1, 76, seq, 2) == 32 + 40 + 5000 && memcmp(flat + 120, big, 5000) == 0);
    /* corrupt ring pointer refused */
    w32(sq + 32, 63);
    CHECK(gsp_rpc_rm_free(&q, 1, 2, &seq, &why) == -1 && strcmp(why, "ring pointer corrupt") == 0);
    w32(sq + 32, 1);

    /* payload outside the covered elements refused */
    gsp_seg sg = { big, 4096 };
    CHECK(gsp_mq_send(&q, 1, &sg, 1, 16, 1, &seq, &why) == -1 && strcmp(why, "payload beyond the elements the length covers") == 0);
    CHECK(gsp_mq_send(&q, 1, &sg, 1, 4096, 1, &seq, &why) == 0);
    gsp_seg zs = { 0, 65457 };
    CHECK(gsp_mq_send(&q, 1, &zs, 1, 65457, 1, &seq, &why) == -1 && strcmp(why, "message length out of range") == 0);
    CHECK(gsp_mq_send(&q, 1, &sg, 1, 5000, 1, &seq, &why) == -1 && strcmp(why, "payload shorter than the RPC length") == 0);
    free(big);

    /* status queue: empty, then GSP_INIT_DONE, then checks */
    uint8_t out[65536];
    uint32_t fn = 0, res = 0;
    CHECK(gsp_mq_recv(&q, out, sizeof out, &fn, &res, &why) == -1);   /* not created yet */
    gsp_post(sq, 0, 0, GSP_EV_INIT_DONE, 0, 0);
    w32(sq + 16, 0);
    CHECK(gsp_mq_recv(&q, out, sizeof out, &fn, &res, &why) == 0);    /* created, empty */
    w32(sq + 16, 1);
    CHECK(gsp_mq_recv(&q, out, sizeof out, &fn, &res, &why) == 1 && fn == 0x1001 && res == 0);
    CHECK(r32(cq + 32) == 1 && q.rx_seq == 1);                         /* our readPtr, SWAP_RX */
    CHECK(gsp_mq_recv(&q, out, sizeof out, &fn, &res, &why) == 0);
    gsp_post(sq, 1, 1, 103, 0, 20);
    sq[4096 + 4096 + 90] ^= 1;
    CHECK(gsp_mq_recv(&q, out, sizeof out, &fn, &res, &why) == -1 && strcmp(why, "checksum not zero") == 0);
    gsp_post(sq, 1, 5, 103, 0, 20);
    CHECK(gsp_mq_recv(&q, out, sizeof out, &fn, &res, &why) == -1 && strcmp(why, "bad sequence number") == 0);
    gsp_post(sq, 1, 1, 103, 0, 20);
    CHECK(gsp_mq_recv(&q, out, 64, &fn, &res, &why) == -1 && strcmp(why, "output too small") == 0);
    CHECK(gsp_mq_recv(&q, out, sizeof out, &fn, &res, &why) == 1 && fn == 103 && out[80] == 1 && out[99] == 20);
    gsp_post(sq, 2, 2, 103, 0, 20);
    w32(sq + 4096 + 2 * 4096 + 40, 5);
    CHECK(gsp_mq_recv(&q, out, sizeof out, &fn, &res, &why) == -1 && strcmp(why, "element count bad") == 0);
    free(flat);
}

int main(void)
{
    arena_mem = aligned_alloc(4096, ARENA_CAP);
    elf = malloc(ELF_SIZE); bl = malloc(196608); sig = malloc(SIG_SIZE); fmc = malloc(FMC_SIZE);
    fill(elf, ELF_SIZE, 1); fill(bl, 196608, 2); fill(sig, SIG_SIZE, 3); fill(fmc, FMC_SIZE, 4);
    fill(hash, sizeof hash, 5); fill(key, sizeof key, 6); fill(fsig, sizeof fsig, 7); fill(sysinfo, sizeof sysinfo, 8);
    test_layout();
    test_refusals();
    test_rpcs();
    free(arena_mem); free(elf); free(bl); free(sig); free(fmc);
    return ck_t_verdict("test_gb10_gsp_boot");
}
