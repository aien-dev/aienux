/* ck_sky_gen.c -- makes native/kernel/ui/sky_data.c: the real sky Allen
 * opens on, as tables the kernel draws from with integer arithmetic only.
 *
 * Inputs (scripts/ck_sky_gen.sh downloads them, checks each against its
 * sha256 and unpacks the two .gz files; nothing is fetched while the kernel
 * runs), all in the --cache directory:
 *   bsc5.dat            Yale Bright Star Catalogue, 5th revised edition
 *                       (Hoffleit and Warren 1991; CDS V/50): every star to
 *                       about magnitude 6.5, the naked-eye limit, with J2000
 *                       position, proper motion, V magnitude and B-V colour;
 *   gaia_hpx6.csv      Gaia DR3 source counts and integrated G-band flux per HEALPix


 *   NGC.csv, addendum.csv
 *                       OpenNGC (Mattia Verga, CC BY-SA 4.0): galaxy
 *                       positions, sizes and orientations;
 *   GW150914_skymap.fits
 *                       the GWTC-1 sky map of GW150914 (LIGO/Virgo,
 *                       LIGO-P1800381): where the first black-hole merger
 *                       ever heard came from.
 *
 * What it works out (floating point is fine here; the kernel gets integers):
 *   - the view: a gnomonic (pinhole camera) projection centred on GW150914's
 *     most probable position, north up, a set width of sky across the screen;
 *   - the stars in that view as unit vectors (the kernel projects them), with
 *     a brightness code (flux = 10^(-0.4 m)) and a colour from B-V through a
 *     real temperature (Ballesteros 2012) to the colour of a black body of
 *     that temperature (CIE 1931 observer, sRGB);


 *   - the galaxies that are in the view and bright enough to see with the eye,
 *     at their true size and angle.
 *
 * Usage: ck_sky_gen --cache DIR --out sky_data.c [--report FILE]
 *        [--inputs FILE] [--ra DEG --dec DEG] [--fov DEG] [--roll DEG]
 *        [--mag LIMIT] [--grid N] [--gain X] [--skyglow MAG]
 *        [--exposure X]
 * Host tool only; plain C11 and libm.
 */
#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define EPOCH 2026.8 /* the stars are moved along their proper motion to this year */
/* The HEALPix level of the Gaia aggregates: 6 is 49152 cells (nside 64), 7 is 196608 (nside 128).
 * scripts/ck_sky_gen.sh sets it (SKY_HPX_LEVEL); 6 is the committed default. */
#ifndef HPX_LEVEL
#define HPX_LEVEL 6
#endif
#define NSIDE (1 << HPX_LEVEL)
#define NCELLS (12 * NSIDE * NSIDE)
/* Gaia DR3 Vega zero points: a source of 1 e-/s has G 25.6874, BP 25.3385, RP 24.7479 (Gaia DR3 documentation,
 * section 5.4.1, Table 5.4; Riello et al. 2021, A&A 649, A3). */
#define GAIA_ZP_G 25.6874
#define GAIA_ZP_BP 25.3385
#define GAIA_ZP_RP 24.7479

/* The screen shape the grid is made for: centre of the view at half the width
 * and 40% of the height (where allen_ui.c puts the merger); rows cover a 16:10
 * screen so a 1280x800 one is covered as well as 3840x2160. */
#define CX_FRAC 0.5
#define CY_FRAC 0.40
#define H_OVER_W_MAX (10.0 / 16.0)

static const double PI = 3.14159265358979323846;

static void die(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
#include <stdarg.h>
static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "ck_sky_gen: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p)
        die("out of memory (%zu bytes)", n);
    return p;
}

static void *xrealloc(void *p, size_t n)
{
    p = realloc(p, n ? n : 1);
    if (!p)
        die("out of memory (%zu bytes)", n);
    return p;
}

static double rad(double d) { return d * (PI / 180.0); }
static double deg(double r) { return r * (180.0 / PI); }
static double pmod(double x, double m)
{
    double r = fmod(x, m);
    return r < 0 ? r + m : r;
}
static long rnd(double x) { return (long)nearbyint(x); } /* half to even, like the rest of the tables' maths */
static double clampd(double x, double lo, double hi) { return x < lo ? lo : x > hi ? hi : x; }

/* ---- a text report, kept in memory and printed at the end ---- */

static char *rep;
static size_t replen;
static void report(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void report(const char *fmt, ...)
{
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0)
        die("report format");
    if ((size_t)n >= sizeof buf)
        n = (int)sizeof buf - 1;
    rep = xrealloc(rep, replen + (size_t)n + 2);
    memcpy(rep + replen, buf, (size_t)n);
    replen += (size_t)n;
    rep[replen++] = '\n';
    rep[replen] = 0;
}

static uint8_t *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        die("cannot open %s (run scripts/ck_sky_gen.sh, it fetches the inputs)", path);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = xmalloc((size_t)n + 1);
    if (fread(b, 1, (size_t)n, f) != (size_t)n)
        die("short read of %s", path);
    fclose(f);
    b[n] = 0;
    *len = (size_t)n;
    return b;
}

/* ---- small vector helpers ---- */

typedef struct {
    double x, y, z;
} vec3;

static vec3 vec(double ra, double dec)
{
    double r = rad(ra), d = rad(dec);
    return (vec3){cos(d) * cos(r), cos(d) * sin(r), sin(d)};
}
static double dot(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static double sep_deg(vec3 a, vec3 b) { return deg(acos(clampd(dot(a, b), -1.0, 1.0))); }

/* ---- the GW150914 sky map (HEALPix, nested order) ---- */

static void healpix_nest_ang(int nside, long ipix, double *ra, double *dec)
{
    static const int jrll[12] = {2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4};
    static const int jpll[12] = {1, 3, 5, 7, 0, 2, 4, 6, 1, 3, 5, 7};
    long npface = (long)nside * nside;
    int face = (int)(ipix / npface);
    long ipf = ipix % npface;
    long ix = 0, iy = 0;
    for (int b = 0; b < 16; b++) {
        ix |= ((ipf >> (2 * b)) & 1) << b;
        iy |= (((ipf >> 1) >> (2 * b)) & 1) << b;
    }
    long nl4 = 4L * nside;
    long jr = (long)jrll[face] * nside - ix - iy - 1;
    long nr, kshift;
    double z;
    if (jr < nside) {
        nr = jr;
        z = 1 - (double)(jr * jr) / (3.0 * (double)npface);
        kshift = 0;
    } else if (jr > 3L * nside) {
        nr = nl4 - jr;
        z = (double)(nr * nr) / (3.0 * (double)npface) - 1;
        kshift = 0;
    } else {
        nr = nside;
        z = (double)(2L * nside - jr) * 2.0 / (3.0 * nside);
        kshift = (jr - nside) & 1;
    }
    long t = (long)jpll[face] * nr + ix - iy + 1 + kshift;
    long jp = t >= 0 ? t / 2 : -((-t + 1) / 2); /* floor division */
    if (jp > nl4)
        jp -= nl4;
    if (jp < 1)
        jp += nl4;
    double phi = ((double)jp - (double)(kshift + 1) * 0.5) * (PI / 2 / (double)nr);
    *ra = pmod(deg(phi), 360.0);
    *dec = 90.0 - deg(acos(z));
}

static const double *g_prob;
static int by_prob(const void *a, const void *b)
{
    long i = *(const long *)a, j = *(const long *)b;
    if (g_prob[i] != g_prob[j])
        return g_prob[i] > g_prob[j] ? -1 : 1;
    return i < j ? -1 : i > j; /* ties by pixel number, like a stable sort */
}

typedef struct {
    int nside;
    double max_ra, max_dec, area90;
    long n90;
    double *region_ra, *region_dec;
} gwmap;

/* FITS: the header is a run of 2880-byte blocks of 80-character cards; the
 * binary table's first column is PROB, big-endian doubles, NAXIS1 bytes a row. */
static int fits_card_int(const char *blk, const char *key, long *out)
{
    for (int i = 0; i < 36; i++) {
        const char *c = blk + i * 80;
        size_t kl = strlen(key);
        if (strncmp(c, key, kl) == 0 && (c[kl] == ' ' || kl == 8) && c[8] == '=') {
            *out = strtol(c + 10, NULL, 10);
            return 1;
        }
    }
    return 0;
}

static int fits_has_end(const char *blk)
{
    for (int i = 0; i < 36; i++)
        if (strncmp(blk + i * 80, "END     ", 8) == 0)
            return 1;
    return 0;
}

static gwmap gw_load(const char *path)
{
    size_t len;
    uint8_t *raw = slurp(path, &len);
    size_t off = 0;
    int hdr = 0;
    long nside = 0, rows = 0, width = 0;
    int nested = 1;
    for (;;) {
        if (off + 2880 > len)
            die("%s: header runs off the end", path);
        const char *blk = (const char *)raw + off;
        off += 2880;
        if (hdr == 1) {
            fits_card_int(blk, "NSIDE", &nside);
            fits_card_int(blk, "NAXIS1", &width);
            fits_card_int(blk, "NAXIS2", &rows);
            for (int i = 0; i < 36; i++) {
                const char *c = blk + i * 80;
                if (strncmp(c, "ORDERING", 8) == 0 && c[8] == '=')
                    nested = strstr(c, "NEST") != NULL;
            }
        }
        if (fits_has_end(blk)) {
            if (++hdr == 2)
                break;
        }
    }
    if (!nested)
        die("%s: not NESTED ordering", path);
    if (!nside || !rows || width < 8 || off + (size_t)rows * (size_t)width > len)
        die("%s: bad table (nside %ld rows %ld width %ld)", path, nside, rows, width);
    double *prob = xmalloc((size_t)rows * sizeof(double));
    for (long i = 0; i < rows; i++) {
        const uint8_t *p = raw + off + (size_t)i * (size_t)width;
        uint64_t u = 0;
        for (int k = 0; k < 8; k++)
            u = (u << 8) | p[k];
        memcpy(&prob[i], &u, 8);
    }
    long *order = xmalloc((size_t)rows * sizeof(long));
    for (long i = 0; i < rows; i++)
        order[i] = i;
    g_prob = prob;
    qsort(order, (size_t)rows, sizeof(long), by_prob);
    double tot = 0;
    for (long i = 0; i < rows; i++)
        tot += prob[i];
    double cum = 0;
    long n90 = 0;
    for (long k = 0; k < rows; k++) {
        cum += prob[order[k]];
        if (cum >= 0.9 * tot) {
            n90 = k + 1;
            break;
        }
    }
    gwmap g;
    g.nside = (int)nside;
    healpix_nest_ang(g.nside, order[0], &g.max_ra, &g.max_dec);
    double pixarea = 4 * PI / (12.0 * (double)nside * (double)nside) * (180 / PI) * (180 / PI);
    g.area90 = (double)n90 * pixarea;
    g.n90 = n90;
    g.region_ra = xmalloc((size_t)n90 * sizeof(double));
    g.region_dec = xmalloc((size_t)n90 * sizeof(double));
    for (long k = 0; k < n90; k++)
        healpix_nest_ang(g.nside, order[k], &g.region_ra[k], &g.region_dec[k]);
    free(order);
    free(prob);
    free(raw);
    return g;
}

/* ---- the view ---- */

typedef struct {
    double ra, dec, fov, roll;
    vec3 rows[3]; /* right, up, forward */
    double t;     /* tangent-plane units across half the width */
} view_t;

static view_t view_make(double ra, double dec, double fov, double roll)
{
    view_t v;
    v.ra = ra, v.dec = dec, v.fov = fov, v.roll = roll;
    double a = rad(ra), d = rad(dec);
    vec3 d0 = vec(ra, dec);
    vec3 east = {-sin(a), cos(a), 0.0};
    vec3 north = {-sin(d) * cos(a), -sin(d) * sin(a), cos(d)};
    vec3 west = {-east.x, -east.y, -east.z};
    double cr = cos(rad(roll)), sr = sin(rad(roll));
    /* screen right and screen up; roll turns the view clockwise on screen */
    v.rows[0] = (vec3){cr * west.x + sr * north.x, cr * west.y + sr * north.y, cr * west.z + sr * north.z};
    v.rows[1] = (vec3){-sr * west.x + cr * north.x, -sr * west.y + cr * north.y, -sr * west.z + cr * north.z};
    v.rows[2] = d0;
    v.t = tan(rad(fov) / 2);
    return v;
}

/* sky direction -> screen px (x right, y down); 0 if behind the camera */
static int to_screen(const view_t *v, vec3 s, double w, double h, double *px, double *py)
{
    double x = dot(v->rows[0], s), y = dot(v->rows[1], s), z = dot(v->rows[2], s);
    if (z <= 1e-6)
        return 0;
    double p = (w / 2) / v->t;
    *px = w * CX_FRAC + p * x / z;
    *py = h * CY_FRAC - p * y / z;
    return 1;
}

/* screen offset from the view centre, in widths -> sky direction */
static vec3 from_frac(const view_t *v, double fx, double fy)
{
    double u = fx * 2 * v->t, w = -fy * 2 * v->t;
    vec3 d;
    d.x = v->rows[2].x + u * v->rows[0].x + w * v->rows[1].x;
    d.y = v->rows[2].y + u * v->rows[0].y + w * v->rows[1].y;
    d.z = v->rows[2].z + u * v->rows[0].z + w * v->rows[1].z;
    double n = sqrt(dot(d, d));
    return (vec3){d.x / n, d.y / n, d.z / n};
}

/* ---- equatorial (J2000) to galactic (Hipparcos Vol. 1, sect. 1.5.3) ---- */

static const vec3 GAL[3] = {{-0.0548755604, -0.8734370902, -0.4838350155},
                            {0.4941094279, -0.4448296300, 0.7469822445},
                            {-0.8676661490, -0.1980763734, 0.4559837762}};

static void galactic(vec3 v, double *l, double *b)
{
    double g0 = dot(GAL[0], v), g1 = dot(GAL[1], v), g2 = dot(GAL[2], v);
    *l = pmod(deg(atan2(g1, g0)), 360.0);
    *b = deg(asin(clampd(g2, -1, 1)));
}

/* ---- colour: B-V -> temperature -> black body -> sRGB ---- */

/* B-V -> effective temperature, K (Ballesteros 2012, EPL 97, 34008) */
static double ballesteros(double bv) { return 4600.0 * (1.0 / (0.92 * bv + 1.7) + 1.0 / (0.92 * bv + 0.62)); }

static double lobe(double x, double mu, double s1, double s2)
{
    double s = x < mu ? s1 : s2, t = (x - mu) / s;
    return exp(-0.5 * t * t);
}

/* CIE 1931 2-degree observer, the multi-lobe fit of Wyman, Sloan and Shirley
 * 2013 (JCGT 2(2)); lam in nm */
static void cie1931(double lam, double *xb, double *yb, double *zb)
{
    *xb = 1.056 * lobe(lam, 599.8, 37.9, 31.0) + 0.362 * lobe(lam, 442.0, 16.0, 26.7) - 0.065 * lobe(lam, 501.1, 20.4, 26.2);
    *yb = 0.821 * lobe(lam, 568.8, 46.9, 40.5) + 0.286 * lobe(lam, 530.9, 16.3, 31.1);
    *zb = 1.217 * lobe(lam, 437.0, 11.8, 36.0) + 0.681 * lobe(lam, 459.0, 26.0, 13.8);
}

/* linear sRGB of a black body at T kelvin, brightest channel 1 */
static void blackbody_rgb(double T, double out[3])
{
    double X = 0, Y = 0, Z = 0;
    for (int lam = 380; lam <= 780; lam += 5) {
        double l = lam * 1e-9;
        double b = 1.0 / (pow(l, 5) * (exp(0.014387769 / (l * T)) - 1.0));
        double xb, yb, zb;
        cie1931(lam, &xb, &yb, &zb);
        X += b * xb, Y += b * yb, Z += b * zb;
    }
    double r = 3.2404542 * X - 1.5371385 * Y - 0.4985314 * Z;
    double g = -0.9692660 * X + 1.8760108 * Y + 0.0415560 * Z;
    double bl = 0.0556434 * X - 0.2040259 * Y + 1.0572252 * Z;
    r = r > 0 ? r : 0, g = g > 0 ? g : 0, bl = bl > 0 ? bl : 0;
    double m = r > g ? r : g;
    m = m > bl ? m : bl;
    out[0] = r / m, out[1] = g / m, out[2] = bl / m;
}

static double srgb_decode(double v)
{
    v /= 255.0;
    return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
}

static double srgb_encode(double l)
{
    l = clampd(l, 0.0, 1.0);
    double v = l <= 0.0031308 ? l * 12.92 : 1.055 * pow(l, 1 / 2.4) - 0.055;
    return v * 255.0;
}

/* ---- the Yale Bright Star Catalogue ---- */

typedef struct {
    int hr;
    char name[16];
    double ra, dec, v, bv;
    int has_bv;
} star_t;

static void field(const char *line, int a, int b, char *out)
{
    int n = b - a;
    memcpy(out, line + a, (size_t)n);
    out[n] = 0;
}

static int blank(const char *s)
{
    for (; *s; s++)
        if (!isspace((unsigned char)*s))
            return 0;
    return 1;
}

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1]))
        s[--n] = 0;
    char *p = s;
    while (isspace((unsigned char)*p))
        p++;
    memmove(s, p, strlen(p) + 1);
}

static star_t *bsc5_load(const char *path, size_t *count, int *nobv)
{
    size_t len;
    char *txt = (char *)slurp(path, &len);
    size_t cap = 10000, n = 0;
    star_t *st = xmalloc(cap * sizeof *st);
    *nobv = 0;
    char *p = txt;
    while (*p) {
        char *e = strchr(p, '\n');
        size_t ll = e ? (size_t)(e - p) : strlen(p);
        char line[256];
        memset(line, ' ', sizeof line);
        memcpy(line, p, ll < 197 ? ll : 197);
        line[197] = 0;
        p = e ? e + 1 : p + ll;
        char f[32];
        field(line, 75, 77, f);
        int nopos = blank(f);
        field(line, 102, 107, f);
        if (nopos || blank(f))
            continue; /* the 14 entries with no position (novae, clusters taken out) */
        star_t s;
        memset(&s, 0, sizeof s);
        field(line, 0, 4, f);
        s.hr = atoi(f);
        double rah, ram, ras;
        field(line, 75, 77, f), rah = atof(f);
        field(line, 77, 79, f), ram = atof(f);
        field(line, 79, 83, f), ras = atof(f);
        double ra = (rah + ram / 60 + ras / 3600) * 15;
        double dd, dm, ds;
        field(line, 84, 86, f), dd = atof(f);
        field(line, 86, 88, f), dm = atof(f);
        field(line, 88, 90, f), ds = atof(f);
        double dec = dd + dm / 60 + ds / 3600;
        if (line[83] == '-')
            dec = -dec;
        field(line, 102, 107, f);
        s.v = atof(f);
        field(line, 109, 114, f);
        if (!blank(f))
            s.bv = atof(f), s.has_bv = 1;
        else
            (*nobv)++;
        double pmra = 0, pmde = 0;
        field(line, 148, 154, f);
        if (!blank(f))
            pmra = atof(f); /* arcsec/yr, along RA (times cos dec) */
        field(line, 154, 160, f);
        if (!blank(f))
            pmde = atof(f);
        double dt = EPOCH - 2000.0;
        double dec2 = dec + pmde * dt / 3600;
        double cd = cos(rad(dec));
        double ra2 = ra + pmra * dt / 3600 / (cd > 1e-6 ? cd : 1e-6);
        field(line, 4, 14, s.name);
        trim(s.name);
        s.ra = pmod(ra2, 360.0);
        s.dec = dec2;
        if (n == cap)
            st = xrealloc(st, (cap *= 2) * sizeof *st);
        st[n++] = s;
    }
    free(txt);
    *count = n;
    return st;
}

/* ---- OpenNGC ---- */

typedef struct {
    char name[32], common[160];
    double ra, dec, vmag, bv, maj, min, pa;
    int has_bv;
} galaxy_t;

/* splits one ';' separated line in place; fields may be "quoted" */
static int csv_split(char *line, char **f, int max)
{
    int n = 0;
    char *p = line;
    for (;;) {
        if (n == max)
            return n;
        if (*p == '"') {
            char *w = ++p;
            f[n++] = w;
            for (;;) {
                if (!*p)
                    return n;
                if (*p == '"') {
                    if (p[1] == '"') {
                        *w++ = '"', p += 2;
                        continue;
                    }
                    p++;
                    break;
                }
                *w++ = *p++;
            }
            *w = 0;
            if (*p == ';') {
                p++;
                continue;
            }
            return n;
        }
        f[n++] = p;
        while (*p && *p != ';')
            p++;
        if (*p == ';') {
            *p++ = 0;
            continue;
        }
        return n;
    }
}

static int col_of(char **h, int nh, const char *name)
{
    for (int i = 0; i < nh; i++)
        if (strcmp(h[i], name) == 0)
            return i;
    die("OpenNGC: no column %s", name);
}

static double fnum(const char *s) { return *s ? atof(s) : 0.0; }

static void openngc_load(const char *path, galaxy_t **out, size_t *n, size_t *cap)
{
    size_t len;
    char *txt = (char *)slurp(path, &len);
    char *p = txt;
    char *hf[64], *f[64];
    int nh = 0, cName = 0, cType = 0, cRA = 0, cDec = 0, cMaj = 0, cMin = 0, cPA = 0, cB = 0, cV = 0, cCommon = 0;
    int first = 1;
    while (*p) {
        char *e = strchr(p, '\n');
        char *line = p;
        if (e)
            *e = 0, p = e + 1;
        else
            p += strlen(p);
        size_t ll = strlen(line);
        if (ll && line[ll - 1] == '\r')
            line[ll - 1] = 0;
        if (first) {
            nh = csv_split(line, hf, 64);
            cName = col_of(hf, nh, "Name"), cType = col_of(hf, nh, "Type"), cRA = col_of(hf, nh, "RA");
            cDec = col_of(hf, nh, "Dec"), cMaj = col_of(hf, nh, "MajAx"), cMin = col_of(hf, nh, "MinAx");
            cPA = col_of(hf, nh, "PosAng"), cB = col_of(hf, nh, "B-Mag"), cV = col_of(hf, nh, "V-Mag");
            cCommon = col_of(hf, nh, "Common names");
            first = 0;
            continue;
        }
        if (!*line)
            continue;
        int nf = csv_split(line, f, 64);
        int need = cCommon > cV ? cCommon : cV;
        if (nf <= need)
            continue;
        if (strcmp(f[cType], "G") != 0 || !*f[cRA])
            continue;
        const char *mag = *f[cV] ? f[cV] : f[cB];
        if (!*mag)
            continue;
        galaxy_t g;
        memset(&g, 0, sizeof g);
        snprintf(g.name, sizeof g.name, "%s", f[cName]);
        snprintf(g.common, sizeof g.common, "%s", f[cCommon]);
        double h, m, s;
        if (sscanf(f[cRA], "%lf:%lf:%lf", &h, &m, &s) != 3)
            continue;
        g.ra = (h + m / 60 + s / 3600) * 15;
        int sd = f[cDec][0] == '-' ? -1 : 1;
        double d, dm, ds;
        if (sscanf(f[cDec], "%lf:%lf:%lf", &d, &dm, &ds) != 3)
            continue;
        g.dec = sd * (fabs(d) + fabs(dm) / 60 + fabs(ds) / 3600);
        g.vmag = atof(mag);
        if (*f[cB] && *f[cV])
            g.bv = atof(f[cB]) - atof(f[cV]), g.has_bv = 1;
        g.maj = fnum(f[cMaj]);
        g.min = *f[cMin] ? atof(f[cMin]) : g.maj;
        g.pa = fnum(f[cPA]);
        if (*n == *cap)
            *out = xrealloc(*out, (*cap *= 2) * sizeof **out);
        (*out)[(*n)++] = g;
    }
    free(txt);
}



/* ---- the Milky Way from Gaia DR3: source counts and integrated G-band
 * flux per HEALPix level-6 cell (nside=64, 49152 cells), reddened by
 * G-band extinction from gaiadr3.astrophysical_parameters.
 * ESA/Gaia/DPAC, CC BY-NC 3.0 IGO.  Credit: ESA/Gaia/DPAC. */

typedef struct {
    long nsrc;
    double g_flux, bp_flux, rp_flux;
    double mean_ag; /* mean G-band extinction from GSP-Phot */
    double v_s10;   /* its light as V surface brightness, S10 (gaia_v_light) */
    double ra, dec; /* cell centre, degrees */
} gaia_cell_t;

/* Parse a CSV with header line; column order: hpx6, nsrc, g_flux, bp_flux, rp_flux (or hpx6, mean_ag, n).
 * Returns array of NCELLS (49152) cells indexed by hpx6; unvisited cells are zero. */
static gaia_cell_t *parse_gaia_csv(const char *flux_path, const char *dust_path)
{

    gaia_cell_t *cells = xmalloc(NCELLS * sizeof *cells);
    for (int i = 0; i < NCELLS; i++) cells[i] = (gaia_cell_t){0};

    /* Flux CSV: hpx6, nsrc, g_flux, bp_flux, rp_flux */
    size_t len; uint8_t *raw = slurp(flux_path, &len);
    char *p = (char *)raw, *e = p + len;
    /* skip header */
    while (p < e && *p != '\n') p++;
        if (p < e) p++;
    while (p < e) {
        long hpx = -1; long nsrc = 0; double gf = 0, bpf = 0, rpf = 0;
        int r = sscanf(p, "%lf,%ld,%lf,%lf,%lf", (double*)&hpx, &nsrc, &gf, &bpf, &rpf);
        (void)r;
        hpx = (long)strtod(p, NULL); /* hpx6 may have .0 decimal */
        if (hpx >= 0 && hpx < NCELLS) {
            cells[hpx].nsrc = nsrc; cells[hpx].g_flux = gf;
            cells[hpx].bp_flux = bpf; cells[hpx].rp_flux = rpf;
        }
        while (p < e && *p != '\n') p++;
        if (p < e) p++;
    }
    free(raw);

    /* Dust CSV: hpx6, mean_ag, n */
    raw = slurp(dust_path, &len);
    p = (char *)raw; e = p + len;
    while (p < e && *p != '\n') p++;
        if (p < e) p++; /* skip header */
    while (p < e) {
        long hpx = (long)strtod(p, NULL);
        if (hpx >= 0 && hpx < NCELLS) {
            char *q = p; while (q < e && *q != ',') q++; if (q < e) q++;
            cells[hpx].mean_ag = strtod(q, NULL);
        }
        while (p < e && *p != '\n') p++;
        if (p < e) p++;
    }
    free(raw);
    return cells;
}

/* Build 360 x 180 equirectangular grid (1 deg/cell) from Gaia cells.
 * Each grid point averages the cells within SPLAT_DEG of it: 1.5 deg for level 6 (cells ~0.9 deg apart),
 * 0.75 deg for level 7 (~0.46 deg apart, about 8 cells per point, so no point is empty and the Small
 * Magellanic Cloud, 5 x 3 deg, is not smeared over a 3 deg disk). Returns a flattened (rows*cols)*3 double array: [luma, co, cg]
 * in the same YCbCr-like space as the old photograph, in 16ths-of-level units.
 * flux_gain: linear display light per (g_flux / nsrc) unit. */
#define SPLAT_DEG (HPX_LEVEL == 7 ? 0.75 : 1.5)
static double *gaia_to_grid(const gaia_cell_t *cells)
{
#define GCOLS 360
#define GROWS 180
    double *grid  = xmalloc((size_t)GROWS * (size_t)(GCOLS + 1) * 3 * sizeof *grid);
    double *count = xmalloc((size_t)GROWS * (size_t)(GCOLS + 1) * sizeof *count);
    for (size_t i = 0; i < (size_t)GROWS * (size_t)(GCOLS + 1) * 3; i++) grid[i]  = 0;
    for (size_t i = 0; i < (size_t)GROWS * (size_t)(GCOLS + 1);     i++) count[i] = 0;

    for (int k = 0; k < NCELLS; k++) {
        const gaia_cell_t *c = &cells[k];
        if (c->nsrc == 0) continue;
        double ra = c->ra, dec = c->dec;
        /* E(B-V) ~ A_G / 2.285 (Cardelli 1989); applied as reddening of the colour */
        double ebv = c->mean_ag / 2.285;
        /* the cell's light: V surface brightness in S10 (gaia_v_light), physical; emit() scales by a->g_phys */
        double luma_lin = c->v_s10;
        /* Color from BP/RP ratio (log scale). Higher = redder (dust or cool stars).
         * Map to the chrominance offsets used by the photograph approach (linear, 0..1 range). */
        double bprp = (c->bp_flux > 1.0) ? log(c->rp_flux / c->bp_flux) : 0.5;
        bprp += ebv * 0.3;  /* add dust reddening */
        /* Milky Way colour: warm orange-yellow in the galactic plane, cooler at the poles.
         * co: orange-red offset; cg: green offset (in the same linear units as luma_lin). */
        /* Colour at fixed luminance: the three channel weights have Y = 0.2126 R + 0.7152 G + 0.0722 B
         * equal to 1, so the cell's luminance (its light) is exactly luma_lin and the colour only
         * moves the light between the channels. An orange tint from BP-RP (t), a small green one. */
        double t = 0.2 * (bprp - 0.3);
        double wR = 1.0 + t, wB = 1.0 - t, wG = 1.0 + 0.08 * (1.0 - fabs(bprp - 0.8));
        double Yw = 0.2126 * wR + 0.7152 * wG + 0.0722 * wB;
        double r_lin = luma_lin * wR / Yw, g_lin = luma_lin * wG / Yw, b_lin = luma_lin * wB / Yw;

        /* Splat onto the grid points within SPLAT_DEG (row j is centred at Dec 89.5 - j) */
        const double R = SPLAT_DEG;
        int j0 = (int)floor(89.5 - dec - R), j1 = (int)ceil(89.5 - dec + R);
        j0 = j0 < 0 ? 0 : j0 >= GROWS ? GROWS - 1 : j0;
        j1 = j1 < 0 ? 0 : j1 >= GROWS ? GROWS - 1 : j1;
        for (int j = j0; j <= j1; j++) {
            double dec_j = 90.0 - j - 0.5;
            double ddec = dec_j - dec;
            if (ddec * ddec > R * R) continue;
            double cos_dec = cos(rad(dec_j));
            double dra_max = cos_dec > 0.01 ? R / cos_dec : 360.0;
            int i0 = (int)floor(ra - dra_max - 0.5), i1 = (int)ceil(ra + dra_max - 0.5);
            if (dra_max >= 180) i0 = 0, i1 = GCOLS - 1; /* near a pole: every column once */
            for (int ii = i0; ii <= i1; ii++) {
                int i = ((ii % GCOLS) + GCOLS) % GCOLS;
                double ra_i = i + 0.5;
                double dra = ra_i - ra;
                if (dra > 180) dra -= 360;
                if (dra < -180) dra += 360;
                double dra_deg = dra * cos_dec;
                if (dra_deg * dra_deg + ddec * ddec > R * R) continue;
                size_t off = (size_t)j * (size_t)(GCOLS + 1) + (size_t)i;
                grid[off * 3]     += r_lin;
                grid[off * 3 + 1] += g_lin;
                grid[off * 3 + 2] += b_lin;
                count[off]++;
            }
        }
    }

    /* Average and add glow floor */
    /* The glow floor (dark sky brightness) is handled by the emit() function
     * which adds glow[k] from the blackbody colour. Our grid stores SIGNAL only.
     * Average the accumulated values. */
    for (int j = 0; j < GROWS; j++) {
        for (int i = 0; i <= GCOLS; i++) {
            size_t off = (size_t)j * (size_t)(GCOLS + 1) + (size_t)i;
            if (count[off] > 0) {
                grid[off * 3]     /= count[off];
                grid[off * 3 + 1] /= count[off];
                grid[off * 3 + 2] /= count[off];
            }
            /* no separate glow: emit() adds the dark-sky blackbody glow */

        }
    }

    /* Copy col 0 to col GCOLS for wraparound bilinear */
    for (int j = 0; j < GROWS; j++) {
        size_t src = (size_t)j * (size_t)(GCOLS + 1);
        size_t dst = src + GCOLS;
        grid[dst * 3]     = grid[src * 3];
        grid[dst * 3 + 1] = grid[src * 3 + 1];
        grid[dst * 3 + 2] = grid[src * 3 + 2];
    }

    free(count);
    return grid;
}

/* Each cell's light as V surface brightness in S10. The cell's G flux F (e-/s) over its area A (arcsec^2) is
 * G = ZP_G - 2.5 log10(F / A) per arcsec^2; 1 S10 is V 27.78 mag/arcsec^2. G is moved to Johnson V with the
 * cell's own integrated colour c = BP - RP (from its summed BP and RP fluxes and the DR3 zero points):
 * G - V = -0.02704 + 0.01424 c - 0.2156 c^2 + 0.01426 c^3, valid -0.5 < c < 5, scatter 0.03 mag (Gaia DR3
 * documentation, chapter 5, "relationships with other photometric systems", Table 5.9). At c = 2 (the
 * reddened bulge) G - V is -0.75: V is that much fainter than G. Returns the number of cells with no BP/RP flux (those keep G as V). */
static long gaia_v_light(gaia_cell_t *cells, double cell_arcsec2)
{
    long nocol = 0;
    double g_s10 = pow(10, 0.4 * (27.78 - GAIA_ZP_G)) / cell_arcsec2;
    for (int k = 0; k < NCELLS; k++) {
        gaia_cell_t *c = &cells[k];
        double gv = 0;
        if (c->bp_flux > 0 && c->rp_flux > 0) {
            double x = clampd(-2.5 * log10(c->bp_flux / c->rp_flux) + GAIA_ZP_BP - GAIA_ZP_RP, -0.5, 5.0);
            gv = -0.02704 + x * (0.01424 + x * (-0.2156 + x * 0.01426));
        } else if (c->nsrc > 0) {
            nocol++;
        }
        c->v_s10 = c->g_flux * g_s10 * pow(10, 0.4 * gv);
    }
    return nocol;
}

static int cmpd(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

/* The Magellanic Clouds: Gaia resolves their brightest stars (the LMC is 50 kpc away, distance modulus 18.5,
 * so Gaia's G 20.7 limit reaches absolute G +2) but not the faint, crowded rest of their light. For each
 * catalogue galaxy at least CLOUD_MIN_ARCMIN long (the grid's 3 degree splat sees its shape), the Gaia light
 * inside its catalogue ellipse (OpenNGC major and minor axes and position angle, the D25 isophote) above the
 * local sky (the median cell between 1.2 and 1.6 ellipse radii: Milky Way foreground and background) is the
 * resolved part. The catalogue's total V says how much light the galaxy has; the missing part is added in the
 * pattern of the resolved stars themselves (each cell's excess times total / resolved), so the total inside the
 * ellipse is the catalogue's and the shape is Gaia's. Only where Gaia resolves at least CLOUD_MIN_SHARE of the
 * light: a farther galaxy (M31) shows Gaia only a few supergiants, too few to carry its shape. */
#define CLOUD_MIN_ARCMIN 120.0
#define CLOUD_MIN_SHARE 0.25
static void gaia_clouds(gaia_cell_t *cells, const galaxy_t *gx, size_t ngx, double cell_deg2)
{
    double *rho2 = xmalloc(NCELLS * sizeof *rho2), *ann = xmalloc(NCELLS * sizeof *ann);
    for (size_t gi = 0; gi < ngx; gi++) {
        const galaxy_t *g = &gx[gi];
        if (g->maj < CLOUD_MIN_ARCMIN || g->min <= 0 || g->vmag > 20)
            continue;
        vec3 gc = vec(g->ra, g->dec);
        double a_ = rad(g->ra), d_ = rad(g->dec), cp = cos(rad(g->pa)), sp = sin(rad(g->pa));
        vec3 east = {-sin(a_), cos(a_), 0.0};
        vec3 north = {-sin(d_) * cos(a_), -sin(d_) * sin(a_), cos(d_)};
        double ha = g->maj / 120.0, hb = g->min / 120.0; /* half axes, degrees */
        size_t nann = 0, nin = 0, nctr = 0;
        double ctr = 0;
        for (int k = 0; k < NCELLS; k++) {
            vec3 c = vec(cells[k].ra, cells[k].dec);
            double cg = dot(c, gc);
            rho2[k] = 1e9;
            if (cg < 0.5)
                continue;
            double x = deg(dot(c, east) / cg), y = deg(dot(c, north) / cg); /* tangent plane, degrees */
            double u = x * sp + y * cp, w = x * cp - y * sp;               /* along the long and short axes */
            rho2[k] = (u / ha) * (u / ha) + (w / hb) * (w / hb);
            if (rho2[k] >= 1.44 && rho2[k] <= 2.56)
                ann[nann++] = cells[k].v_s10;
        }
        if (nann < 8)
            continue;
        qsort(ann, nann, sizeof *ann, cmpd);
        double bg = ann[nann / 2], ex = 0;
        for (int k = 0; k < NCELLS; k++)
            if (rho2[k] <= 1.0) {
                nin++;
                ex += fmax(0, cells[k].v_s10 - bg);
                if (rho2[k] <= 0.04)
                    ctr += cells[k].v_s10, nctr++;
            }
        double f_gaia = ex * cell_deg2, f_cat = pow(10, 0.4 * (10 - g->vmag)); /* light in V 10 stars */
        double share = f_cat > 0 ? f_gaia / f_cat : 0;
        double v_gaia = f_gaia > 0 ? 10 - 2.5 * log10(f_gaia) : 99;
        char common[160];
        snprintf(common, sizeof common, "%s", g->common);
        char *cm = strchr(common, ',');
        if (cm)
            *cm = 0;
        if (share < CLOUD_MIN_SHARE) {
            report("cloud light %s %s: Gaia resolves V %.2f of the catalogue's V %.2f (%.1f%%), under %.0f%%: not added",
                   g->name, common, v_gaia, g->vmag, 100 * share, 100 * CLOUD_MIN_SHARE);
            continue;
        }
        double k_ = share < 1 ? 1 / share : 1, ctr_after = 0;
        for (int k = 0; k < NCELLS; k++)
            if (rho2[k] <= 1.0) {
                cells[k].v_s10 += (k_ - 1) * fmax(0, cells[k].v_s10 - bg);
                if (rho2[k] <= 0.04)
                    ctr_after += cells[k].v_s10;
            }
        ctr = nctr ? ctr / (double)nctr : 0, ctr_after = nctr ? ctr_after / (double)nctr : 0;
        report("cloud light %s %s: %zu Gaia cells in the %.0fx%.0f arcmin ellipse, local sky %.1f S10 (median of %zu "
               "cells at 1.2-1.6 radii); Gaia resolves V %.2f of the catalogue's V %.2f (%.0f%%); the rest added in the "
               "Gaia stars' pattern (excess x %.2f); centre (inner fifth) V %.2f -> %.2f mag/arcsec2, %.0f -> %.0f S10",
               g->name, common, nin, g->maj, g->min, bg, nann, v_gaia, g->vmag, 100 * share, k_,
               27.78 - 2.5 * log10(ctr), 27.78 - 2.5 * log10(ctr_after), ctr, ctr_after);
    }
    free(rho2);
    free(ann);
}

/* ---- the run ---- */

typedef struct {
    const char *cache, *out, *report, *inputs;
    double ra, dec, fov, roll, mag, gain, skyglow, exposure;
    int have_ra, have_dec, grid;
    double glow_lin, g_phys;
} opts_t;

typedef struct {
    star_t *s;
    vec3 v;
} sel_t;

static int by_v(const void *a, const void *b)
{
    const sel_t *x = a, *y = b;
    if (x->s->v != y->s->v)
        return x->s->v < y->s->v ? -1 : 1;
    return x->s < y->s ? -1 : x->s > y->s;
}

typedef struct {
    galaxy_t *g;
    vec3 v;
} gsel_t;

static int by_vmag(const void *a, const void *b)
{
    const gsel_t *x = a, *y = b;
    if (x->g->vmag != y->g->vmag)
        return x->g->vmag < y->g->vmag ? -1 : 1;
    return x->g < y->g ? -1 : x->g > y->g;
}

static const double W = 3840, H = 2160;

static int on_169(const view_t *view, vec3 v)
{
    double x, y;
    return to_screen(view, v, W, H, &x, &y) && 0 <= x && x < W && 0 <= y && y < H;
}

static char *path_join(const char *dir, const char *name)
{
    size_t n = strlen(dir) + strlen(name) + 2;
    char *s = xmalloc(n);
    snprintf(s, n, "%s/%s", dir, name);
    return s;
}

static void emit(const opts_t *a, const view_t *view, const sel_t *sel, size_t nsel, double pal[128][3], const double *grid,
                 int gw_cols, int gw_rows, double gy0, double step, double floor_, const gsel_t *gals, size_t ngal, double ra,
                 double dec);

int main(int argc, char **argv)
{
    opts_t a = {0};
    a.fov = 80.0, a.mag = 6.5, a.grid = 240, a.gain = 1.0, a.skyglow = 22.0, a.exposure = 0.06;
    for (int i = 1; i < argc; i++) {
        const char *k = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!v)
            die("%s needs a value", k);
        i++;
        if (!strcmp(k, "--cache")) a.cache = v;
        else if (!strcmp(k, "--out")) a.out = v;
        else if (!strcmp(k, "--report")) a.report = v;
        else if (!strcmp(k, "--inputs")) a.inputs = v;
        else if (!strcmp(k, "--ra")) a.ra = atof(v), a.have_ra = 1;
        else if (!strcmp(k, "--dec")) a.dec = atof(v), a.have_dec = 1;
        else if (!strcmp(k, "--fov")) a.fov = atof(v);
        else if (!strcmp(k, "--roll")) a.roll = atof(v);
        else if (!strcmp(k, "--mag")) a.mag = atof(v);
        else if (!strcmp(k, "--grid")) a.grid = atoi(v);
        else if (!strcmp(k, "--gain")) a.gain = atof(v);
        else if (!strcmp(k, "--skyglow")) a.skyglow = atof(v);
        else if (!strcmp(k, "--exposure")) a.exposure = atof(v);
        else die("unknown option %s", k);
    }
    if (!a.cache || !a.out)
        die("--cache and --out are needed");
    time_t t0 = time(NULL);

    if (a.inputs) { /* "input name: url sha256 ..." lines made by scripts/ck_sky_gen.sh */
        size_t len;
        char *t = (char *)slurp(a.inputs, &len);
        char *p = t;
        while (*p) {
            char *e = strchr(p, '\n');
            if (e)
                *e = 0;
            if (*p)
                report("%s", p);
            p = e ? e + 1 : p + strlen(p);
        }
        free(t);
    }

    gwmap gw = gw_load(path_join(a.cache, "GW150914_skymap.fits"));
    double ra = a.have_ra ? a.ra : rnd(gw.max_ra * 1000) / 1000.0;
    double dec = a.have_dec ? a.dec : rnd(gw.max_dec * 1000) / 1000.0;
    report("GW150914 sky map: HEALPix nside %d; most probable point RA %.3f Dec %.3f deg; 90%% region %.1f sq deg", gw.nside,
           gw.max_ra, gw.max_dec, gw.area90);
    view_t view = view_make(ra, dec, a.fov, a.roll);
    report("view: gnomonic, centre RA %.3f Dec %.3f deg, %.1f deg across the width, roll %.1f deg, "
           "centre at %.0f%% across and %.0f%% down",
           ra, dec, a.fov, a.roll, CX_FRAC * 100, CY_FRAC * 100);
    vec3 tl = from_frac(&view, -0.5, -CY_FRAC * H / W), br = from_frac(&view, 0.5, (1 - CY_FRAC) * H / W);
    report("3840x2160: the top-left corner is %.1f deg from the centre, the bottom-right %.1f deg", sep_deg(tl, view.rows[2]),
           sep_deg(br, view.rows[2]));
    double lc, bc;
    galactic(view.rows[2], &lc, &bc);
    report("the centre in galactic coordinates: l %.2f b %.2f deg", lc, bc);
    long inside = 0;
    for (long i = 0; i < gw.n90; i++) {
        double x, y;
        if (to_screen(&view, vec(gw.region_ra[i], gw.region_dec[i]), W, H, &x, &y) && 0 <= x && x < W && 0 <= y && y < H)
            inside++;
    }
    report("of the GW150914 90%% region, %.0f%% of its pixels are on the 3840x2160 screen", 100.0 * (double)inside / (double)gw.n90);

    /* ---- stars ---- */
    size_t nstars;
    int nobv;
    star_t *stars = bsc5_load(path_join(a.cache, "bsc5.dat"), &nstars, &nobv);
    report("catalogue: %zu stars with positions; %d have no B-V (drawn at B-V 0.6, close to the Sun's)", nstars, nobv);
    const double margin = 64, hmax = W * H_OVER_W_MAX;
    sel_t *sel = xmalloc(nstars * sizeof *sel);
    size_t nsel = 0, allsky = 0;
    static const double lims[7] = {4.0, 5.0, 5.5, 6.0, 6.5, 7.0, 99};
    long counts[7] = {0};
    for (size_t i = 0; i < nstars; i++) {
        vec3 v = vec(stars[i].ra, stars[i].dec);
        int s169 = on_169(&view, v);
        for (int k = 0; k < 7; k++)
            if (stars[i].v <= lims[k] && s169)
                counts[k]++;
        if (stars[i].v <= a.mag) {
            allsky++;
            double x, y;
            if (to_screen(&view, v, W, hmax, &x, &y) && -margin <= x && x < W + margin && -margin <= y && y < hmax + margin)
                sel[nsel++] = (sel_t){&stars[i], v};
        }
    }
    {
        char buf[512];
        int n = 0;
        buf[0] = 0;
        for (int k = 0; k < 7; k++) {
            if (!counts[k])
                continue;
            char lim[16];
            if (k == 6)
                snprintf(lim, sizeof lim, "all");
            else
                snprintf(lim, sizeof lim, "%.1f", lims[k]);
            n += snprintf(buf + n, sizeof buf - (size_t)n, "%sV<=%s: %ld", n ? ", " : "", lim, counts[k]);
        }
        report("stars on the 3840x2160 screen by limit: %s", buf);
    }
    report("stars to V %.1f in the whole sky: %zu (8 bytes each: %zu bytes); in this view with its margin: %zu", a.mag, allsky,
           allsky * 8, nsel);
    qsort(sel, nsel, sizeof *sel, by_v);

    /* colour palette: B-V from -0.40 to 2.14 in steps of 0.02 */
    static double pal[128][3];
    for (int i = 0; i < 128; i++)
        blackbody_rgb(ballesteros(-0.40 + 0.02 * i), pal[i]);
    static const double bvs[4] = {-0.3, 0.0, 0.65, 1.5};
    for (int i = 0; i < 4; i++) {
        double rgb[3];
        double T = ballesteros(bvs[i]);
        blackbody_rgb(T, rgb);
        report("colour: B-V %.2f -> %.0f K -> sRGB (%.0f, %.0f, %.0f)", bvs[i], T, srgb_encode(rgb[0]), srgb_encode(rgb[1]),
               srgb_encode(rgb[2]));
    }

    /* ---- the Milky Way from Gaia DR3 ---- */
    /* CSVs downloaded by scripts/ck_sky_gen.sh and passed via --cache */
    double r0 = 1.2; /* design px: the core of a star */
    double star0 = a.exposure * (PI * r0 * r0 / 4) / 0.8 * pow(10, 0.4 * 6.5);
    double px_deg2 = (a.fov / 1920.0) * (a.fov / 1920.0);
    double s10 = 1e-4 * star0 * px_deg2; /* linear light per design px of 1 S10 */
    /* The dark sky's own glow is physical: no gain on it. The gain (--gain) is on the Milky Way only. */
    a.glow_lin = pow(10, 0.4 * (27.78 - a.skyglow)) * s10;
    report("display: a magnitude 6.5 star peaks at %.3f linear (sRGB level %.0f); "
           "a magnitude 0 star holds %.1f linear x design px; 1 S10 is %.3g linear per design px",
           a.exposure, srgb_encode(a.exposure), star0, s10);
    gaia_cell_t *cells = parse_gaia_csv(path_join(a.cache, HPX_LEVEL == 7 ? "gaia_hpx7.csv" : "gaia_hpx6.csv"),
                                        path_join(a.cache, HPX_LEVEL == 7 ? "gaia_dust7.csv" : "gaia_dust.csv"));
    /* Compute direction (ra, dec) of each HEALPix nested cell (nside=64) */
    for (int k = 0; k < NCELLS; k++)
        healpix_nest_ang(NSIDE, k, &cells[k].ra, &cells[k].dec);
    /* The physical scale, with no anchor: each cell's light is its own Gaia flux, in V (gaia_v_light). */
    double cell_arcsec2 = (4 * PI / (12.0 * NSIDE * NSIDE)) * (180.0 * 3600.0 / PI) * (180.0 * 3600.0 / PI);
    long nocol = gaia_v_light(cells, cell_arcsec2);
    long nnonempty = 0;
    for (int k = 0; k < NCELLS; k++) if (cells[k].nsrc > 0) nnonempty++;
    report("Gaia DR3 Milky Way: %d HEALPix level-%d cells (nside %d), %ld non-empty, %ld with no BP/RP colour (G kept as V); "
           "sky glow %.5f linear/design-px (V %.1f, physical, no gain)",
           NCELLS, HPX_LEVEL, NSIDE, nnonempty, nocol, a.glow_lin, a.skyglow);
    /* the catalogue galaxies (OpenNGC): the Clouds' light here, the galaxies in the view below */
    size_t gcap = 4096, ngx = 0;
    galaxy_t *gx = xmalloc(gcap * sizeof *gx);
    openngc_load(path_join(a.cache, "NGC.csv"), &gx, &ngx, &gcap);
    openngc_load(path_join(a.cache, "addendum.csv"), &gx, &ngx, &gcap);
    gaia_clouds(cells, gx, ngx, cell_arcsec2 / (3600.0 * 3600.0));
    double *grid = gaia_to_grid(cells);
    free(cells);
    int gw_cols = GCOLS, gw_rows = GROWS;
    double gy0 = 0.0, step = 1.0 / GCOLS;
    double floor_ = 0.0; /* the glow is handled in emit() */
    double top = 0;
    for (int j = 0; j < gw_rows; j++)
        for (int i = 0; i <= gw_cols; i++) {
            double v = grid[((size_t)j * (size_t)(gw_cols + 1) + (size_t)i) * 3];
            if (v > top) top = v;
        }
    /* The grid is in S10 already (gaia_to_grid), so the light is physical: g_phys is one S10 of linear light
     * per design pixel. The gain (--gain, 1.0) is only for a deliberate test; the physical value is 1.0. */
    if (top <= 0)
        die("the Gaia Milky Way grid is empty");
    a.g_phys = s10;
    report("Milky Way: brightest band %.1f S10 = V %.2f mag/arcsec2 (Gaia flux in V, physical scale, no anchor; "
           "the published Bochum value is V 20.7, Leinert et al. 1998); %.2f times the dark sky; gain %.2f",
           top, 27.78 - 2.5 * log10(top), top / (pow(10, 0.4 * (27.78 - a.skyglow))), a.gain);

    /* galaxies in the view, bright enough for the eye */
    gsel_t *gals = xmalloc((ngx + 1) * sizeof *gals);
    size_t ngal = 0;
    for (size_t i = 0; i < ngx; i++) {
        vec3 v = vec(gx[i].ra, gx[i].dec);
        if (gx[i].vmag > a.mag || !on_169(&view, v))
            continue;
        gals[ngal++] = (gsel_t){&gx[i], v};
    }
    qsort(gals, ngal, sizeof *gals, by_vmag);
    {
        char buf[2048];
        int n = 0;
        buf[0] = 0;
        for (size_t i = 0; i < ngal; i++) {
            char common[160];
            snprintf(common, sizeof common, "%s", gals[i].g->common);
            char *c = strchr(common, ',');
            if (c)
                *c = 0;
            n += snprintf(buf + n, sizeof buf - (size_t)n, "%s%s %s V %.2f %.0fx%.0f arcmin PA %.0f", n ? ", " : "",
                          gals[i].g->name, common, gals[i].g->vmag, gals[i].g->maj, gals[i].g->min, gals[i].g->pa);
        }
        report("galaxies on screen brighter than V %.1f (OpenNGC): %s", a.mag, ngal ? buf : "none");
    }

    emit(&a, &view, sel, nsel, pal, grid, gw_cols, gw_rows, gy0, step, floor_, gals, ngal, ra, dec);
    report("made in %ld s", (long)(time(NULL) - t0));
    fputs(rep, stdout);
    if (a.report) {
        FILE *f = fopen(a.report, "w");
        if (!f)
            die("cannot write %s", a.report);
        fputs(rep, f);
        fclose(f);
    }
    return 0;
}

static void emit(const opts_t *a, const view_t *view, const sel_t *sel, size_t nsel, double pal[128][3], const double *grid,
                 int gw_cols, int gw_rows, double gy0, double step, double floor_, const gsel_t *gals, size_t ngal, double ra,
                 double dec)
{
    const double Q30 = (double)(1 << 30);
    FILE *o = fopen(a->out, "w");
    if (!o)
        die("cannot write %s", a->out);
    fprintf(o, "/* sky_data.c -- GENERATED by native/kernel/tools/ck_sky_gen.c; do not edit.\n");
    fprintf(o, " * Regenerate: scripts/ck_sky_gen.sh (downloads the inputs, checks their sha256).\n *\n");
    fprintf(o, " * Stars: Yale Bright Star Catalogue, 5th revised ed. (Hoffleit & Warren 1991),\n");
    fprintf(o, " *   CDS catalogue V/50, positions moved by proper motion to %.1f.\n", EPOCH);
    fprintf(o, " * Milky Way: Gaia DR3 source counts and integrated G-band flux per HEALPix level-%d cell\n", HPX_LEVEL);
    fprintf(o, " *   (nside=%d); G-band extinction from gaiadr3.astrophysical_parameters.\n", NSIDE);
    fprintf(o, " *   ESA/Gaia/DPAC, CC BY-NC 3.0 IGO. Credit: ESA/Gaia/DPAC.\n");
    fprintf(o, " * Galaxies: OpenNGC (Mattia Verga), CC BY-SA 4.0.\n");
    fprintf(o, " * View centre: the most probable point of the GW150914 sky map, GWTC-1,\n");
    fprintf(o, " *   LIGO/Virgo (LIGO-P1800381-v7).\n");
    fprintf(o, " * Settings: centre RA %.3f Dec %.3f deg, %.1f deg across, roll %.1f deg, faintest V %.1f,\n", ra, dec, a->fov,
            a->roll, a->mag);
    fprintf(o, " *   grid %d, exposure %.4f, sky glow %.1f mag/arcsec2, Milky Way and Clouds gain %.2f (1.00 = physical). */\n", gw_cols, a->exposure,
            a->skyglow, a->gain);
    fprintf(o, "#include \"sky_data.h\"\n\n");
    fprintf(o, "const ck_sky_view ck_sky_the_view = {\n");
    fprintf(o, "    .ra_mdeg = %ld, .dec_mdeg = %ld, .fov_mdeg = %ld, .roll_mdeg = %ld,\n", rnd(ra * 1000), rnd(dec * 1000),
            rnd(a->fov * 1000), rnd(a->roll * 1000));
    fprintf(o, "    .tan_half_q30 = %ld,\n", rnd(view->t * Q30));
    fprintf(o, "    .rot_q30 = {\n");
    for (int r = 0; r < 3; r++)
        fprintf(o, "        {%ld, %ld, %ld},\n", rnd(view->rows[r].x * Q30), rnd(view->rows[r].y * Q30), rnd(view->rows[r].z * Q30));
    fprintf(o, "    },\n};\n\n");

    /* stars */
    fprintf(o, "/* x, y, z: the direction, Q15 (J2000 axes); mag: (V + 2) * 20; bv: palette index */\n");
    fprintf(o, "const ck_sky_star ck_sky_stars[%zu] = {\n", nsel);
    for (size_t i = 0; i < nsel; i++) {
        const star_t *s = sel[i].s;
        double bv = s->has_bv ? s->bv : 0.6;
        long bvi = rnd((bv + 0.40) / 0.02);
        bvi = bvi < 0 ? 0 : bvi > 127 ? 127 : bvi;
        long mc = rnd((s->v + 2.0) * 20);
        mc = mc < 0 ? 0 : mc > 255 ? 255 : mc;
        char bvs[16];
        if (s->has_bv)
            snprintf(bvs, sizeof bvs, "%.2f", s->bv);
        else
            snprintf(bvs, sizeof bvs, "none");
        fprintf(o, "    {%ld, %ld, %ld, %ld, %ld}, /* HR %d %s V %.2f B-V %s */\n", rnd(sel[i].v.x * 32767), rnd(sel[i].v.y * 32767),
                rnd(sel[i].v.z * 32767), mc, bvi, s->hr, s->name, s->v, bvs);
    }
    fprintf(o, "};\nconst unsigned ck_sky_nstars = %zu;\n\n", nsel);

    /* brightness of a star at each magnitude code: linear light at the centre of its core at the reference scale
     * (k = 256, 1920 px wide), Q16 */
    fprintf(o, "/* light at the middle of a star of magnitude code m, linear, Q16: exposure x 10^(-0.4 (V - 6.5)) */\n");
    fprintf(o, "const uint32_t ck_sky_flux[256] = {\n");
    for (int i = 0; i < 256; i += 8) {
        fprintf(o, "    ");
        for (int k = 0; k < 8; k++) {
            double V = (i + k) / 20.0 - 2.0;
            long v = rnd(a->exposure * pow(10, -0.4 * (V - 6.5)) * 65536);
            if (v > 0x7fffffff)
                v = 0x7fffffff;
            fprintf(o, "%ld%s", v, k < 7 ? ", " : ",\n");
        }
    }
    fprintf(o, "};\n\n");
    fprintf(o, "/* the colour of a black body at the temperature of B-V = -0.40 + 0.02 i (Ballesteros 2012),\n");
    fprintf(o, " * linear sRGB, the brightest channel 4096 */\n");
    fprintf(o, "const uint16_t ck_sky_palette[128][3] = {\n");
    for (int i = 0; i < 128; i++)
        fprintf(o, "    {%ld, %ld, %ld}, /* B-V %.2f, %.0f K */\n", rnd(pal[i][0] * 4096), rnd(pal[i][1] * 4096), rnd(pal[i][2] * 4096),
                (2 * i - 40) / 100.0, ballesteros(-0.40 + 0.02 * i));
    fprintf(o, "};\n\n");

    /* sRGB tables */
    fprintf(o, "/* sRGB level -> linear light, Q15 */\n");
    fprintf(o, "const uint16_t ck_sky_lin[256] = {\n");
    for (int i = 0; i < 256; i += 16) {
        fprintf(o, "    ");
        for (int k = 0; k < 16; k++)
            fprintf(o, "%ld%s", rnd(srgb_decode(i + k) * 32768), k < 15 ? ", " : ",\n");
    }
    fprintf(o, "};\n");
    fprintf(o, "/* linear light (Q15 >> 3, 0..4095) -> sRGB level x 16 (sixteenths, for dithering) */\n");
    fprintf(o, "const uint16_t ck_sky_srgb16[4096] = {\n");
    for (int i = 0; i < 4096; i += 16) {
        fprintf(o, "    ");
        for (int k = 0; k < 16; k++) {
            long v = rnd(srgb_encode((i + k) / 4095.0) * 16);
            fprintf(o, "%ld%s", v > 4080 ? 4080 : v, k < 15 ? ", " : ",\n");
        }
    }
    fprintf(o, "};\n\n");

    /* the Milky Way grid: luma full, chroma at half, all in sixteenths-of-level space */
    double br[3], bgl[3];
    (void)bgl;
    blackbody_rgb(ballesteros(0.8), br); /* the dark sky's colour, B-V about 0.8 (Benn and Ellison 1998) */
    double gl = 0.2126 * br[0] + 0.7152 * br[1] + 0.0722 * br[2];
    double glow[3] = {a->glow_lin * br[0] / gl, a->glow_lin * br[1] / gl, a->glow_lin * br[2] / gl};
    int cols = gw_cols + 1;
    size_t ng = (size_t)cols * (size_t)gw_rows;
    double *lv = xmalloc(ng * 3 * sizeof(double));
    for (size_t i = 0; i < ng; i++)
        for (int k = 0; k < 3; k++) {
            /* surface brightness is the same wherever it lands on the screen (radiance is conserved) */
            double c = grid[i * 3 + (size_t)k] - floor_;
            double cc = glow[k] + (c > 0 ? c : 0) * a->g_phys * a->gain;
            lv[i * 3 + (size_t)k] = srgb_encode(cc); /* sRGB levels 0..255 (float) */
        }
    uint8_t *Y = xmalloc(ng);
    for (size_t i = 0; i < ng; i++) {
        long y = rnd((lv[i * 3] + 2 * lv[i * 3 + 1] + lv[i * 3 + 2]) / 4);
        Y[i] = (uint8_t)(y < 0 ? 0 : y > 255 ? 255 : y);
    }
    int hc = (cols + 1) / 2, hr = (gw_rows + 1) / 2;
    size_t nco = (size_t)hc * (size_t)hr;
    int8_t *CO = xmalloc(nco), *CG = xmalloc(nco);
    for (int j = 0; j < hr; j++)
        for (int i = 0; i < hc; i++) {
            double ao = 0, ag = 0;
            int n = 0;
            for (int dj = 0; dj < 2; dj++)
                for (int di = 0; di < 2; di++) {
                    int ii = 2 * i + di < cols - 1 ? 2 * i + di : cols - 1;
                    int jj = 2 * j + dj < gw_rows - 1 ? 2 * j + dj : gw_rows - 1;
                    const double *p = lv + ((size_t)jj * (size_t)cols + (size_t)ii) * 3;
                    ao += (p[0] - p[2]) / 2;
                    ag += (-p[0] + 2 * p[1] - p[2]) / 4;
                    n++;
                }
            long co = rnd(ao / n * 2), cg = rnd(ag / n * 2); /* half-levels */
            CO[(size_t)j * (size_t)hc + (size_t)i] = (int8_t)(co < -127 ? -127 : co > 127 ? 127 : co);
            CG[(size_t)j * (size_t)hc + (size_t)i] = (int8_t)(cg < -127 ? -127 : cg > 127 ? 127 : cg);
        }
    fprintf(o, "/* the Milky Way in the view: %d x %d samples, %.15g widths apart, the first row %.6f widths\n", cols, gw_rows, step,
            gy0);
    fprintf(o, " * above the view centre and the first column half a width left of it; luma (R + 2G + B) / 4 in sRGB\n");
    fprintf(o, " * levels; colour (Co, Cg) at half the spacing, in half levels */\n");
    fprintf(o, "const ck_sky_map ck_sky_milkyway = {\n");
    fprintf(o, "    .cols = %d, .rows = %d, .per_width = %d, .row0_q16 = 0,\n};\n", cols, gw_rows, gw_cols);
    fprintf(o, "const uint8_t ck_sky_luma[%zu] = {\n", ng);
    for (size_t i = 0; i < ng; i += 24) {
        fprintf(o, "    ");
        for (size_t k = i; k < i + 24 && k < ng; k++)
            fprintf(o, "%u%s", Y[k], k + 1 < i + 24 && k + 1 < ng ? ", " : ",\n");
    }
    fprintf(o, "};\nconst int8_t ck_sky_chroma[%zu][2] = {\n", nco);
    for (size_t i = 0; i < nco; i += 12) {
        fprintf(o, "    ");
        for (size_t k = i; k < i + 12 && k < nco; k++)
            fprintf(o, "{%d, %d}%s", CO[k], CG[k], k + 1 < i + 12 && k + 1 < nco ? ", " : ",\n");
    }
    fprintf(o, "};\n\n");

    /* galaxies: centre direction, axes and angle on screen worked out here */
    fprintf(o, "/* galaxies in the view bright enough to see: centre (Q15 direction), half axes in widths Q16,\n");
    fprintf(o, " * long axis angle on screen (turns / 65536, from screen right towards down), tint (sRGB) */\n");
    fprintf(o, "const ck_sky_galaxy ck_sky_galaxies[%zu] = {\n", ngal ? ngal : 1);
    for (size_t gi = 0; gi < ngal; gi++) {
        const galaxy_t *g = gals[gi].g;
        vec3 v = gals[gi].v;
        double p0x, p0y, p1x, p1y, p2x, p2y;
        to_screen(view, v, W, W, &p0x, &p0y);
        double pa = rad(g->pa), a_ = rad(g->ra), d_ = rad(g->dec);
        vec3 east = {-sin(a_), cos(a_), 0.0};
        vec3 north = {-sin(d_) * cos(a_), -sin(d_) * sin(a_), cos(d_)};
        double half = rad(g->maj / 120.0);
        double cp = cos(pa), sp = sin(pa);
        vec3 dirv = {cos(half) * v.x + sin(half) * (cp * north.x + sp * east.x), cos(half) * v.y + sin(half) * (cp * north.y + sp * east.y),
                     cos(half) * v.z + sin(half) * (cp * north.z + sp * east.z)};
        double halfm = rad(g->min / 120.0);
        vec3 dirm = {cos(halfm) * v.x + sin(halfm) * (-sp * north.x + cp * east.x),
                     cos(halfm) * v.y + sin(halfm) * (-sp * north.y + cp * east.y),
                     cos(halfm) * v.z + sin(halfm) * (-sp * north.z + cp * east.z)};
        to_screen(view, dirv, W, W, &p1x, &p1y);
        to_screen(view, dirm, W, W, &p2x, &p2y);
        double ax = p1x - p0x, ay = p1y - p0y;
        double amaj = hypot(ax, ay) / W, amin = hypot(p2x - p0x, p2y - p0y) / W;
        double ang = pmod(atan2(ay, ax) / (2 * PI), 1.0);
        double rgb[3];
        blackbody_rgb(ballesteros(g->has_bv ? g->bv : 0.6), rgb);
        long t0 = rnd(srgb_encode(rgb[0])), t1 = rnd(srgb_encode(rgb[1])), t2 = rnd(srgb_encode(rgb[2]));
        char common[160];
        snprintf(common, sizeof common, "%s", g->common);
        char *c = strchr(common, ',');
        if (c)
            *c = 0;
        fprintf(o, "    {{%ld, %ld, %ld}, %ld, %ld, %ld, 0x%02lx%02lx%02lxu}, /* %s, %s: V %.2f, %.1f x %.1f arcmin, PA %.0f deg */\n",
                rnd(v.x * 32767), rnd(v.y * 32767), rnd(v.z * 32767), rnd(amaj * 65536), rnd(amin * 65536), rnd(ang * 65536) % 65536,
                t0, t1, t2, g->name, common, g->vmag, g->maj, g->min, g->pa);
        report("galaxy %s: on screen half axes %.1f x %.1f px at 3840 wide, long axis at %.1f deg from right", g->name, amaj * W,
               amin * W, ang * 360);
    }
    if (!ngal)
        fprintf(o, "    {{0, 0, 0}, 0, 0, 0, 0u},\n");
    fprintf(o, "};\nconst unsigned ck_sky_ngalaxies = %zu;\n\n", ngal);

    /* a check list for the host test: named stars and where the generator puts them at 3840x2160 */
    fprintf(o, "/* for the host test: where these stars land on a 3840 x 2160 screen, 16ths of a pixel */\n");
    size_t chk_i[8], chk_n = 0;
    double chk_x[8], chk_y[8];
    for (size_t i = 0; i < nsel && i < 40 && chk_n < 8; i++) {
        double x, y;
        if (to_screen(view, sel[i].v, 3840, 2160, &x, &y) && 0 <= x && x < 3840 && 0 <= y && y < 2160)
            chk_i[chk_n] = i, chk_x[chk_n] = x, chk_y[chk_n] = y, chk_n++;
    }
    fprintf(o, "const ck_sky_check ck_sky_checks[%zu] = {\n", chk_n);
    for (size_t k = 0; k < chk_n; k++)
        fprintf(o, "    {%zu, %ld, %ld}, /* HR %d %s */\n", chk_i[k], rnd(chk_x[k] * 16), rnd(chk_y[k] * 16), sel[chk_i[k]].s->hr,
                sel[chk_i[k]].s->name);
    fprintf(o, "};\nconst unsigned ck_sky_nchecks = %zu;\n", chk_n);
    fclose(o);
    report("tables: stars %zu B, Milky Way luma %zu B + colour %zu B, other tables about %d B", nsel * 8, ng, 2 * nco,
           256 * 4 + 128 * 6 + 512 + 8192);
    free(Y), free(CO), free(CG), free(lv);
}
