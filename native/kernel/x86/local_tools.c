/* Bounded BSP-only Omega runner. Intel SDM Vol 3A: paging 5.6, TSS/IRET 7.14;
 * Vol 1 10.5: FXSAVE. Local ABI supports x87/SSE2, never AVX or Linux syscalls.
 * The kernel identity map remains supervisor-only. A private CR3 exposes only
 * these fixed user pages; neither model output nor requests supply addresses. */
#include "local_tools.h"
#include "local_wire.h"
#include "../core/ck_internal.h"
#include "../artifact/ck_artifact.h"
#include "sha256.h"
#include "local_bundle.h"

#define PAGE 4096u
#define BASE (1ull << 46)
#define CODE (BASE + PAGE)
#define STUB (BASE + 18 * PAGE)
#define META (BASE + 20 * PAGE)
#define INPUT (BASE + 22 * PAGE)
#define WORK (BASE + 24 * PAGE)
#define STACK (BASE + 57 * PAGE)
#define TOP (STACK + 16 * PAGE)
#define NX (1ull << 63)
#define USER 5ull
#define WRITE 2ull
#define ADDR 0x000ffffffffff000ull
#define ALIGNED __attribute__((aligned(PAGE)))

static uint64_t root[512] ALIGNED, pdpt[512] ALIGNED, pd[512] ALIGNED, pt[512] ALIGNED;
static uint8_t code[16 * PAGE] ALIGNED, stub[PAGE] ALIGNED, meta[PAGE] ALIGNED;
static uint8_t input[PAGE] ALIGNED, workspace[32 * PAGE] ALIGNED, stack[16 * PAGE] ALIGNED;
static uint8_t irqstack[16 * PAGE] ALIGNED;
uint8_t ck_local_user_fp[512] __attribute__((aligned(16)));
uint8_t ck_local_kernel_fp[512] __attribute__((aligned(16)));
uint64_t ck_local_kernel_sp;
extern void ck_local_enter(struct ck_x86_frame *);
static struct ck_x86_frame frame;
static uint64_t kernel_root, hz, active_request, started, slice_start, cpu_used;
static uint64_t snapshot[5];
static unsigned used;
static struct {
    struct ck_local_result result;
    unsigned tool;
    size_t input_len;
    uint8_t input_hash[32];
} calls[CK_LOCAL_CALLS];
static struct ck_local_result refusal;
static struct ck_local_result *running;
static struct { const uint8_t *code; size_t len; uint8_t hash[32]; int ok; } approved[3];

static void wipe(void *p, size_t n)
{
    volatile uint8_t *b = p;
    while (n--) *b++ = 0;
}
static int equal(const uint8_t *a, const uint8_t *b, size_t n)
{
    unsigned d = 0;
    for (size_t i = 0; i < n; i++) d |= a[i] ^ b[i];
    return d == 0;
}
static void clear_task(void)
{
    /* CR3 was restored before this is called. No global user translations. */
    wipe(root, sizeof root); wipe(pdpt, sizeof pdpt); wipe(pd, sizeof pd); wipe(pt, sizeof pt);
    wipe(code, sizeof code); wipe(stub, sizeof stub); wipe(meta, sizeof meta);
    wipe(input, sizeof input); wipe(workspace, sizeof workspace); wipe(stack, sizeof stack);
    wipe(&frame, sizeof frame); wipe(ck_local_user_fp, sizeof ck_local_user_fp);
    wipe(irqstack, sizeof irqstack);
    ck_local_machine_stack(0);
    running = 0;
}
static void finish(unsigned state, unsigned reason)
{
    running->state = state;
    running->reason = reason;
    if (state != CK_LOCAL_RETURNED) {
        running->length = 0;
        wipe(running->output, sizeof running->output);
    }
}
void ck_local_cancel(void)
{
    if (running) {
        finish(CK_LOCAL_ABNORMAL, CK_LOCAL_CANCELLED);
        clear_task();
    }
}
void ck_local_begin(uint64_t request)
{
    ck_local_cancel();
    wipe(calls, sizeof calls);
    wipe(&refusal, sizeof refusal);
    active_request = request;
    used = 0;
}
void ck_local_snapshot(uint64_t cores, uint64_t ram, uint64_t w, uint64_t h, uint64_t network)
{
    snapshot[0] = cores; snapshot[1] = ram; snapshot[2] = w;
    snapshot[3] = h; snapshot[4] = network;
}

/* Signature + immutable build allowlist + closed ABI + tighter local quotas.
 * Existing ARM parser entry points still require the ARM profile. */
static int admit(const struct ck_local_artifact *a, const uint8_t **bytes, size_t *len)
{
    struct cka_parsed p;
    struct cka_ident id;
    uint8_t fp[32], digest[32];
    if (cka_verify_profile(a->bytes, a->length, ck_local_keys, 1, &p, &id, fp,
                           CKA_TARGET_X86_64_LE, CKA_ABI_OMEGA_X86_LOCAL_V1))
        return -1;
    sha256_hash(a->bytes, a->length, digest);
    if (!equal(digest, a->hash, 32) || p.entry || p.ncaps || p.code.flen > sizeof code ||
        p.env.code_pages > 16 || p.env.data_pages != 32 || p.env.stack_pages != 16 ||
        p.env.max_caps || p.env.ipc_msgs || p.env.ipc_bytes || p.env.cpu != 100000 ||
        p.env.elapsed != 500000 || p.env.syscalls != 1 || p.data.flen != 1 ||
        a->bytes[p.payload_off + p.data.rel])
        return -1;
    *bytes = a->bytes + p.payload_off;
    *len = p.code.flen;
    return 0;
}
void ck_local_init(uint64_t kr, uint64_t frequency)
{
    kernel_root = kr; hz = frequency;
    for (unsigned i = 0; i < 3; i++) {
        approved[i].ok = !admit(&ck_local_artifacts[i], &approved[i].code, &approved[i].len);
        if (approved[i].ok)
            sha256_hash(approved[i].code, approved[i].len, approved[i].hash);
    }
    ck_printf("local_tools: approved=%u/3 profile=omega-x86-local-v1 slice_us=1000 cpu_us=100000 elapsed_us=500000\n",
              (unsigned)(approved[0].ok + approved[1].ok + approved[2].ok));
}
static void map_pages(unsigned first, uint8_t *backing, unsigned n, uint64_t flags)
{
    for (unsigned i = 0; i < n; i++)
        pt[first + i] = (uint64_t)(uintptr_t)(backing + i * PAGE) | USER | flags;
}
static int prepare(const uint8_t *bytes, size_t n, const void *in, size_t length)
{
    if (!kernel_root || !hz || !n || n > sizeof code || length > sizeof input)
        return -1;
    memcpy(root, (void *)(uintptr_t)kernel_root, sizeof root);
    /* A future kernel mapping must not silently be replaced. All inherited
     * entries are made supervisor-only, even if a future kernel adds U/S. */
    if (root[128]) return -1;
    for (unsigned i = 0; i < 512; i++) root[i] &= ~4ull;
    root[128] = (uint64_t)(uintptr_t)pdpt | USER | WRITE;
    pdpt[0] = (uint64_t)(uintptr_t)pd | USER | WRITE;
    pd[0] = (uint64_t)(uintptr_t)pt | USER | WRITE;
    memcpy(code, bytes, n);
    if (length) memcpy(input, in, length);
    /* Two trusted return veneers: MOV EDX,reason; INT 80h; UD2. */
    static const uint8_t veneers[] = {0xba,1,0,0,0,0xcd,0x80,0x0f,0x0b,
                                     0xba,2,0,0,0,0xcd,0x80,0x0f,0x0b};
    memcpy(stub, veneers, sizeof veneers);
    uint64_t *vt = (void *)meta;
    for (unsigned i = 0; i < 32; i++) vt[i] = STUB + 16; /* UD2 for unsupported runtime entries */
    vt[2] = STUB + 9;
    map_pages(1, code, (unsigned)((n + PAGE - 1) / PAGE), 0);
    map_pages(18, stub, 1, 0); map_pages(20, meta, 1, NX);
    map_pages(22, input, 1, NX); map_pages(24, workspace, 32, NX | WRITE);
    map_pages(57, stack, 16, NX | WRITE);
    /* Slots for three fixed-array/scalar parameters; disjoint from callee's
     * initial 272-byte frame and return address. No kernel addresses escape. */
    uint64_t *end = (void *)(stack + sizeof stack);
    end[-1] = INPUT; end[-2] = WORK; end[-3] = length;
    end[-33] = STUB;
    frame.rip = CODE; frame.cs = 0x2b; frame.ss = 0x33; frame.rflags = 0x202;
    frame.rsp = TOP - 264; frame.rbp = TOP; frame.r12 = TOP - 264;
    frame.r14 = META; frame.r15 = STACK;
    ck_local_user_fp[0] = 0x7f; ck_local_user_fp[1] = 3; /* FCW=037f, empty tags */
    ck_local_user_fp[24] = 0x80; ck_local_user_fp[25] = 0x1f; /* MXCSR=1f80 */
    started = ck_rdtsc(); cpu_used = 0;
    ck_local_machine_stack((uint64_t)(uintptr_t)(irqstack + sizeof irqstack));
    return 0;
}
static const struct ck_local_result *refuse(uint64_t request, uint64_t invocation, unsigned why)
{
    wipe(&refusal, sizeof refusal);
    refusal.request = request; refusal.invocation = invocation;
    refusal.state = CK_LOCAL_REFUSED; refusal.reason = why;
    return &refusal;
}
const struct ck_local_result *ck_local_status(uint64_t request, uint64_t invocation)
{
    if (!request || request != active_request || !invocation) return 0;
    for (unsigned i = 0; i < used; i++)
        if (calls[i].result.invocation == invocation) return &calls[i].result;
    return 0;
}
const struct ck_local_result *ck_local_start(uint64_t request, uint64_t invocation,
    unsigned tool, const void *in, size_t length)
{
    if (!request || request != active_request || !invocation || length > CK_LOCAL_INPUT ||
        (length && !in) || tool < 1 || tool > 3)
        return refuse(request, invocation, CK_LOCAL_BAD_REQUEST);
    uint8_t digest[32]; sha256_hash(in, length, digest);
    for (unsigned i = 0; i < used; i++)
        if (calls[i].result.invocation == invocation) {
            if (calls[i].tool != tool || calls[i].input_len != length ||
                !equal(calls[i].input_hash, digest, 32))
                return refuse(request, invocation, CK_LOCAL_BAD_REQUEST);
            return &calls[i].result;
        }
    if (running || used == CK_LOCAL_CALLS)
        return refuse(request, invocation, CK_LOCAL_LIMIT);
    unsigned i = used++;
    calls[i].tool = tool; calls[i].input_len = length;
    memcpy(calls[i].input_hash, digest, 32);
    running = &calls[i].result;
    running->request = request; running->invocation = invocation;
    running->state = CK_LOCAL_RUNNING;
    if ((tool == 1 && length != 24) || (tool == 3 && length)) {
        finish(CK_LOCAL_REFUSED, CK_LOCAL_BAD_REQUEST);
    } else if (!approved[tool - 1].ok) {
        finish(CK_LOCAL_REFUSED, CK_LOCAL_ADMISSION);
    } else {
        if (tool == 3) { in = snapshot; length = sizeof snapshot; }
        if (prepare(approved[tool - 1].code, approved[tool - 1].len, in, length)) {
            finish(CK_LOCAL_REFUSED, CK_LOCAL_ADMISSION);
        } else {
            sha256_hash(code, approved[tool - 1].len, digest);
            if (!equal(digest, approved[tool - 1].hash, 32))
                finish(CK_LOCAL_REFUSED, CK_LOCAL_ADMISSION);
        }
    }
    if (running->state != CK_LOCAL_RUNNING) clear_task();
    return &calls[i].result;
}
void ck_local_trap(struct ck_x86_frame *f)
{
    ck_wr_cr3(kernel_root);
    ck_local_machine_timer(0);
    ck_local_machine_eoi(f->vector);
    if (!running || (f->cs & 3) != 3 || f->vector == 2 || f->vector == 8 || f->vector == 18)
        ck_panic("local_tools: fatal interrupt %llu", (unsigned long long)f->vector);
    uint64_t now = ck_rdtsc();
    cpu_used += now - slice_start;
    frame = *f;
    /* Prevent user NT/VM/TF/IOPL/AC from changing the next kernel IRET. */
    frame.rflags = (f->rflags & 0xcd5ull) | 0x202;
    frame.cs = 0x2b; frame.ss = 0x33;
    if (cpu_used >= hz / 10) finish(CK_LOCAL_ABNORMAL, CK_LOCAL_CPU);
    else if (now - started >= hz / 2) finish(CK_LOCAL_ABNORMAL, CK_LOCAL_ELAPSED);
    else if (f->vector == 0x80) {
        if (f->rip == STUB + 7 && f->rdx == 1 && f->rax <= CK_LOCAL_OUTPUT) {
            running->length = (unsigned)f->rax;
            memcpy(running->output, workspace, running->length);
            finish(CK_LOCAL_RETURNED, 0);
        } else if (f->rip == STUB + 16 && f->rdx == 2 && f->rsi >= 1 && f->rsi <= 14) {
            finish(CK_LOCAL_TRAPPED, (unsigned)f->rsi);
        } else finish(CK_LOCAL_ABNORMAL, CK_LOCAL_BAD_RETURN);
    } else if (f->vector < 32) finish(CK_LOCAL_ABNORMAL, CK_LOCAL_FAULT);
    /* Timer/other external IRQ: return to device polling with saved user state. */
}
void ck_local_step(void)
{
    if (!running) return;
    if (ck_rdtsc() - started >= hz / 2) {
        finish(CK_LOCAL_ABNORMAL, CK_LOCAL_ELAPSED); clear_task(); return;
    }
    uint64_t cr0 = ck_rd_cr0(), cr4 = ck_rd_cr4(), efer = ck_rdmsr(0xc0000080);
    uint64_t sysenter = ck_rdmsr(0x174);
    uint16_t ds, es, fs_sel, gs_sel, ss;
    __asm__ volatile("mov %%ds,%0; mov %%es,%1; mov %%fs,%2; mov %%gs,%3; mov %%ss,%4"
        : "=r"(ds), "=r"(es), "=r"(fs_sel), "=r"(gs_sel), "=r"(ss));
    uint64_t fs = ck_rdmsr(0xc0000100), gs = ck_rdmsr(0xc0000101);
    /* Disable FSGSBASE, OSXSAVE/AVX, PKE/WRPKRU and user RDPMC. SCE off makes
     * any Linux SYSCALL a contained #UD, with no firmware LSTAR to reach. */
    __asm__ volatile("mov %0, %%cr0" :: "r"((cr0 | 2) & ~12ull) : "memory");
    __asm__ volatile("mov %0, %%cr4" :: "r"((cr4 | 0x600) & ~((1ull<<16)|(1ull<<18)|(1ull<<22)|(1ull<<8))) : "memory");
    ck_wrmsr(0xc0000080, efer & ~1ull);
    ck_wrmsr(0x174, 0);
    if (ck_local_machine_timer(1)) finish(CK_LOCAL_ABNORMAL, CK_LOCAL_CPU);
    else {
        slice_start = ck_rdtsc();
        ck_wr_cr3((uint64_t)(uintptr_t)root);
        ck_local_enter(&frame);
    }
    __asm__ volatile("mov %0,%%ds; mov %1,%%es; mov %2,%%fs; mov %3,%%gs; mov %4,%%ss"
        :: "r"(ds), "r"(es), "r"(fs_sel), "r"(gs_sel), "r"(ss) : "memory");
    ck_wrmsr(0x174, sysenter);
    ck_wrmsr(0xc0000100, fs); ck_wrmsr(0xc0000101, gs);
    ck_wrmsr(0xc0000080, efer);
    __asm__ volatile("mov %0, %%cr4" :: "r"(cr4) : "memory");
    __asm__ volatile("mov %0, %%cr0" :: "r"(cr0) : "memory");
    wipe(ck_local_kernel_fp, sizeof ck_local_kernel_fp); ck_local_kernel_sp = 0;
    if (running->state != CK_LOCAL_RUNNING) clear_task();
}

#ifndef CK_LOCAL_TEST
void ck_local_selftest(void) {}
#else
#include "local_tests.inc"
#endif
