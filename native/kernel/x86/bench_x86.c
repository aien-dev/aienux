/* bench_x86.c -- AIENOS GPU bench console (LAB BUILD ONLY: CK_X86_REPORT=1 with
 * CK_X86_BENCH=1; the default image gets the stubs at the bottom).
 *
 * Authenticated GPU register RPC. The Spark sends bench commands (tools/
 * ck_gpu_bench.c) and the kernel executes them against the GPU's BAR5 (256 KiB
 * register aperture) and BAR0 (VRAM aperture), found by pci_x86.c. HMAC-SHA256
 * with a 32-byte key bound into the signed image at build time; a monotone 64-bit
 * counter gives replay protection. Every response is HMAC'd, auth errors included.
 *
 * Safety invariants enforced here:
 *   - BAR5 writes restricted to the display and SMC-mailbox allowlist (bench_proto.h).
 *   - Any read returning 0xFFFFFFFF is "device gone" (status BENCH_ERR_ALLONES).
 *     The kernel answers that request and carries on with the next packet; the
 *     Spark tool is what stops the batch (ck_gpu_bench stops at ALLONES).
 *   - Every write logs the value before the write.
 *   - All waits are TSC-bounded.
 *
 * TRANSPORT:
 *   - Hardware: UDP port 47035 through tg3 (ck_x86_tg3_recv_bench, ck_x86_udp_send,
 *     tg3_x86.c). Compiled in the lab build; NOT tested here (QEMU has no BCM57766).
 *   - CK_X86_BENCH_TEST (QEMU test build only): the same 64-byte packets travel as
 *     text lines over COM1, which QEMU exposes as a socket. Request line:
 *       "ABCHREQ " + 128 hex digits
 *     reply line:
 *       "ABCHRESP " + 128 hex digits
 *     handle_bench_packet is the same code on both paths.
 *
 * DEVICE in the test build: an EMULATED BAR, not PCI emulation. The BARs are RAM
 * stand-ins (test_bar5, test_bar0, test_cfg) with a known word at register 0x10
 * and a word at 0x0FFF preset to 0xFFFFFFFF (a device that has gone). Reads and
 * writes go through the same volatile accesses and allowlist as the real BARs.
 *
 * This file qualifies nothing physical. */
#include "../core/ck_internal.h"
#include "../../native/argus/sha256.h"
#include "x86.h"
#include "bench_proto.h"
#include "report_x86.h"

#ifndef CK_X86_BENCH
#define CK_X86_BENCH 0
#endif

#if CK_X86_BENCH

/* HMAC-SHA256 key bound into the signed image: the build's own key from a
 * header made in the build dir (make CK_X86_BENCH_KEY_FILE=..., never on a
 * command line), else the test key. */
#ifdef CK_X86_BENCH_KEY_H
#include CK_X86_BENCH_KEY_H
#define BENCH_KEY_KIND "this image's own key"
#endif
#ifndef CK_X86_BENCH_KEY
#define CK_X86_BENCH_KEY BENCH_TEST_KEY_INIT
#define BENCH_KEY_KIND "the TEST key"
#endif
#ifndef BENCH_KEY_KIND
#define BENCH_KEY_KIND "a key set at build time"
#endif
static const uint8_t bench_key[32] = { CK_X86_BENCH_KEY };

/* GPU device state populated by ck_x86_bench_init. */
static volatile uint32_t *bar5;          /* 256 KiB register aperture */
static volatile uint32_t *bar0;          /* VRAM aperture (first 256 KiB) */
static volatile uint8_t  *pci_cfg;       /* PCI config space (ECAM) */
static int bench_dev_ready;

/* Monotone counter: reject any request with counter <= last_counter. */
static uint64_t last_counter;

#ifdef CK_X86_BENCH_TEST
/* RAM stand-in for the BARs (QEMU test build only; see the header). */
#define TEST_BAR5_WORDS  0x10000u
#define TEST_BAR0_WORDS  0x10000u
#define TEST_ALLONES_REG 0x0FFFu   /* read as 0xFFFFFFFF: the device is gone */
#define TEST_KNOWN_REG   0x0010u   /* read as 0xC0FFEE00 */
static volatile uint32_t test_bar5[TEST_BAR5_WORDS];
static volatile uint32_t test_bar0[TEST_BAR0_WORDS];
static volatile uint8_t  test_cfg[256];
static int test_inited;
static uint64_t test_tsc_hz;
#define COM1_BASE 0x3f8u
#endif

/* ---- Inline HMAC-SHA256 (uses argus/sha256.h, already in the build) ---- */
static void bench_hmac(const uint8_t key[32], const uint8_t *msg, size_t len, uint8_t out[32])
{
    uint8_t ipad[64], opad[64], inner[32];
    for (unsigned i = 0; i < 32; i++) {
        ipad[i] = key[i] ^ 0x36u;
        opad[i] = key[i] ^ 0x5cu;
    }
    for (unsigned i = 32; i < 64; i++) {
        ipad[i] = 0x36u;
        opad[i] = 0x5cu;
    }
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, ipad, 64);
    sha256_update(&c, msg, len);
    sha256_final(&c, inner);
    sha256_init(&c);
    sha256_update(&c, opad, 64);
    sha256_update(&c, inner, 32);
    sha256_final(&c, out);
}

/* Constant-time 32-byte compare (avoid timing oracle on HMAC check). */
static int ct_eq32(const uint8_t *a, const uint8_t *b)
{
    uint8_t diff = 0;
    for (unsigned i = 0; i < 32; i++) diff |= a[i] ^ b[i];
    return diff == 0;
}

/* ---- Register allowlist ---- */
static int bar5_write_allowed(uint32_t reg_idx)
{
    return (reg_idx >= BENCH_BAR5_WRITE_LO0 && reg_idx <= BENCH_BAR5_WRITE_HI0) ||
           (reg_idx >= BENCH_BAR5_WRITE_LO1 && reg_idx <= BENCH_BAR5_WRITE_HI1) ||
           (reg_idx >= BENCH_BAR5_WRITE_LO2 && reg_idx <= BENCH_BAR5_WRITE_HI2) ||
           (reg_idx >= BENCH_BAR5_WRITE_LO3 && reg_idx <= BENCH_BAR5_WRITE_HI3) ||
           (reg_idx >= BENCH_BAR5_WRITE_LO4 && reg_idx <= BENCH_BAR5_WRITE_HI4);
}

/* ---- Command execution ---- */
static uint8_t exec_cmd(const uint8_t *req, uint32_t *result_out, uint32_t *before_out,
                         uint64_t tsc_hz)
{
    uint8_t cmd   = req[5];
    uint32_t arg0 = bench_r32(req + 16);
    uint32_t arg1 = bench_r32(req + 20);
    uint32_t arg2 = bench_r32(req + 24);
    uint32_t arg3 = bench_r32(req + 28);
    *result_out = 0;
    *before_out = 0;

    if (!bench_dev_ready && cmd != BENCH_CMD_PING)
        return BENCH_ERR_NODEV;

    switch (cmd) {
    case BENCH_CMD_PING:
        return BENCH_OK;

    case BENCH_CMD_PCI_CFG: {
        /* dword-aligned offsets inside the 256-byte config space (no wraparound) */
        if (arg0 > 252u || (arg0 & 3u)) return BENCH_ERR_RANGE;
        uint32_t v = *(volatile uint32_t *)(pci_cfg + arg0);
        if (v == BENCH_ALLONES) return BENCH_ERR_ALLONES;
        *result_out = v;
        return BENCH_OK;
    }
    case BENCH_CMD_BAR5_READ: {
        if (arg0 > BENCH_BAR5_READ_MAX) return BENCH_ERR_RANGE;
        uint32_t v = bar5[arg0];
        if (v == BENCH_ALLONES) return BENCH_ERR_ALLONES;
        *result_out = v;
        return BENCH_OK;
    }
    case BENCH_CMD_BAR5_WRITE: {
        if (!bar5_write_allowed(arg0)) return BENCH_ERR_RANGE;
        uint32_t before = bar5[arg0];
        if (before == BENCH_ALLONES) return BENCH_ERR_ALLONES;
        bar5[arg0] = arg1;
        uint32_t after = bar5[arg0];
        ck_printf("bench: bar5_write reg=0x%x 0x%08x -> 0x%08x\n", arg0, before, after);
        *before_out = before;
        *result_out = after;
        return BENCH_OK;
    }
    case BENCH_CMD_BAR0_READ: {
        if (arg0 >= BENCH_BAR0_MAX_WORDS) return BENCH_ERR_RANGE;
        uint32_t v = bar0[arg0];
        if (v == BENCH_ALLONES) return BENCH_ERR_ALLONES;
        *result_out = v;
        return BENCH_OK;
    }
    case BENCH_CMD_BAR0_WRITE: {
        if (arg0 >= BENCH_BAR0_MAX_WORDS) return BENCH_ERR_RANGE;
        uint32_t before = bar0[arg0];
        if (before == BENCH_ALLONES) return BENCH_ERR_ALLONES;
        bar0[arg0] = arg1;
        ck_printf("bench: bar0_write off=0x%x 0x%08x -> 0x%08x\n", arg0, before, bar0[arg0]);
        *before_out = before;
        *result_out = bar0[arg0];
        return BENCH_OK;
    }
    case BENCH_CMD_BAR5_POLL: {
        if (arg0 > BENCH_BAR5_READ_MAX) return BENCH_ERR_RANGE;
        uint64_t t0 = ck_rdtsc(), limit = t0 + (uint64_t)tsc_hz / 1000 * arg3;
        uint32_t v;
        do {
            v = bar5[arg0];
            if (v == BENCH_ALLONES) return BENCH_ERR_ALLONES;
            if ((v & arg1) == (arg2 & arg1)) { *result_out = v; return BENCH_OK; }
            ck_pause();
        } while (ck_rdtsc() < limit);
        *result_out = v;
        return BENCH_ERR_TIMEOUT;
    }
    case BENCH_CMD_BAR5_RMW: {
        if (!bar5_write_allowed(arg0)) return BENCH_ERR_RANGE;
        uint32_t before = bar5[arg0];
        if (before == BENCH_ALLONES) return BENCH_ERR_ALLONES;
        uint32_t newval = (before & ~arg1) | (arg2 & arg1);
        bar5[arg0] = newval;
        uint32_t after = bar5[arg0];
        ck_printf("bench: bar5_rmw reg=0x%x 0x%08x -> 0x%08x\n", arg0, before, after);
        *before_out = before;
        *result_out = after;
        (void)arg3;
        return BENCH_OK;
    }
    default:
        return BENCH_ERR_CMD;
    }
}

/* ---- Protocol: build a response, HMAC'd, and send it ---- */
/* Builds one response into out[BENCH_RESP_BYTES]. Returns byte count. */
static size_t build_resp(uint8_t status, uint64_t counter, uint64_t tsc,
                          uint32_t result, uint32_t before, uint8_t out[BENCH_RESP_BYTES])
{
    memset(out, 0, BENCH_RESP_BYTES);
    memcpy(out, BENCH_MAGIC_RESP, 4);
    out[4] = BENCH_VERSION;
    out[5] = status;
    bench_w64(out + 8,  counter);
    bench_w64(out + 16, tsc);
    bench_w32(out + 24, result);
    bench_w32(out + 28, before);
    bench_hmac(bench_key, out, BENCH_HMAC_OFF, out + BENCH_HMAC_OFF);
    return BENCH_RESP_BYTES;
}

#ifdef CK_X86_BENCH_TEST
static void test_reply(const uint8_t body[BENCH_RESP_BYTES])
{
    static const char hex[] = "0123456789abcdef";
    char line[9 + 2 * BENCH_RESP_BYTES + 2];
    memcpy(line, "ABCHRESP ", 9);
    for (unsigned i = 0; i < BENCH_RESP_BYTES; i++) {
        line[9 + 2 * i]     = hex[body[i] >> 4];
        line[9 + 2 * i + 1] = hex[body[i] & 15];
    }
    line[9 + 2 * BENCH_RESP_BYTES]     = '\n';
    line[9 + 2 * BENCH_RESP_BYTES + 1] = 0;
    ck_puts(line);
}
#endif

static void reply(const uint8_t src_mac[6], const uint8_t body[BENCH_RESP_BYTES])
{
#ifdef CK_X86_BENCH_TEST
    (void)src_mac;
    test_reply(body);
#else
    ck_x86_udp_send(src_mac, body, BENCH_RESP_BYTES, CK_X86_BENCH_PORT, CK_X86_BENCH_PORT);
#endif
}

static void handle_bench_packet(const uint8_t *pkt, size_t pkt_len,
                                  const uint8_t src_mac[6], uint64_t tsc_hz)
{
    if (pkt_len < BENCH_REQ_BYTES) return;
    if (memcmp(pkt, BENCH_MAGIC_REQ, 4) != 0) return;
    if (pkt[4] != BENCH_VERSION) return;

    /* Verify HMAC (covers bytes 0..31, hmac is at 32..63). An auth failure is
     * answered with BENCH_ERR_AUTH and the counter field echoed as 0. */
    uint8_t expected[32];
    bench_hmac(bench_key, pkt, BENCH_HMAC_OFF, expected);
    uint8_t body[BENCH_RESP_BYTES];
    if (!ct_eq32(expected, pkt + BENCH_HMAC_OFF)) {
        build_resp(BENCH_ERR_AUTH, 0, ck_rdtsc(), 0, 0, body);
        reply(src_mac, body);
        return;
    }

    uint64_t counter = bench_r64(pkt + 8);
    if (counter <= last_counter) {          /* replay or stale counter */
        build_resp(BENCH_ERR_AUTH, counter, ck_rdtsc(), 0, 0, body);
        reply(src_mac, body);
        return;
    }
    last_counter = counter;

    uint32_t result = 0, before = 0;
    uint8_t status = exec_cmd(pkt, &result, &before, tsc_hz);
    build_resp(status, counter, ck_rdtsc(), result, before, body);
    reply(src_mac, body);
}

#ifdef CK_X86_BENCH_TEST
/* ---- Test transport: request lines on COM1 input ---- */
static int hexnib(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static char rxline[8 + 2 * BENCH_REQ_BYTES + 8];
static unsigned rxn;

static void test_line(void)
{
    if (rxn != 8 + 2 * BENCH_REQ_BYTES || memcmp(rxline, "ABCHREQ ", 8) != 0)
        return;
    uint8_t pkt[BENCH_REQ_BYTES];
    for (unsigned i = 0; i < BENCH_REQ_BYTES; i++) {
        int hi = hexnib(rxline[8 + 2 * i]), lo = hexnib(rxline[9 + 2 * i]);
        if (hi < 0 || lo < 0) return;
        pkt[i] = (uint8_t)(hi << 4 | lo);
    }
    static const uint8_t no_mac[6];
    handle_bench_packet(pkt, sizeof pkt, no_mac, test_tsc_hz);
}

static void test_serial_poll(void)
{
    if (ck_inb(COM1_BASE + 5) == 0xff) return;        /* no 16550 here */
    while (ck_inb(COM1_BASE + 5) & 0x01) {
        char c = (char)ck_inb(COM1_BASE);
        if (c == '\r') continue;
        if (c == '\n') {
            if (rxn) { rxline[rxn] = 0; test_line(); }
            rxn = 0;
            continue;
        }
        if (rxn < sizeof rxline - 1) rxline[rxn++] = c;
        else rxn = 0;                                 /* over-long line: drop it */
    }
}

/* The emulated BARs (see the header). */
static void test_bar_init(void)
{
    if (test_inited) return;
    test_inited = 1;
    *(volatile uint32_t *)(volatile void *)test_cfg = 0x67ef1002u;   /* 1002:67ef */
    test_bar5[TEST_KNOWN_REG]   = 0xC0FFEE00u;
    test_bar5[TEST_ALLONES_REG] = BENCH_ALLONES;
    pci_cfg = test_cfg;
    bar5    = test_bar5;
    bar0    = test_bar0;
    bench_dev_ready = 1;
    ck_puts("bench: TEST BUILD: BARs are RAM stand-ins (emulated), not PCI hardware\n");
}
#endif

/* ---- Public API ---- */

/* "ready" means the main loop polls the console now: ck_gpu_bench sends its first request on this
 * line, and a request sent while the kernel is still booting waits unanswered (the tool gives up). */
static void announce_ready(void)
{
    static int said;
    if (said || !bench_dev_ready)
        return;
    said = 1;
    /* one format string, so the line goes out whole (the test tool's requests share the wire) */
    ck_printf("bench: GPU bench console ready port=%u (HMAC-SHA256, " BENCH_KEY_KIND " bound in image)\n",
              (unsigned)CK_X86_BENCH_PORT);
}

void ck_x86_bench_init(volatile uint8_t *gpu_cfg, volatile uint32_t *gpu_bar5,
                        volatile uint32_t *gpu_bar0)
{
#ifdef CK_X86_BENCH_TEST
    (void)gpu_cfg; (void)gpu_bar5; (void)gpu_bar0;
    test_bar_init();
#else
    if (!gpu_cfg || !gpu_bar5 || !gpu_bar0) {
        ck_puts("bench: GPU not found (no BAR5/BAR0); bench console disabled\n");
        return;
    }
    pci_cfg = gpu_cfg;
    bar5    = gpu_bar5;
    bar0    = gpu_bar0;
    bench_dev_ready = 1;
    ck_puts("bench: GPU registers mapped (BAR5, BAR0); the console answers from the main loop\n");
#endif
}

void ck_x86_bench_step(uint64_t tsc_hz)
{
    announce_ready();
#ifdef CK_X86_BENCH_TEST
    test_tsc_hz = tsc_hz;
    if (test_inited)
        test_serial_poll();
#else
    /* Poll for a received bench packet from tg3 (LAB BUILD, untested on hardware). */
    static uint8_t pkt[2048];
    size_t pkt_len = 0;
    uint8_t src_mac[6];
    /* ck_x86_tg3_recv_bench: fills pkt, pkt_len with the UDP payload (no IP/UDP
     * headers) and src_mac when a bench packet arrives on port 47035. */
    if (ck_x86_tg3_recv_bench(pkt, sizeof pkt, &pkt_len, src_mac) == 0 && pkt_len)
        handle_bench_packet(pkt, pkt_len, src_mac, tsc_hz);
#endif
}

#else  /* !CK_X86_BENCH */
void ck_x86_bench_init(volatile uint8_t *cfg, volatile uint32_t *b5, volatile uint32_t *b0)
{ (void)cfg; (void)b5; (void)b0; }
void ck_x86_bench_step(uint64_t tsc_hz) { (void)tsc_hz; }
#endif /* CK_X86_BENCH */
