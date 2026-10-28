/* sky_real.c -- the real sky: projection, stars, Milky Way from Gaia DR3,
 * galaxies, and the sidereal time for "the sky above you now"; see sky_real.h.
 *
 * Light is added the way light adds: each star's light is put on in linear
 * light (the sRGB curve undone, the light added, the curve put back), so a
 * faint star on the Milky Way brightens it by as much as it would in the
 * sky.
 *
 * The Milky Way grid (sky_data.c) is a full-sphere equirectangular map from
 * Gaia DR3 source counts and integrated G-band flux, reddened by measured
 * extinction; rendering inverse-projects each screen point to (RA, Dec) and
 * bilinear-samples the grid. */
#include "sky_real.h"

/* ---- projection ---- */

int ck_sky_project(const ck_sky_view *v, const int16_t d[3], int32_t w, int32_t cx, int32_t cy, int32_t *x16,
                   int32_t *y16)
{
    int64_t a[3];
    for (int r = 0; r < 3; r++)
        a[r] = (int64_t)v->rot_q30[r][0] * d[0] + (int64_t)v->rot_q30[r][1] * d[1] + (int64_t)v->rot_q30[r][2] * d[2];
    if (a[2] < (1ll << 40)) /* Q45: closer than 1/32 to edge-on, about 88 degrees from the centre */
        return 0;
    int64_t z = a[2] >> 16;                       /* Q29 */
    int64_t u = a[0] * 16384 / z, t = a[1] * 16384 / z; /* tangent-plane x and y, Q30 */
    int64_t s = (int64_t)w * 8;                   /* 16ths of a pixel per half width */
    *x16 = cx * 16 + (int32_t)(u * s / v->tan_half_q30);
    *y16 = cy * 16 - (int32_t)(t * s / v->tan_half_q30);
    return 1;
}

/* ---- Milky Way: equirectangular full-sphere grid, bilinear sampling ---- */


static int32_t bilinear_s8(const int8_t (*p)[2], int k, int32_t cols, int32_t rows, int64_t c, int64_t r) /* x 256 */
{
    c = c < 0 ? 0 : c > (int64_t)(cols - 1) << 16 ? (int64_t)(cols - 1) << 16 : c;
    r = r < 0 ? 0 : r > (int64_t)(rows - 1) << 16 ? (int64_t)(rows - 1) << 16 : r;
    int32_t i = (int32_t)(c >> 16), j = (int32_t)(r >> 16), i1 = ck_min(i + 1, cols - 1), j1 = ck_min(j + 1, rows - 1);
    int32_t fx = (int32_t)(c >> 8 & 255), fy = (int32_t)(r >> 8 & 255);
    int32_t a = p[j * cols + i][k] * (256 - fx) + p[j * cols + i1][k] * fx;
    int32_t b = p[j1 * cols + i][k] * (256 - fx) + p[j1 * cols + i1][k] * fx;
    return (a * (256 - fy) + b * fy) >> 8;
}

/* Inverse-project a half-pixel screen point to equirectangular grid coords.
 * (x2, y2) in half-pixels; screen centre at (2*cx, 2*cy).
 * Returns (col_q16, row_q16) for grid sampling. The map covers the full
 * sphere: col 0 = RA 0 deg, col cols-1 = RA ~360 deg; row 0 = Dec +90,
 * row rows-1 = Dec -90.  col wraps; row is clamped.
 * Returns 0 if the point is behind the camera. */
static int grid_at_eq(const ck_sky_view *v, int32_t x2, int32_t y2, int32_t w, int32_t cx, int32_t cy,
                      int64_t *col, int64_t *row)
{
    const ck_sky_map *m = &ck_sky_milkyway;

    /* Tangent-plane offset from the view centre, Q30. The forward projection (ck_sky_project) puts a
     * tangent-plane offset u at (u / tan_half) half-widths, i.e. (u / tan_half) * w / 2 pixels, so the
     * inverse is u = px * 2 * tan_half / w with px = half-pixels / 2: u = half_px * tan_half / w. */
    int64_t u  = (int64_t)(x2 - 2 * cx) * (int64_t)v->tan_half_q30 / (int64_t)w;
    int64_t vv = (int64_t)(2 * cy - y2) * (int64_t)v->tan_half_q30 / (int64_t)w;

    /* Unnormalised sky direction in J2000 frame, ~Q30 */
    int64_t dx = (int64_t)v->rot_q30[2][0] + ((int64_t)v->rot_q30[0][0] * u >> 30) + ((int64_t)v->rot_q30[1][0] * vv >> 30);
    int64_t dy = (int64_t)v->rot_q30[2][1] + ((int64_t)v->rot_q30[0][1] * u >> 30) + ((int64_t)v->rot_q30[1][1] * vv >> 30);
    int64_t dz = (int64_t)v->rot_q30[2][2] + ((int64_t)v->rot_q30[0][2] * u >> 30) + ((int64_t)v->rot_q30[1][2] * vv >> 30);

    /* Reject behind-camera directions (dz Q30 < 0 is not enough; check full forward component) */
    if ((int64_t)v->rot_q30[2][0] * dx + (int64_t)v->rot_q30[2][1] * dy + (int64_t)v->rot_q30[2][2] * dz < (1ll << 40))
        return 0;

    /* RA: atan2(dy, dx) as turns [0,65536) */
    uint32_t ra_turn = ck_atan2((int32_t)(dy >> 8), (int32_t)(dx >> 8));
    /* col_q16: ra_turn * cols (since ra_turn / 65536 = RA fraction, col = fraction * cols * 65536 = ra_turn * cols) */
    *col = (int64_t)ra_turn * m->cols;

    /* row: equirectangular Dec spacing (1 deg/row); use ck_atan2 for correct
     * linear-in-Dec rows. Dec turn: ck_atan2(dz, h_flat) where h_flat is the
     * horizontal magnitude.  Dec=+90 → turn 16384, Dec=-90 → turn 49152. */
    int32_t dx15 = (int32_t)(dx >> 15), dy15 = (int32_t)(dy >> 15), dz15 = (int32_t)(dz >> 15);
    uint32_t h15 = ck_isqrt((uint64_t)((int64_t)dx15 * dx15 + (int64_t)dy15 * dy15));
    int32_t dec_turn = (int32_t)ck_atan2(dz15, (int32_t)h15); /* turns [0,65536) */
    /* signed: > 32768 means south of equator */
    int32_t dec_turn_s = dec_turn > 32768 ? dec_turn - 65536 : dec_turn;
    /* row 0 = Dec +90 (dec_turn_s=16384), row rows = Dec -90 (dec_turn_s=-16384)
     * row_q16 = (16384 - dec_turn_s) * rows * 2 */
    *row = (int64_t)(16384 - dec_turn_s) * (int64_t)m->rows * 2;
    return 1;
}

/* Decode one luma-grid texel, reconstructing its half-resolution chroma
 * first. The generated map is encoded sRGB; interpolate its light, not
 * its display levels. Retain the chroma's fractional levels on decoding. */
static void milkyway_texel(int32_t i, int32_t j, uint32_t light[3])
{
    const ck_sky_map *m = &ck_sky_milkyway;
    int32_t hc = (m->cols + 1) / 2, hr = (m->rows + 1) / 2;
    int64_t cc = ((int64_t)i * 65536 - 32768) / 2, cr = ((int64_t)j * 65536 - 32768) / 2;
    if (cc < 0)
        cc += (int64_t)hc * 65536;
    int32_t y = ck_sky_luma[(size_t)j * m->cols + i] * 16;
    int32_t co = bilinear_s8(ck_sky_chroma, 0, hc, hr, cc, cr) >> 5;
    int32_t cg = bilinear_s8(ck_sky_chroma, 1, hc, hr, cc, cr) >> 5;
    int32_t rgb[3] = {ck_clamp(y + co - cg, 0, 4080), ck_clamp(y + cg, 0, 4080),
                      ck_clamp(y - co - cg, 0, 4080)};
    for (int k = 0; k < 3; k++) {
        uint32_t lo = (uint32_t)rgb[k] >> 4, f = (uint32_t)rgb[k] & 15u;
        uint32_t hi = lo < 255 ? lo + 1 : lo;
        light[k] = ck_sky_lin[lo] + ((ck_sky_lin[hi] - ck_sky_lin[lo]) * f >> 4);
    }
}

/* The equirectangular Milky Way at half-pixel (x2, y2) through view v:
 * three channels of linear light, Q15. */
static void milkyway_at(const ck_sky_view *v, int32_t x2, int32_t y2, int32_t w, int32_t cx, int32_t cy,
                        int32_t rgb[3])
{
    const ck_sky_map *m = &ck_sky_milkyway;
    int64_t col, row;
    if (!grid_at_eq(v, x2, y2, w, cx, cy, &col, &row)) {
        rgb[0] = rgb[1] = rgb[2] = 0;
        return;
    }
    /* Wrap col in RA */
    int64_t maxcol = (int64_t)m->cols * 65536;
    while (col < 0) col += maxcol;
    while (col >= maxcol) col -= maxcol;
    col = col > (int64_t)(m->cols - 1) << 16 ? (int64_t)(m->cols - 1) << 16 : col;
    row = row < 0 ? 0 : row > (int64_t)(m->rows - 1) << 16 ? (int64_t)(m->rows - 1) << 16 : row;
    int32_t i = (int32_t)(col >> 16), j = (int32_t)(row >> 16);
    int32_t i1 = ck_min(i + 1, m->cols - 1), j1 = ck_min(j + 1, m->rows - 1);
    uint32_t fx = (uint32_t)(col >> 8) & 255u, fy = (uint32_t)(row >> 8) & 255u;
    uint32_t taps[4][3];
    milkyway_texel(i, j, taps[0]);
    milkyway_texel(i1, j, taps[1]);
    milkyway_texel(i, j1, taps[2]);
    milkyway_texel(i1, j1, taps[3]);
    for (int k = 0; k < 3; k++) {
        uint32_t a = taps[0][k] * (256 - fx) + taps[1][k] * fx;
        uint32_t b = taps[2][k] * (256 - fx) + taps[3][k] * fx;
        rgb[k] = (int32_t)((a * (256 - fy) + b * fy) >> 16);
    }
}

void ck_sky_milkyway_q(uint16_t *q, int32_t qw, int32_t qh, int32_t w, int32_t cx, int32_t cy,
                       const ck_sky_view *v)
{
    for (int32_t j = 0; j < qh; j++)
        for (int32_t i = 0; i < qw; i++) {
            int32_t rgb[3];
            milkyway_at(v, 8 * i + 1, 8 * j + 1, w, cx, cy, rgb);
            uint16_t *c = q + ((size_t)j * (size_t)qw + (size_t)i) * 3;
            c[0] = (uint16_t)rgb[0], c[1] = (uint16_t)rgb[1], c[2] = (uint16_t)rgb[2];
        }
}

/* ---- stars ---- */

/* adds light round (x16, y16): radius r16, centre light peak (linear, Q16), colour col (linear, 4096 the
 * brightest channel); the light falls off as (1 - d^2 / r^2)^3 */
static void light_add(ck_canvas *cv, int32_t x16, int32_t y16, int32_t r16, uint64_t peak, const uint16_t col[3])
{
    if (r16 < 4 || peak < 4)
        return;
    int32_t x0 = ck_max(0, (x16 - r16) >> 4), x1 = ck_min(cv->w - 1, (x16 + r16) >> 4);
    int32_t y0 = ck_max(0, (y16 - r16) >> 4), y1 = ck_min(cv->h - 1, (y16 + r16) >> 4);
    int64_t r2 = (int64_t)r16 * r16;
    uint64_t inv = (1ull << 40) / (uint64_t)r2;
    uint64_t pc[3];
    for (int k = 0; k < 3; k++)
        pc[k] = peak * col[k] >> 13; /* Q15 linear at the centre, per channel */
    for (int32_t y = y0; y <= y1; y++) {
        int64_t dy = (int64_t)y * 16 + 8 - y16;
        uint32_t *row = cv->px + (size_t)y * (size_t)cv->w;
        for (int32_t x = x0; x <= x1; x++) {
            int64_t dx = (int64_t)x * 16 + 8 - x16, d2 = dx * dx + dy * dy;
            if (d2 >= r2)
                continue;
            uint64_t f = 65536u - ((uint64_t)d2 * inv >> 24);   /* Q16 */
            uint64_t f3 = ((f * f >> 16) * f) >> 16;              /* Q16 */
            uint32_t c = row[x], out = 0;
            int any = 0;
            for (int k = 0; k < 3; k++) {
                uint32_t lv = c >> (16 - 8 * k) & 255u;
                uint64_t add = pc[k] * f3 >> 16;
                if (add) {
                    uint64_t l = ck_sky_lin[lv] + add;
                    lv = (ck_sky_srgb16[l >= 32767 ? 4095 : l >> 3] + 8) >> 4;
                    lv = lv > 255 ? 255 : lv;
                    any = 1;
                }
                out = out << 8 | lv;
            }
            if (any)
                row[x] = (c & 0xff000000u) | out;
        }
    }
}

static uint32_t srgb_of(const uint16_t col[3])
{
    uint32_t c = 0;
    for (int k = 0; k < 3; k++) {
        uint32_t i = col[k] >= 4096 ? 4095 : col[k];
        c = c << 8 | ((ck_sky_srgb16[i] + 8) >> 4);
    }
    return c;
}

void ck_sky_draw_stars(ck_canvas *cv, uint32_t k, int32_t cx, int32_t cy, const ck_sky_view *v,
                       ck_night_star *tw, unsigned max, unsigned *ntw)
{
    int32_t r0 = (int32_t)(k * 3 / 40); /* 16ths of a pixel: 1.2 design px */
    if (r0 < 12)
        r0 = 12;
    /* the core grows with the scale and its middle keeps its light, so a star's total light grows as the
     * square of the scale and it looks the same on any screen */
    *ntw = 0;
    for (unsigned i = 0; i < ck_sky_nstars; i++) {
        const ck_sky_star *s = &ck_sky_stars[i];
        int16_t d[3] = {s->x, s->y, s->z};
        int32_t x16, y16;
        if (!ck_sky_project(v, d, cv->w, cx, cy, &x16, &y16))
            continue;
        int32_t reach = r0 * 12;
        if (x16 < -reach || y16 < -reach || x16 > cv->w * 16 + reach || y16 > cv->h * 16 + reach)
            continue;
        const uint16_t *col = ck_sky_palette[s->bv < 128 ? s->bv : 127];
        uint64_t core = ck_sky_flux[s->mag]; /* the light at the middle of the core */
        light_add(cv, x16, y16, r0, core, col);
        light_add(cv, x16, y16, r0 * 4, (core * 15) >> 7, col);
        light_add(cv, x16, y16, r0 * 12, (core * 5) >> 7, col);
        if (s->mag <= 59 && *ntw < max) { /* V <= 1.0: bright enough to twinkle */
            tw[*ntw].x = x16, tw[*ntw].y = y16, tw[*ntw].r = r0 * 4;
            tw[*ntw].color = srgb_of(col);
            tw[*ntw].period = (uint16_t)(1200 + (i * 137) % 2400);
            tw[*ntw].phase = (uint16_t)((i * 919) % 65536);
            (*ntw)++;
        }
    }
}

/* ---- galaxies ---- */

ck_rect ck_sky_galaxy_bound(uint32_t w, unsigned i)
{
    if (i >= ck_sky_ngalaxies)
        return (ck_rect){0, 0, 0, 0};
    const ck_sky_galaxy *g = &ck_sky_galaxies[i];
    /* the bound uses the default view; galaxies only draw near their canonical position */
    int32_t a = (int32_t)((int64_t)g->a_q16 * (int32_t)w >> 16) + 4;
    return (ck_rect){0, 0, a * 2 + 1, a * 2 + 1};
}

int ck_sky_galaxy_in_view(uint32_t w, uint32_t h, int32_t cx, int32_t cy, const ck_sky_view *v, unsigned i,
                          int32_t *gx, int32_t *gy)
{
    if (i >= ck_sky_ngalaxies)
        return 0;
    int32_t x16, y16;
    if (!ck_sky_project(v, ck_sky_galaxies[i].dir, (int32_t)w, cx, cy, &x16, &y16))
        return 0;
    *gx = (x16 + 8) >> 4;
    *gy = (y16 + 8) >> 4;
    return *gx >= 0 && *gx < (int32_t)w && *gy >= 0 && *gy < (int32_t)h;
}

ck_rect ck_sky_galaxy_box(uint32_t w, uint32_t h, int32_t cx, int32_t cy, const ck_sky_view *v, unsigned i)
{
    int32_t gx, gy;
    if (!ck_sky_galaxy_in_view(w, h, cx, cy, v, i, &gx, &gy))
        return (ck_rect){0, 0, 0, 0};
    const ck_sky_galaxy *g = &ck_sky_galaxies[i];
    int32_t a = (int32_t)((int64_t)g->a_q16 * (int32_t)w >> 16) + 4;
    int32_t x0 = ck_max(0, gx - a), y0 = ck_max(0, gy - a);
    int32_t x1 = ck_min((int32_t)w - 1, gx + a), y1 = ck_min((int32_t)h - 1, gy + a);
    if (x0 >= x1 || y0 >= y1)
        return (ck_rect){0, 0, 0, 0};
    return (ck_rect){x0, y0, x1 - x0, y1 - y0};
}

void ck_sky_galaxy_maps(ck_night_galaxy *g, unsigned i, uint32_t w, int32_t cx, int32_t cy,
                        const ck_sky_view *v)
{
    if (i >= ck_sky_ngalaxies || g->box.w == 0)
        return;
    int32_t gx, gy;
    if (!ck_sky_galaxy_in_view(w, 0x7fffffff, cx, cy, v, i, &gx, &gy))
        return;
    const ck_sky_galaxy *gal = &ck_sky_galaxies[i];
    int32_t a = (int32_t)((int64_t)gal->a_q16 * (int32_t)w >> 16);
    int32_t b = (int32_t)((int64_t)gal->b_q16 * (int32_t)w >> 16) + 1;
    int32_t ca = (int32_t)ck_cos(gal->angle), sa = (int32_t)ck_sin(gal->angle);
    g->tint = gal->tint;
    g->period = (uint16_t)(2400 + i * 400);
    for (int32_t py = 0; py < g->box.h; py++)
        for (int32_t px = 0; px < g->box.w; px++) {
            int32_t dx = (g->box.x + px) - gx, dy = (g->box.y + py) - gy;
            int32_t u = (dx * ca + dy * sa) >> 14; /* along the long axis */
            int32_t vv = (-dx * sa + dy * ca) >> 14; /* along the short */
            size_t off = (size_t)py * (size_t)g->box.w + (size_t)px;
            if (a == 0 || b == 0 || (int64_t)u * u * b * b + (int64_t)vv * vv * a * a > (int64_t)a * a * b * b) {
                g->light[off] = 0;
                g->phase[off] = 128;
                continue;
            }
            /* light falls off as (1 - (u/a)^2 - (v/b)^2)^2 */
            int64_t f = 65536 - (int64_t)u * u * 65536 / (a * a) - (int64_t)vv * vv * 65536 / (b * b);
            if (f < 0) f = 0;
            g->light[off] = (uint8_t)((f * f >> 16) * 255 >> 16);
            g->phase[off] = (uint8_t)((u + a) * 255 / (2 * a + 1));
        }
}

/* ---- the sky above you now: sidereal time, the zenith, precession ---- */

#define J2000_UNIX 946728000ll /* 2000 January 1, 12:00 UT */

uint32_t ck_sky_gmst(int64_t t)
{
    /* GMST = 18.697374558 h + 24.06570982441908 h x D, D days from J2000 (USNO; Aoki et al. 1982),
     * here in turns x 2^48: 0.7790572732 + 1.0027379093508 D */
    int64_t s = t - J2000_UNIX, days = s / 86400, rem = s % 86400;
    if (rem < 0)
        rem += 86400, days -= 1;
    uint64_t turns = 219285127830237ull + (uint64_t)days * 770652970752ull + (uint64_t)rem * 3266731825ull;
    return (uint32_t)(turns >> 16);
}

/* (cos, sin) of a small angle a (radians, Q30), Q30 */
static void small_cs(int64_t a, int64_t *c, int64_t *s)
{
    int64_t a2 = a * a >> 30;
    *c = (1ll << 30) - a2 / 2 + (a2 * a2 >> 30) / 24;
    *s = a - (a2 * a >> 30) / 6;
}

static void rot_z(int64_t v[3], int64_t a) /* frame turned by a about z */
{
    int64_t c, s;
    small_cs(a, &c, &s);
    int64_t x = (v[0] * c + v[1] * s) >> 30, y = (v[1] * c - v[0] * s) >> 30;
    v[0] = x, v[1] = y;
}

static void rot_y(int64_t v[3], int64_t a) /* frame turned by a about y */
{
    int64_t c, s;
    small_cs(a, &c, &s);
    int64_t x = (v[0] * c - v[2] * s) >> 30, z = (v[0] * s + v[2] * c) >> 30;
    v[0] = x, v[2] = z;
}

/* sin and cos of a in millionths of a degree, Q30, from the Q14 table refined by one Newton-like step */
static void cs_udeg(int64_t a_udeg, int64_t *c, int64_t *s)
{
    a_udeg %= 360000000ll;
    if (a_udeg < 0)
        a_udeg += 360000000ll;
    uint32_t turn = (uint32_t)(a_udeg * 65536 / 360000000ll);
    /* the remainder past the table's step, radians Q30: (a - turn) x 2 pi / 360e6 */
    int64_t rest = a_udeg - (int64_t)turn * 360000000ll / 65536;
    int64_t e = rest * 18740 / 1000; /* 2 pi / 360e6 x 2^30 = 18.740 */
    int64_t c0 = (int64_t)ck_cos(turn) * 65536, s0 = (int64_t)ck_sin(turn) * 65536;
    *c = c0 - (s0 * e >> 30);
    *s = s0 + (c0 * e >> 30);
}

void ck_sky_zenith(int64_t t, int32_t lat_udeg, int32_t lon_udeg, int16_t d[3], int32_t *ra_udeg,
                   int32_t *dec_udeg)
{
    /* straight up: right ascension the local sidereal time, declination the latitude (of the date) */
    int64_t lst = (int64_t)((uint64_t)ck_sky_gmst(t) * 360000000ull >> 32) + lon_udeg;
    int64_t ca, sa, cd, sd;
    cs_udeg(lst, &ca, &sa);
    cs_udeg(lat_udeg, &cd, &sd);
    int64_t v[3] = {cd * ca >> 30, cd * sa >> 30, sd};
    /* back from the equator of the date to J2000 (Lieske et al. 1977, IAU 1976):
     * v_J2000 = R3(zeta) R2(-theta) R3(z) v_date; the angles in radians Q40 per century */
    int64_t T = (t - J2000_UNIX) * (1ll << 30) / (36525ll * 86400); /* centuries, Q30 */
    int64_t T2 = T * T >> 30;
    int64_t zeta = (12293486530ll * T + 1609196ll * T2) >> 40; /* 2306.2181" T + 0.30188" T^2, Q30 rad */
    int64_t zz = (12293486530ll * T + 5835282ll * T2) >> 40;   /* 2306.2181" T + 1.09468" T^2 */
    int64_t th = (10684145203ll * T - 2274293ll * T2) >> 40;   /* 2004.3109" T - 0.42665" T^2 */
    rot_z(v, zz);
    rot_y(v, -th);
    rot_z(v, zeta);
    for (int k = 0; k < 3; k++)
        d[k] = (int16_t)ck_clamp((int32_t)(v[k] >> 15), -32767, 32767);
    if (ra_udeg) {
        uint32_t ra = ck_atan2((int32_t)(v[1] >> 8), (int32_t)(v[0] >> 8));
        *ra_udeg = (int32_t)((int64_t)ra * 360000000ll / 65536);
    }
    if (dec_udeg) {
        int64_t h = (int64_t)ck_isqrt((uint64_t)((v[0] >> 8) * (v[0] >> 8) + (v[1] >> 8) * (v[1] >> 8)));
        uint32_t de = ck_atan2((int32_t)(v[2] >> 8), (int32_t)h);
        *dec_udeg = (int32_t)((int64_t)(int16_t)de * 360000000ll / 65536);
    }
}

void ck_sky_view_at(ck_sky_view *v, int32_t ra_udeg, int32_t dec_udeg, int32_t fov_mdeg, int32_t roll_mdeg)
{
    int64_t ca, sa, cd, sd, cr, sr, ch, sh;
    cs_udeg(ra_udeg, &ca, &sa);
    cs_udeg(dec_udeg, &cd, &sd);
    cs_udeg((int64_t)roll_mdeg * 1000, &cr, &sr);
    cs_udeg((int64_t)fov_mdeg * 500, &ch, &sh);
    int64_t west[3] = {sa, -ca, 0}, north[3] = {-(sd * ca >> 30), -(sd * sa >> 30), cd};
    int64_t fwd[3] = {cd * ca >> 30, cd * sa >> 30, sd};
    for (int k = 0; k < 3; k++) {
        v->rot_q30[0][k] = (int32_t)((cr * west[k] + sr * north[k]) >> 30);
        v->rot_q30[1][k] = (int32_t)((cr * north[k] - sr * west[k]) >> 30);
        v->rot_q30[2][k] = (int32_t)fwd[k];
    }
    v->tan_half_q30 = (int32_t)((sh << 30) / ch);
    v->ra_mdeg = ra_udeg / 1000, v->dec_mdeg = dec_udeg / 1000, v->fov_mdeg = fov_mdeg, v->roll_mdeg = roll_mdeg;
}