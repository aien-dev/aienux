/* night.c -- the night sky and the merging black holes; see night.h.
 *
 * The sky is worked out at a quarter of the screen's resolution for the
 * soft parts (the Milky Way, dust and nebulae: smooth anyway), scaled up
 * with a 4 x 4 ordered dither so the faint gradients show no bands, and the
 * stars and galaxies are then drawn at full resolution on top.
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

#define S(v) ((int32_t)(((int64_t)(v) * (int64_t)n->k + 128) / 256))
#define Q30 (1ull << 30)

/* ---- the galaxies ---- */

enum { SPIRAL, ROUND, EDGE };
static const struct {
    uint16_t fx, fy; /* thousandths of the width and height */
    uint16_t radius; /* design px */
    int16_t tilt, turn; /* degrees: how far it leans from face-on, and the angle of its long axis */
    uint8_t kind, bright;
    uint32_t tint;
    uint16_t period;
} gdef[CK_NIGHT_GALAXIES] = {
    {150, 205, 112, 38, 30, SPIRAL, 160, CK_RGB(190, 205, 255), 3400},
    {805, 135, 76, 62, -25, SPIRAL, 140, CK_RGB(200, 212, 255), 2900},
    {265, 705, 42, 45, 60, ROUND, 130, CK_RGB(255, 228, 196), 2600},
    {870, 750, 88, 84, 15, EDGE, 150, CK_RGB(210, 216, 255), 3800},
    {60, 545, 40, 22, 0, SPIRAL, 120, CK_RGB(190, 205, 255), 3100},
    {600, 95, 28, 52, 70, SPIRAL, 110, CK_RGB(200, 212, 255), 2700},
};

static uint8_t band_t[256]; /* the sweeping band: ((1 + cos) / 2)^6, 0..255 */

static int32_t half_box(uint32_t k, unsigned i) { return (int32_t)((gdef[i].radius * 13 / 10 * k + 128) / 256) + 1; }

static void galaxy_box(uint32_t w, uint32_t h, uint32_t k, unsigned i, ck_rect *b)
{
    int32_t hb = half_box(k, i), gx = (int32_t)(w * gdef[i].fx / 1000), gy = (int32_t)(h * gdef[i].fy / 1000);
    ck_rect all = {0, 0, (int32_t)w, (int32_t)h};
    *b = (ck_rect){gx - hb, gy - hb, 2 * hb + 1, 2 * hb + 1};
    if (!ck_clip(b, &all))
        *b = (ck_rect){0, 0, 0, 0};
}

size_t ck_night_mem_bytes(uint32_t w, uint32_t h, uint32_t k)
{
    size_t n = (size_t)w * h * 4 + (size_t)(w / 4 + 1) * (h / 4 + 1) + 64;
    for (unsigned i = 0; i < CK_NIGHT_GALAXIES; i++) {
        ck_rect b;
        galaxy_box(w, h, k, i, &b);
        n += (size_t)b.w * (size_t)b.h * 2;
    }
    return n;
}

/* ---- the Milky Way's geometry, shared by the clouds and the stars ---- */

struct band {
    int64_t x0, y0, dx, dy, len, half;
};

static struct band band_of(const ck_night *n)
{
    struct band b = {-(int64_t)n->w / 20, (int64_t)n->h * 90 / 100, (int64_t)n->w * 110 / 100,
                     -(int64_t)n->h * 92 / 100, 0, (int64_t)n->h * 16 / 100};
    b.len = (int64_t)ck_isqrt((uint64_t)(b.dx * b.dx + b.dy * b.dy));
    return b;
}

/* signed distance across the band and distance along it, px */
static void band_at(const struct band *b, int64_t x, int64_t y, int64_t *across, int64_t *along)
{
    *across = ((x - b->x0) * b->dy - (y - b->y0) * b->dx) / b->len;
    *along = ((x - b->x0) * b->dx + (y - b->y0) * b->dy) / b->len;
}

static uint32_t gauss(int64_t v, int64_t width) /* exp(-(v/width)^2), Q16 */
{
    int64_t q = v * 256 / (width ? width : 1);
    if (q > 4095 || q < -4095)
        return 0;
    return ck_exp_neg((uint32_t)(q * q >> 8));
}

/* ---- the soft layer: a quarter of the resolution, 16ths of a level ---- */

static void clouds(ck_night *n, uint16_t *q, int32_t qw, int32_t qh)
{
    struct band b = band_of(n);
    int64_t H = n->h;
    for (int32_t qy = 0; qy < qh; qy++)
        for (int32_t qx = 0; qx < qw; qx++) {
            int64_t x = (int64_t)qx * 4, y = (int64_t)qy * 4, across, along;
            band_at(&b, x, y, &across, &along);
            int32_t u3 = (int32_t)(x * 768 / H), v3 = (int32_t)(y * 768 / H);
            uint64_t core = gauss(across, b.half), wide = gauss(across, b.half * 2);
            uint64_t n1 = ck_fbm(u3, v3, 101, 5);
            uint64_t n2 = ck_fbm((int32_t)(x * 1792 / H), (int32_t)(y * 1792 / H), 202, 4);
            /* dark dust: a lane just off the middle of the band, broken up by noise */
            uint64_t lane = gauss(across - H * 2 / 100, H * 5 / 100);
            uint64_t dust = lane * ck_smooth((int32_t)(((int64_t)n2 - 22000) * 256 / 26000)) >> 8;
            uint64_t bright = gauss(along - b.len * 40 / 100, b.len * 38 / 100);
            uint64_t mw = core * (32768 + n1 / 2) >> 16;
            mw = mw * (65536 - dust * 52428 / 65536) >> 16;
            mw = mw * (29491 + bright * 36045 / 65536) >> 16;
            uint64_t halo = wide * (39321 + n1 * 26214 / 65536) >> 16;
            /* nebulae: faint violet, blue and rose clouds where the noise runs high */
            uint32_t n4 = ck_fbm((int32_t)(x * 512 / H), (int32_t)(y * 512 / H), 404, 4);
            uint32_t n5 = ck_fbm((int32_t)(x * 666 / H), (int32_t)(y * 666 / H), 505, 4);
            uint32_t n6 = ck_fbm((int32_t)(x * 1024 / H), (int32_t)(y * 1024 / H), 606, 4);
            uint64_t vi = ck_smooth((int32_t)(((int64_t)n4 - 36000) * 256 / 29535));
            uint64_t bl = ck_smooth((int32_t)(((int64_t)n5 - 35000) * 256 / 30535));
            uint64_t ro = (uint64_t)ck_smooth((int32_t)(((int64_t)n6 - 33000) * 256 / 32535)) * wide >> 16;
            vi = vi * vi >> 8, bl = bl * bl >> 8;
            /* level x 16 per channel */
            int64_t top = 256 - y * 256 / H;
            int64_t r = 26 + top / 32 + (int64_t)(mw * 44 * 16 >> 16) + (int64_t)(halo * 6 * 16 >> 16) +
                        (int64_t)(vi * 22 * 16 >> 8) + (int64_t)(bl * 5 * 16 >> 8) + (int64_t)(ro * 22 * 16 >> 8);
            int64_t g = 34 + top / 32 + (int64_t)(mw * 40 * 16 >> 16) + (int64_t)(halo * 8 * 16 >> 16) +
                        (int64_t)(vi * 8 * 16 >> 8) + (int64_t)(bl * 12 * 16 >> 8) + (int64_t)(ro * 7 * 16 >> 8);
            int64_t bb = 92 + top / 16 + (int64_t)(mw * 46 * 16 >> 16) + (int64_t)(halo * 16 * 16 >> 16) +
                         (int64_t)(vi * 28 * 16 >> 8) + (int64_t)(bl * 30 * 16 >> 8) + (int64_t)(ro * 15 * 16 >> 8);
            uint16_t *c = q + ((size_t)qy * (size_t)qw + (size_t)qx) * 3;
            c[0] = (uint16_t)ck_clamp((int32_t)r, 0, 4080);
            c[1] = (uint16_t)ck_clamp((int32_t)g, 0, 4080);
            c[2] = (uint16_t)ck_clamp((int32_t)bb, 0, 4080);
        }
}

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
                uint32_t v = ((a * (uint32_t)(4 - fy) + b * (uint32_t)fy) / 16 + d) >> 4;
                c = c << 8 | (v > 255 ? 255u : v);
            }
            row[x] = c;
        }
    }
}

/* ---- stars ---- */

static uint32_t star_color(uint32_t h)
{
    uint32_t p = h % 100;
    return p < 10 ? CK_RGB(170, 192, 255) : p < 55 ? CK_RGB(218, 228, 255) : p < 85 ? CK_RGB(255, 250, 242)
         : p < 97 ? CK_RGB(255, 234, 196) : CK_RGB(255, 204, 160);
}

static void stars(ck_night *n)
{
    ck_canvas cv = {n->sky, n->w, n->h};
    ck_rect all = {0, 0, n->w, n->h};
    struct band b = band_of(n);
    uint32_t k = n->k, area = (uint32_t)n->w * (uint32_t)n->h;
    /* field stars: many faint, few bright; more of them along the band */
    for (uint32_t i = 0; i < area / 800; i++) {
        int32_t x = (int32_t)(ck_hash((int32_t)i, 1, 77) % (uint32_t)(n->w * 16));
        int32_t y = (int32_t)(ck_hash((int32_t)i, 2, 77) % (uint32_t)(n->h * 16));
        int64_t across, along;
        band_at(&b, x >> 4, y >> 4, &across, &along);
        if ((ck_hash((int32_t)i, 3, 77) & 65535u) > 36000u + gauss(across, b.half) * 29535u / 65536u)
            continue;
        uint64_t u = ck_hash((int32_t)i, 4, 77) & 255u;
        uint32_t lv = 14u + (uint32_t)(241u * (u * u * u * u) >> 32);
        int32_t r = (int32_t)((20u + 16u * lv / 255u) * k / 256u);
        ck_glow(&cv, &all, x, y, r, star_color(ck_hash((int32_t)i, 5, 77)), lv);
    }
    /* star dust in the band: faint points where it is densest */
    for (uint32_t i = 0; i < area / 170; i++) {
        int32_t x = (int32_t)(ck_hash((int32_t)i, 6, 78) % (uint32_t)(n->w * 16));
        int32_t y = (int32_t)(ck_hash((int32_t)i, 7, 78) % (uint32_t)(n->h * 16));
        int64_t across, along;
        band_at(&b, x >> 4, y >> 4, &across, &along);
        uint64_t c = gauss(across, b.half * 8 / 10);
        if ((ck_hash((int32_t)i, 8, 78) & 65535u) > (c * c >> 16))
            continue;
        ck_glow(&cv, &all, x, y, (int32_t)(20u * k / 256u), CK_RGB(226, 230, 255),
                10u + ck_hash((int32_t)i, 9, 78) % 44u);
    }
    /* the bright stars: a soft halo each; these twinkle */
    uint32_t nb = area / 50000u;
    nb = nb < 24 ? 24 : nb > CK_NIGHT_TWINKLES ? CK_NIGHT_TWINKLES : nb;
    for (uint32_t i = 0; i < nb; i++) {
        ck_night_star *s = &n->tw[n->ntw++];
        s->x = (int32_t)(ck_hash((int32_t)i, 1, 91) % (uint32_t)(n->w * 16));
        s->y = (int32_t)(ck_hash((int32_t)i, 2, 91) % (uint32_t)(n->h * 16));
        s->color = star_color(ck_hash((int32_t)i, 3, 91) | 1u);
        uint32_t lv = 190u + ck_hash((int32_t)i, 4, 91) % 66u;
        int32_t r = (int32_t)(34u * k / 256u);
        ck_glow(&cv, &all, s->x, s->y, r, s->color, lv);
        ck_glow(&cv, &all, s->x, s->y, r * 5, s->color, 22);
        s->r = r * 2;
        s->period = (uint16_t)(2600u + ck_hash((int32_t)i, 5, 91) % 4200u);
        s->phase = (uint16_t)(ck_hash((int32_t)i, 6, 91) % s->period);
    }
}

/* ---- galaxies: light added to the sky, and the maps the sweep follows ---- */

static void galaxy(ck_night *n, unsigned i, uint8_t **pool)
{
    ck_night_galaxy *g = &n->gal[i];
    galaxy_box((uint32_t)n->w, (uint32_t)n->h, n->k, i, &g->box);
    g->light = *pool;
    g->phase = *pool + (size_t)g->box.w * (size_t)g->box.h;
    *pool += (size_t)g->box.w * (size_t)g->box.h * 2;
    g->tint = gdef[i].tint;
    g->period = gdef[i].period;
    int64_t gx = (int64_t)n->w * gdef[i].fx / 1000 * 16 + 8, gy = (int64_t)n->h * gdef[i].fy / 1000 * 16 + 8;
    int64_t rg = (int64_t)S(gdef[i].radius) * 16;
    uint32_t turn = (uint32_t)((int32_t)gdef[i].turn * 65536 / 360);
    int64_t ct = ck_cos(turn), st = ck_sin(turn), ci = ck_cos((uint32_t)(gdef[i].tilt * 65536 / 360));
    if (ci < 600)
        ci = 600;
    uint32_t kind = gdef[i].kind, bright = gdef[i].bright;
    for (int32_t y = g->box.y; y < g->box.y + g->box.h; y++)
        for (int32_t x = g->box.x; x < g->box.x + g->box.w; x++) {
            size_t at = (size_t)(y - g->box.y) * (size_t)g->box.w + (size_t)(x - g->box.x);
            int64_t dx = (int64_t)x * 16 + 8 - gx, dy = (int64_t)y * 16 + 8 - gy;
            int64_t u = (dx * ct + dy * st) >> 14, v = (dy * ct - dx * st) >> 14;
            int64_t vd = v * 16384 / ci; /* back to face-on */
            int64_t rq = (int64_t)ck_isqrt((uint64_t)(u * u + vd * vd)) * 256 / rg; /* radius, 256 = the galaxy's */
            g->light[at] = 0;
            g->phase[at] = 0;
            if (rq > 330)
                continue;
            uint32_t th = ck_atan2((int32_t)vd, (int32_t)u);
            uint32_t nz = ck_fbm((int32_t)(u * 1536 / rg), (int32_t)(vd * 1536 / rg), 700 + i, 3);
            uint32_t fine = ck_fbm((int32_t)(u * 5120 / rg), (int32_t)(vd * 5120 / rg), 750 + i, 2);
            /* the bulge is rounder than the disc: flattened only half as much */
            int64_t vb = v * 32768 / (16384 + ci), rb = (int64_t)ck_isqrt((uint64_t)(u * u + vb * vb)) * 256 / rg;
            int64_t lb, ld, knot = 0; /* bulge and disc light, Q16 of full */
            uint32_t ph;
            if (kind == ROUND) {
                lb = (int64_t)gauss(rb, 30) * 8 / 10;
                ld = (int64_t)ck_exp_neg((uint32_t)(rb * 1075 / 256)) * 6 / 10;
                ph = (uint32_t)(rb * 512);
            } else {
                int64_t lr = (int64_t)ck_log2_q16((uint64_t)(rq < 4 ? 4 : rq)) - 8 * 65536; /* log2(r) */
                ph = (uint32_t)(2 * th + (uint32_t)(lr * 30000 >> 16)); /* two arms, wound about a turn and a half */
                int64_t arm = (16384 + ck_cos(ph)) >> 1;
                arm = arm * arm >> 14; /* Q14 */
                /* the arms begin outside the core; inside it the light is even */
                int64_t from = ck_smooth((int32_t)(rq - 34) * 256 / 90);
                arm = 16384 - ((16384 - arm) * from >> 8);
                int64_t clump = 32768 + (int64_t)nz * 22000 / 65536 + (int64_t)fine * 10768 / 65536;
                lb = (int64_t)gauss(rb, 58) * 9 / 10;
                ld = (int64_t)ck_exp_neg((uint32_t)(rq * 12 / 5)) * (8192 + (arm * 57344 >> 14) * clump / 65536) >> 16;
                if (arm > 11468 && nz > 44000 && rq > 50)
                    knot = ld * (int64_t)(nz - 44000) / 21535;
                if (kind == EDGE) { /* the dust lane along the middle of a disc seen edge-on */
                    int64_t lane = gauss(v, rg * 5 / 100);
                    if (rq < 240)
                        ld = ld * (65536 - lane * 56000 / 65536) >> 16, lb = lb * (65536 - lane * 40000 / 65536) >> 16;
                    ph = (uint32_t)(u * 32768 / rg);
                }
            }
            /* a soft edge at 1.3 radii */
            int64_t edge = 65536 - (int64_t)ck_smooth((int32_t)(rq - 250) * 256 / 80) * 256;
            lb = lb * edge >> 16, ld = ld * edge >> 16, knot = knot * edge >> 16;
            uint32_t ab = (uint32_t)(lb * bright >> 16), ad = (uint32_t)(ld * bright >> 16),
                     ak = (uint32_t)(knot * bright >> 16);
            uint32_t *p = n->sky + (size_t)y * (size_t)n->w + x;
            *p = ck_add(*p, CK_RGB(255, 222, 178), ab);
            *p = ck_add(*p, CK_RGB(150, 170, 255), ad);
            *p = ck_add(*p, CK_RGB(255, 140, 190), ak / 2);
            uint32_t l = ab + ad;
            g->light[at] = (uint8_t)(l > 255 ? 255 : l);
            g->phase[at] = (uint8_t)(ph >> 8);
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
    if (!mem || !scratch || w < 64 || h < 64 || len < ck_night_mem_bytes(w, h, k))
        return -1;
    (void)ck_sin(0); /* the tables, before anything else could ask for them */
    for (unsigned i = 0; i < 256; i++) {
        uint64_t c = (uint64_t)(16384 + ck_cos(i * 256u)) >> 1; /* Q14 */
        uint64_t c2 = c * c >> 14, c6 = (c2 * c2 >> 14) * c2 >> 14;
        band_t[i] = (uint8_t)(c6 * 255 >> 14);
    }
    *n = (ck_night){0};
    n->w = (int32_t)w, n->h = (int32_t)h, n->k = k, n->cx = cx, n->cy = cy;
    uint8_t *p = mem;
    n->sky = (uint32_t *)p;
    p += (size_t)w * h * 4;
    n->appear = p;
    n->aw = (int32_t)(w / 4 + 1);
    p += (size_t)(w / 4 + 1) * (h / 4 + 1);
    int32_t qw = (int32_t)(w / 4 + 2), qh = (int32_t)(h / 4 + 2);
    clouds(n, scratch, qw, qh);
    scale_up(n, scratch, qw);
    stars(n);
    for (unsigned i = 0; i < CK_NIGHT_GALAXIES; i++)
        galaxy(n, i, &p);
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
    return ((((a & 0xff00ffu) * (16 - f) + (b & 0xff00ffu) * f) >> 4) & 0xff00ffu) |
           ((((a & 0x00ff00u) * (16 - f) + (b & 0x00ff00u) * f) >> 4) & 0x00ff00u);
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

/* where (X, Y) (16ths of a pixel) looks in the sky: offset (ax, ay), and the ripple's own faint light */
static void warp(const ck_night *n, int64_t X, int64_t Y, int32_t *ax, int32_t *ay, uint32_t *light)
{
    int64_t sx = 0, sy = 0;
    *light = 0;
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
        sx -= dx * f >> 16;
        sy -= dy * f >> 16;
    }
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
