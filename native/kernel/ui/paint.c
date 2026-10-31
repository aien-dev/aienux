/* paint.c -- the drawing kit; see paint.h. */
#include "paint.h"

/* ---- tables, filled on first use ---- */

static int16_t sin_t[4097]; /* a full turn in 4096 steps, Q14; one extra for interpolation */
static uint32_t exp_t[4096]; /* exp(-k/256), Q16 */
static uint32_t log_t[257];  /* log2(1 + k/256), Q16 */
static int ready;

static void setup(void)
{
    /* sine: a rotation by a 4096th of a turn, applied over and over (Q30) */
    const int64_t C = 1073740561, S = 1647099;
    int64_t c = 1ll << 30, s = 0;
    for (unsigned k = 0; k <= 1024; k++) {
        int16_t v = k == 1024 ? 16384 : (int16_t)((s + (1 << 15)) >> 16);
        sin_t[k] = v;
        sin_t[2048 - k] = v;
        sin_t[(2048 + k) & 4095] = (int16_t)-v;
        sin_t[(4096 - k) & 4095] = (int16_t)-v;
        int64_t nc = (c * C - s * S) >> 30, ns = (s * C + c * S) >> 30;
        c = nc, s = ns;
    }
    sin_t[4096] = sin_t[0];
    /* exp(-k/256): one factor of exp(-1/256) (Q30) per step */
    uint64_t e = 1ull << 30;
    for (unsigned k = 0; k < 4096; k++) {
        exp_t[k] = (uint32_t)((e + (1u << 13)) >> 14);
        e = e * 1069555701ull >> 30;
    }
    /* log2(1 + k/256): square the value 16 times, taking a bit each time it passes 2 */
    for (unsigned k = 0; k <= 256; k++) {
        uint64_t y = (256u + k) << 8; /* Q16, in [1, 2] */
        uint32_t r = 0;
        if (k == 256) {
            log_t[k] = 65536;
            continue;
        }
        for (int b = 15; b >= 0; b--) {
            y = y * y >> 16;
            if (y >= 2u << 16) {
                y >>= 1;
                r |= 1u << b;
            }
        }
        log_t[k] = r;
    }
    ready = 1;
}

int32_t ck_sin(uint32_t a)
{
    if (!ready)
        setup();
    a &= 65535u;
    uint32_t i = a >> 4, f = a & 15u;
    return sin_t[i] + (((int32_t)sin_t[i + 1] - sin_t[i]) * (int32_t)f >> 4);
}

int32_t ck_cos(uint32_t a) { return ck_sin(a + 16384u); }

uint32_t ck_atan2(int32_t y, int32_t x)
{
    static const uint16_t step[14] = {8192, 4836, 2555, 1297, 651, 326, 163, 81, 41, 20, 10, 5, 3, 1};
    int64_t X = x, Y = y;
    uint32_t ang = 0;
    if (X == 0 && Y == 0)
        return 0;
    if (X < 0) { /* turn half a turn into the right half-plane */
        X = -X, Y = -Y;
        ang = 32768;
    }
    int64_t m = X > (Y < 0 ? -Y : Y) ? X : (Y < 0 ? -Y : Y);
    while (m < (1 << 20))
        X <<= 1, Y <<= 1, m <<= 1;
    while (m >= (1 << 22))
        X >>= 1, Y >>= 1, m >>= 1;
    int32_t z = 0;
    for (int i = 0; i < 14; i++) {
        int64_t nx;
        if (Y > 0) {
            nx = X + (Y >> i);
            Y -= X >> i;
            z += step[i];
        } else {
            nx = X - (Y >> i);
            Y += X >> i;
            z -= step[i];
        }
        X = nx;
    }
    return (ang + (uint32_t)z) & 65535u;
}

uint32_t ck_exp_neg(uint32_t x)
{
    if (!ready)
        setup();
    return x < 4096 ? exp_t[x] : 0;
}

uint32_t ck_log2_q16(uint64_t v)
{
    if (!ready)
        setup();
    if (!v)
        return 0;
    int n = 63 - __builtin_clzll(v);
    uint64_t m = n >= 16 ? v >> (n - 16) : v << (16 - n); /* Q16 in [1, 2) */
    uint32_t k = (uint32_t)(m >> 8 & 255u), f = (uint32_t)(m & 255u);
    return (uint32_t)n * 65536u + log_t[k] + ((log_t[k + 1] - log_t[k]) * f >> 8);
}

uint64_t ck_isqrt(uint64_t v) { return ck_af_isqrt(v); }

uint32_t ck_hash(int32_t x, int32_t y, uint32_t seed)
{
    uint32_t h = (uint32_t)x * 0x8da6b343u ^ (uint32_t)y * 0xd8163841u ^ seed * 0xcb1ab31fu;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    h *= 0x27d4eb2du;
    h ^= h >> 16;
    return h;
}

uint32_t ck_noise(int32_t x, int32_t y, uint32_t seed)
{
    int32_t ix = x >> 8, iy = y >> 8;
    uint32_t fx = ck_smooth(x & 255), fy = ck_smooth(y & 255);
    uint64_t v00 = ck_hash(ix, iy, seed) >> 16, v10 = ck_hash(ix + 1, iy, seed) >> 16;
    uint64_t v01 = ck_hash(ix, iy + 1, seed) >> 16, v11 = ck_hash(ix + 1, iy + 1, seed) >> 16;
    uint64_t a = v00 * (256 - fx) + v10 * fx, b = v01 * (256 - fx) + v11 * fx;
    return (uint32_t)((a * (256 - fy) + b * fy) >> 16);
}

uint32_t ck_fbm(int32_t x, int32_t y, uint32_t seed, int octaves)
{
    uint64_t sum = 0, total = 0;
    for (int o = 0; o < octaves; o++) {
        uint32_t w = 1u << (octaves - 1 - o);
        sum += (uint64_t)ck_noise(x << o, y << o, seed + (uint32_t)o * 7919u) * w;
        total += w;
    }
    return (uint32_t)(sum / total);
}

/* ---- shapes ---- */

/* d becomes the part of d inside both the clip and the canvas */
static int fit(const ck_canvas *cv, const ck_rect *clip, ck_rect *d)
{
    ck_rect all = {0, 0, cv->w, cv->h};
    return ck_clip(d, clip) && ck_clip(d, &all);
}

void ck_glow(ck_canvas *cv, const ck_rect *clip, int32_t cx, int32_t cy, int32_t r, uint32_t color, uint32_t a)
{
    if (r <= 0 || !a)
        return;
    ck_rect d = {(cx - r) >> 4, (cy - r) >> 4, 0, 0};
    d.w = ((cx + r) >> 4) - d.x + 1;
    d.h = ((cy + r) >> 4) - d.y + 1;
    if (!fit(cv, clip, &d))
        return;
    int64_t r2 = (int64_t)r * r;
    uint64_t inv = (1ull << 40) / (uint64_t)r2;
    for (int32_t y = d.y; y < d.y + d.h; y++) {
        int64_t dy = (int64_t)y * 16 + 8 - cy, dy2 = dy * dy;
        if (dy2 >= r2)
            continue;
        int32_t half = (int32_t)ck_isqrt((uint64_t)(r2 - dy2));
        int32_t x0 = ck_max(d.x, (cx - half) >> 4), x1 = ck_min(d.x + d.w - 1, (cx + half) >> 4);
        uint32_t *row = cv->px + (size_t)y * (size_t)cv->w;
        for (int32_t x = x0; x <= x1; x++) {
            int64_t dx = (int64_t)x * 16 + 8 - cx, d2 = dx * dx + dy2;
            if (d2 >= r2)
                continue;
            uint32_t f = 256u - (uint32_t)((uint64_t)d2 * inv >> 32);
            uint32_t w = (f * f >> 8) * f >> 8; /* 0..256 */
            row[x] = ck_add(row[x], color, w * a >> 8);
        }
    }
}

static void plot(ck_canvas *cv, const ck_rect *clip, int32_t x, int32_t y, uint32_t color, uint32_t a)
{
    if (!a || x < clip->x || y < clip->y || x >= clip->x + clip->w || y >= clip->y + clip->h || x < 0 || y < 0 ||
        x >= cv->w || y >= cv->h)
        return;
    uint32_t *p = cv->px + (size_t)y * (size_t)cv->w + x;
    *p = ck_add(*p, color, a);
}

void ck_thread(ck_canvas *cv, const ck_rect *clip, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t color,
               uint32_t a0, uint32_t a1)
{
    ck_rect box = {(ck_min(x0, x1) >> 4) - 1, (ck_min(y0, y1) >> 4) - 1, 0, 0};
    box.w = (ck_max(x0, x1) >> 4) - box.x + 3;
    box.h = (ck_max(y0, y1) >> 4) - box.y + 3;
    if (!ck_meets(box, clip) || (!a0 && !a1))
        return;
    int steep = ck_abs(y1 - y0) > ck_abs(x1 - x0);
    if (steep) {
        int32_t t = x0;
        x0 = y0, y0 = t;
        t = x1;
        x1 = y1, y1 = t;
    }
    if (x0 > x1) {
        int32_t t = x0;
        x0 = x1, x1 = t;
        t = y0;
        y0 = y1, y1 = t;
        uint32_t u = a0;
        a0 = a1, a1 = u;
    }
    int32_t len = x1 - x0;
    if (len < 1)
        len = 1;
    int64_t grad = (int64_t)(y1 - y0) * 65536 / len;
    /* a slanted thread covers more ground per column: weigh it by its length per column */
    uint32_t slant = (uint32_t)(ck_isqrt((uint64_t)(65536ll * 65536ll + grad * grad)) >> 8); /* 256 = flat */
    int32_t first = (x0 + 7) >> 4, last = (x1 - 8) >> 4;
    for (int32_t X = first; X <= last; X++) {
        int32_t along = X * 16 + 8 - x0;
        int32_t yc = y0 + (int32_t)(along * grad >> 16) - 8;
        int32_t Y = yc >> 4, f = yc & 15;
        uint32_t a = (uint32_t)((int32_t)a0 + ((int32_t)a1 - (int32_t)a0) * along / len) * slant >> 8;
        if (steep) {
            plot(cv, clip, Y, X, color, a * (uint32_t)(16 - f) >> 4);
            plot(cv, clip, Y + 1, X, color, a * (uint32_t)f >> 4);
        } else {
            plot(cv, clip, X, Y, color, a * (uint32_t)(16 - f) >> 4);
            plot(cv, clip, X, Y + 1, color, a * (uint32_t)f >> 4);
        }
    }
}

int32_t ck_sd_rrect(int32_t x, int32_t y, const ck_rect *r, int32_t rad)
{
    int64_t px = (int64_t)x * 16 + 8, py = (int64_t)y * 16 + 8;
    int64_t cx = (int64_t)r->x * 16 + (int64_t)r->w * 8, cy = (int64_t)r->y * 16 + (int64_t)r->h * 8;
    int64_t qx = (px > cx ? px - cx : cx - px) - ((int64_t)r->w * 8 - rad * 16);
    int64_t qy = (py > cy ? py - cy : cy - py) - ((int64_t)r->h * 8 - rad * 16);
    int64_t out = qx > 0 && qy > 0 ? (int64_t)ck_isqrt((uint64_t)(qx * qx + qy * qy)) : qx > qy ? qx : qy;
    return (int32_t)(out - rad * 16);
}

uint64_t ck_letter(ck_canvas *cv, const ck_rect *clip, ck_af_face *f, uint64_t pen, int32_t y, char c,
                   uint32_t color, uint32_t a, int add)
{
    const ck_af_glyph *g = a ? ck_af_glyph_get(f, c) : 0;
    if (g && g->alpha) {
        int32_t gx = (int32_t)((pen + 128) / 256) + g->ox, gy = y + g->oy;
        ck_rect d = {gx, gy, g->w, g->h};
        if (fit(cv, clip, &d))
            for (int32_t yy = d.y; yy < d.y + d.h; yy++) {
                uint32_t *row = cv->px + (size_t)yy * (size_t)cv->w;
                const uint8_t *m = g->alpha + (size_t)(yy - gy) * g->w;
                for (int32_t xx = d.x; xx < d.x + d.w; xx++) {
                    uint32_t cov = m[xx - gx];
                    if (cov) {
                        uint32_t w = (cov + (cov >> 7)) * a >> 8;
                        row[xx] = add ? ck_add(row[xx], color, w) : ck_mix(row[xx], color, w);
                    }
                }
            }
    }
    return pen + ck_af_advance(f, c);
}
