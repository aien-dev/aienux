/* Boot-only experiment. Every floating-point inference operation is in llama.om.
 * This hardware/ABI adapter owns signatures, pages, deadlines and text display. */
static void model_init(void)
{
    struct cka_parsed p;
    struct cka_ident id;
    uint8_t fp[32], digest[32];
    const struct ck_local_artifact *a = &ck_model_artifact;
    if (!equal(ck_model_key, ck_local_keys[0], 32) ||
        cka_verify_profile(a->bytes, a->length, ck_local_keys, 1, &p, &id, fp,
                           CKA_TARGET_X86_64_LE, CKA_ABI_OMEGA_X86_MODEL_V1)) return;
    sha256_hash(a->bytes, a->length, digest);
    if (!equal(digest, a->hash, 32) || p.entry || p.ncaps || p.data.flen != 64 ||
        p.code.flen > sizeof model_code || ck_model_weights_end - ck_model_weights != 60816028)
        return;
    const uint8_t *bindings = a->bytes + p.payload_off + p.data.rel;
    sha256_hash(ck_model_weights, ck_model_weights_end - ck_model_weights, digest);
    if (!equal(digest, bindings, 32)) return;
    sha256_hash(ck_model_tokenizer, ck_model_tokenizer_end - ck_model_tokenizer, digest);
    if (!equal(digest, bindings + 32, 32)) return;
    model_bytes = a->bytes + p.payload_off; model_len = p.code.flen;
    sha256_hash(model_bytes, model_len, model_code_hash);
    model_ok = 1;
    ck_puts("local_model: admitted=1 model=TinyStories15M inference=Omega backend=CPU-f32 profile=3\n");
}
static void model_map(uint64_t address, const uint8_t *backing, size_t length, uint64_t flags)
{
    size_t pages = (length + PAGE - 1) / PAGE;
    for (size_t i = 0; i < pages; i++, address += PAGE) {
        unsigned dir = (unsigned)((address - BASE) >> 21);
        unsigned leaf = (unsigned)((address >> 12) & 511);
        pd[dir] = (uint64_t)(uintptr_t)model_pt[dir - 1] | USER | WRITE;
        model_pt[dir - 1][leaf] = (uint64_t)(uintptr_t)(backing + i * PAGE) | USER | flags;
    }
}
static int model_prepare(void)
{
    /* prepare supplies the common entry veneer, stack, input, and FP state. */
    const uint8_t placeholder[] = {0x0f, 0x0b};
    uint64_t args[] = {1, 32, 60816028 / 4, 1}; /* public BOS, greedy, 32 tokens */
    if (!model_ok || prepare(placeholder, sizeof placeholder, args, sizeof args)) return -1;
    model_running = 1;
    memcpy(model_code, model_bytes, model_len);
    uint8_t digest[32]; sha256_hash(model_code, model_len, digest);
    if (!equal(digest, model_code_hash, 32)) return -1;
    /* Remove the small tools' code and work mappings from this address space. */
    pt[1] = 0;
    for (unsigned i = 24; i < 56; i++) pt[i] = 0;
    model_map(MODEL_CODE, model_code, model_len, 0);
    model_map(MODEL_WORK, model_work, sizeof model_work, NX | WRITE);
    model_map(MODEL_WEIGHTS, ck_model_weights, ck_model_weights_end - ck_model_weights, NX);
    uint64_t *end = (void *)(stack + sizeof stack);
    end[-2] = MODEL_WORK; end[-4] = MODEL_WEIGHTS;
    frame.rip = MODEL_CODE;
    cpu_limit = 120 * hz; elapsed_limit = 180 * hz;
    return 0;
}
/* The tokenizer is a signed fixed asset. Bounds are checked before every read;
 * no tokenizer pointer is ever exposed to the model. */
static int model_decode(const struct ck_local_result *r, char *out, size_t cap)
{
    if (r->state != CK_LOCAL_RETURNED || r->length < 64 || cap < 1) return -1;
    uint32_t count; memcpy(&count, r->output, 4);
    if (count > 32 || r->length != 4 * (16 + count)) return -1;
    const uint8_t *end = ck_model_tokenizer_end;
    size_t used = 0;
    for (unsigned i = 0; i < count; i++) {
        uint32_t token; memcpy(&token, r->output + 4 * (16 + i), 4);
        if (token >= 32000) return -1;
        const uint8_t *p = ck_model_tokenizer + 4;
        uint32_t len = 0;
        for (uint32_t j = 0; j <= token; j++) {
            if (end - p < 8) return -1;
            memcpy(&len, p + 4, 4); p += 8;
            if (len > (size_t)(end - p)) return -1;
            if (j != token) p += len;
        }
        if (i == 0 && len && p[0] == ' ') { p++; len--; }
        if (len > cap - 1 - used) return -1;
        for (uint32_t j = 0; j < len; j++) {
            unsigned char c = p[j];
            out[used++] = (c >= 32 && c < 127) ? (char)c : ' ';
        }
    }
    out[used] = 0;
    return (int)count;
}
const char *ck_local_model_demo_step(void)
{
    static char display[1024];
    if (!model_demo_state) {
        model_demo_state = 1;
        if (!model_ok || running || active_request) {
            model_demo_state = 2;
            ck_puts("local_model: experiment skipped (admission or active request)\n");
            return 0;
        }
        wipe(&model_result, sizeof model_result);
        running = &model_result; running->state = CK_LOCAL_RUNNING;
        if (model_prepare()) { finish(CK_LOCAL_REFUSED, CK_LOCAL_ADMISSION); clear_task(); }
        ck_puts("local_model: starting public BOS experiment tokens=32 cpu_limit_s=120\n");
    }
    if (model_demo_state != 1 || model_running) return 0;
    model_demo_state = 2;
    char sample[768];
    int tokens = model_decode(&model_result, sample, sizeof sample);
    if (tokens < 0) {
        ck_printf("local_model: incomplete state=%u reason=%u\n", model_result.state, model_result.reason);
        return 0;
    }
    ck_printf("local_model: completed tokens=%u cpu_ms=%llu elapsed_ms=%llu real_cpl3=yes\n",
              (unsigned)tokens, (unsigned long long)model_cpu_ms, (unsigned long long)model_elapsed_ms);
    /* Only this fixed public prompt is logged, never a user conversation. */
    ck_printf("local_model: sample=%s\n", sample);
    ck_snprintf(display, sizeof display,
             "Native Omega model test: %u tokens in %llu ms on this iMac's CPU. TinyStories15M says: %s",
             (unsigned)tokens, (unsigned long long)model_elapsed_ms, sample);
    return display;
}
