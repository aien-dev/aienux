/* Test-only raw-code admission is absent from the deployable image. */
static void model_reclaimed(void)
{
    reclaimed();
    check(!model_running && zeroed(model_pt, sizeof model_pt) &&
          zeroed(model_code, sizeof model_code) && zeroed(model_work, sizeof model_work),
          "model scratch/code/translations reclaimed");
}
static void model_run_done(void)
{
    uint64_t report_at = ck_rdtsc() + 10 * hz;
    while (running) {
        ck_local_step(); /* production elapsed quota bounds this */
        if (running && ck_rdtsc() >= report_at) {
            uint32_t progress; memcpy(&progress, model_work, 4);
            ck_printf("local_model_test: progress=%u cpu_ms=%llu\n", progress,
                (unsigned long long)(cpu_used / (hz / 1000)));
            report_at = ck_rdtsc() + 10 * hz;
        }
    }
    model_reclaimed();
}
static void model_begin_test(void)
{
    ck_local_begin(0); wipe(&model_result, sizeof model_result);
    running = &model_result; running->state = CK_LOCAL_RUNNING;
    check(!model_prepare(), "signed model prepared");
}
static void model_fault_probe(uint64_t address, int write, int execute)
{
    model_begin_test();
    uint8_t b[16] = {0x48,0xb8};
    memcpy(b+2, &address, 8);
    b[10] = execute ? 0xff : 0x48;
    b[11] = execute ? 0xe0 : write ? 0x89 : 0x8b;
    b[12] = 0; b[13] = 0xc3;
    memcpy(model_code, b, sizeof b);
    model_run_done();
    check(model_result.state == CK_LOCAL_ABNORMAL && model_result.reason == CK_LOCAL_FAULT &&
          !model_result.length, "model forbidden memory access contained");
}
static void model_selftest(void)
{
    check(model_ok, "owner-signed native model admitted");
    model_begin_test();
    model_run_done();
    static const uint32_t expected[] = {9038,2501,263,931,29892,727,471,263,
        2217,7826,4257,365,2354,29889,2296,18012,304,1708,5377,297,278,6575,
        845,457,29889,3118,2462,29892,1183,4446,263,4802};
    uint32_t count; memcpy(&count, model_result.output, 4);
    ck_printf("local_model_test: state=%u reason=%u count=%u bytes=%u cpu_ms=%llu elapsed_ms=%llu\n",
        model_result.state, model_result.reason, count, model_result.length,
        (unsigned long long)model_cpu_ms, (unsigned long long)model_elapsed_ms);
    check(model_result.state == CK_LOCAL_RETURNED && model_result.length == 192 && count == 32 &&
          equal(model_result.output+64, (const void *)expected, sizeof expected),
          "15M native Omega tokens exactly match independent reference");
    char text[768];
    check(model_decode(&model_result, text, sizeof text) == 32, "signed tokenizer decoded real output");
    ck_printf("local_model_test: generation PASS tokens=32 cpu_ms=%llu elapsed_ms=%llu real_cpl3=yes\n",
              (unsigned long long)model_cpu_ms, (unsigned long long)model_elapsed_ms);
    ck_printf("local_model_test: sample=%s\n", text);
    model_fault_probe(MODEL_WEIGHTS, 1, 0);
    model_fault_probe(MODEL_WEIGHTS, 0, 1);
    model_fault_probe(MODEL_CODE, 1, 0);
    model_fault_probe(MODEL_WORK, 0, 1);
    model_fault_probe(MODEL_WORK - PAGE, 0, 0);
    model_fault_probe(MODEL_WORK + sizeof model_work, 0, 0);
    model_fault_probe(kernel_root, 0, 0);
    model_begin_test();
    ((uint64_t *)input)[3] = 32000;
    model_run_done();
    check(model_result.state == CK_LOCAL_TRAPPED && model_result.reason == 3 && !model_result.length,
          "out-of-vocabulary input fails closed");
    model_begin_test();
    ((uint64_t *)input)[2]--;
    model_run_done();
    check(model_result.state == CK_LOCAL_TRAPPED && model_result.reason == 3 && !model_result.length,
          "wrong checkpoint length fails closed");
    model_begin_test(); ck_local_step(); ck_local_cancel(); model_reclaimed();
    check(model_result.reason == CK_LOCAL_CANCELLED, "model can cancel after one slice");
    model_begin_test(); started = ck_rdtsc() - 181 * hz; ck_local_step(); model_reclaimed();
    check(model_result.reason == CK_LOCAL_ELAPSED, "model elapsed ceiling enforced");
    model_begin_test(); cpu_used = 121 * hz; ck_local_step(); model_reclaimed();
    check(model_result.reason == CK_LOCAL_CPU, "model CPU ceiling enforced");
    int64_t arithmetic[] = {3,123,456};
    ck_local_begin(919);
    const struct ck_local_result *r = ck_local_start(919, 1, 1, arithmetic, sizeof arithmetic);
    run_done(); int64_t result; memcpy(&result, r->output, 8);
    check(r->state == CK_LOCAL_RETURNED && result == 56088, "small tools still work after model faults");
    ck_local_begin(0);
    model_demo_state = 2; /* no duplicate diagnostic run in fixture images */
    ck_puts("local_model_test: PASS isolation=1 quotas=1 cancellation=1 cleanup=1\n");
}
