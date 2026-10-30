/* boot_show_check.c -- host checker for the boot sequence QEMU gates
 * (scripts/qemu_ck_boot_show_test.sh):
 *   boot_show_check wav FILE   the sound a VM played (QEMU -audiodev wav):
 *       48 kHz stereo S16 expected; finds where the chirp starts and
 *       compares every frame of it with ck_chirp_fill (ui/chirp.c), the
 *       same formula the kernel played from. Prints one line, exit 0 when
 *       the chirp is there whole (correlation >= 0.99), 1 otherwise.
 *   boot_show_check ppm FILE   a QEMU screendump (P6): prints its size, the
 *       share of lit pixels and the brightest pixel; exit 0 when the screen
 *       is not blank (stars are lit: > 0.05% of pixels above 64).
 * Integer and double math on the host only. */
#include "../chirp.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char *slurp(const char *path, long *n)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    *n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc((size_t)*n + 1);
    if (!b || fread(b, 1, (size_t)*n, f) != (size_t)*n) {
        fclose(f);
        free(b);
        return 0;
    }
    fclose(f);
    return b;
}

static uint32_t le32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const unsigned char *p) { return (uint16_t)(p[0] | p[1] << 8); }

static int wav(const char *path)
{
    long n;
    unsigned char *b = slurp(path, &n);
    if (!b || n < 44 || memcmp(b, "RIFF", 4) || memcmp(b + 8, "WAVE", 4)) {
        printf("wav: %s unreadable or not a WAVE file\n", path);
        return 1;
    }
    /* walk the chunks for fmt and data */
    long at = 12, data = -1, dlen = 0;
    unsigned ch = 0, rate = 0, bits = 0;
    while (at + 8 <= n) {
        uint32_t len = le32(b + at + 4);
        if (!memcmp(b + at, "fmt ", 4) && len >= 16)
            ch = le16(b + at + 10), rate = le32(b + at + 12), bits = le16(b + at + 22);
        if (!memcmp(b + at, "data", 4)) {
            data = at + 8, dlen = (long)len;
            if (data + dlen > n || dlen == 0)
                dlen = n - data; /* QEMU fills in the length at close; a killed VM leaves 0 */
            break;
        }
        at += 8 + len + (len & 1);
    }
    if (data < 0 || ch != 2 || rate != CK_CHIRP_RATE || bits != 16) {
        printf("wav: format %u ch %u Hz %u bits (want 2 ch %u Hz 16 bits)\n", ch, rate, bits, CK_CHIRP_RATE);
        return 1;
    }
    long frames = dlen / 4;
    const int16_t *s = (const int16_t *)(const void *)(b + data);
    /* the chirp's first audible frame in the reference and in the capture */
    long ref0 = -1, cap0 = -1;
    int16_t *ref = malloc(sizeof(int16_t) * 2 * CK_CHIRP_FRAMES);
    ck_chirp_fill(ref, 0, CK_CHIRP_FRAMES);
    for (long i = 0; i < (long)CK_CHIRP_FRAMES && ref0 < 0; i++)
        if (abs(ref[2 * i]) > 200)
            ref0 = i;
    for (long i = 0; i < frames && cap0 < 0; i++)
        if (abs(s[2 * i]) > 200)
            cap0 = i;
    if (ref0 < 0 || cap0 < 0) {
        printf("wav: %ld frames (%ld ms), no chirp found (silent)\n", frames, frames / 48);
        free(ref);
        return 1;
    }
    /* best alignment within +-64 frames of the threshold crossing */
    long len = (long)CK_CHIRP_FRAMES - ref0, best_off = 0;
    double best = -2;
    for (long off = -64; off <= 64; off++) {
        double sxy = 0, sxx = 0, syy = 0;
        for (long i = 0; i < len; i++) {
            long c = cap0 + off + i, r = ref0 + i;
            if (c < 0 || c >= frames)
                continue;
            double x = s[2 * c], y = ref[2 * r];
            sxy += x * y, sxx += x * x, syy += y * y;
        }
        double corr = sxx > 0 && syy > 0 ? sxy / sqrt(sxx * syy) : -1;
        if (corr > best)
            best = corr, best_off = off;
    }
    long maxd = 0, covered = 0;
    for (long i = 0; i < len; i++) {
        long c = cap0 + best_off + i;
        if (c < 0 || c >= frames)
            continue;
        covered++;
        long d = labs((long)s[2 * c] - ref[2 * (ref0 + i)]);
        if (d > maxd)
            maxd = d;
    }
    long start = cap0 + best_off - ref0; /* capture frame of the night's 0 ms */
    printf("wav: %ld frames (%ld ms) 2 ch 48000 Hz; chirp at capture %ld ms, %ld of %ld chirp frames present, "
           "correlation %.4f, largest sample difference %ld\n",
           frames, frames / 48, (cap0 + best_off) / 48, covered, len, best, maxd);
    (void)start;
    free(ref);
    free(b);
    return best >= 0.99 && covered == len ? 0 : 1;
}

static int ppm(const char *path)
{
    long n;
    unsigned char *b = slurp(path, &n);
    unsigned w = 0, h = 0, mx = 0;
    int off = 0;
    if (!b || sscanf((char *)b, "P6 %u %u %u%n", &w, &h, &mx, &off) != 3 || mx != 255 ||
        (long)off + 1 + (long)w * h * 3 > n) {
        printf("ppm: %s unreadable\n", path);
        return 1;
    }
    const unsigned char *p = b + off + 1;
    unsigned long lit = 0, peak = 0;
    for (unsigned long i = 0; i < (unsigned long)w * h; i++) {
        unsigned l = (p[3 * i] * 2u + p[3 * i + 1] * 5u + p[3 * i + 2]) / 8u;
        if (l > 64)
            lit++;
        if (l > peak)
            peak = l;
    }
    double share = 100.0 * (double)lit / ((double)w * h);
    printf("ppm: %ux%u, %.3f%% of pixels lit (above 64 of 255), brightest %lu\n", w, h, share, peak);
    free(b);
    return share > 0.05 ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "wav"))
        return wav(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "ppm"))
        return ppm(argv[2]);
    fprintf(stderr, "usage: boot_show_check wav|ppm FILE\n");
    return 2;
}
