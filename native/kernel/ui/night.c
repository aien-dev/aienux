/* night.c -- the night sky and the merging black holes; see night.h.
 *
 * The sky is the real one (ui/sky_real.c, from ui/sky_data.c): the
 * Milky Way from a photograph of the whole sky, worked out at a quarter of
 * the screen's resolution (it is smooth anyway; the photograph's own
 * stars are taken out), scaled up with a 4 x 4 ordered dither so the faint
 * gradients show no bands; then the catalogue's stars, at full resolution,
 * on top.
 *
 * Bent light: a point mass moves the image of a star seen at angle theta
 * from it to where the star really is, beta = theta - E^2 / theta (E the
 * Einstein radius). Each pixel shows the sky at its own beta (with both
 * holes' pulls added), so stars behind a hole stretch into arcs round it and
 * a second, inner image appears on the far side. The pull is eased to zero
 * between 2.2 E and 4.4 E so the bent region has no edge. It is worked out
 * exactly near the holes and on an 8-pixel grid (and interpolated) further
 * out, where it is smooth. The ripple adds a displacement along the radius
 * that stretches one way and squeezes the other (cos 2 theta), the pattern
 * a gravitational wave makes. */
#include "night.h"
#include "sky_real.h"

#define S(v) ((int32_t)(((int64_t)(v) * (int64_t)n->k + 128) / 256))
#define Q30 (1ull << 30)

/* ---- the galaxies: the real ones in the view (sky_real.c) ---- */

static uint8_t band_t[256]; /* the sweeping band: ((1 + cos) / 2)^6, 0..255 */
static uint32_t pow2_t[257]; /* 2^(k/256), Q16: for the dusk's distances without a square root per pixel */

static uint64_t pow2_q16(uint32_t x) /* 2^(x / 65536), Q16; x below 40 << 16 */
{
    uint32_t n = x >> 16, f = x & 65535u, k = f >> 8, g = f & 255u;
    uint64_t m = pow2_t[k] + ((uint64_t)(pow2_t[k + 1] - pow2_t[k]) * g >> 8);
    return m << n;
}

size_t ck_night_mem_bytes(uint32_t w, uint32_t h, uint32_t k)
{
    (void)k;
    size_t n = (size_t)w * h * 4 + (size_t)(w / 4 + 1) * (h / 4 + 1) + 64;
    for (unsigned i = 0; i < CK_NIGHT_GALAXIES; i++) {
        ck_rect b = ck_sky_galaxy_bound(w, i);
        n += (size_t)b.w * (size_t)b.h * 2;
    }
    return n;
}

/* exp(-(v / width)^2), Q16 */
static uint32_t gauss(int64_t v, int64_t width) /* exp(-(v/width)^2), Q16 */
{
    int64_t q = v * 256 / (width ? width : 1);
    if (q > 4095 || q < -4095)
        return 0;
    return ck_exp_neg((uint32_t)(q * q >> 8));
}

/* ---- the soft layer (the Milky Way): a quarter-resolution linear-light Q15 grid ---- */

static void scale_up(ck_night *n, const uint16_t *q, int32_t qw)
{
    static const uint8_t bayer[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
    for (int32_t y = 0; y < n->h; y++) {
        int32_t qy = y >> 2, fy = y & 3;
        const uint16_t *r0 = q + (size_t)qy * (size_t)qw * 3, *r1 = r0 + (size_t)qw * 3;
        uint32_t *row = n->sky + (size_t)y * (size_t)n->w;
        for (int32_t x = 0; x < n->w; x++) {
            int32_t qx = x >> 2, fx = x & 3;
            uint32_t d = bayer[(y & 3) * 4 + (x & 3)], c = 0;
            for (int k = 0; k < 3; k++) {
                uint32_t a = r0[qx * 3 + k] * (uint32_t)(4 - fx) + r0[qx * 3 + 3 + k] * (uint32_t)fx;
                uint32_t b = r1[qx * 3 + k] * (uint32_t)(4 - fx) + r1[qx * 3 + 3 + k] * (uint32_t)fx;
                uint32_t light = (a * (uint32_t)(4 - fy) + b * (uint32_t)fy) / 16;
                uint32_t v = (ck_sky_srgb16[light >= 32767u ? 4095u : light >> 3] + d) >> 4;
                c = c << 8 | v;
            }
            row[x] = c;
        }
    }
}

/* ---- coming out of the dark ---- */

static void appear_map(ck_night *n)
{
    int32_t ah = n->h / 4 + 1;
    int64_t far = 0;
    int32_t cs[4][2] = {{0, 0}, {n->w, 0}, {0, n->h}, {n->w, n->h}};
    for (int i = 0; i < 4; i++) {
        int64_t dx = cs[i][0] - n->cx, dy = cs[i][1] - n->cy, d = (int64_t)ck_isqrt((uint64_t)(dx * dx + dy * dy));
        far = d > far ? d : far;
    }
    for (int32_t y = 0; y < ah; y++)
        for (int32_t x = 0; x < n->aw; x++) {
            int64_t px = (int64_t)x * 4, py = (int64_t)y * 4, dx = px - n->cx, dy = py - n->cy;
            uint32_t a = ck_fbm((int32_t)(px * 640 / n->h), (int32_t)(py * 640 / n->h), 909, 4) >> 8;
            uint32_t r = (uint32_t)((int64_t)ck_isqrt((uint64_t)(dx * dx + dy * dy)) * 255 / (far ? far : 1));
            n->appear[(size_t)y * (size_t)n->aw + (size_t)x] = (uint8_t)((a * 6 + r * 4) / 10);
        }
}

uint32_t ck_night_faded(const ck_night *n, int32_t x, int32_t y, uint32_t l)
{
    if (n->t >= CK_NIGHT_FADE_END_MS)
        return 256;
    x = ck_clamp(x, 0, n->w - 1), y = ck_clamp(y, 0, n->h - 1);
    int32_t from = 300 + n->appear[(size_t)(y >> 2) * (size_t)n->aw + (size_t)(x >> 2)] * 12 + (int32_t)(255 - l) * 8;
    int32_t dt = (int32_t)n->t - from;
    return dt <= 0 ? 0 : dt >= 1365 ? 256 : ck_smooth(dt * 48 >> 8);
}

static uint32_t fade_px(const ck_night *n, uint32_t c, int32_t x, int32_t y)
{
    uint32_t r = c >> 16 & 255, g = c >> 8 & 255, b = c & 255, l = r > g ? r : g;
    l = l > b ? l : b;
    uint32_t f = ck_night_faded(n, x, y, l);
    return f >= 256 ? c : ck_scale(c, f);
}

int ck_night_init(ck_night *n, uint32_t w, uint32_t h, uint32_t k, int32_t cx, int32_t cy, void *mem, size_t len,
                  void *scratch)
{
    if (!mem || !scratch || w < 64 || h < 64 || w > CK_NIGHT_W_MAX || len < ck_night_mem_bytes(w, h, k))
        return -1;
    (void)ck_sin(0); /* the tables, before anything else could ask for them */
    for (unsigned i = 0; i < 256; i++) {
        uint64_t c = (uint64_t)(16384 + ck_cos(i * 256u)) >> 1; /* Q14 */
        uint64_t c2 = c * c >> 14, c6 = (c2 * c2 >> 14) * c2 >> 14;
        band_t[i] = (uint8_t)(c6 * 255 >> 14);
    }
    uint64_t e2 = 1ull << 30; /* 2^(k/256): one factor of 2^(1/256) (Q30) per step */
    for (unsigned i = 0; i < 256; i++) {
        pow2_t[i] = (uint32_t)((e2 + (1u << 13)) >> 14);
        e2 = e2 * 1076652777ull >> 30;
    }
    pow2_t[256] = 131072;
    *n = (ck_night){0};
    n->w = (int32_t)w, n->h = (int32_t)h, n->k = k, n->cx = cx, n->cy = cy;
    uint8_t *p = mem;
    n->sky = (uint32_t *)p;
    p += (size_t)w * h * 4;
    n->appear = p;
    n->aw = (int32_t)(w / 4 + 1);
    p += (size_t)(w / 4 + 1) * (h / 4 + 1);
    int32_t qw = (int32_t)(w / 4 + 2), qh = (int32_t)(h / 4 + 2);
    n->view = ck_sky_the_view; /* default view: GW150914 region */
    ck_sky_milkyway_q(scratch, qw, qh, n->w, cx, cy, &n->view);
    scale_up(n, scratch, qw);
    ck_canvas cv = {n->sky, n->w, n->h};
    ck_sky_draw_stars(&cv, k, cx, cy, &n->view, n->tw, CK_NIGHT_TWINKLES, &n->ntw);
    for (unsigned i = 0; i < CK_NIGHT_GALAXIES; i++) {
        ck_night_galaxy *g = &n->gal[i];
        g->box = ck_sky_galaxy_box(w, h, cx, cy, &n->view, i);
        g->light = p;
        g->phase = p + (size_t)g->box.w * (size_t)g->box.h;
        p += (size_t)g->box.w * (size_t)g->box.h * 2;
        ck_sky_galaxy_maps(g, i, w, cx, cy, &n->view);
    }
    appear_map(n);
    ck_night_at(n, 0);
    return 0;
}

/* ---- the black holes and the ripple at a time ---- */

static uint64_t root8(uint64_t l) /* l^(1/8), Q30 */
{
    for (int i = 0; i < 3; i++)
        l = ck_isqrt(l << 30);
    return l;
}

#define ORBIT_MS 5200u /* one turn of the wide orbit */

static uint32_t orbit_angle(uint32_t t, int32_t *sep16, int32_t a0)
{
    uint32_t spiral_from = (CK_NIGHT_SPIRAL_MS - CK_NIGHT_HOLES_MS) * 65536u / ORBIT_MS;
    if (t < CK_NIGHT_SPIRAL_MS) {
        *sep16 = a0;
        return (t - CK_NIGHT_HOLES_MS) * 65536u / ORBIT_MS;
    }
    uint64_t left = ((uint64_t)(CK_NIGHT_MERGE_MS - t) << 30) / (CK_NIGHT_MERGE_MS - CK_NIGHT_SPIRAL_MS);
    uint64_t r = root8(left), r2 = r * r >> 30, r5 = (r2 * r2 >> 30) * r >> 30;
    *sep16 = (int32_t)((uint64_t)a0 * r2 >> 30);
    /* the spiral's angle carries on at the wide orbit's speed: K (1 - L^(5/8)), K = 8/5 x its turns */
    uint64_t turns = (uint64_t)65536 * 8 * (CK_NIGHT_MERGE_MS - CK_NIGHT_SPIRAL_MS) / (5ull * ORBIT_MS);
    return spiral_from + (uint32_t)(turns * (Q30 - r5) >> 30);
}

void ck_night_at(ck_night *n, uint32_t t)
{
    n->t = t;
    n->holes = 0;
    n->falling = 0;
    n->lens = (ck_rect){0, 0, 0, 0};
    n->ripple_w = 0;
    int32_t e_full = S(60) * 16, rs_full = S(20) * 16, a0 = S(270) * 16;
    int32_t cx = n->cx * 16 + 8, cy = n->cy * 16 + 8;
    int32_t sep;
    uint32_t last = orbit_angle(CK_NIGHT_MERGE_MS - 1, &sep, a0);
    if (t >= CK_NIGHT_HOLES_MS && t < CK_NIGHT_MERGE_MS) {
        uint32_t ang = orbit_angle(t, &sep, a0);
        int32_t e = (int32_t)((int64_t)e_full * ck_smooth(ck_prog(t, CK_NIGHT_HOLES_MS, CK_NIGHT_HOLES_MS + 2600)) >> 8);
        int32_t rs = (int32_t)((int64_t)rs_full *
                               ck_smooth(ck_prog(t, CK_NIGHT_HOLES_MS + 1400, CK_NIGHT_HOLES_MS + 3400)) >> 8);
        int32_t hx = (int32_t)((int64_t)(sep / 2) * ck_cos(ang) >> 14);
        int32_t hy = (int32_t)((int64_t)(sep / 2) * ck_sin(ang) >> 15); /* the orbit leans away: half as tall */
        n->hole[0] = (__typeof__(n->hole[0])){cx + hx, cy + hy, e, rs};
        n->hole[1] = (__typeof__(n->hole[1])){cx - hx, cy - hy, e, rs};
        n->holes = 2;
    } else if (t >= CK_NIGHT_MERGE_MS && t < CK_NIGHT_GONE_MS) {
        uint32_t tm = t - CK_NIGHT_MERGE_MS;
        int32_t e = e_full * 362 / 256, rs0 = rs_full * 323 / 256, rs1 = rs_full * 2;
        int32_t rs = rs0 + (int32_t)((int64_t)(rs1 - rs0) * ck_smooth(ck_prog(tm, 0, 180)) >> 8);
        /* it rings: a quick wobble that dies away */
        int64_t wob = (int64_t)ck_sin(tm * 65536u / 95u) * ck_exp_neg(tm * 256u / 260u) >> 16;
        rs += (int32_t)(rs * wob * 7 / 100 >> 14);
        uint32_t keep = 256 - ck_smooth(ck_prog(t, CK_NIGHT_MERGE_MS + 900, CK_NIGHT_GONE_MS));
        n->hole[0] = (__typeof__(n->hole[0])){cx, cy, (int32_t)((int64_t)e * keep >> 8),
                                             (int32_t)((int64_t)rs * (keep * keep >> 8) >> 8)};
        n->holes = 1;
    }
    for (unsigned i = 0; i < n->holes; i++) {
        int32_t reach = (n->hole[i].e * 44 / 10 >> 4) + 2;
        if (n->hole[i].e <= 0 && n->hole[i].rs <= 0)
            continue;
        reach = ck_max(reach, (n->hole[i].rs * 16 / 10 >> 4) + 2);
        ck_rect b = {(n->hole[i].x >> 4) - reach, (n->hole[i].y >> 4) - reach, 2 * reach + 1, 2 * reach + 1};
        n->lens = ck_union(n->lens, b);
    }
    ck_rect all = {0, 0, n->w, n->h};
    if (n->lens.w && !ck_clip(&n->lens, &all))
        n->lens = (ck_rect){0, 0, 0, 0};
    if (t >= CK_NIGHT_MERGE_MS && t < CK_NIGHT_MERGE_MS + CK_NIGHT_RIPPLE_MS) {
        uint32_t tr = t - CK_NIGHT_MERGE_MS;
        int32_t w = ck_clamp(S(60), 4, 200), reach = w * 22 / 10;
        int64_t far = 0;
        int32_t cs[4][2] = {{0, 0}, {n->w, 0}, {0, n->h}, {n->w, n->h}};
        for (int i = 0; i < 4; i++) {
            int64_t dx = cs[i][0] - n->cx, dy = cs[i][1] - n->cy, d = (int64_t)ck_isqrt((uint64_t)(dx * dx + dy * dy));
            far = d > far ? d : far;
        }
        far += reach;
        n->ripple_r = (int32_t)(far * tr / CK_NIGHT_RIPPLE_MS);
        n->ripple_w = w;
        uint32_t env = ck_smooth(ck_prog(tr, 0, 220)) *
                           (256 - ck_smooth(ck_prog(tr, CK_NIGHT_RIPPLE_MS * 6 / 10, CK_NIGHT_RIPPLE_MS))) >> 8;
        int64_t amp = (int64_t)S(6) * 16 * (256 - 150 * n->ripple_r / far) >> 8;
        amp = amp * env >> 8;
        int32_t lambda = ck_max(S(110), 8);
        for (int32_t i = 0; i <= 2 * reach && i < 1024; i++) {
            int32_t o = i - reach;
            int64_t s = ck_sin((uint32_t)((int64_t)o * 65536 / lambda));
            n->ripple_disp[i] = (int32_t)((amp * s >> 14) * gauss(o, w) >> 16);
            n->ripple_light[i] = (uint8_t)((uint64_t)gauss(o, w * 6 / 10) * env * 46 >> 24);
        }
        /* it stretches along the line the pair last lay on, and squeezes across it */
        n->ripple_c2 = ck_cos(2 * last);
        n->ripple_s2 = ck_sin(2 * last);
    }
}

/* ---- drawing the base ---- */

static uint32_t lerp16(uint32_t a, uint32_t b, uint32_t f) /* f: 0..16 */
{
    return ck_mix(a, b, f * 16);
}

/* the sky at (sx, sy), 16ths of a pixel (pixel i's centre at 16 i + 8) */
static uint32_t sample(const ck_night *n, int32_t sx, int32_t sy)
{
    sx = ck_clamp(sx - 8, 0, (n->w - 1) * 16);
    sy = ck_clamp(sy - 8, 0, (n->h - 1) * 16);
    int32_t ix = sx >> 4, iy = sy >> 4, ix1 = ck_min(ix + 1, n->w - 1), iy1 = ck_min(iy + 1, n->h - 1);
    const uint32_t *r0 = n->sky + (size_t)iy * (size_t)n->w, *r1 = n->sky + (size_t)iy1 * (size_t)n->w;
    uint32_t fx = (uint32_t)sx & 15u, fy = (uint32_t)sy & 15u;
    return lerp16(lerp16(r0[ix], r0[ix1], fx), lerp16(r1[ix], r1[ix1], fx), fy);
}

/* the holes' pull on the light at (X, Y) (16ths of a pixel): added to (sx, sy), where the sky is read from */
static void lens_at(const ck_night *n, int64_t X, int64_t Y, int64_t *sx, int64_t *sy)
{
    for (unsigned i = 0; i < n->holes; i++) {
        int64_t e = n->hole[i].e, dx = X - n->hole[i].x, dy = Y - n->hole[i].y, d2 = dx * dx + dy * dy;
        int64_t out = e * e * 1936 / 100, in = e * e * 484 / 100; /* (4.4 E)^2, (2.2 E)^2 */
        if (e <= 0 || d2 >= out)
            continue;
        if (d2 < 64)
            d2 = 64;
        int64_t f = e * e * 65536 / d2;
        if (d2 > in) {
            int64_t w = (out - d2) * 65536 / (out - in);
            f = f * (w * w >> 16) >> 16;
        }
        *sx -= dx * f >> 16;
        *sy -= dy * f >> 16;
    }
}

/* where (X, Y) (16ths of a pixel) looks in the sky: offset (ax, ay), and the ripple's own faint light */
static void warp(const ck_night *n, int64_t X, int64_t Y, int32_t *ax, int32_t *ay, uint32_t *light)
{
    int64_t sx = 0, sy = 0;
    *light = 0;
    lens_at(n, X, Y, &sx, &sy);
    if (n->ripple_w) {
        int64_t dx = X - (n->cx * 16 + 8), dy = Y - (n->cy * 16 + 8), d2 = dx * dx + dy * dy;
        int64_t r16 = (int64_t)ck_isqrt((uint64_t)d2), reach = n->ripple_w * 22 / 10;
        int64_t i = (r16 >> 4) - n->ripple_r + reach;
        if (r16 > 0 && i >= 0 && i <= 2 * reach && i < 1024) {
            int64_t q = ((dx * dx - dy * dy) * n->ripple_c2 + 2 * dx * dy * n->ripple_s2) / d2; /* cos 2(theta - psi), Q14 */
            int64_t m = (int64_t)n->ripple_disp[i] * (5734 + (q * 10650 >> 14)) >> 14;
            sx -= m * dx / r16;
            sy -= m * dy / r16;
            *light = n->ripple_light[i];
        }
    }
    *ax = (int32_t)sx;
    *ay = (int32_t)sy;
}

static uint32_t finish(const ck_night *n, uint32_t c, int32_t x, int32_t y, uint32_t light)
{
    if (n->t < CK_NIGHT_FADE_END_MS)
        c = fade_px(n, c, x, y);
    return light ? ck_add(c, CK_RGB(86, 108, 176), light) : c;
}

/* a pixel close to a hole: the pull worked out exactly, then the shadow and its thin ring of light */
static uint32_t near_px(const ck_night *n, int32_t x, int32_t y)
{
    int64_t X = (int64_t)x * 16 + 8, Y = (int64_t)y * 16 + 8;
    int32_t ax, ay;
    uint32_t light;
    warp(n, X, Y, &ax, &ay, &light);
    uint32_t c = finish(n, sample(n, (int32_t)X + ax, (int32_t)Y + ay), x, y, light);
    for (unsigned i = 0; i < n->holes; i++) {
        int64_t rs = n->hole[i].rs, dx = X - n->hole[i].x, dy = Y - n->hole[i].y, d2 = dx * dx + dy * dy;
        if (rs <= 0 || d2 >= rs * rs * 9 / 4)
            continue;
        int32_t r = (int32_t)ck_isqrt((uint64_t)d2);
        c = ck_mix(c, 0, ck_cover(r - (int32_t)rs));
        int32_t ring = ck_abs(r - (int32_t)(rs * 104 / 100));
        uint32_t a = ring < 22 ? (uint32_t)(22 - ring) * 6 : 0;
        if (r > rs)
            a += (uint32_t)((rs * 3 / 2 - r) * 46 / (rs / 2 + 1)) * (r < rs * 3 / 2);
        c = ck_add(c, CK_RGB(255, 228, 196), a);
    }
    return c;
}

static int near_hole(const ck_night *n, int32_t x, int32_t y)
{
    int64_t X = (int64_t)x * 16 + 8, Y = (int64_t)y * 16 + 8;
    for (unsigned i = 0; i < n->holes; i++) {
        int64_t reach = ck_max(n->hole[i].e, n->hole[i].rs * 3 / 2) + 8 * 16;
        int64_t dx = X - n->hole[i].x, dy = Y - n->hole[i].y;
        if (dx * dx + dy * dy < reach * reach)
            return 1;
    }
    return 0;
}

/* does the 8 x 8 block at (bx, by) meet the band the ripple bends? */
static int in_ripple(const ck_night *n, int32_t bx, int32_t by)
{
    if (!n->ripple_w)
        return 0;
    int64_t reach = n->ripple_w * 22 / 10 + 1, lo = ck_max(0, n->ripple_r - (int32_t)reach), hi = n->ripple_r + reach;
    int64_t nx = ck_clamp(n->cx, bx, bx + 8) - n->cx, ny = ck_clamp(n->cy, by, by + 8) - n->cy;
    int64_t fx = ck_max(ck_abs(bx - n->cx), ck_abs(bx + 8 - n->cx)), fy = ck_max(ck_abs(by - n->cy), ck_abs(by + 8 - n->cy));
    return nx * nx + ny * ny <= hi * hi && fx * fx + fy * fy >= lo * lo;
}

void ck_night_base(const ck_night *n, ck_canvas *out, const ck_rect *r)
{
    ck_rect d = *r, all = {0, 0, n->w, n->h};
    if (!ck_clip(&d, &all))
        return;
    int fading = n->t < CK_NIGHT_FADE_END_MS;
    for (int32_t y = d.y; y < d.y + d.h; y++) {
        const uint32_t *src = n->sky + (size_t)y * (size_t)n->w;
        uint32_t *dst = out->px + (size_t)y * (size_t)out->w;
        if (fading)
            for (int32_t x = d.x; x < d.x + d.w; x++)
                dst[x] = fade_px(n, src[x], x, y);
        else
            for (int32_t x = d.x; x < d.x + d.w; x++)
                dst[x] = src[x];
    }
    if (!n->lens.w && !n->ripple_w)
        return;
    /* the bent parts, in 8 x 8 blocks on a grid fixed to the screen, so any split of r draws the same */
    for (int32_t by = d.y & ~7; by < d.y + d.h; by += 8)
        for (int32_t bx = d.x & ~7; bx < d.x + d.w; bx += 8) {
            ck_rect b = {bx, by, 8, 8};
            int lens = n->lens.w && ck_meets(b, &n->lens);
            if (!lens && !in_ripple(n, bx, by))
                continue;
            if (!ck_clip(&b, &d))
                continue;
            if (lens && near_hole(n, bx + 4, by + 4)) {
                for (int32_t y = b.y; y < b.y + b.h; y++)
                    for (int32_t x = b.x; x < b.x + b.w; x++)
                        out->px[(size_t)y * (size_t)out->w + (size_t)x] = near_px(n, x, y);
                continue;
            }
            int32_t ax[4], ay[4];
            uint32_t li[4];
            for (int c = 0; c < 4; c++)
                warp(n, ((int64_t)bx + (c & 1) * 8) * 16, ((int64_t)by + (c >> 1) * 8) * 16, &ax[c], &ay[c], &li[c]);
            for (int32_t y = b.y; y < b.y + b.h; y++) {
                int32_t wy = 2 * (y - by) + 1;
                for (int32_t x = b.x; x < b.x + b.w; x++) {
                    int32_t wx = 2 * (x - bx) + 1;
                    int32_t w00 = (16 - wx) * (16 - wy), w10 = wx * (16 - wy), w01 = (16 - wx) * wy, w11 = wx * wy;
                    int32_t sx = (ax[0] * w00 + ax[1] * w10 + ax[2] * w01 + ax[3] * w11) >> 8;
                    int32_t sy = (ay[0] * w00 + ay[1] * w10 + ay[2] * w01 + ay[3] * w11) >> 8;
                    uint32_t l = (uint32_t)((int32_t)li[0] * w00 + (int32_t)li[1] * w10 + (int32_t)li[2] * w01 +
                                            (int32_t)li[3] * w11) >> 8;
                    uint32_t c = sample(n, x * 16 + 8 + sx, y * 16 + 8 + sy);
                    out->px[(size_t)y * (size_t)out->w + (size_t)x] = finish(n, c, x, y, l);
                }
            }
        }
}

/* ---- the dusk: everything on the screen falls into one black hole ---- */

size_t ck_night_picture_bytes(uint32_t w, uint32_t h)
{
    size_t n = 64 + (size_t)w * h * 4;
    for (unsigned i = 1; i < CK_NIGHT_LEVELS; i++)
        n += (size_t)(w >> i) * (size_t)(h >> i) * 4 * 2;
    return n;
}

/* half the width and height of `from` (sw x sh) into to: the mean of each 2 x 2 */
static void halve(const uint32_t *from, uint32_t sw, uint32_t sh, uint32_t *to)
{
    uint32_t dw = sw / 2, dh = sh / 2;
    for (uint32_t y = 0; y < dh; y++) {
        const uint32_t *r0 = from + (size_t)(2 * y) * sw, *r1 = r0 + sw;
        uint32_t *d = to + (size_t)y * dw;
        for (uint32_t x = 0; x < dw; x++) {
            uint32_t a = r0[2 * x], b = r0[2 * x + 1], c = r1[2 * x], e = r1[2 * x + 1];
            uint32_t color = 0;
            for (int s = 0; s <= 16; s += 8) {
                uint32_t light = ck_sky_lin[ck_ch(a, s)] + ck_sky_lin[ck_ch(b, s)] +
                                 ck_sky_lin[ck_ch(c, s)] + ck_sky_lin[ck_ch(e, s)];
                color |= ck_light_encode(light >> 2) << s;
            }
            d[x] = color;
        }
    }
}

void ck_night_picture_take(const ck_night *n, const uint32_t *from, void *mem, ck_night_picture *kept)
{
    uint32_t w = (uint32_t)n->w, h = (uint32_t)n->h, *p = mem;
    size_t count = (size_t)w * h;
    for (size_t i = 0; i < count; i++)
        p[i] = from[i];
    kept->level[0] = p;
    kept->sky[0] = n->sky;
    p += count;
    for (unsigned l = 1; l < CK_NIGHT_LEVELS; l++) {
        uint32_t sw = w >> (l - 1), sh = h >> (l - 1);
        halve(kept->level[l - 1], sw, sh, p);
        kept->level[l] = p;
        p += (size_t)(sw / 2) * (sh / 2);
        halve(kept->sky[l - 1], sw, sh, p);
        kept->sky[l] = p;
        p += (size_t)(sw / 2) * (sh / 2);
    }
}

void ck_night_fall(ck_night *n, const ck_night_dusk *d)
{
    n->falling = 1;
    n->dusk = *d;
    int32_t cx = n->cx * 16 + 8, cy = n->cy * 16 + 8, rs = d->rs;
    n->hole[0] = (__typeof__(n->hole[0])){cx, cy, d->e, rs};
    n->holes = 1;
    n->ripple_w = 0;
    n->lens = (ck_rect){0, 0, n->w, n->h};
    /* The disk and the ring, by distance from the hole, in 1024 steps out to 3.3 shadow radii. The disk is
     * dark inside 1.15 radii, brightest at 1.5 and dies away beyond; its colour runs from warm white at the
     * inner edge through gold to the blue of the sky's glow. The photon ring is a thin bright line at 1.06
     * radii, a pixel and a quarter each way (wider on a big screen). */
    n->disk_far = rs * 33 / 10 + 16;
    int32_t in = rs * 115 / 100, peak = rs * 150 / 100, ring = rs * 106 / 100, hw = 20 + rs / 64;
    for (unsigned i = 0; i < 1024; i++) {
        int32_t r = (int32_t)((int64_t)n->disk_far * (int64_t)i / 1024);
        uint32_t a = 0, c = CK_RGB(255, 222, 180);
        if (r >= in && peak > in) {
            if (r < peak) {
                uint32_t u = (uint32_t)((int64_t)(r - in) * 256 / (peak - in));
                a = ck_smooth((int32_t)u) * 220 >> 8;
                c = ck_mix(CK_RGB(255, 222, 180), CK_RGB(255, 212, 150), u);
            } else {
                uint32_t v = (uint32_t)((int64_t)(r - peak) * 256 / (rs / 2 + 1)); /* half radii past the peak, 256 each */
                a = (ck_exp_neg(v) >> 8) * 220 >> 8;
                c = ck_mix(CK_RGB(255, 212, 150), CK_RGB(150, 170, 255), ck_smooth((int32_t)(v * 256 / 360)));
            }
        }
        int32_t off = ck_abs(r - ring);
        if (off < hw) {
            uint32_t ar = (uint32_t)(hw - off) * 255 / (uint32_t)hw;
            ar = ar * ar >> 8;
            c = a + ar ? ck_mix(c, CK_RGB(255, 244, 228), ar * 256 / (a + ar)) : c;
            a += ar;
        }
        a = a * d->disk >> 8;
        n->disk_a[i] = (uint8_t)(a > 255 ? 255 : a);
        n->disk_c[i] = c;
    }
}

/* a corner of the 8 x 8 grid during the dusk: where the picture is read from, and the distance to the hole */
struct corner {
    int32_t ax, ay;   /* offset to the picture point shown here, 16ths of a pixel */
    int32_t sx, sy;   /* that point itself, 16ths (for telling whether a block shows only the dark beyond the edge) */
    uint32_t crowd;   /* how much brighter the crowded light is drawn, 256 = as it is */
    uint32_t h;       /* log2 of the crowding of the area, Q16 */
    int32_t r;        /* distance from the hole, 16ths of a pixel */
    int32_t inv;      /* 2^28 / r */
    uint32_t vig;     /* how much of the light the edges of the screen keep here, 0..256 */
};

static void corner_at(const ck_night *n, int32_t x, int32_t y, struct corner *c)
{
    const ck_night_dusk *d = &n->dusk;
    int64_t X = (int64_t)x * 16, Y = (int64_t)y * 16, dx = X - n->hole[0].x, dy = Y - n->hole[0].y;
    int64_t d2 = dx * dx + dy * dy;
    if (d2 < 256)
        d2 = 256;
    uint32_t l0 = ck_log2_q16((uint64_t)d2), l1 = ck_log2_q16((uint64_t)(d2 + d->fallen)), h = l1 - l0;
    c->h = h;
    uint64_t crowd = pow2_q16(h) >> 8;
    c->crowd = crowd > d->crowd ? d->crowd : (uint32_t)crowd;
    /* the picture point: the same direction turned by swirl per doubling, at the stretched distance */
    int64_t r = (int64_t)(pow2_q16(l0 >> 1) >> 16); /* the distance, 16ths, without a square root */
    uint32_t ang = (uint32_t)((uint64_t)d->swirl * (h >> 1) >> 16);
    ang = (uint32_t)((int64_t)ang * d->calm / (d->calm + r));
    int64_t stretch = (int64_t)pow2_q16(h >> 1), cs = ck_cos(ang), sn = ck_sin(ang);
    int64_t ux = (dx * cs - dy * sn) >> 14, uy = (dx * sn + dy * cs) >> 14;
    int64_t sx = (ux * stretch >> 16) - dx, sy = (uy * stretch >> 16) - dy;
    lens_at(n, X, Y, &sx, &sy);
    /* far beyond the picture is still far beyond it: held where the block arithmetic stays in 32 bits */
    sx = sx > (1 << 22) ? (1 << 22) : sx < -(1 << 22) ? -(1 << 22) : sx;
    sy = sy > (1 << 22) ? (1 << 22) : sy < -(1 << 22) ? -(1 << 22) : sy;
    c->ax = (int32_t)sx;
    c->ay = (int32_t)sy;
    c->sx = (int32_t)X + c->ax;
    c->sy = (int32_t)Y + c->ay;
    c->r = (int32_t)r;
    c->inv = (int32_t)((1 << 28) / (r > 0 ? r : 1));
    /* the edges go dark first: from `near` out to `far` the light falls off smoothly by up to `shade` */
    c->vig = 256 - (d->shade * ck_smooth(ck_prog((uint32_t)r, (uint32_t)d->near, (uint32_t)d->far)) >> 8);
}

/* the kept picture at (sx, sy) (16ths of a pixel of copy l, w x h): the picture itself inside its edges, the
 * sky mirrored in them beyond (so the stars flow in from past the screen), black further out than that. */
static uint32_t sample_kept(const ck_night_picture *kept, unsigned l, int32_t w, int32_t h, int32_t sx, int32_t sy)
{
    const uint32_t *px = kept->level[l];
    int32_t W = (w - 1) * 16, H = (h - 1) * 16;
    sx -= 8, sy -= 8;
    if (sx < 0 || sy < 0 || sx > W || sy > H) {
        sx = sx < 0 ? -sx : sx > W ? 2 * W - sx : sx;
        sy = sy < 0 ? -sy : sy > H ? 2 * H - sy : sy;
        if (sx < 0 || sy < 0 || sx > W || sy > H)
            return 0;
        px = kept->sky[l];
    }
    int32_t ix = sx >> 4, iy = sy >> 4, ix1 = ck_min(ix + 1, w - 1), iy1 = ck_min(iy + 1, h - 1);
    const uint32_t *r0 = px + (size_t)iy * (size_t)w, *r1 = px + (size_t)iy1 * (size_t)w;
    uint32_t fx = (uint32_t)sx & 15u, fy = (uint32_t)sy & 15u;
    return lerp16(lerp16(r0[ix], r0[ix1], fx), lerp16(r1[ix], r1[ix1], fx), fy);
}

/* c brightened g/256 times (g above 256), the excess warmer than the light itself */
static uint32_t brighten(uint32_t c, uint32_t g)
{
    if (g <= 256 || !c)
        return c;
    uint32_t x = g - 256;
    uint32_t r = ck_light_encode(ck_sky_lin[ck_ch(c, 16)] * (256 + x) >> 8);
    uint32_t gg = ck_light_encode(ck_sky_lin[ck_ch(c, 8)] * (256 + x * 7 / 8) >> 8);
    uint32_t b = ck_light_encode(ck_sky_lin[ck_ch(c, 0)] * (256 + x * 5 / 8) >> 8);
    return r << 16 | gg << 8 | b;
}

/* One 8 x 8 block of the dusk, clipped to d. Share the smooth warp's four
 * corners while resolving the kept picture, shadow and light at native pixels. */
static void fall_block(const ck_night *n, const ck_night_picture *kept, ck_canvas *out, const ck_rect *d, int32_t bx,
                       int32_t by, const struct corner *c00, const struct corner *c10, const struct corner *c01,
                       const struct corner *c11)
{
    ck_rect b = {bx, by, 8, 8};
    if (!ck_clip(&b, d))
        return;
    const ck_night_dusk *k = &n->dusk;
    int32_t rs = k->rs, hx = n->hole[0].x, hy = n->hole[0].y;
    int32_t rmin = ck_min(ck_min(c00->r, c10->r), ck_min(c01->r, c11->r)) - 182; /* the block's diagonal: 8 px * sqrt 2 */
    int32_t rmax = ck_max(ck_max(c00->r, c10->r), ck_max(c01->r, c11->r));
    int32_t W16 = n->w * 32 + 16, H16 = n->h * 32 + 16, X16 = -n->w * 16 - 16, Y16 = -n->h * 16 - 16;
    /* all four corners read from the dark beyond the same edge of the mirrored sky: so does everything between */
    int beyond = (c00->sx < X16 && c10->sx < X16 && c01->sx < X16 && c11->sx < X16) ||
                 (c00->sy < Y16 && c10->sy < Y16 && c01->sy < Y16 && c11->sy < Y16) ||
                 (c00->sx > W16 && c10->sx > W16 && c01->sx > W16 && c11->sx > W16) ||
                 (c00->sy > H16 && c10->sy > H16 && c01->sy > H16 && c11->sy > H16);
    int disk = k->disk && rmin < n->disk_far, edge = rmin < rs + 16;
    /* the light kept here: the closing of the horizon, and the edges of the screen going dark first */
    uint32_t keep = k->keep * ((c00->vig + c10->vig + c01->vig + c11->vig) >> 2) >> 8;
    if ((!keep && !disk) || rmax < rs - 16 || (beyond && !disk)) { /* only the dark: inside the shadow, or past the edge */
        for (int32_t y = b.y; y < b.y + b.h; y++)
            for (int32_t x = b.x; x < b.x + b.w; x++)
                out->px[(size_t)y * (size_t)out->w + (size_t)x] = 0;
        return;
    }
    /* Use smaller copies only when the warp actually minifies the source.
     * Dimming is not minification: faint stars and text still need their detail. */
    uint32_t hm = (c00->h >> 2) + (c10->h >> 2) + (c01->h >> 2) + (c11->h >> 2), lvl = hm >> 17;
    lvl = lvl >= CK_NIGHT_LEVELS ? CK_NIGHT_LEVELS - 1 : lvl;
    int32_t lw = n->w >> lvl, lh = n->h >> lvl;
    int crowded = c00->crowd > 256 || c10->crowd > 256 || c01->crowd > 256 || c11->crowd > 256;
    int64_t cb = ck_cos(k->beam), sb = ck_sin(k->beam);
    uint32_t inv_far = (uint32_t)((1u << 30) / (uint32_t)(n->disk_far > 0 ? n->disk_far : 1));
    /* Resolve every native destination pixel, even far from the hole. The
     * shared corner grid accelerates the warp without upscaling 2 x 2 samples. */
    for (int32_t y = b.y; y < b.y + b.h; y++) {
        int32_t wy = 2 * (y - by) + 1;
        uint32_t *row = out->px + (size_t)y * (size_t)out->w;
        for (int32_t x = b.x; x < b.x + b.w; x++) {
            int32_t wx = 2 * (x - bx) + 1;
            int32_t w00 = (16 - wx) * (16 - wy), w10 = wx * (16 - wy), w01 = (16 - wx) * wy, w11 = wx * wy;
            uint32_t c = 0;
            if (!beyond) {
                int32_t ax = (c00->ax * w00 + c10->ax * w10 + c01->ax * w01 + c11->ax * w11) >> 8;
                int32_t ay = (c00->ay * w00 + c10->ay * w10 + c01->ay * w01 + c11->ay * w11) >> 8;
                c = sample_kept(kept, lvl, lw, lh, (x * 16 + 8 + ax) >> lvl, (y * 16 + 8 + ay) >> lvl);
                if (c && crowded) {
                    uint32_t g = (uint32_t)((int32_t)c00->crowd * w00 + (int32_t)c10->crowd * w10 +
                                            (int32_t)c01->crowd * w01 + (int32_t)c11->crowd * w11) >> 8;
                    c = brighten(c, g);
                }
                if (keep < 256) /* the picture goes before the disk does */
                    c = ck_scale(c, keep);
            }
            if (disk || edge) {
                int32_t r = (c00->r * w00 + c10->r * w10 + c01->r * w01 + c11->r * w11) >> 8;
                if (disk && r < n->disk_far) {
                    uint32_t i = (uint32_t)r * inv_far >> 20, a = n->disk_a[i < 1024 ? i : 1023];
                    if (a) {
                        /* brighter on the side turning toward the eye: by the cosine of the angle from the beam */
                        int64_t inv = ((int64_t)c00->inv * w00 + (int64_t)c10->inv * w10 + (int64_t)c01->inv * w01 +
                                       (int64_t)c11->inv * w11) >> 8;
                        int64_t dot = ((int64_t)(x * 16 + 8 - hx) * cb + (int64_t)(y * 16 + 8 - hy) * sb) >> 14;
                        int64_t cosb = dot * inv >> 14; /* Q14 */
                        a = (uint32_t)((int64_t)a * (16384 + (cosb * 110 >> 8)) >> 14);
                        c = ck_add(c, n->disk_c[i < 1024 ? i : 1023], a > 255 ? 255 : a);
                    }
                }
                if (edge)
                    c = ck_mix(c, 0, ck_cover(r - rs));
            }
            row[x] = c;
        }
    }
}

#define CHUNK 32 /* blocks of a row worked at once (their corners shared); 2.4 KiB of stack for the two rows */

void ck_night_base_from(const ck_night *n, const ck_night_picture *kept, ck_canvas *out, const ck_rect *r)
{
    ck_rect d = *r, all = {0, 0, n->w, n->h};
    if (!ck_clip(&d, &all))
        return;
    if (!n->falling) {
        ck_night_base(n, out, &d);
        return;
    }
    struct corner rows[2][CHUNK + 1];
    int32_t bx0 = d.x & ~7, bx1 = d.x + d.w, by0 = d.y & ~7, by1 = d.y + d.h;
    for (int32_t cx0 = bx0; cx0 < bx1; cx0 += CHUNK * 8) {
        int32_t ncol = ck_min(CHUNK, (bx1 - cx0 + 7) / 8);
        struct corner *top = rows[0], *bottom = rows[1];
        for (int32_t i = 0; i <= ncol; i++)
            corner_at(n, cx0 + 8 * i, by0, &top[i]);
        for (int32_t by = by0; by < by1; by += 8) {
            for (int32_t i = 0; i <= ncol; i++)
                corner_at(n, cx0 + 8 * i, by + 8, &bottom[i]);
            for (int32_t i = 0; i < ncol; i++)
                fall_block(n, kept, out, &d, cx0 + 8 * i, by, &top[i], &top[i + 1], &bottom[i], &bottom[i + 1]);
            struct corner *t = top;
            top = bottom, bottom = t;
        }
    }
}

/* ---- the lights on top ---- */

ck_rect ck_night_star_box(const ck_night *n, unsigned i)
{
    const ck_night_star *s = &n->tw[i];
    return (ck_rect){(s->x - s->r) >> 4, (s->y - s->r) >> 4, (2 * s->r >> 4) + 2, (2 * s->r >> 4) + 2};
}

void ck_night_lights(const ck_night *n, ck_canvas *out, const ck_rect *r, uint32_t shimmer, uint32_t voice)
{
    for (unsigned i = 0; i < n->ntw; i++) {
        const ck_night_star *s = &n->tw[i];
        ck_rect b = ck_night_star_box(n, i);
        if (!ck_meets(b, r) || (n->lens.w && ck_meets(b, &n->lens)))
            continue;
        uint32_t tw = (uint32_t)(16384 + ck_sin((n->t + s->phase) % s->period * 65536u / s->period)) >> 7;
        uint32_t a = (tw * tw >> 8) * 120 >> 8;
        a = a * ck_night_faded(n, s->x >> 4, s->y >> 4, 255) >> 8;
        ck_glow(out, r, s->x, s->y, s->r, s->color, a);
    }
    if (!shimmer && !voice)
        return;
    for (unsigned i = 0; i < CK_NIGHT_GALAXIES; i++) {
        const ck_night_galaxy *g = &n->gal[i];
        ck_rect b = g->box;
        if (!g->box.w || !ck_clip(&b, r))
            continue;
        uint32_t sweep = n->t % g->period * 256u / g->period;
        for (int32_t y = b.y; y < b.y + b.h; y++) {
            const uint8_t *lp = g->light + (size_t)(y - g->box.y) * (size_t)g->box.w - (size_t)g->box.x;
            const uint8_t *pp = g->phase + (size_t)(y - g->box.y) * (size_t)g->box.w - (size_t)g->box.x;
            uint32_t *row = out->px + (size_t)y * (size_t)out->w;
            for (int32_t x = b.x; x < b.x + b.w; x++) {
                uint32_t l = lp[x];
                if (!l)
                    continue;
                uint32_t a = (l * band_t[(pp[x] - sweep) & 255u] >> 8) * shimmer >> 8;
                a += (l * l >> 8) * voice >> 9;
                if (a)
                    row[x] = ck_add(row[x], g->tint, a);
            }
        }
    }
}
/* ---- rebuilding the sky for a new view ---- */

/* Resets the sky background and redraws it with the new view v. The sky's
 * glow (dark sky background) is written first, then the Milky Way and the
 * catalogue stars on top of it. Galaxy bounds and maps are updated too.
 * scratch: at least n->w * n->h * 4 bytes of temporary workspace. */
void ck_night_view(ck_night *n, const ck_sky_view *v, void *scratch)
{
    n->view = *v;
    int32_t qw = (int32_t)(n->w / 4 + 2), qh = (int32_t)(n->h / 4 + 2);
    /* clear the sky to the dark-sky glow (the same glow set in the original
     * sky grid; bilinear_u8 of the empty dark grid gives the glow level) */
    ck_canvas cv = {n->sky, n->w, n->h};
    ck_sky_milkyway_q(scratch, qw, qh, n->w, n->cx, n->cy, &n->view);
    scale_up(n, scratch, qw);
    /* redraw stars over the new Milky Way */
    ck_sky_draw_stars(&cv, n->k, n->cx, n->cy, &n->view, n->tw, CK_NIGHT_TWINKLES, &n->ntw);
    /* recompute galaxy boxes (some may enter or leave the view) */
    uint8_t *p = (uint8_t *)n->sky + (size_t)n->w * n->h * 4;
    p += (size_t)(n->w / 4 + 1) * (n->h / 4 + 1);
    for (unsigned i = 0; i < CK_NIGHT_GALAXIES; i++) {
        ck_night_galaxy *g = &n->gal[i];
        g->box = ck_sky_galaxy_box((uint32_t)n->w, (uint32_t)n->h, n->cx, n->cy, &n->view, i);
        ck_sky_galaxy_maps(g, i, (uint32_t)n->w, n->cx, n->cy, &n->view);
    }
}
