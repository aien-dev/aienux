/* Deliberately hostile machine code, only in CK_LOCAL_TEST images. These paths
 * are absent from release builds: network callers can only name signed tools. */
static unsigned local_checks;
static char wire_answer[256];
static int wire_send(const char *s, unsigned n)
{
    if (n >= sizeof wire_answer) return -1;
    memcpy(wire_answer, s, n); wire_answer[n] = 0; return 0;
}
static void check(int ok, const char *name)
{
    if (!ok) ck_panic("local_test: FAIL %s", name);
    local_checks++;
}
static int zeroed(const void *p, size_t n)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) if (b[i]) return 0;
    return 1;
}
static void reclaimed(void)
{
    check(!running && ck_rd_cr3() == kernel_root && zeroed(root, sizeof root) &&
        zeroed(pt, sizeof pt) && zeroed(code, sizeof code) && zeroed(input, sizeof input) &&
        zeroed(workspace, sizeof workspace) && zeroed(stack, sizeof stack) &&
        zeroed(meta, sizeof meta) && zeroed(stub, sizeof stub) && zeroed(&frame, sizeof frame) &&
        zeroed(ck_local_user_fp, sizeof ck_local_user_fp) && zeroed(irqstack, sizeof irqstack),
        "private bytes and translations reclaimed");
}
static void run_done(void)
{
    for (unsigned i = 0; running && i < 2000; i++) ck_local_step();
    check(!running, "bounded completion");
    reclaimed();
}
static void raw(const uint8_t *bytes, size_t len, unsigned state, unsigned why)
{
    ck_local_begin(91);
    running = &calls[0].result;
    running->state = CK_LOCAL_RUNNING;
    check(!prepare(bytes, len, 0, 0), "hostile fixture prepared");
    run_done();
    check(calls[0].result.state == state && calls[0].result.reason == why &&
          !calls[0].result.length, "hostile fixture contained");
    int64_t a[3] = {1, 20, 22};
    ck_local_begin(92);
    const struct ck_local_result *r = ck_local_start(92,1,1,a,sizeof a);
    run_done();
    check(r->state == CK_LOCAL_RETURNED && r->length == 8 && r->output[0] == 42,
          "successful tool immediately after fault");
}
static void access_probe(uint64_t address, int write, int execute)
{
    uint8_t b[16] = {0x48,0xb8}; /* MOVABS RAX, address; access; RET */
    memcpy(b + 2, &address, 8);
    b[10] = execute ? 0xff : 0x48;
    b[11] = execute ? 0xe0 : write ? 0x89 : 0x8b;
    b[12] = 0x00; b[13] = 0xc3;
    raw(b, 14, CK_LOCAL_ABNORMAL, CK_LOCAL_FAULT);
}
void ck_local_selftest(void)
{
    check(approved[0].ok && approved[1].ok && approved[2].ok, "signed tools admitted");
    static uint8_t mutated[66000];
    struct ck_local_artifact mutant = ck_local_artifacts[0];
    mutant.bytes = mutated;
    const uint8_t *loaded; size_t loaded_len;
    const unsigned offsets[] = {12,14,16,36,128+2,192,240,0};
    for (unsigned i = 0; i < sizeof offsets / sizeof offsets[0]; i++) {
        memcpy(mutated, ck_local_artifacts[0].bytes, mutant.length);
        mutated[offsets[i]] ^= 0x40;
        check(admit(&mutant,&loaded,&loaded_len) != 0, "tampered artifact refused");
    }
    memcpy(mutated, ck_local_artifacts[0].bytes, mutant.length);
    mutated[mutant.length-1] ^= 1;
    check(admit(&mutant,&loaded,&loaded_len) != 0, "bad signature refused");
    memcpy(mutated, ck_local_artifacts[0].bytes, mutant.length);
    mutant.length--;
    check(admit(&mutant,&loaded,&loaded_len) != 0, "truncated artifact refused");
    mutant = ck_local_artifacts[0]; mutant.hash[0] ^= 1;
    check(admit(&mutant,&loaded,&loaded_len) != 0, "nonallowlisted identity refused");
    struct cka_parsed parsed;
    check(cka_parse(ck_local_artifacts[0].bytes, ck_local_artifacts[0].length, &parsed) == CKA_WRONG_TARGET,
          "ARM admission still excludes x86");
    int64_t args[3] = {1, 20, 22};
    ck_local_begin(1);
    const struct ck_local_result *r = ck_local_start(1, 1, 1, args, sizeof args);
    run_done();
    int64_t value; memcpy(&value, r->output, 8);
    check(r->state == CK_LOCAL_RETURNED && r->length == 8 && value == 42, "Omega arithmetic ABI");
    check(ck_local_start(1, 1, 1, args, sizeof args) == r && !running && used == 1, "idempotent replay");
    args[1]++;
    check(ck_local_start(1, 1, 1, args, sizeof args)->state == CK_LOCAL_REFUSED, "changed replay refused");
    check(ck_local_start(2, 1, 1, args, sizeof args)->state == CK_LOCAL_REFUSED, "stale request refused");
    for (unsigned i = 2; i <= 4; i++) { ck_local_start(1, i, 1, args, sizeof args); run_done(); }
    check(ck_local_start(1, 5, 1, args, sizeof args)->state == CK_LOCAL_REFUSED, "four call ceiling");
    ck_local_begin(2);
    args[0] = 1; args[1] = INT64_MAX; args[2] = 1;
    r = ck_local_start(2, 1, 1, args, sizeof args); run_done();
    check(r->state == CK_LOCAL_TRAPPED && r->reason == 1 && !r->length, "signed overflow");
    args[0] = 4; args[2] = 0;
    r = ck_local_start(2, 2, 1, args, sizeof args); run_done();
    check(r->state == CK_LOCAL_TRAPPED && r->reason == 2 && !r->length, "division by zero");
    const char text[] = "one\ttwo\nthree ";
    r = ck_local_start(2, 3, 2, text, sizeof text - 1); run_done();
    uint64_t counts[3]; memcpy(counts, r->output, sizeof counts);
    check(r->state == CK_LOCAL_RETURNED && r->length == 24 && counts[0] == sizeof text - 1 &&
          counts[1] == 3 && counts[2] == 1, "Omega text ABI");
    r = ck_local_start(2, 4, 3, 0, 0); run_done();
    check(r->state == CK_LOCAL_RETURNED && r->length == 40 && equal(r->output, (void *)snapshot, 40),
          "Omega cached system ABI");
    ck_local_begin(12); args[0] = 2; args[1] = -8; args[2] = -5;
    r = ck_local_start(12,1,1,args,sizeof args); run_done(); memcpy(&value,r->output,8);
    check(r->state == CK_LOCAL_RETURNED && value == -3, "signed subtraction");
    args[0] = 4; args[1] = -22; args[2] = 7;
    r = ck_local_start(12,2,1,args,sizeof args); run_done(); memcpy(&value,r->output,8);
    check(r->state == CK_LOCAL_RETURNED && value == -3, "signed division truncates toward zero");
    args[1] = INT64_MIN; args[2] = -1;
    r = ck_local_start(12,3,1,args,sizeof args); run_done();
    check(r->state == CK_LOCAL_TRAPPED && r->reason == 1, "minimum divided by minus one");
    static uint8_t longtext[4096]; memset(longtext,'a',sizeof longtext); longtext[4095] = '\n';
    r = ck_local_start(12,4,2,longtext,sizeof longtext); run_done(); memcpy(counts,r->output,sizeof counts);
    check(r->state == CK_LOCAL_RETURNED && counts[0] == 4096 && counts[1] == 1 && counts[2] == 1,
          "full input page");

    access_probe(kernel_root, 0, 0); access_probe(kernel_root, 1, 0);
    access_probe(CODE, 1, 0); access_probe(INPUT, 1, 0); access_probe(META, 1, 0);
    access_probe(STACK - PAGE, 0, 0); access_probe(TOP, 1, 0);
    access_probe(WORK, 0, 1); access_probe(STACK, 0, 1);
    access_probe(0xfee00000, 0, 0); /* LAPIC device memory */
    const uint8_t cli[] = {0xfa,0xc3}, hlt[] = {0xf4,0xc3}, io[] = {0xe6,0x80,0xc3};
    const uint8_t syscall[] = {0x0f,0x05,0xc3}, sysenter[] = {0x0f,0x34,0xc3};
    raw(cli, sizeof cli, CK_LOCAL_ABNORMAL, CK_LOCAL_FAULT);
    raw(hlt, sizeof hlt, CK_LOCAL_ABNORMAL, CK_LOCAL_FAULT);
    raw(io, sizeof io, CK_LOCAL_ABNORMAL, CK_LOCAL_FAULT);
    raw(syscall, sizeof syscall, CK_LOCAL_ABNORMAL, CK_LOCAL_FAULT);
    raw(sysenter, sizeof sysenter, CK_LOCAL_ABNORMAL, CK_LOCAL_FAULT);
    const uint8_t avx[] = {0xc5,0xf8,0x77,0xc3}; /* VZEROUPPER: outside the SSE2 local ABI */
    raw(avx, sizeof avx, CK_LOCAL_ABNORMAL, CK_LOCAL_FAULT);
    const uint8_t spoof[] = {0xcd,0x80,0xc3};
    raw(spoof, sizeof spoof, CK_LOCAL_ABNORMAL, CK_LOCAL_BAD_RETURN);
    /* Dirty output followed by UD2 must not publish even one byte. */
    uint8_t dirty[] = {0x48,0xb8,0,0,0,0,0,0,0,0,0xc6,0x00,0x41,0x0f,0x0b};
    uint64_t dest = WORK; memcpy(dirty+2,&dest,8);
    raw(dirty,sizeof dirty,CK_LOCAL_ABNORMAL,CK_LOCAL_FAULT);
    /* XMM0 and caller XMM14 survive timer exits; the initial user XMM14 is
     * zero rather than the kernel sentinel. The long loop forces preemption. */
    uint8_t fpcode[] = {0x48,0xb8,42,0,0,0,0,0,0,0,0x66,0x48,0x0f,0x6e,0xc0,
        0xb9,0x40,0x4b,0x4c,0, /* ECX = 5000000 */
        0xff,0xc9,0x75,0xfc,0x48,0xb8,0,0,0,0,0,0,0,0,
        0x66,0x0f,0xd6,0x00,0x66,0x44,0x0f,0xd6,0x70,0x08,
        0xb8,16,0,0,0,0xc3};
    dest = WORK; memcpy(fpcode+26,&dest,8);
    uint8_t saved_fp[512] __attribute__((aligned(16)));
    uint64_t sentinel[2] = {0x12345678,0x87654321}, observed[2];
    __asm__ volatile("fxsave64 %0; movdqu %1, %%xmm14" : "=m"(saved_fp) : "m"(sentinel) : "memory");
    ck_local_begin(93); running = &calls[0].result; running->state = CK_LOCAL_RUNNING;
    check(!prepare(fpcode,sizeof fpcode,0,0), "FP fixture prepared");
    ck_local_step();
    check(running != 0, "timer preempted register fixture");
    run_done();
    __asm__ volatile("movdqu %%xmm14, %0; fxrstor64 %1" : "=m"(observed) : "m"(saved_fp) : "memory");
    check(equal((void *)sentinel,(void *)observed,16), "kernel FP preserved");
    check(calls[0].result.state == CK_LOCAL_RETURNED && calls[0].result.length == 16 &&
        calls[0].result.output[0] == 42 && zeroed(calls[0].result.output+8,8),
        "user FP preserved and kernel FP not disclosed");
    const uint8_t loop[] = {0xeb,0xfe};
    raw(loop, sizeof loop, CK_LOCAL_ABNORMAL, CK_LOCAL_CPU);
    ck_local_begin(3); running = &calls[0].result; running->state = CK_LOCAL_RUNNING;
    check(!prepare(loop, sizeof loop, 0, 0), "cancel fixture prepared");
    ck_local_step(); ck_local_cancel(); reclaimed();
    check(calls[0].result.reason == CK_LOCAL_CANCELLED, "cancel between slices");
    ck_local_begin(4); running = &calls[0].result; running->state = CK_LOCAL_RUNNING;
    check(!prepare(loop, sizeof loop, 0, 0), "elapsed fixture prepared");
    started = ck_rdtsc() - hz; ck_local_step(); reclaimed();
    check(calls[0].result.reason == CK_LOCAL_ELAPSED, "elapsed ceiling");
    ck_local_wire_begin(77);
    ck_local_wire_line("CALL 77 1 1 24 0 010000000000000014000000000000001600000000000000");
    run_done();
    const ck_courier fake = {.send=wire_send};
    ck_local_wire_step(&fake);
    check(!memcmp(wire_answer, "RESULT 77 1 2 0 2a00000000000000", 31), "wire to real Omega result");
    wire_answer[0] = 0;
    ck_local_wire_line("POLL 76 1"); ck_local_wire_step(&fake);
    check(!wire_answer[0], "old wire request ignored");
    ck_local_wire_line("POLL 77 1"); ck_local_wire_step(&fake);
    check(wire_answer[0] == 'R', "result replay");
    ck_local_wire_begin(0);
    args[0] = 3; args[1] = -7; args[2] = 6;
    for (unsigned i = 0; i < 1000; i++) {
        ck_local_begin(100 + i);
        r = ck_local_start(100 + i, 1, 1, args, sizeof args); run_done();
        memcpy(&value, r->output, 8);
        check(r->state == CK_LOCAL_RETURNED && value == -42, "repeated successful cleanup");
    }
    ck_local_begin(0);
    ck_printf("local_test: PASS checks=%u repeated=1000 real_cpl3=yes\n", local_checks);
    /* Optional COM2 fixture is exercised by the full courier oracle. The
     * model selects calls; every result comes from this real CPL3 runner. */
    const ck_courier *wire = ck_x86_courier_init();
    if (wire) {
        uint64_t start = ck_rdtsc(); int sent = 0, done = 0;
        ck_local_wire_begin(4242);
        while (ck_rdtsc() - start < 30 * hz && !done) {
            char line[320];
            if (wire->ready() && !sent) {
                const char job[] = "JOB 4242 do Exercise the approved local tools.\tCheck arithmetic, text and system.\tlocal-tools=v1";
                check(!wire->send(job, sizeof job - 1), "courier test job sent"); sent = 1;
            }
            while (wire->take(line, sizeof line)) {
                if (ck_local_wire_line(line)) continue;
                if (!memcmp(line,"SAY 4242 1/1 VERIFIED",20)) done = 1;
            }
            ck_local_step(); ck_local_wire_step(wire);
        }
        check(done, "courier model loop completed");
        ck_local_wire_begin(0);
        ck_puts("local_courier: PASS real_cpl3_results=yes\n");
    }
}
