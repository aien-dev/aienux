/* sky_test.c -- host test of the real sky (ui/sky_real.c, ui/sky_data.c):
 *  1. the kernel's integer projection puts the catalogue's stars where the
 *     generator (floating point) put them, within a pixel at 3840 x 2160;
 *  2. a view built with integer trigonometry matches the generated one;
 *  3. sidereal time matches published values (Meeus, Astronomical
 *     Algorithms, 2nd ed., examples 12.a and 12.b; USNO's value at J2000);
 *  4. the zenith, taken back to J2000 by the kernel, goes forward again
 *     (Meeus's rigorous precession, chapter 21, in double precision) to
 *     the local sidereal time and the latitude;
 *  5. the sky is made at 3840 x 2160 (timed) and pictures are written for a
 *     person to look at: the whole sky after the eyes have adjusted, and
 *     pieces at full size.
 * Usage: sky_test OUTDIR. Ends "SKY_TEST: PASS" or "SKY_TEST: FAIL". */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../night.h"
#include "../sky_real.h"

static int fails, checks;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(c)) {                                                                                                    \
            fails++;                                                                                                   \
            printf("FAIL  ");                                                                                          \
        } else                                                                                                         \
            printf("ok    ");                                                                                          \
        printf(__VA_ARGS__);                                                                                           \
        printf("\n");                                                                                                  \
    } while (0)

/* ---- a PNG writer (stored deflate) ---- */
static uint32_t crc_t[256];
static uint32_t crc(uint32_t c, const uint8_t *p, size_t n)
{
    if (!crc_t[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = i;
            for (int k = 0; k < 8; k++)
                v = v & 1 ? 0xedb88320u ^ (v >> 1) : v >> 1;
            crc_t[i] = v;
        }
    c = ~c;
    while (n--)
        c = crc_t[(c ^ *p++) & 255] ^ (c >> 8);
    return ~c;
}
static void be32(uint8_t *p, uint32_t v) { p[0] = v >> 24, p[1] = v >> 16, p[2] = v >> 8, p[3] = v; }
static void chunk(FILE *f, const char *t, const uint8_t *d, uint32_t n)
{
    uint8_t h[8];
    be32(h, n), memcpy(h + 4, t, 4);
    fwrite(h, 1, 8, f), fwrite(d, 1, n, f);
    uint32_t c = crc(crc(0, (const uint8_t *)t, 4), d, n);
    be32(h, c), fwrite(h, 1, 4, f);
}
/* writes the w x h piece at (x0, y0) of a pitch-wide XRGB buffer, shrunk by s (box average) */
static void png(const char *path, const uint32_t *px, int pitch, int x0, int y0, int w, int h, int s)
{
    int ow = w / s, oh = h / s;
    size_t raw_n = (size_t)oh * (ow * 3 + 1);
    uint8_t *raw = malloc(raw_n);
    for (int y = 0; y < oh; y++) {
        uint8_t *r = raw + (size_t)y * (ow * 3 + 1);
        r[0] = 0;
        for (int x = 0; x < ow; x++)
            for (int k = 0; k < 3; k++) {
                unsigned acc = 0;
                for (int j = 0; j < s; j++)
                    for (int i = 0; i < s; i++)
                        acc += px[(size_t)(y0 + y * s + j) * pitch + x0 + x * s + i] >> (16 - 8 * k) & 255;
                r[1 + x * 3 + k] = (uint8_t)(acc / (s * s));
            }
    }
    size_t nb = (raw_n + 65534) / 65535, zn = 2 + raw_n + nb * 5 + 4;
    uint8_t *z = malloc(zn), *q = z;
    *q++ = 0x78, *q++ = 1;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_n; i++)
        a = (a + raw[i]) % 65521, b = (b + a) % 65521;
    for (size_t o = 0; o < raw_n; o += 65535) {
        size_t n = raw_n - o < 65535 ? raw_n - o : 65535;
        *q++ = o + n == raw_n;
        *q++ = n & 255, *q++ = n >> 8, *q++ = ~n & 255, *q++ = (~n >> 8) & 255;
        memcpy(q, raw + o, n), q += n;
    }
    be32(q, b << 16 | a), q += 4;
    FILE *f = fopen(path, "wb");
    if (!f) {
        printf("note: cannot write %s\n", path);
        free(raw), free(z);
        return;
    }
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    uint8_t ih[13];
    be32(ih, ow), be32(ih + 4, oh), ih[8] = 8, ih[9] = 2, ih[10] = ih[11] = ih[12] = 0;
    chunk(f, "IHDR", ih, 13);
    chunk(f, "IDAT", z, (uint32_t)(q - z));
    chunk(f, "IEND", NULL, 0);
    fclose(f);
    printf("note: wrote %s (%dx%d)\n", path, ow, oh);
    free(raw), free(z);
}

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static const double D2R = M_PI / 180.0;

/* Meeus 21.4: J2000 -> mean equator and equinox of the date */
static void precess_forward(double ra0, double dec0, double T, double *ra, double *dec)
{
    double as = 1.0 / 3600 * D2R;
    double zeta = (2306.2181 * T + 0.30188 * T * T + 0.017998 * T * T * T) * as;
    double z = (2306.2181 * T + 1.09468 * T * T + 0.018203 * T * T * T) * as;
    double th = (2004.3109 * T - 0.42665 * T * T - 0.041833 * T * T * T) * as;
    double A = cos(dec0) * sin(ra0 + zeta);
    double B = cos(th) * cos(dec0) * cos(ra0 + zeta) - sin(th) * sin(dec0);
    double C = sin(th) * cos(dec0) * cos(ra0 + zeta) + cos(th) * sin(dec0);
    *ra = atan2(A, B) + z;
    *dec = asin(C);
}

static double wrap180(double d)
{
    while (d > 180)
        d -= 360;
    while (d < -180)
        d += 360;
    return d;
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : NULL;
    const ck_sky_view *v = &ck_sky_the_view;
    char path[512];

    /* 1. the projection */
    double worst = 0;
    for (unsigned i = 0; i < ck_sky_nchecks; i++) {
        const ck_sky_check *c = &ck_sky_checks[i];
        const ck_sky_star *s = &ck_sky_stars[c->star];
        int16_t d[3] = {s->x, s->y, s->z};
        int32_t x16, y16;
        int ok = ck_sky_project(v, d, 3840, 1920, 864, &x16, &y16);
        double e = ok ? hypot(x16 - c->x16, y16 - c->y16) / 16.0 : 1e9;
        worst = e > worst ? e : worst;
    }
    CHECK(ck_sky_nchecks >= 4 && worst < 1.0, "projection: %u stars land where the generator put them, worst %.3f px",
          ck_sky_nchecks, worst);

    /* 2. the view, built with integer trigonometry */
    ck_sky_view w;
    ck_sky_view_at(&w, v->ra_mdeg * 1000, v->dec_mdeg * 1000, v->fov_mdeg, v->roll_mdeg);
    double dm = 0;
    for (int r = 0; r < 3; r++)
        for (int k = 0; k < 3; k++) {
            double e = fabs((double)w.rot_q30[r][k] - v->rot_q30[r][k]) / (1 << 30);
            dm = e > dm ? e : dm;
        }
    double dt = fabs((double)w.tan_half_q30 - v->tan_half_q30) / (1 << 30);
    CHECK(dm < 1e-4 && dt < 1e-4, "view: built in integers it matches the generated one (worst %.2g, tan %.2g)", dm, dt);

    /* 3. sidereal time */
    struct {
        int64_t t;
        double hours;
        const char *what;
    } st[] = {
        {946728000, 18.697374558, "2000 Jan 1 12:00 UT (USNO)"},
        {545011200, 13 + 10 / 60.0 + 46.3668 / 3600, "1987 Apr 10 0:00 UT (Meeus 12.a)"},
        {545080860, 8 + 34 / 60.0 + 57.0896 / 3600, "1987 Apr 10 19:21 UT (Meeus 12.b)"},
    };
    for (unsigned i = 0; i < sizeof st / sizeof st[0]; i++) {
        double h = ck_sky_gmst(st[i].t) / 4294967296.0 * 24;
        double e = fabs(h - st[i].hours) * 3600;
        CHECK(e < 0.02, "sidereal time at %s: %.6f h, off by %.4f s", st[i].what, h, e);
    }

    /* 4. the zenith: back to J2000 in integers, forward again in doubles */
    struct {
        int64_t t;
        int32_t lat, lon;
    } zs[] = {{1792368000, 29760427, -95369803}, /* 2026 Oct 19 (Houston-like place) */
              {1792368000, -33856000, 151215000},
              {946728000, 0, 0},
              {2524608000, 64000000, -21000000}}; /* 2050 */
    double zworst = 0;
    for (unsigned i = 0; i < sizeof zs / sizeof zs[0]; i++) {
        int16_t d[3];
        int32_t ra, dec;
        ck_sky_zenith(zs[i].t, zs[i].lat, zs[i].lon, d, &ra, &dec);
        double T = (zs[i].t - 946728000) / (36525.0 * 86400);
        double rd, dd;
        precess_forward(ra / 1e6 * D2R, dec / 1e6 * D2R, T, &rd, &dd);
        double lst = ck_sky_gmst(zs[i].t) / 4294967296.0 * 360 + zs[i].lon / 1e6;
        double e1 = fabs(wrap180(rd / D2R - lst)) * cos(dd), e2 = fabs(dd / D2R - zs[i].lat / 1e6);
        double e = hypot(e1, e2) * 3600;
        zworst = e > zworst ? e : zworst;
        printf("note: zenith at %lld, lat %.3f lon %.3f: J2000 RA %.4f Dec %.4f deg (date RA %.4f)\n",
               (long long)zs[i].t, zs[i].lat / 1e6, zs[i].lon / 1e6, ra / 1e6, dec / 1e6, fmod(lst + 720, 360));
    }
    CHECK(zworst < 30, "zenith: precessed back to J2000 and forward again, worst %.1f arcsec", zworst);
    CHECK(!CK_SKY_HAVE_LOCATION, "the location setting is unset by default");

    /* 5. the sky at 3840 x 2160 */
    uint32_t W = 3840, H = 2160, k = W * 256 / 1920;
    size_t nb = ck_night_mem_bytes(W, H, k);
    void *mem = malloc(nb);
    ck_night n;
    double best = 1e9;
    for (int r = 0; r < 3; r++) {
        double t0 = now_ms();
        int rc = ck_night_init(&n, W, H, k, (int32_t)W / 2, (int32_t)(H * 40 / 100), mem, nb);
        double t1 = now_ms() - t0;
        best = t1 < best ? t1 : best;
        CHECK(rc == 0, "the sky is made (%.1f ms)", t1);
    }
    printf("note: the sky made in %.1f ms at 3840x2160 (best of 3, this host, one core)\n", best);
    unsigned ng = 0;
    for (unsigned i = 0; i < CK_NIGHT_GALAXIES; i++)
        ng += n.gal[i].box.w > 0;
    CHECK(ng == ck_sky_ngalaxies, "galaxies in the view: %u (the catalogue's in the view: %u)", ng, ck_sky_ngalaxies);
    CHECK(n.ntw > 0, "the brightest stars twinkle: %u of them", n.ntw);
    ck_night_at(&n, CK_NIGHT_GONE_MS + 1000); /* the eyes adjusted, the holes gone */
    uint32_t *fb = malloc((size_t)W * H * 4);
    ck_canvas out = {fb, (int32_t)W, (int32_t)H};
    ck_rect all = {0, 0, (int32_t)W, (int32_t)H};
    ck_night_base(&n, &out, &all);
    ck_night_lights(&n, &out, &all, 0, 0);
    unsigned black = 0;
    uint64_t sum = 0;
    for (size_t i = 0; i < (size_t)W * H; i++) {
        uint32_t c = fb[i] & 0xffffff;
        black += c == 0;
        sum += (c >> 16 & 255) + (c >> 8 & 255) + (c & 255);
    }
    printf("note: mean level %.2f\n", sum / 3.0 / W / H);
    CHECK(black == 0, "no pixel of the sky is black (the night sky glows): %u black", black);
    /* 6. the Magellanic Clouds glow where they are: the median light (linear luminance, so the stars do not
     * count) at each catalogue galaxy's centre (inside 0.3 of its short half axis) over the median of a ring
     * at 1.3 to 1.6 long half axes. The generator puts the Clouds' Gaia light, raised to the catalogue total,
     * into the grid: about 2.5 times the surround at their centres (sky_data.txt, "cloud light"), about 2 on
     * screen after the 1 degree grid. Measured: LMC 1.99, SMC 1.81; with the screen-to-sky step of the grid off by
     * two (the bug fixed at 9ab1281) the glow lands elsewhere and both read 0.95 to 0.99. The bar is 1.5. */
    {
        static double lin[256];
        for (int i = 0; i < 256; i++)
            lin[i] = i <= 10 ? i / 255.0 / 12.92 : pow((i / 255.0 + 0.055) / 1.055, 2.4);
        size_t cap = (size_t)W * H / 4;
        double *in = malloc(cap * sizeof *in), *ring = malloc(cap * sizeof *ring);
        for (unsigned i = 0; i < ck_sky_ngalaxies; i++) {
            int32_t x16, y16;
            if (!ck_sky_project(&n.view, ck_sky_galaxies[i].dir, (int32_t)W, (int32_t)W / 2, (int32_t)(H * 40 / 100), &x16, &y16))
                continue;
            int32_t gx = (x16 + 8) >> 4, gy = (y16 + 8) >> 4;
            double a = (double)ck_sky_galaxies[i].a_q16 * W / 65536, b = (double)ck_sky_galaxies[i].b_q16 * W / 65536;
            size_t nin = 0, nring = 0;
            int32_t r = (int32_t)(1.6 * a) + 1;
            for (int32_t y = gy - r; y <= gy + r; y++)
                for (int32_t x = gx - r; x <= gx + r; x++) {
                    if (x < 0 || y < 0 || x >= (int32_t)W || y >= (int32_t)H)
                        continue;
                    double d = sqrt((double)(x - gx) * (x - gx) + (double)(y - gy) * (y - gy));
                    uint32_t c = fb[(size_t)y * W + (size_t)x];
                    double l = 0.2126 * lin[c >> 16 & 255] + 0.7152 * lin[c >> 8 & 255] + 0.0722 * lin[c & 255];
                    if (d <= 0.3 * b && nin < cap)
                        in[nin++] = l;
                    else if (d >= 1.3 * a && d <= 1.6 * a && nring < cap)
                        ring[nring++] = l;
                }
            qsort(in, nin, sizeof *in, cmp_double);
            qsort(ring, nring, sizeof *ring, cmp_double);
            double ratio = nin && nring && ring[nring / 2] > 0 ? in[nin / 2] / ring[nring / 2] : 0;
            CHECK(ratio >= 1.5, "galaxy %u glows at its place: centre %.5f over surround %.5f linear = %.2f (at least 1.5)", i,
                  nin ? in[nin / 2] : 0, nring ? ring[nring / 2] : 0, ratio);
        }
        free(in);
        free(ring);
    }
    if (dir) {
        snprintf(path, sizeof path, "%s/sky-whole.png", dir);
        png(path, fb, W, 0, 0, W, H, 2);
        /* pieces at full size: the view centre (where GW150914 came from), the Large Magellanic Cloud, Canopus */
        snprintf(path, sizeof path, "%s/sky-centre-1to1.png", dir);
        png(path, fb, W, 1920 - 640, 864 - 360, 1280, 720, 1);
        for (unsigned i = 0; i < ck_sky_ngalaxies && i < 2; i++) {
            ck_rect b = n.gal[i].box;
            int cx = b.x + b.w / 2, cy = b.y + b.h / 2;
            cx = cx < 640 ? 640 : cx > (int)W - 640 ? (int)W - 640 : cx;
            cy = cy < 360 ? 360 : cy > (int)H - 360 ? (int)H - 360 : cy;
            snprintf(path, sizeof path, "%s/sky-galaxy%u-1to1.png", dir, i);
            png(path, fb, W, cx - 640, cy - 360, 1280, 720, 1);
        }
        if (n.ntw) {
            int cx = n.tw[0].x >> 4, cy = n.tw[0].y >> 4;
            cx = cx < 320 ? 320 : cx > (int)W - 320 ? (int)W - 320 : cx;
            cy = cy < 180 ? 180 : cy > (int)H - 180 ? (int)H - 180 : cy;
            snprintf(path, sizeof path, "%s/sky-brightest-1to1.png", dir);
            png(path, fb, W, cx - 320, cy - 180, 640, 360, 1);
        }
    }
    printf("SKY_TEST: %s (%d checks, %d failed)\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails ? 1 : 0;
}
