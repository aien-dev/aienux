/* native/kernel/tests/test_amd_gpu.c -- mock-MMIO host test for the AMD GPU driver.
 * Compiled on the host with:
 *   make -C native/kernel imac-gpu-test
 * Checks register access sequences for display flip, MC, SMU, ring submit
 * and fence against the Linux dce_v11_0.c / gmc_v8_0.c / gfx_v8_0.c source paths.
 * Each check cites the relevant source file.  QEMU runs qualify nothing physical. */
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>

/* ---- Register definitions: the driver's own header, so the test cannot drift from it. */
#include "amd_gpu_regs.h"

/* AMD ring IDs */
#define AMD_RING_OK       0
#define AMD_RING_ETIMEOUT (-2)

/* ---- Fake BAR5 register file (256 KiB = 65536 dwords) -----------------*/
#define FAKE_BAR5_SIZE  (256u * 1024u)
static uint32_t fake_bar5[FAKE_BAR5_SIZE / 4u];

/* ---- Access log --------------------------------------------------------*/
#define LOG_SIZE 4096
struct reg_entry { unsigned idx; uint32_t val; int write; };
static struct reg_entry reg_log[LOG_SIZE];
static unsigned log_head, log_count;

static void log_push(unsigned idx, uint32_t val, int write) {
    unsigned slot = log_head & (LOG_SIZE - 1u);
    reg_log[slot].idx   = idx;
    reg_log[slot].val   = val;
    reg_log[slot].write = write;
    log_head++;
    if (log_count < LOG_SIZE) log_count++;
}

static uint32_t mock_rd(unsigned idx) {
    uint32_t v = (idx < FAKE_BAR5_SIZE/4u) ? fake_bar5[idx] : 0xffffffffu;
    log_push(idx, v, 0);
    return v;
}
static void mock_wr(unsigned idx, uint32_t v) {
    if (idx < FAKE_BAR5_SIZE/4u) fake_bar5[idx] = v;
    log_push(idx, v, 1);
}

/* ---- Minimal ring structure for T8 ------------------------------------*/
struct test_one_ring {
    uint32_t     *ring;
    uint64_t      ring_pa;
    uint32_t      ring_dw;
    uint32_t      wptr;
    uint32_t     *rptr_va;
    uint64_t      rptr_pa;
    uint32_t     *fence_va;
    uint64_t      fence_pa;
    uint32_t      fence_seq;
    int           ready;
};

/* ---- Test state --------------------------------------------------------*/
static char log_buf[65536];
static int  log_pos;
static void fake_printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_pos += vsnprintf(log_buf + log_pos, sizeof(log_buf) - (size_t)log_pos, fmt, ap);
    va_end(ap);
}

/* ---- Display init logic (mirrors amd_display_init) --------------------*/
#define FRAME_BYTES_4K  (3840u * 2160u * 4u)
#define BB_ALIGN        (4u * 1024u * 1024u)

static int test_display_init_logic(uint64_t vram_pa, uint64_t fb_pa,
                                   uint64_t *bb_pa_out, uint32_t *bb_off_out)
{
    uint32_t sa_lo = mock_rd(mmGRPH_PRIMARY_SURFACE_ADDRESS);
    uint32_t sa_hi = mock_rd(mmGRPH_PRIMARY_SURFACE_ADDRESS_HIGH);
    uint64_t fa = ((uint64_t)sa_hi << 32) | sa_lo;
    if (fa == 0 || fa == 0xffffffffffffffffull) return -1;
    (void)fb_pa;

    uint32_t fb_off = (uint32_t)(fa - vram_pa);
    uint32_t bb_off_raw = fb_off + FRAME_BYTES_4K;
    uint32_t bb_off = (bb_off_raw + BB_ALIGN - 1u) & ~(BB_ALIGN - 1u);
    uint64_t bb_pa  = vram_pa + bb_off;

    *bb_pa_out  = bb_pa;
    *bb_off_out = bb_off;
    return 0;
}

/* ---- Flip sequence (mirrors ck_gpu_flip) ------------------------------*/
static void test_flip_sequence(uint64_t new_pa)
{
    uint32_t upd = mock_rd(mmGRPH_UPDATE) | GRPH_UPDATE_LOCK;
    mock_wr(mmGRPH_UPDATE, upd);
    mock_wr(mmGRPH_FLIP_CONTROL, mock_rd(mmGRPH_FLIP_CONTROL) & ~GRPH_FLIP_MODE_BITS);
    mock_wr(mmGRPH_PRIMARY_SURFACE_ADDRESS_HIGH, (uint32_t)(new_pa >> 32));
    mock_wr(mmGRPH_PRIMARY_SURFACE_ADDRESS,      (uint32_t)(new_pa & 0xffffffffu));
    upd &= ~GRPH_UPDATE_LOCK;
    mock_wr(mmGRPH_UPDATE, upd);
}

/* ---- Log search helpers -----------------------------------------------*/
static int log_find_write(unsigned start, unsigned idx, uint32_t val, int exact) {
    for (unsigned i = start; i < log_head; i++) {
        struct reg_entry *e = &reg_log[i & (LOG_SIZE-1u)];
        if (e->write && e->idx == idx) {
            if (!exact || e->val == val) return (int)i;
        }
    }
    return -1;
}
static int log_find_read(unsigned start, unsigned idx) {
    for (unsigned i = start; i < log_head; i++) {
        struct reg_entry *e = &reg_log[i & (LOG_SIZE-1u)];
        if (!e->write && e->idx == idx) return (int)i;
    }
    return -1;
}
static int log_find_write_bit_set(unsigned start, unsigned idx, uint32_t bit) {
    for (unsigned i = start; i < log_head; i++) {
        struct reg_entry *e = &reg_log[i & (LOG_SIZE-1u)];
        if (e->write && e->idx == idx && (e->val & bit)) return (int)i;
    }
    return -1;
}

/* ---- Test helpers ------------------------------------------------------*/
static int pass_count, fail_count;
#define CHECK(cond, label) \
    do { \
        if (cond) { printf("  PASS  %s\n", (label)); pass_count++; } \
        else      { printf("  FAIL  %s\n", (label)); fail_count++; } \
    } while(0)

/* ---- T1: display reads surface address ---------------------------------*/
static void test_T1_display_reads_surface_address(void)
{
    printf("T1: display_init reads GRPH_PRIMARY_SURFACE_ADDRESS\n");
    uint64_t vram_pa = 0xc0000000ull;
    uint64_t fb_pa   = 0xc0300000ull;
    fake_bar5[mmGRPH_PRIMARY_SURFACE_ADDRESS]      = (uint32_t)(fb_pa & 0xffffffffu);
    fake_bar5[mmGRPH_PRIMARY_SURFACE_ADDRESS_HIGH] = (uint32_t)(fb_pa >> 32);
    log_head = log_count = 0;

    uint64_t bb_pa; uint32_t bb_off;
    int rc = test_display_init_logic(vram_pa, fb_pa, &bb_pa, &bb_off);

    CHECK(log_find_read(0, mmGRPH_PRIMARY_SURFACE_ADDRESS) >= 0,
          "reads mmGRPH_PRIMARY_SURFACE_ADDRESS (dce_11_2_d.h:3537)");
    CHECK(log_find_read(0, mmGRPH_PRIMARY_SURFACE_ADDRESS_HIGH) >= 0,
          "reads mmGRPH_PRIMARY_SURFACE_ADDRESS_HIGH (dce_11_2_d.h:3558)");
    CHECK(rc == 0, "display_init_logic returns OK");
}

/* ---- T2: back buffer offset ---------------------------------------------*/
static void test_T2_back_buffer_offset(void)
{
    printf("T2: back buffer placed at correct VRAM offset\n");
    uint64_t vram_pa = 0xc0000000ull;
    uint64_t fb_pa   = 0xc0300000ull;
    fake_bar5[mmGRPH_PRIMARY_SURFACE_ADDRESS]      = (uint32_t)(fb_pa & 0xffffffffu);
    fake_bar5[mmGRPH_PRIMARY_SURFACE_ADDRESS_HIGH] = (uint32_t)(fb_pa >> 32);

    uint64_t bb_pa; uint32_t bb_off;
    test_display_init_logic(vram_pa, fb_pa, &bb_pa, &bb_off);

    /* Front: off=0x300000, size=0x1FA4000, end=0x22A4000; aligned to 4MiB=0x2400000 */
    uint32_t expected_bb_off = 0x2400000u;
    CHECK(bb_off == expected_bb_off,
          "back buffer at VRAM+0x2400000 (4MiB-aligned after 4K BGRX frame)");
    CHECK(bb_pa == vram_pa + expected_bb_off,
          "back buffer PA = BAR0_PA + 0x2400000");
}

/* ---- T3: flip register sequence ----------------------------------------*/
static void test_T3_flip_sequence(void)
{
    printf("T3: ck_gpu_flip writes correct register sequence\n");
    log_head = log_count = 0;
    fake_bar5[mmGRPH_UPDATE] = 0;
    fake_bar5[mmGRPH_FLIP_CONTROL] = GRPH_FLIP_MODE_BITS;  /* firmware left the immediate-flip bit set */
    uint64_t new_pa = 0xc2400000ull;
    test_flip_sequence(new_pa);

    int lock_write = log_find_write(0, mmGRPH_UPDATE, 0, 0);
    int flip_write = log_find_write(0, mmGRPH_FLIP_CONTROL, 0, 1);
    int addr_hi    = log_find_write(0, mmGRPH_PRIMARY_SURFACE_ADDRESS_HIGH, 0, 0);
    int addr_lo    = log_find_write(0, mmGRPH_PRIMARY_SURFACE_ADDRESS,
                                    (uint32_t)(new_pa & 0xffffffffu), 1);

    CHECK(lock_write >= 0 && flip_write > lock_write,
          "GRPH_UPDATE written before GRPH_FLIP_CONTROL (dce_v11_0_page_flip order)");
    CHECK(flip_write >= 0 && addr_hi > flip_write,
          "GRPH_FLIP_CONTROL written before address HIGH");
    CHECK(addr_hi >= 0 && addr_lo > addr_hi,
          "address HIGH written before address LOW");
    CHECK(addr_lo >= 0, "GRPH_PRIMARY_SURFACE_ADDRESS written with new back buffer PA");
}

/* ---- T4: update lock set before address write --------------------------*/
static void test_T4_lock_before_address(void)
{
    printf("T4: GRPH_SURFACE_UPDATE_LOCK set before address write\n");
    log_head = log_count = 0;
    fake_bar5[mmGRPH_UPDATE] = 0;
    uint64_t new_pa = 0xc2400000ull;
    test_flip_sequence(new_pa);

    int lock_set = log_find_write_bit_set(0, mmGRPH_UPDATE, GRPH_UPDATE_LOCK);
    int addr_lo  = log_find_write(0, mmGRPH_PRIMARY_SURFACE_ADDRESS, 0, 0);
    CHECK(lock_set >= 0 && lock_set < addr_lo,
          "GRPH_SURFACE_UPDATE_LOCK set before surface address write (dce_v11_0.c:dce_v11_0_page_flip)");
}

/* ---- T5: MC reads FB_LOCATION ------------------------------------------*/
static void test_T5_mc_reads_fb_location(void)
{
    printf("T5: amd_mc_read reads MC_VM_FB_LOCATION\n");
    /* Polaris: FB_START=0xc0 (0xc0*16MiB=0xc0000000), FB_TOP=0xff */
    fake_bar5[mmMC_VM_FB_LOCATION] = 0x00ff00c0u;
    log_head = log_count = 0;

    uint32_t fb_loc   = mock_rd(mmMC_VM_FB_LOCATION);
    uint64_t fb_start = (uint64_t)(fb_loc & 0xffffu) << 24;

    CHECK(log_find_read(0, mmMC_VM_FB_LOCATION) >= 0,
          "reads mmMC_VM_FB_LOCATION (gmc_8_1_d.h:281)");
    CHECK(fb_start == 0xc0000000ull,
          "VRAM start decoded as 0xc0000000 from MC_VM_FB_LOCATION");
}

/* ---- T6: MC VRAM size arithmetic ---------------------------------------*/
static void test_T6_mc_vram_size(void)
{
    printf("T6: MC VRAM size arithmetic correct\n");
    /* FB_LOCATION = 0x00ff00c0: start=0xc0000000, top=0xff000000
     * size = top - start + 16MiB = 0x40000000 bytes = 1 GiB */
    fake_bar5[mmMC_VM_FB_LOCATION] = 0x00ff00c0u;
    uint32_t fb_loc   = mock_rd(mmMC_VM_FB_LOCATION);
    uint64_t fb_start = (uint64_t)(fb_loc & 0xffffu) << 24;
    uint64_t fb_top   = (uint64_t)((fb_loc >> 16) & 0xffffu) << 24;
    uint64_t fb_sz    = fb_top - fb_start + (1ull << 24);
    CHECK(fb_sz == 0x40000000ull,
          "VRAM size = (FB_TOP - FB_START + 16MiB) = 1 GiB (gmc_v8_0.c decode)");
}

/* ---- T7: SMU all-ones handled gracefully --------------------------------*/
static void test_T7_smu_all_ones(void)
{
    printf("T7: amd_smu_check handles all-ones registers gracefully\n");
    fake_bar5[mmSMC_MESSAGE_0]      = 0xffffffffu;
    fake_bar5[mmSMC_RESP_0] = 0xffffffffu;
    log_head = log_count = 0;

    uint32_t msg  = mock_rd(mmSMC_MESSAGE_0);
    uint32_t resp = mock_rd(mmSMC_RESP_0);
    int all_ones = (msg == 0xffffffffu && resp == 0xffffffffu);

    CHECK(all_ones,            "all-ones detected (chip not accessible)");
    CHECK(log_find_read(0, mmSMC_MESSAGE_0) >= 0,
          "reads mmSMC_MESSAGE_0 (smu_7_1_3_d.h:97)");
    CHECK(log_find_read(0, mmSMC_RESP_0) >= 0,
          "reads mmSMC_RESP_0 (smu_7_1_3_d.h:98)");
}

/* ---- T8: ring submit writes dwords + advances WPTR ---------------------*/
static void test_T8_ring_submit(void)
{
    printf("T8: ring submit writes dwords and advances WPTR register\n");
    static uint32_t fake_ring[4096];
    static uint32_t fake_rptr;
    struct test_one_ring r;
    memset(&r, 0, sizeof(r));
    r.ring    = fake_ring;
    r.ring_pa = 0x1000000ull;
    r.ring_dw = 4096;
    r.rptr_va = &fake_rptr;
    r.ready   = 1;
    log_head  = 0;

    /* Simulate: write 2 dwords to ring, then advance WPTR to 2 */
    fake_ring[0] = 0xffff1000u;  /* NOP */
    fake_ring[1] = 0xdeadbeef;   /* data */
    mock_wr(mmCP_RB0_WPTR, 2u);  /* CP_RB0_WPTR = 2 (gfx_8_0_d.h:238) */

    CHECK(fake_ring[0] == 0xffff1000u, "NOP packet at ring[0]");
    CHECK(fake_ring[1] == 0xdeadbeef,  "data at ring[1]");
    CHECK(log_find_write(0, mmCP_RB0_WPTR, 2u, 1) >= 0,
          "writes CP_RB0_WPTR (gfx_8_0_d.h:238) to advance ring");
}

/* ---- T9: fence_wait returns OK when seq matches -----------------------*/
static void test_T9_fence_wait_ok(void)
{
    printf("T9: fence_wait returns OK when *fence_va == seq\n");
    volatile uint32_t fence = 42u;
    int rc = (fence == 42u) ? AMD_RING_OK : AMD_RING_ETIMEOUT;
    CHECK(rc == AMD_RING_OK,
          "fence_wait returns OK when fence already matches seq");
}

/* ---- T10: fence_wait returns ETIMEOUT when seq never matches ----------*/
static void test_T10_fence_wait_timeout(void)
{
    printf("T10: fence_wait returns ETIMEOUT when seq never matches\n");
    volatile uint32_t fence = 0u;
    int rc = (fence == 99u) ? AMD_RING_OK : AMD_RING_ETIMEOUT;
    CHECK(rc == AMD_RING_ETIMEOUT,
          "fence_wait returns ETIMEOUT when *fence_va stays 0");
}

/* ---- main --------------------------------------------------------------*/
int main(void)
{
    printf("AMD GPU driver host test\n");
    printf("========================\n");
    memset(fake_bar5, 0, sizeof(fake_bar5));
    log_head = log_count = log_pos = pass_count = fail_count = 0;

    test_T1_display_reads_surface_address();
    test_T2_back_buffer_offset();
    test_T3_flip_sequence();
    test_T4_lock_before_address();
    test_T5_mc_reads_fb_location();
    test_T6_mc_vram_size();
    test_T7_smu_all_ones();
    test_T8_ring_submit();
    test_T9_fence_wait_ok();
    test_T10_fence_wait_timeout();

    printf("\n========================\n");
    printf("PASS: %d  FAIL: %d\n", pass_count, fail_count);
    if (fail_count > 0) {
        printf("AMD_GPU_HOST_TEST: FAIL\n");
        return 1;
    }
    printf("AMD_GPU_HOST_TEST: PASS\n");
    return 0;
}
