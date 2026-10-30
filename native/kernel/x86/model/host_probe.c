/* Test-only file/ABI adapter. All inference executes the compiled Omega blob. */
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

uint64_t host_saved_sp, host_vtable[32];
unsigned host_trap_code;
extern void host_trap(void);
extern uint64_t omega_run(void *, const uint64_t *, uint32_t *, uint64_t, const uint32_t *);
static void *map(const char *path, size_t *len, int prot) {
    int fd = open(path, O_RDONLY); struct stat st;
    if (fd < 0 || fstat(fd, &st) || st.st_size < 1) { perror(path); exit(2); }
    *len = st.st_size;
    void *p = mmap(NULL, *len, prot, MAP_PRIVATE, fd, 0); close(fd);
    if (p == MAP_FAILED) { perror("mmap"); exit(2); }
    return p;
}
int main(int argc, char **argv) {
    if (argc < 5) { fputs("probe RAW WEIGHTS NEW OUT [PROMPT_ID...]\n", stderr); return 2; }
    size_t nc, nw;
    void *code = map(argv[1], &nc, PROT_READ | PROT_EXEC);
    const uint32_t *weights = map(argv[2], &nw, PROT_READ);
    uint32_t *work = calloc(1048576, 4);
    if (!work || nw % 4 || nw > 64000000 || nc > 131072) return 2;
    uint64_t args[512] = {1, strtoul(argv[3], NULL, 10), nw / 4, 1};
    if (argc > 5) {
        args[0] = argc - 5;
        for (int i = 5; i < argc; i++) args[i - 2] = strtoul(argv[i], NULL, 10);
    }
    for (unsigned i = 0; i < 32; i++) host_vtable[i] = (uintptr_t)host_trap;
    struct timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
    uint64_t bytes = omega_run(code, args, work, (args[0] + 3) * 8, weights);
    clock_gettime(CLOCK_MONOTONIC, &b);
    double elapsed = b.tv_sec - a.tv_sec + (b.tv_nsec - a.tv_nsec) / 1e9;
    printf("OMEGA_MODEL trap=%u bytes=%llu tokens=%u elapsed=%.6f ids=", host_trap_code,
           (unsigned long long)bytes, work[0], elapsed);
    if (host_trap_code || work[0] > 32 || bytes != 4 * (16 + work[0])) { puts("REFUSED"); return 1; }
    for (unsigned i = 0; i < work[0]; i++) printf("%s%u", i ? "," : "", work[16+i]);
    puts("");
    if (work[1] + work[2] > 1048576) return 2;
    FILE *f = fopen(argv[4], "wb");
    if (!f || fwrite(work + work[1], 4, work[2], f) != work[2] || fclose(f)) return 2;
    return 0;
}
