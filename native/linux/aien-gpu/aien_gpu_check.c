/* aien_gpu_check: window-0 user-space check of /dev/aien-gpu (L332 rung 2). Prints what the module read,
 * maps the arena, writes a pattern through the mapping and reads it back. Exit 0 = all checks passed. */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "aien_gpu_uapi.h"

int main(void)
{
    int fails = 0, fd = open("/dev/aien-gpu", O_RDWR);
    if (fd < 0) { perror("open /dev/aien-gpu"); return 2; }
    struct aien_gpu_info in = {0};
    if (ioctl(fd, AIEN_GPU_INFO, &in) != 0) { perror("AIEN_GPU_INFO"); return 2; }
    printf("boot0=%08x boot42=%08x bar0=%llu fw_bytes=%llu fw_ok=%u arena=%llu iova=%llx\n", in.boot0, in.boot42,
           (unsigned long long)in.bar0_bytes, (unsigned long long)in.fw_bytes, in.fw_ok,
           (unsigned long long)in.arena_bytes, (unsigned long long)in.arena_iova);
    fails += in.boot0 == 0 || in.boot0 == 0xffffffffu; /* all-ones = BAR0 not answering */
    fails += !in.fw_ok;
    volatile uint32_t *a = mmap(0, in.arena_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (a == MAP_FAILED) { perror("mmap"); return 2; }
    uint64_t n = in.arena_bytes / 4;
    for (uint64_t i = 0; i < n; i++) a[i] = (uint32_t)(i * 2654435761u);
    for (uint64_t i = 0; i < n; i++) if (a[i] != (uint32_t)(i * 2654435761u)) { fails++; break; }
    printf("arena pattern %s, checks failed %d\n", fails ? "or identity FAIL" : "PASS", fails);
    munmap((void *)a, in.arena_bytes);
    close(fd);
    return fails ? 1 : 0;
}
