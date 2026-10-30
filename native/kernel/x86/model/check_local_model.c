// SPDX-License-Identifier: Apache-2.0
// Qualification only. Compare native Omega to the pinned independent C decoder.
//
// Usage: check_local_model EVIDENCE_DIR
// The directory contains omega-model-probe, reference-probe, llama.raw and the
// two pinned checkpoints. No Python or C inference runtime is linked into the
// iMac image; this host program only drives the two probes and compares them.
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/utsname.h>
#include <unistd.h>
#include "sha256.h"

#define MAX_ARGS 48
#define MAX_OUT 8192
#define MAX_CASES 16

static const char *root;
static bool need_qemu;

static void fail(const char *what, const char *detail) {
    fprintf(stderr, "LOCAL_MODEL_HOST: FAIL %s%s%s\n", what, detail ? ": " : "", detail ? detail : "");
    exit(1);
}

static void path_in(char *out, size_t cap, const char *name) {
    if ((size_t)snprintf(out, cap, "%s/%s", root, name) >= cap) fail("path too long", name);
}

static uint8_t *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) fail("cannot open", path);
    size_t cap = 1 << 20, n = 0;
    uint8_t *buf = malloc(cap);
    for (size_t got; buf && (got = fread(buf + n, 1, cap - n, f)) > 0;) {
        n += got;
        if (n == cap) buf = realloc(buf, cap *= 2);
    }
    if (!buf || ferror(f)) fail("cannot read", path);
    fclose(f);
    *len = n;
    return buf;
}

static void check_pin(const char *name, const char *pin) {
    char path[4096], hex[65];
    uint8_t digest[SHA256_DIGEST_SIZE];
    size_t len;
    path_in(path, sizeof path, name);
    uint8_t *data = slurp(path, &len);
    sha256_hash(data, len, digest);
    free(data);
    for (int i = 0; i < SHA256_DIGEST_SIZE; i++) sprintf(hex + 2 * i, "%02x", digest[i]);
    if (strcmp(hex, pin)) fail("checkpoint hash does not match its pin", name);
}

/* Run ARGV (without the qemu prefix), capture stdout, return the exit status. */
static int run(char **argv, bool with_prefix, bool quiet_stderr, char *out, size_t cap) {
    char *full[MAX_ARGS + 2];
    int n = 0;
    if (with_prefix && need_qemu) full[n++] = (char *)"qemu-x86_64";
    for (int i = 0; argv[i]; i++) {
        if (n >= MAX_ARGS) fail("too many arguments", NULL);
        full[n++] = argv[i];
    }
    full[n] = NULL;
    int fds[2];
    if (pipe(fds)) fail("pipe", strerror(errno));
    pid_t pid = fork();
    if (pid < 0) fail("fork", strerror(errno));
    if (pid == 0) {
        dup2(fds[1], 1);
        close(fds[0]); close(fds[1]);
        if (quiet_stderr) dup2(open("/dev/null", O_WRONLY), 2);
        execvp(full[0], full);
        _exit(127);
    }
    close(fds[1]);
    if (cap == 0) fail("empty capture buffer", NULL);
    size_t len = 0;
    bool overflow = false;
    char drain[256];
    for (;;) {
        bool full = len + 1 == cap;
        ssize_t got = read(fds[0], full ? drain : out + len,
                           full ? sizeof drain : cap - 1 - len);
        if (got < 0 && errno == EINTR) continue;
        if (got < 0) fail("read probe output", strerror(errno));
        if (got == 0) break;
        if (full) overflow = true;
        else len += (size_t)got;
    }
    close(fds[0]);
    out[len] = 0;
    int status;
    while (waitpid(pid, &status, 0) < 0)
        if (errno != EINTR) fail("waitpid", strerror(errno));
    if (overflow) fail("probe output exceeds capture limit", full[0]);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

/* Text after the first "ids=", trimmed, as the greedy-token fingerprint. */
static const char *ids_of(char *text, const char *case_name) {
    char *p = strstr(text, "ids=");
    if (!p) fail("no ids= in probe output", case_name);
    p += 4;
    size_t n = strlen(p);
    while (n && isspace((unsigned char)p[n - 1])) p[--n] = 0;
    return p;
}

static void json_string(FILE *f, const char *s) {
    fputc('"', f);
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') fprintf(f, "\\%c", *s);
        else if (*s == '\n') fputs("\\n", f);
        else if ((unsigned char)*s < 0x20) fprintf(f, "\\u%04x", *s);
        else fputc(*s, f);
    }
    fputc('"', f);
}

typedef struct {
    char name[64];
    bool invalid;
    double worst, max_abs;
    char native[MAX_OUT];
    int invalid_count, prompt_length;
} result;

static result results[MAX_CASES];
static int nresults;

static bool logit_lengths_ok(size_t a, size_t b, size_t vocab) {
    return vocab != 0 && vocab <= SIZE_MAX / 4 && a == vocab * 4 && b == a;
}

/* These negative controls require no weights or model runtime. An empty/short
 * pair must not qualify, nor may capture silently hide a probe's trailing output. */
static int self_test(void) {
    static const struct { size_t a, b, vocab; bool ok; } sizes[] = {
        { 0, 0, 512, false }, { 4, 4, 512, false },
        { 2044, 2044, 512, false }, { 2049, 2049, 512, false },
        { 2048, 2044, 512, false }, { 2048, 2048, 512, true },
        { 128000, 128000, 32000, true }, { 0, 0, SIZE_MAX, false },
    };
    unsigned checks = 0;
    for (size_t i = 0; i < sizeof sizes / sizeof *sizes; i++) {
        if (logit_lengths_ok(sizes[i].a, sizes[i].b, sizes[i].vocab) != sizes[i].ok)
            fail("self-test: incomplete logits accepted or valid logits refused", NULL);
        checks++;
    }
    static const struct { const char *cmd; int status; } captures[] = {
        { "head -c 0 /dev/zero", 0 }, { "head -c 31 /dev/zero", 0 },
        { "head -c 32 /dev/zero", 1 }, { "head -c 4096 /dev/zero", 1 },
    };
    for (size_t i = 0; i < sizeof captures / sizeof *captures; i++) {
        pid_t pid = fork();
        if (pid < 0) fail("self-test fork", strerror(errno));
        if (pid == 0) {
            int fd = open("/dev/null", O_WRONLY);
            if (fd < 0 || dup2(fd, 2) < 0) _exit(2);
            close(fd);
            char out[32];
            char *args[] = { (char *)"sh", (char *)"-c", (char *)captures[i].cmd, NULL };
            _exit(run(args, false, false, out, sizeof out));
        }
        int status;
        while (waitpid(pid, &status, 0) < 0)
            if (errno != EINTR) fail("self-test waitpid", strerror(errno));
        if (!WIFEXITED(status) || WEXITSTATUS(status) != captures[i].status)
            fail("self-test: capture boundary was not enforced", captures[i].cmd);
        checks++;
    }
    printf("LOCAL_MODEL_CHECKER_SELF_TEST: PASS checks=%u\n", checks);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--self-test")) return self_test();
    if (argc != 2) { fputs("usage: check_local_model EVIDENCE_DIR | --self-test\n", stderr); return 2; }
    root = realpath(argv[1], NULL);
    if (!root) fail("no such directory", argv[1]);
    struct utsname un;
    uname(&un);
    need_qemu = strcmp(un.machine, "x86_64") != 0;

    check_pin("stories15M.bin", "cd590644d963867a2b6e5a1107f51fad663c41d79c149fbecbbb1f95fa81f49a");
    check_pin("stories260K.bin", "b0a507e7ad0f626624f17112325e66691f9076d622e1d3274d103d00299f2696");

    static const struct { const char *model; int count; int plen; int prompt[8]; } cases[] = {
        { "stories260K", 1, 1, { 1 } }, { "stories260K", 8, 1, { 1 } },
        { "stories15M", 1, 1, { 1 } }, { "stories15M", 8, 1, { 1 } },
        { "stories15M", 32, 1, { 1 } }, { "stories15M", 8, 5, { 1, 9038, 2501, 263, 931 } },
    };
    char omega_probe[4096], ref_probe[4096], raw[4096];
    path_in(omega_probe, sizeof omega_probe, "omega-model-probe");
    path_in(ref_probe, sizeof ref_probe, "reference-probe");
    path_in(raw, sizeof raw, "llama.raw");

    for (size_t c = 0; c < sizeof cases / sizeof *cases; c++) {
        result *r = &results[nresults++];
        snprintf(r->name, sizeof r->name, "%s-%d-p%d", cases[c].model, cases[c].count, cases[c].plen);
        char ckpt[4096], omega_out[4096], ref_out[4096], count[16], file[96];
        snprintf(file, sizeof file, "%s.bin", cases[c].model);
        path_in(ckpt, sizeof ckpt, file);
        snprintf(file, sizeof file, "%s-omega.f32", r->name);
        path_in(omega_out, sizeof omega_out, file);
        snprintf(file, sizeof file, "%s-reference.f32", r->name);
        path_in(ref_out, sizeof ref_out, file);
        snprintf(count, sizeof count, "%d", cases[c].count);
        char prompt_s[8][16];
        char *na[MAX_ARGS], *ra[MAX_ARGS];
        int nn = 0, rn = 0;
        na[nn++] = omega_probe; na[nn++] = raw; na[nn++] = ckpt; na[nn++] = count; na[nn++] = omega_out;
        ra[rn++] = ref_probe; ra[rn++] = ckpt; ra[rn++] = count; ra[rn++] = ref_out;
        for (int i = 0; i < cases[c].plen; i++) {
            snprintf(prompt_s[i], sizeof prompt_s[i], "%d", cases[c].prompt[i]);
            na[nn++] = prompt_s[i]; ra[rn++] = prompt_s[i];
        }
        na[nn] = ra[rn] = NULL;
        static char ref_text[MAX_OUT];
        if (run(na, true, false, r->native, sizeof r->native)) fail("omega probe failed", r->name);
        if (run(ra, false, false, ref_text, sizeof ref_text)) fail("reference probe failed", r->name);
        char native_copy[MAX_OUT], ref_copy[MAX_OUT];
        snprintf(native_copy, sizeof native_copy, "%s", r->native);
        snprintf(ref_copy, sizeof ref_copy, "%s", ref_text);
        if (strcmp(ids_of(native_copy, r->name), ids_of(ref_copy, r->name))) fail("greedy token ids differ", r->name);

        size_t alen, blen;
        uint8_t *ab = slurp(omega_out, &alen), *bb = slurp(ref_out, &blen);
        size_t vocab = !strcmp(cases[c].model, "stories260K") ? 512 : 32000;
        if (!logit_lengths_ok(alen, blen, vocab)) fail("logit files must contain the full vocabulary", r->name);
        const float *a = (const float *)(void *)ab, *b = (const float *)(void *)bb;
        for (size_t i = 0; i < alen / 4; i++)
            if (!isfinite(a[i]) || !isfinite(b[i])) fail("non-finite logit", r->name);
        r->worst = r->max_abs = 0;
        for (size_t i = 0; i < alen / 4; i++) {
            double x = a[i], y = b[i];
            double scaled = fabs(x - y) / (1e-3 + 1e-4 * fabs(y));
            if (scaled > r->worst) r->worst = scaled;
            if (fabs(x - y) > r->max_abs) r->max_abs = fabs(x - y);
        }
        free(ab); free(bb);
        if (r->worst > 1) { fprintf(stderr, "scaled error %.17g\n", r->worst); fail("logits outside tolerance", r->name); }
        snprintf(file, sizeof file, "%s.txt", r->name);
        char txt[4096];
        path_in(txt, sizeof txt, file);
        FILE *f = fopen(txt, "w");
        if (!f || fputs(r->native, f) == EOF || fputs(ref_text, f) == EOF || fclose(f)) fail("cannot write", txt);
    }

    /* Invalid inputs must trap instead of returning any generated tokens. */
    static const struct { int count; int plen; int token; } bad[] = {
        { 0, 1, 1 }, { 33, 1, 1 }, { 1, 1, 32000 }, { 1, 33, 1 },
    };
    char ckpt[4096], refused[4096];
    path_in(ckpt, sizeof ckpt, "stories15M.bin");
    path_in(refused, sizeof refused, "refused.f32");
    for (size_t c = 0; c < sizeof bad / sizeof *bad; c++) {
        char count[16], tok[16];
        snprintf(count, sizeof count, "%d", bad[c].count);
        snprintf(tok, sizeof tok, "%d", bad[c].token);
        char *args[MAX_ARGS];
        int n = 0;
        args[n++] = omega_probe; args[n++] = raw; args[n++] = ckpt; args[n++] = count; args[n++] = refused;
        for (int i = 0; i < bad[c].plen; i++) args[n++] = tok;
        args[n] = NULL;
        static char out[MAX_OUT];
        int code = run(args, true, true, out, sizeof out);
        if (code != 1 || !strstr(out, "trap=3") || !strstr(out, "REFUSED")) {
            char why[64];
            snprintf(why, sizeof why, "count=%d prompt_length=%d exit=%d", bad[c].count, bad[c].plen, code);
            fail("invalid input was not refused", why);
        }
        result *r = &results[nresults++];
        r->invalid = true;
        r->invalid_count = bad[c].count;
        r->prompt_length = bad[c].plen;
    }

    char json_path[4096];
    path_in(json_path, sizeof json_path, "host-parity.json");
    FILE *f = fopen(json_path, "w");
    if (!f) fail("cannot write", json_path);
    fputs("[\n", f);
    for (int i = 0; i < nresults; i++) {
        const result *r = &results[i];
        fputs("  {\n", f);
        if (r->invalid) {
            fprintf(f, "    \"invalid_count\": %d,\n    \"prompt_length\": %d,\n    \"refused\": true\n",
                    r->invalid_count, r->prompt_length);
        } else {
            char *nat = (char *)r->native;
            size_t nl = strlen(nat);
            while (nl && isspace((unsigned char)nat[nl - 1])) nat[--nl] = 0;
            fprintf(f, "    \"case\": \"%s\",\n    \"tokens_match\": true,\n", r->name);
            fprintf(f, "    \"worst_scaled_error\": %.17g,\n    \"max_abs_error\": %.17g,\n    \"native\": ",
                    r->worst, r->max_abs);
            json_string(f, nat);
            fputc('\n', f);
        }
        fprintf(f, "  }%s\n", i + 1 < nresults ? "," : "");
    }
    fputs("]\n", f);
    if (fclose(f)) fail("cannot write", json_path);
    printf("LOCAL_MODEL_HOST: PASS cases=%d independent_reference=yes\n", nresults);
    return 0;
}
