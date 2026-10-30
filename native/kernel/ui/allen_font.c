/* allen_font.c -- Allen's stroke typeface; see allen_font.h. */
#include "allen_font.h"

/* Each glyph: "wN" (advance in design units), then pen commands:
 *   Mx,y        move the pen
 *   Lx,y        line from the pen to x,y
 *   Acx,cy,rx,ry,a0,a1   elliptical arc from angle a0 to a1 (degrees; 0 is
 *               right, 90 is down), starting fresh; the pen ends at a1
 *   Dx,y        a dot
 * Every stroke has round caps and joins (distance to the centre line). */
static const char *const glyphs[CK_AF_COUNT] = {
    /*   */ "w6",
    /* ! */ "w6 M3,0 L3,14 D3,20",
    /* " */ "w10 M3,0 L3,5 M7,0 L7,5",
    /* # */ "w15 M6,2 L4,20 M11,2 L9,20 M2,8 L13,8 M1,14 L12,14",
    /* $ */ "w15 A8,5,5,5,-30,-270 A8,15,5,5,270,510 M8,-2 L8,22",
    /* % */ "w16 A4,4,2,3,0,360 A12,16,2,3,0,360 M13,1 L3,19",
    /* & */ "w16 M14,20 L5,9 A7,5,3,4,150,400 L3,14 A7,15,4,5,180,60 L13,11",
    /* ' */ "w6 M3,0 L3,5",
    /* ( */ "w9 A10,10,6,12,240,120",
    /* ) */ "w9 A-1,10,6,12,-60,60",
    /* * */ "w14 M7,2 L7,10 M3,4 L11,8 M11,4 L3,8",
    /* + */ "w14 M2,12 L12,12 M7,7 L7,17",
    /* , */ "w6 M3,19 L2,23",
    /* - */ "w11 M2,13 L9,13",
    /* . */ "w6 D3,20",
    /* / */ "w12 M10,0 L2,20",
    /* 0 */ "w14 A7,10,5,10,0,360",
    /* 1 */ "w14 M3,4 L8,0 L8,20",
    /* 2 */ "w14 A7,6,5,6,200,400 L2,20 L12,20",
    /* 3 */ "w14 A7,5,5,5,200,450 A7,15,5,5,270,520",
    /* 4 */ "w14 M10,20 L10,0 L1,14 L13,14",
    /* 5 */ "w14 M12,0 L4,0 L4,9 A7,14,5,6,235,520",
    /* 6 */ "w14 A7,14,5,6,0,360 M10,0 L3,11",
    /* 7 */ "w14 M2,0 L12,0 L5,20",
    /* 8 */ "w14 A7,5,4,5,0,360 A7,15,5,5,0,360",
    /* 9 */ "w14 A7,6,5,6,0,360 M4,20 L11,9",
    /* : */ "w6 D3,8 D3,20",
    /* ; */ "w6 D3,8 M3,19 L2,23",
    /* < */ "w14 M12,6 L2,12 L12,18",
    /* = */ "w14 M2,10 L12,10 M2,15 L12,15",
    /* > */ "w14 M2,6 L12,12 L2,18",
    /* ? */ "w14 A7,5,5,5,190,450 L7,14 D7,20",
    /* @ */ "w19 A9,12,3,4,0,360 M12,8 L12,14 A14,14,2,2,180,0 A9,11,7,9,0,-300",
    /* A */ "w16 M2,20 L8,0 L14,20 M4,13 L12,13",
    /* B */ "w15 M2,0 L2,20 M2,0 L8,0 A8,5,4,5,270,450 L2,10 M2,10 L9,10 A9,15,4,5,270,450 L2,20",
    /* C */ "w17 A9,10,7,10,45,315",
    /* D */ "w16 M2,0 L2,20 M2,0 L6,0 A6,10,8,10,270,450 L2,20",
    /* E */ "w14 M12,0 L2,0 L2,20 L12,20 M2,10 L10,10",
    /* F */ "w13 M12,0 L2,0 L2,20 M2,10 L10,10",
    /* G */ "w18 A9,10,7,10,-45,-360 M11,10 L16,10",
    /* H */ "w16 M2,0 L2,20 M14,0 L14,20 M2,10 L14,10",
    /* I */ "w6 M3,0 L3,20",
    /* J */ "w13 M10,0 L10,14 A6,14,4,6,0,180",
    /* K */ "w15 M2,0 L2,20 M13,0 L2,12 M6,8 L14,20",
    /* L */ "w13 M2,0 L2,20 L12,20",
    /* M */ "w20 M2,20 L2,0 L10,14 L18,0 L18,20",
    /* N */ "w16 M2,20 L2,0 L14,20 L14,0",
    /* O */ "w20 A10,10,8,10,0,360",
    /* P */ "w15 M2,20 L2,0 L8,0 A8,5,5,5,270,450 L2,10",
    /* Q */ "w20 A10,10,8,10,0,360 M12,14 L18,21",
    /* R */ "w15 M2,20 L2,0 L8,0 A8,5,5,5,270,450 L2,10 M8,10 L14,20",
    /* S */ "w16 A8,5,5,5,-30,-270 A8,15,5,5,270,510",
    /* T */ "w16 M1,0 L15,0 M8,0 L8,20",
    /* U */ "w16 M2,0 L2,13 A8,13,6,7,180,0 L14,0",
    /* V */ "w16 M1,0 L8,20 L15,0",
    /* W */ "w22 M1,0 L6,20 L11,0 L16,20 L21,0",
    /* X */ "w16 M2,0 L14,20 M14,0 L2,20",
    /* Y */ "w16 M1,0 L8,10 L15,0 M8,10 L8,20",
    /* Z */ "w16 M2,0 L14,0 L2,20 L14,20",
    /* [ */ "w9 M7,-1 L3,-1 L3,21 L7,21",
    /* \ */ "w12 M2,0 L10,20",
    /* ] */ "w9 M2,-1 L6,-1 L6,21 L2,21",
    /* ^ */ "w12 M2,8 L6,1 L10,8",
    /* _ */ "w14 M1,23 L13,23",
    /* ` */ "w7 M2,0 L4,4",
    /* a */ "w15 A7,14,5,6,0,360 M12,8 L12,20",
    /* b */ "w15 M2,0 L2,20 A7,14,5,6,0,360",
    /* c */ "w13 A7,14,5,6,40,320",
    /* d */ "w15 A7,14,5,6,0,360 M12,0 L12,20",
    /* e */ "w14 M2,14 L12,14 A7,14,5,6,0,-320",
    /* f */ "w11 M5,20 L5,4 A9,4,4,4,180,300 M2,8 L10,8",
    /* g */ "w15 A7,14,5,6,0,360 M12,8 L12,22 A7,22,5,5,0,150",
    /* h */ "w15 M2,0 L2,20 A7,13,5,5,180,360 L12,20",
    /* i */ "w6 M3,8 L3,20 D3,3",
    /* j */ "w7 M4,8 L4,22 A0,22,4,5,0,120 D4,3",
    /* k */ "w13 M2,0 L2,20 M11,8 L2,15 M5,13 L12,20",
    /* l */ "w6 M3,0 L3,20",
    /* m */ "w21 M2,8 L2,20 A6,12,4,4,180,360 L10,20 A14,12,4,4,180,360 L18,20",
    /* n */ "w15 M2,8 L2,20 A7,13,5,5,180,360 L12,20",
    /* o */ "w14 A7,14,5,6,0,360",
    /* p */ "w15 M2,8 L2,27 A7,14,5,6,0,360",
    /* q */ "w15 A7,14,5,6,0,360 M12,8 L12,27",
    /* r */ "w10 M2,8 L2,20 A7,13,5,5,180,290",
    /* s */ "w14 A7,11,4,3,-30,-270 A7,17,4,3,270,510",
    /* t */ "w11 M5,2 L5,17 A8,17,3,3,180,40 M2,8 L10,8",
    /* u */ "w15 M2,8 L2,15 A7,15,5,5,180,0 M12,8 L12,20",
    /* v */ "w14 M2,8 L7,20 L12,8",
    /* w */ "w20 M2,8 L6,20 L10,8 L14,20 L18,8",
    /* x */ "w14 M2,8 L12,20 M12,8 L2,20",
    /* y */ "w14 M2,8 L7,20 M12,8 L4,27",
    /* z */ "w14 M2,8 L12,8 L2,20 L12,20",
    /* { */ "w10 M8,-1 L6,0 L6,8 L4,10 L6,12 L6,20 L8,21",
    /* | */ "w6 M3,-1 L3,23",
    /* } */ "w10 M2,-1 L4,0 L4,8 L6,10 L4,12 L4,20 L2,21",
    /* ~ */ "w14 M2,13 L5,11 L9,14 L12,12",
};

static const uint16_t sin_q[91] = {
    0,     286,   572,   857,   1143,  1428,  1713,  1997,  2280,  2563,  2845,  3126,  3406,  3686,  3964,
    4240,  4516,  4790,  5063,  5334,  5604,  5872,  6138,  6402,  6664,  6924,  7182,  7438,  7692,  7943,
    8192,  8438,  8682,  8923,  9162,  9397,  9630,  9860,  10087, 10311, 10531, 10749, 10963, 11174, 11381,
    11585, 11786, 11982, 12176, 12365, 12551, 12733, 12911, 13085, 13255, 13421, 13583, 13741, 13894, 14044,
    14189, 14330, 14466, 14598, 14726, 14849, 14968, 15082, 15191, 15296, 15396, 15491, 15582, 15668, 15749,
    15826, 15897, 15964, 16026, 16083, 16135, 16182, 16225, 16262, 16294, 16322, 16344, 16362, 16374, 16382,
    16384,
};

int32_t ck_af_sin(int32_t deg)
{
    deg %= 360;
    if (deg < 0)
        deg += 360;
    if (deg <= 90)
        return sin_q[deg];
    if (deg <= 180)
        return sin_q[180 - deg];
    if (deg <= 270)
        return -(int32_t)sin_q[deg - 180];
    return -(int32_t)sin_q[360 - deg];
}

int32_t ck_af_cos(int32_t deg) { return ck_af_sin(deg + 90); }

uint64_t ck_af_isqrt(uint64_t v)
{
    uint64_t r = 0, bit = (uint64_t)1 << 62;
    while (bit > v)
        bit >>= 2;
    while (bit) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return r;
}

static int idx(char c)
{
    unsigned char u = (unsigned char)c;
    return u >= CK_AF_FIRST && u <= CK_AF_LAST ? u - CK_AF_FIRST : '?' - CK_AF_FIRST;
}

void ck_af_face_init(ck_af_face *f, unsigned cap_px, unsigned weight, uint8_t *pool, size_t pool_len)
{
    f->s256 = cap_px * 256u / 20u;
    f->hw256 = (uint32_t)((uint64_t)weight * f->s256 / 1000u);
    f->pool = pool;
    f->pool_len = pool_len;
    f->pool_used = 0;
    for (unsigned i = 0; i < CK_AF_COUNT; i++)
        f->g[i] = (ck_af_glyph){0};
}

/* Reads a signed integer at *p; advances past it and one ',' if present. */
static int num(const char **p)
{
    int neg = 0, v = 0;
    if (**p == '-') {
        neg = 1;
        (*p)++;
    }
    while (**p >= '0' && **p <= '9')
        v = v * 10 + (*(*p)++ - '0');
    if (**p == ',')
        (*p)++;
    return neg ? -v : v;
}

uint32_t ck_af_advance(const ck_af_face *f, char c)
{
    const char *p = glyphs[idx(c)] + 1; /* past 'w' */
    return (uint32_t)num(&p) * f->s256;
}

uint32_t ck_af_measure(const ck_af_face *f, const char *s, size_t n)
{
    uint64_t w = 0;
    for (size_t i = 0; i < n && s[i]; i++)
        w += ck_af_advance(f, s[i]);
    return (uint32_t)((w + 255) / 256);
}

#define SEG_MAX 160
struct seg {
    int32_t x0, y0, x1, y1; /* 1/256 px, relative to the pen; y up from the baseline is negative */
};

/* Turns one glyph's commands into segments. Returns the count. */
static unsigned segments(const ck_af_face *f, const char *p, struct seg *out)
{
    int32_t s = (int32_t)f->s256;
    unsigned n = 0;
    int32_t px = 0, py = 0;
#define PT_X(u) ((int32_t)(u) * s)
#define PT_Y(u) (((int32_t)(u) - 20) * s)
    while (*p) {
        char cmd = *p++;
        if (cmd == ' ')
            continue;
        if (cmd == 'w') {
            (void)num(&p);
        } else if (cmd == 'M') {
            int x = num(&p), y = num(&p);
            px = PT_X(x);
            py = PT_Y(y);
        } else if (cmd == 'L' || cmd == 'D') {
            int x = num(&p), y = num(&p);
            int32_t nx = PT_X(x), ny = PT_Y(y);
            if (cmd == 'D')
                px = nx, py = ny;
            if (n < SEG_MAX)
                out[n++] = (struct seg){px, py, nx, ny};
            px = nx;
            py = ny;
        } else if (cmd == 'A') {
            int cx = num(&p), cy = num(&p), rx = num(&p), ry = num(&p), a0 = num(&p), a1 = num(&p);
            int span = a1 > a0 ? a1 - a0 : a0 - a1;
            int steps = span / 9 < 2 ? 2 : span / 9;
            int32_t lx = 0, ly = 0;
            for (int k = 0; k <= steps; k++) {
                int32_t a = a0 + (a1 - a0) * k / steps;
                int32_t x = PT_X(cx) + (int32_t)((int64_t)rx * s * ck_af_cos(a) / 16384);
                int32_t y = PT_Y(cy) + (int32_t)((int64_t)ry * s * ck_af_sin(a) / 16384);
                if (k && n < SEG_MAX)
                    out[n++] = (struct seg){lx, ly, x, y};
                lx = x;
                ly = y;
            }
            px = lx;
            py = ly;
        }
    }
#undef PT_X
#undef PT_Y
    return n;
}

static int32_t floor_px(int32_t v) { return v >= 0 ? v / 256 : -((-v + 255) / 256); }

const ck_af_glyph *ck_af_glyph_get(ck_af_face *f, char c)
{
    ck_af_glyph *g = &f->g[idx(c)];
    if (g->alpha || (g->w == 0 && g->h == 1)) /* h == 1, w == 0: an empty glyph already done */
        return g;
    struct seg sg[SEG_MAX];
    unsigned n = segments(f, glyphs[idx(c)], sg);
    if (n == 0) {
        g->w = 0;
        g->h = 1;
        return g;
    }
    int32_t minx = sg[0].x0, maxx = minx, miny = sg[0].y0, maxy = miny;
    for (unsigned i = 0; i < n; i++) {
        int32_t xs[2] = {sg[i].x0, sg[i].x1}, ys[2] = {sg[i].y0, sg[i].y1};
        for (int k = 0; k < 2; k++) {
            if (xs[k] < minx) minx = xs[k];
            if (xs[k] > maxx) maxx = xs[k];
            if (ys[k] < miny) miny = ys[k];
            if (ys[k] > maxy) maxy = ys[k];
        }
    }
    int32_t pad = (int32_t)f->hw256 + 256;
    int32_t x0 = floor_px(minx - pad), y0 = floor_px(miny - pad);
    int32_t x1 = floor_px(maxx + pad) + 1, y1 = floor_px(maxy + pad) + 1;
    size_t w = (size_t)(x1 - x0), h = (size_t)(y1 - y0);
    if (f->pool_len - f->pool_used < w * h)
        return 0;
    uint8_t *a = f->pool + f->pool_used;
    int64_t hw = f->hw256;
    for (size_t yy = 0; yy < h; yy++) {
        int64_t py = (int64_t)(y0 + (int32_t)yy) * 256 + 128;
        for (size_t xx = 0; xx < w; xx++) {
            int64_t pxc = (int64_t)(x0 + (int32_t)xx) * 256 + 128;
            int64_t best = INT64_MAX;
            for (unsigned i = 0; i < n; i++) {
                int64_t ax = sg[i].x0, ay = sg[i].y0, dx = sg[i].x1 - ax, dy = sg[i].y1 - ay;
                int64_t qx = pxc - ax, qy = py - ay, len2 = dx * dx + dy * dy, ex, ey;
                if (len2 == 0) {
                    ex = qx;
                    ey = qy;
                } else {
                    int64_t t = qx * dx + qy * dy; /* projection, scaled by len2 */
                    if (t <= 0) {
                        ex = qx;
                        ey = qy;
                    } else if (t >= len2) {
                        ex = qx - dx;
                        ey = qy - dy;
                    } else {
                        ex = qx - dx * t / len2;
                        ey = qy - dy * t / len2;
                    }
                }
                int64_t d2 = ex * ex + ey * ey;
                if (d2 < best)
                    best = d2;
            }
            int64_t d = (int64_t)ck_af_isqrt((uint64_t)best);
            int64_t cov = hw + 128 - d; /* 1/256 px of the pixel inside the stroke */
            a[yy * w + xx] = cov <= 0 ? 0 : cov >= 256 ? 255 : (uint8_t)(cov * 255 / 256);
        }
    }
    f->pool_used += w * h;
    g->w = (uint16_t)w;
    g->h = (uint16_t)h;
    g->ox = (int16_t)x0;
    g->oy = (int16_t)y0;
    g->alpha = a;
    return g;
}
