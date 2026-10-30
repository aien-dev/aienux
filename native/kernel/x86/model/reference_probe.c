/* Independent upstream C reference, qualification only. Not linked into kernel. */
#define main upstream_main
#include CK_LLAMA_REFERENCE
#undef main
int main(int argc, char **argv) {
    if (argc < 4) return 2;
    Transformer t; build_transformer(&t, argv[1]);
    unsigned count = strtoul(argv[2], NULL, 10), nt = argc > 4 ? argc - 4 : 1;
    int token = argc > 4 ? atoi(argv[4]) : 1;
    float *logits = NULL;
    printf("REFERENCE_MODEL ids=");
    unsigned generated = 0;
    for (unsigned pos = 0; pos < count + nt - 1; pos++) {
        logits = forward(&t, token, pos);
        token = 0;
        for (int i = 1; i < t.config.vocab_size; i++) if (logits[i] > logits[token]) token = i;
        if (pos + 1 < nt) token = atoi(argv[5 + pos]);
        else {
            if (token == 1) break;
            printf("%s%d", generated++ ? "," : "", token);
        }
    }
    puts("");
    FILE *f = fopen(argv[3], "wb");
    if (!logits || !f || fwrite(logits, 4, t.config.vocab_size, f) != (size_t)t.config.vocab_size || fclose(f)) return 2;
    free_transformer(&t); return 0;
}
