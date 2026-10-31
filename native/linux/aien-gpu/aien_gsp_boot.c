/* aien_gsp_boot: the GB10 GSP start-up from user space over /dev/aien-gpu (L332 rung 2, window 1).
 *
 * Our own code end to end, NVIDIA's signed firmware on loan:
 *   1. reads the boot package (rung2/window1-package.sh): GSP-RM .fwimage + its gb10y signature, GspRmBoot_GB10B image,
 *      GspRmFmcGfwProdSigned_GB10B image + hash + public key + signature, the 936-byte GspSystemInfo;
 *   2. builds every block into the module's DMA arena with L192's gsp_boot_build (native/kernel/dev/gb10_gsp_boot.c),
 *      the arena's IOVA as the chip address and the module's heap buffer as the sysmem heap;
 *   3. runs the 11 register steps of native/kernel/gpu/gsp_mmio.c over AIEN_GPU_REG_RD / AIEN_GPU_REG_WR (the module
 *      lets only the start-up contract's registers through and rings the SEC2 doorbell once per load);
 *   4. if GSP-RM's core runs, reads the status queue (gsp_mq_recv) until GSP_INIT_DONE, a sequencer request (not
 *      handled in window 1: recorded, then stop) or the time limit;
 *   5. writes every register access with its time, every status message, the GSP log buffers and a one-line verdict
 *      (AIEN_GSP_BOOT: ...) into the output directory. Pass or fail, all of it is evidence.
 *
 * Built with -DAIEN_GSP_BOOT_SIM it runs the same code against the simulated SEC2 + GSP (gpu/gsp_sim.c) and a fake GSP
 * that answers on the status queue, so every path is exercised on a host with no GPU (rung2/window1_sim_test.sh).
 *
 *   aien_gsp_boot --pkg DIR --app-version N --out DIR [--polls N] [--pause-us N] [--wait-ms N] [--plan-only]
 *   sim build only: [--sim-events init|seq|none|junk] [--sim-cfg happy|busy|fmc-error|riscv-dead] [--sim-arena-mib N]
 * Exit 0 = INIT-DONE (or PLAN-ONLY), 1 = the chip stopped somewhere (verdict says where), 2 = refused before the chip.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "gb10_gsp_boot.h"
#include "gsp_mmio.h"
#include "gsp_seq.h"
#ifdef AIEN_GSP_BOOT_SIM
#include "gsp_sim.h"
static uint8_t *volatile g_host_arena;   /* the simulated arena; volatile so the store survives -O and the leak check sees it */
#else
#include "aien_gpu_uapi.h"
#endif

#define MSG_CAP (GSP_ELEM_SIZE_MAX + 8u)

static const char *g_out;
static FILE *g_trace;
static struct timespec g_t0;

static uint64_t now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)((t.tv_sec - g_t0.tv_sec) * 1000000 + (t.tv_nsec - g_t0.tv_nsec) / 1000);
}

static void die(const char *verdict, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "aien_gsp_boot: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    printf("AIEN_GSP_BOOT: %s\n", verdict);
    exit(2);
}

static FILE *out_file(const char *name, const char *mode)
{
    char p[4096];
    snprintf(p, sizeof p, "%s/%s", g_out, name);
    FILE *f = fopen(p, mode);
    if (!f)
        die("TOOL-ERROR", "%s: %s", p, strerror(errno));
    return f;
}

static void out_bytes(const char *name, const void *p, size_t n)
{
    FILE *f = out_file(name, "wb");
    if (n && fwrite(p, 1, n, f) != n)
        die("TOOL-ERROR", "write %s", name);
    fclose(f);
}

/* every loaded package file stays reachable until exit (one-shot tool; the sanitized sim build checks for leaks) */
static uint8_t *volatile g_loaded[8];
static unsigned g_nloaded;

static uint8_t *load(const char *dir, const char *name, size_t *n, size_t want)
{
    char p[4096];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "rb");
    if (!f)
        die("PACKAGE-REFUSED", "%s: %s", p, strerror(errno));
    struct stat st;
    if (fstat(fileno(f), &st) || st.st_size <= 0)
        die("PACKAGE-REFUSED", "%s: empty", p);
    if (want && (size_t)st.st_size != want)
        die("PACKAGE-REFUSED", "%s: %lld bytes, want %zu", p, (long long)st.st_size, want);
    uint8_t *b = malloc((size_t)st.st_size);
    if (!b || fread(b, 1, (size_t)st.st_size, f) != (size_t)st.st_size)
        die("PACKAGE-REFUSED", "%s: read", p);
    fclose(f);
    if (g_nloaded < sizeof g_loaded / sizeof g_loaded[0])
        g_loaded[g_nloaded++] = b;
    *n = (size_t)st.st_size;
    return b;
}

/* ---- the register path: the device through the module, or the simulator ---- */
struct regs_ctx {
    int fd;
    int err;                 /* first refused access (errno); after it nothing more reaches the chip */
    uint32_t err_off;
    unsigned long n;
#ifdef AIEN_GSP_BOOT_SIM
    struct ck_gpu_regs sim;
#endif
};

static uint32_t rd(void *c, uint32_t off)
{
    struct regs_ctx *x = c;
    uint32_t v = 0xffffffffu;
    if (!x->err) {
#ifdef AIEN_GSP_BOOT_SIM
        v = x->sim.rd(x->sim.ctx, off);
#else
        struct aien_gpu_reg r = {off, 0};
        if (ioctl(x->fd, AIEN_GPU_REG_RD, &r)) {
            x->err = errno;
            x->err_off = off;
        } else {
            v = r.val;
        }
#endif
    }
    x->n++;
    fprintf(g_trace, "R %06x %08x t=%llu%s\n", off, v, (unsigned long long)now_us(), x->err ? " REFUSED" : "");
    return v;
}

static void wr(void *c, uint32_t off, uint32_t v)
{
    struct regs_ctx *x = c;
    if (!x->err) {
#ifdef AIEN_GSP_BOOT_SIM
        x->sim.wr(x->sim.ctx, off, v);
#else
        struct aien_gpu_reg r = {off, v};
        if (ioctl(x->fd, AIEN_GPU_REG_WR, &r)) {
            x->err = errno;
            x->err_off = off;
        }
#endif
    }
    x->n++;
    fprintf(g_trace, "W %06x %08x t=%llu%s\n", off, v, (unsigned long long)now_us(), x->err ? " REFUSED" : "");
}

static unsigned g_pause_us = 100;

static void pause_fn(void *c)
{
    struct regs_ctx *x = c;
    if (x->err || !g_pause_us)
        return;              /* refused: let the budget run out fast */
    struct timespec t = {0, (long)g_pause_us * 1000};
    nanosleep(&t, 0);
}

static const char *stage_name(int s)
{
    switch (s) {
    case CK_GSP_OK: return "OK";
    case CK_GSP_E_ARG: return "E_ARG";
    case CK_GSP_E_BUSY: return "E_BUSY (SEC2 channel never emptied)";
    case CK_GSP_E_EMEM: return "E_EMEM (EMEM write pointer did not move)";
    case CK_GSP_E_SEC2: return "E_SEC2 (SEC2 never handled the packet)";
    case CK_GSP_E_MASK: return "E_MASK (GSP target mask never released)";
    case CK_GSP_E_LOCKDOWN: return "E_LOCKDOWN (no lockdown release, no FMC code)";
    case CK_GSP_E_FMC: return "E_FMC (FMC error code in GSP MAILBOX0)";
    case CK_GSP_E_RISCV: return "E_RISCV (GSP RISC-V core not active)";
    }
    return "?";
}

static void region(FILE *f, const char *name, gsp_region r)
{
    fprintf(f, "%-12s pa=%#llx size=%#llx\n", name, (unsigned long long)r.pa, (unsigned long long)r.size);
}

static const char *log_names[GSP_GB10B_LOG_COUNT] = {"LOGINIT", "LOGINTR", "LOGRM", "LOGMNOC", "LOGDBG", "LOGROOT", "LOGKRNL"};

static void dump_logs(const gsp_arena *a, const gsp_boot_plan *p)
{
    char n[64];
    for (unsigned i = 0; i < GSP_GB10B_LOG_COUNT; i++) {
        snprintf(n, sizeof n, "log-%s.bin", log_names[i]);
        out_bytes(n, a->base + (p->log[i].pa - a->pa), p->log[i].size);
    }
    out_bytes("rpc-shared.bin", a->base + (p->rpc_shared.pa - a->pa), p->rpc_shared.size);
}

#ifdef AIEN_GSP_BOOT_SIM
/* The fake GSP: creates the status queue the way GSP-RM's msgqTxCreate would and posts messages on it. With the two
 * queue pointers swapped, gsp_mq_send writes the status queue and reads our readPtr (SWAP_RX), as GSP-RM does. */
static void sim_gsp_answer(const gsp_arena *a, const gsp_boot_plan *p, const char *events)
{
    uint8_t *statq = a->base + (p->statq_pa - a->pa), *cmdq = a->base + (p->cmdq_pa - a->pa);
    const uint32_t h[8] = {0, GSP_MQ_QUEUE_SIZE, GSP_MQ_MSG_SIZE, GSP_MQ_MSG_COUNT, 0, GSP_MQ_FLAGS_SWAP_RX,
                           GSP_MQ_RXHDR_OFF, GSP_MQ_ENTRY_OFF};
    if (!strcmp(events, "none"))
        return;
    memcpy(statq, h, sizeof h);
    gsp_mq gsp = {.cmdq = statq, .statq = cmdq};
    const char *why;
    static const char hello[] = "fake LIBOS print";
    gsp_seg s = {hello, sizeof hello};
    if (gsp_mq_send(&gsp, 0x1007u, &s, 1, sizeof hello, 0, 0, &why))   /* an event the tool does not act on */
        die("TOOL-ERROR", "sim send: %s", why);
    if (!strcmp(events, "junk")) {
        uint32_t two = 2;            /* writePtr past an empty slot: element count 0, a corrupt queue */
        memcpy(statq + 16, &two, 4);
        return;
    }
    if (!strcmp(events, "seq-regs")) {
        /* rpc_run_cpu_sequencer_v17_00: size, cmdIndex, save[8], then commands.
         * One REG_WRITE of falcon mailbox0, then a following INIT_DONE. */
        uint32_t seqb[14] = {0};
        seqb[0] = 4;
        seqb[1] = 3;
        seqb[10] = 0;
        seqb[11] = 0x00110c00; /* GSP queue head, on the module write allowlist */
        seqb[12] = 0x7;
        gsp_seg se = {seqb, sizeof seqb};
        if (gsp_mq_send(&gsp, GSP_EV_RUN_CPU_SEQUENCER, &se, 1, sizeof seqb, 0, 0, &why))
            die("TOOL-ERROR", "sim send: %s", why);
        uint8_t done[8] = {0};
        gsp_seg de = {done, sizeof done};
        if (gsp_mq_send(&gsp, GSP_EV_INIT_DONE, &de, 1, sizeof done, 0, 0, &why))
            die("TOOL-ERROR", "sim send: %s", why);
        return;
    }
    uint8_t body[64] = {0};
    gsp_seg e = {body, sizeof body};
    uint32_t fn = !strcmp(events, "seq") ? GSP_EV_RUN_CPU_SEQUENCER : GSP_EV_INIT_DONE;
    if (gsp_mq_send(&gsp, fn, &e, 1, sizeof body, 0, 0, &why))
        die("TOOL-ERROR", "sim send: %s", why);
}
#endif

int main(int argc, char **argv)
{
    const char *pkg = 0;
    uint32_t app_version = 0, polls = 50000, wait_ms = 30000;
    int have_app = 0, plan_only = 0;
#ifdef AIEN_GSP_BOOT_SIM
    const char *sim_events = "init", *sim_cfg = "happy";
    unsigned sim_arena_mib = 192;
#endif
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : 0;
        if (!strcmp(a, "--plan-only")) { plan_only = 1; continue; }
        if (!v) die("USAGE", "%s needs a value", a);
        i++;
        if (!strcmp(a, "--pkg")) pkg = v;
        else if (!strcmp(a, "--out")) g_out = v;
        else if (!strcmp(a, "--app-version")) { app_version = (uint32_t)strtoul(v, 0, 0); have_app = 1; }
        else if (!strcmp(a, "--polls")) polls = (uint32_t)strtoul(v, 0, 0);
        else if (!strcmp(a, "--pause-us")) g_pause_us = (unsigned)strtoul(v, 0, 0);
        else if (!strcmp(a, "--wait-ms")) wait_ms = (uint32_t)strtoul(v, 0, 0);
#ifdef AIEN_GSP_BOOT_SIM
        else if (!strcmp(a, "--sim-events")) sim_events = v;
        else if (!strcmp(a, "--sim-cfg")) sim_cfg = v;
        else if (!strcmp(a, "--sim-arena-mib")) sim_arena_mib = (unsigned)strtoul(v, 0, 0);
#endif
        else die("USAGE", "unknown option %s", a);
    }
    if (!pkg || !g_out || !have_app)
        die("USAGE", "need --pkg DIR --app-version N --out DIR");
    if (g_pause_us > 100000)
        die("USAGE", "--pause-us at most 100000");
    if (mkdir(g_out, 0755) && errno != EEXIST)
        die("TOOL-ERROR", "%s: %s", g_out, strerror(errno));
    clock_gettime(CLOCK_MONOTONIC, &g_t0);
    g_trace = out_file("regs.txt", "w");
    setvbuf(g_trace, 0, _IOLBF, 0);

    /* 1. the package */
    size_t n_elf, n_sig, n_bl, n_fmc, n_h, n_k, n_s, n_si;
    gsp_boot_in in = {0};
    in.rm_elf = load(pkg, "rm_elf.bin", &n_elf, 0);
    in.rm_sig = load(pkg, "rm_sig.bin", &n_sig, 0);
    in.bootloader = load(pkg, "bootloader.bin", &n_bl, GSP_GB10B_BOOTLOADER_SIZE);
    in.fmc = load(pkg, "fmc.bin", &n_fmc, 0);
    in.fmc_hash = load(pkg, "fmc_hash.bin", &n_h, GSP_COT_HASH_SIZE);
    in.fmc_pubkey = load(pkg, "fmc_pkey.bin", &n_k, GSP_COT_KEY_SIZE);
    in.fmc_sig = load(pkg, "fmc_sig.bin", &n_s, GSP_COT_SIG_SIZE);
    in.system_info = load(pkg, "system_info.bin", &n_si, GSP_SYSTEM_INFO_SIZE);
    in.rm_elf_size = n_elf;
    in.rm_sig_size = (uint32_t)n_sig;
    in.bootloader_size = (uint32_t)n_bl;
    in.fmc_size = (uint32_t)n_fmc;
    in.bl_code_off = GSP_GB10B_BL_CODE_OFF;
    in.bl_data_off = GSP_GB10B_BL_DATA_OFF;
    in.bl_manifest_off = GSP_GB10B_BL_MANIFEST_OFF;

    /* 2. the memory: the module's arena and heap, or host memory standing in for them */
    struct regs_ctx rc = {.fd = -1};
    uint8_t *base;
    uint64_t arena_pa, arena_bytes, heap_pa, heap_bytes;
    unsigned dma_bits;
#ifdef AIEN_GSP_BOOT_SIM
    arena_bytes = (uint64_t)sim_arena_mib << 20;
    arena_pa = 0x0000004000000000ull;          /* SIM-CHOICE: an IOVA-looking address below 40 bits */
    heap_pa = 0x0000005000000000ull;
    heap_bytes = 512ull << 20;
    dma_bits = 40;
    base = aligned_alloc(GSP_PAGE, arena_bytes);
    g_host_arena = base;   /* reachable until exit */
    if (!base)
        die("TOOL-ERROR", "no memory for the host arena");
#else
    rc.fd = open("/dev/aien-gpu", O_RDWR);
    if (rc.fd < 0)
        die("NO-DEVICE", "/dev/aien-gpu: %s", strerror(errno));
    struct aien_gpu_info info;
    if (ioctl(rc.fd, AIEN_GPU_INFO, &info))
        die("NO-DEVICE", "AIEN_GPU_INFO: %s", strerror(errno));
    FILE *fi = out_file("info.txt", "w");
    fprintf(fi, "boot0=%08x boot42=%08x bar0_pa=%#llx bar0_bytes=%llu arena=%#llx@%#llx heap=%#llx@%#llx dma_bits=%u allow_boot=%u doorbells=%u\n",
            info.boot0, info.boot42, (unsigned long long)info.bar0_pa, (unsigned long long)info.bar0_bytes,
            (unsigned long long)info.arena_bytes, (unsigned long long)info.arena_iova,
            (unsigned long long)info.heap_bytes, (unsigned long long)info.heap_iova, info.dma_bits, info.allow_boot,
            info.doorbells);
    fclose(fi);
    if (!plan_only && (!info.allow_boot || info.doorbells))
        die("REFUSED-BY-SHIM", "allow_boot=%u doorbells=%u: load with allow_boot=1, one attempt per load",
            info.allow_boot, info.doorbells);
    if (!info.heap_bytes)
        die("REFUSED-BY-SHIM", "no heap: load with heap_mib=512");
    arena_bytes = info.arena_bytes;
    arena_pa = info.arena_iova;
    heap_pa = info.heap_iova;
    heap_bytes = info.heap_bytes;
    dma_bits = info.dma_bits;
    base = mmap(0, arena_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, rc.fd, 0);
    if (base == MAP_FAILED)
        die("NO-DEVICE", "mmap arena: %s", strerror(errno));
#endif
    in.sysmem_heap_pa = heap_pa;
    in.sysmem_heap_size = heap_bytes;
    in.dma_bits = dma_bits;

    /* The first arena page is the zeroed 4 KiB "notify op" surface NVIDIA's driver allocates before
       GSP_SET_SYSTEM_INFO (kernel_gsp.c:3489-3526 _kgspAllocNotifyOpSharedSurface); its address goes into
       GspSystemInfo.notifyOpSharedSurfacePhysAddr (offset 48, gsp_static_config.h:180) unless the package set one. */
    if (arena_bytes <= GSP_PAGE)
        die("BUILD-REFUSED", "arena too small for the notify page");
    memset(base, 0, GSP_PAGE);
    static uint8_t si_sent[GSP_SYSTEM_INFO_SIZE];
    memcpy(si_sent, in.system_info, GSP_SYSTEM_INFO_SIZE);
    uint64_t notify_pa = 0;
    for (int i = 0; i < 8; i++)
        notify_pa |= (uint64_t)si_sent[48 + i] << (8 * i);
    if (!notify_pa) {
        notify_pa = arena_pa;
        for (int i = 0; i < 8; i++)
            si_sent[48 + i] = (uint8_t)(notify_pa >> (8 * i));
    }
    in.system_info = si_sent;
    FILE *fs = out_file("system_info.sent.bin", "wb");
    if (fwrite(si_sent, 1, sizeof si_sent, fs) != sizeof si_sent)
        die("TOOL-ERROR", "write system_info.sent.bin");
    fclose(fs);

    gsp_arena arena;
    if (gsp_arena_init(&arena, base + GSP_PAGE, arena_pa + GSP_PAGE, arena_bytes - GSP_PAGE))
        die("BUILD-REFUSED", "arena not 4 KiB aligned");
    static gsp_boot_plan plan;
    const char *why = 0;
    if (gsp_boot_build(&arena, &in, &plan, &why))
        die("BUILD-REFUSED", "gsp_boot_build: %s (arena %#llx bytes)", why, (unsigned long long)arena_bytes);
    FILE *fp = out_file("plan.txt", "w");
    fprintf(fp, "arena pa=%#llx cap=%#llx used=%#llx heap pa=%#llx size=%#llx dma_bits=%u app_version=%u\n",
            (unsigned long long)arena.pa, (unsigned long long)arena.cap, (unsigned long long)arena.used,
            (unsigned long long)heap_pa, (unsigned long long)heap_bytes, dma_bits, app_version);
    fprintf(fp, "notify_page pa=%#llx\n", (unsigned long long)notify_pa);
    region(fp, "wpr_meta", plan.wpr_meta);
    region(fp, "libos_args", plan.libos_args);
    for (unsigned i = 0; i < GSP_GB10B_LOG_COUNT; i++)
        region(fp, log_names[i], plan.log[i]);
    region(fp, "args_cached", plan.args_cached);
    region(fp, "rpc_shared", plan.rpc_shared);
    region(fp, "radix3", plan.radix3);
    region(fp, "bootloader", plan.bootloader);
    region(fp, "signature", plan.signature);
    region(fp, "fmc", plan.fmc);
    region(fp, "fmc_params", plan.fmc_params);
    fprintf(fp, "radix3_levels %u %u %u %u elf_data_pa=%#llx cmdq_write_ptr=%u tx_seq=%u cot_packet_len=%u emem_queue_tail=%u\n",
            plan.radix3_level_pages[0], plan.radix3_level_pages[1], plan.radix3_level_pages[2],
            plan.radix3_level_pages[3], (unsigned long long)plan.elf_data_pa, plan.cmdq_write_ptr, plan.tx_seq,
            plan.cot_packet_len, plan.emem_queue_tail);
    fclose(fp);
    out_bytes("cot_packet.bin", plan.cot_packet, plan.cot_packet_len);
    if (plan_only) {
        printf("AIEN_GSP_BOOT: PLAN-ONLY (arena used %#llx of %#llx)\n", (unsigned long long)arena.used,
               (unsigned long long)arena.cap);
        return 0;
    }

    /* 3. the 11 register steps */
#ifdef AIEN_GSP_BOOT_SIM
    static struct ck_gspsim sim;
    struct ck_gspsim_cfg cfg = {0, 3, 4, 2, 5, 0, 0, 0, plan.fmc_params.pa, 0};
    if (!strcmp(sim_cfg, "busy")) cfg.busy_reads = CK_GSPSIM_NO_END;
    else if (!strcmp(sim_cfg, "fmc-error")) cfg.fmc_error = 0x23;
    else if (!strcmp(sim_cfg, "riscv-dead")) cfg.riscv_dead = 1;
    else if (strcmp(sim_cfg, "happy")) die("USAGE", "unknown --sim-cfg %s", sim_cfg);
    ck_gspsim_init(&sim, &cfg);
    rc.sim = ck_gspsim_regs(&sim);
#endif
    struct ck_gpu_regs regs = {rd, wr, &rc};
    struct ck_gsp_mmio_in mi = {plan.cot_packet, plan.cot_packet_len, plan.fmc_params.pa, app_version,
                                polls, polls, polls, polls, pause_fn, &rc};
    struct ck_gsp_mmio_out mo;
    uint64_t t_boot = now_us();
    int stage = ck_gsp_mmio_boot(&regs, &mi, &mo);
    uint64_t t_core = now_us();
    FILE *fr = out_file("result.txt", "w");
    fprintf(fr, "stage=%d %s boot_us=%llu polls=%u,%u,%u,%u dwords=%u ememc=%#x sec2_mb=%#x,%#x hwcfg2=%#x gsp_mb=%#x,%#x cpuctl=%#x accesses=%lu\n",
            stage, stage_name(stage), (unsigned long long)(t_core - t_boot), mo.polls[0], mo.polls[1], mo.polls[2],
            mo.polls[3], mo.dwords, mo.ememc_after, mo.sec2_mb0, mo.sec2_mb1, mo.hwcfg2, mo.gsp_mb0, mo.gsp_mb1,
            mo.cpuctl, rc.n);
    if (rc.err) {
        fprintf(fr, "shim refused offset %#x: %s\n", rc.err_off, strerror(rc.err));
        fclose(fr);
        dump_logs(&arena, &plan);
        die("REFUSED-BY-SHIM", "register %#x: %s", rc.err_off, strerror(rc.err));
    }
    if (stage != CK_GSP_OK) {
        fclose(fr);
        dump_logs(&arena, &plan);
        printf("AIEN_GSP_BOOT: STOPPED-AT %s\n", stage_name(stage));
        return 1;
    }

    /* 4. GSP-RM's core runs: wait on the status queue */
#ifdef AIEN_GSP_BOOT_SIM
    sim_gsp_answer(&arena, &plan, sim_events);
#endif
    gsp_mq q;
    if (gsp_mq_attach(&q, &arena, &plan))
        die("TOOL-ERROR", "gsp_mq_attach");
    static uint8_t msg[MSG_CAP];
    const char *verdict = "NO-INIT-DONE (time limit)";
    int ret = 1, seen_queue = 0;
    unsigned nmsg = 0;
    FILE *fm = out_file("messages.txt", "w");
    while (now_us() - t_core < (uint64_t)wait_ms * 1000u) {
        uint32_t fn = 0, res = 0;
        int r = gsp_mq_recv(&q, msg, sizeof msg, &fn, &res, &why);
        if (r < 0 && !seen_queue && why && !strcmp(why, "status queue header not the driver's shape")) {
            struct timespec t = {0, 1000000};
            nanosleep(&t, 0);      /* GSP-RM has not created its queue yet */
            continue;
        }
        if (r < 0) {
            fprintf(fm, "t=%llu RECV-ERROR %s\n", (unsigned long long)now_us(), why);
            verdict = "STATUS-QUEUE-ERROR";
            break;
        }
        seen_queue = 1;
        if (r == 0) {
            struct timespec t = {0, 1000000};
            nanosleep(&t, 0);
            continue;
        }
        uint32_t len;
        memcpy(&len, msg + GSP_ELEM_HDR_SIZE + 8, 4);   /* rpc.length */
        fprintf(fm, "t=%llu msg %u function=%#x rpc_result=%#x length=%u\n", (unsigned long long)now_us(), nmsg, fn,
                res, len);
        char name[64];
        snprintf(name, sizeof name, "msg-%03u-%#x.bin", nmsg, fn);
        out_bytes(name, msg, GSP_ELEM_HDR_SIZE + len);
        nmsg++;
        if (fn == GSP_EV_INIT_DONE) {
            verdict = "INIT-DONE";
            ret = 0;
            break;
        }
        if (fn == GSP_EV_RUN_CPU_SEQUENCER) {
            /* Replaces kgspExecuteSequencerBuffer for register opcodes. Core opcodes
             * still refuse (they restart the chip). A buffer we cannot run keeps the
             * old stop, with the payload saved. */
            uint32_t pay = len > GSP_RPC_HDR_SIZE ? len - GSP_RPC_HDR_SIZE : 0;
            uint32_t words[64];
            const char *sw = 0;
            int ran = 0;
            if (pay >= 40 && pay / 4 <= 64 && (pay % 4) == 0) {
                memcpy(words, msg + GSP_ELEM_HDR_SIZE + GSP_RPC_HDR_SIZE, pay);
                gsp_seq_io sio;
                memset(&sio, 0, sizeof sio);
                sio.rd = rd;
                sio.wr = wr;
                sio.ctx = &rc;
                sio.poll_budget = 8;
                if (words[0] <= pay / 4 - 10 &&
                    gsp_seq_run(words + 10, words[0], words[1], &sio, &sw) == 0 && !rc.err)
                    ran = 1;
            }
            if (ran) {
                fprintf(fm, "sequencer ran\n");
                continue;
            }
            verdict = "SEQUENCER-REQUESTED (not handled in window 1; payload saved)";
            break;
        }
    }
    uint64_t t_end = now_us();
    fclose(fm);
    uint32_t mb0 = rd(&rc, CK_GSP_FLCN_MAILBOX0), mb1 = rd(&rc, CK_GSP_FLCN_MAILBOX1);
    fprintf(fr, "wait_us=%llu messages=%u verdict=%s gsp_mb=%#x,%#x\n", (unsigned long long)(t_end - t_core), nmsg,
            verdict, mb0, mb1);
    fclose(fr);
    dump_logs(&arena, &plan);
    printf("AIEN_GSP_BOOT: %s\n", verdict);
    return ret;
}
