/* paint.h -- the drawing kit shared by the night sky (ui/night.c), Allen's
 * presence (ui/mind.c) and his home (ui/allen_ui.c): rectangles, colour
 * arithmetic, soft round lights, anti-aliased threads, rounded shapes and
 * text, all into a RAM canvas of 0x00RRGGBB pixels; plus the integer
 * mathematics they share (sine, atan2, exp, log2, hashed noise). Integer
 * only (the kernel builds -mgeneral-regs-only), freestanding, no memory of
 * its own beyond fixed tables. Host tested through ui/tests/allen_ui_test.c.
 *
 * Angles are in 65536ths of a turn. Positions handed to the soft shapes are
 * in 16ths of a pixel, so slow movement is smooth rather than stepping a
 * whole pixel at a time. */
#ifndef AIENOS_PAINT_H
#define AIENOS_PAINT_H
#include <stddef.h>
#include <stdint.h>

#include "allen_font.h"

typedef struct {
    int32_t x, y, w, h;
} ck_rect;

typedef struct {
    uint32_t *px; /* w * h pixels, rows w apart */
    int32_t w, h;
} ck_canvas;

#define CK_RGB(r, g, b) ((uint32_t)(r) << 16 | (uint32_t)(g) << 8 | (uint32_t)(b))

static inline int32_t ck_min(int32_t a, int32_t b) { return a < b ? a : b; }
static inline int32_t ck_max(int32_t a, int32_t b) { return a > b ? a : b; }
static inline int32_t ck_abs(int32_t a) { return a < 0 ? -a : a; }
static inline int32_t ck_clamp(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

/* r becomes its overlap with c; 0 when they do not meet. */
static inline int ck_clip(ck_rect *r, const ck_rect *c)
{
    int32_t x0 = ck_max(r->x, c->x), y0 = ck_max(r->y, c->y);
    int32_t x1 = ck_min(r->x + r->w, c->x + c->w), y1 = ck_min(r->y + r->h, c->y + c->h);
    if (x1 <= x0 || y1 <= y0)
        return 0;
    *r = (ck_rect){x0, y0, x1 - x0, y1 - y0};
    return 1;
}

static inline int ck_meets(ck_rect a, const ck_rect *b) { return ck_clip(&a, b); }
static inline ck_rect ck_grow(ck_rect r, int32_t m) { return (ck_rect){r.x - m, r.y - m, r.w + 2 * m, r.h + 2 * m}; }

/* The smallest rectangle holding both (an empty one adds nothing). */
static inline ck_rect ck_union(ck_rect a, ck_rect b)
{
    if (a.w <= 0 || a.h <= 0)
        return b;
    if (b.w <= 0 || b.h <= 0)
        return a;
    int32_t x0 = ck_min(a.x, b.x), y0 = ck_min(a.y, b.y);
    return (ck_rect){x0, y0, ck_max(a.x + a.w, b.x + b.w) - x0, ck_max(a.y + a.h, b.y + b.h) - y0};
}

/* ---- colour: channels 0..255, weights 0..256 ---- */

static inline uint32_t ck_ch(uint32_t c, int s) { return c >> s & 255u; }

/* c scaled by f/256 (f <= 256), all three channels at once */
static inline uint32_t ck_scale(uint32_t c, uint32_t f)
{
    return ((c & 0xff00ffu) * f >> 8 & 0xff00ffu) | ((c & 0x00ff00u) * f >> 8 & 0x00ff00u);
}

/* d + c * a / 256 per channel, held at 255 */
static inline uint32_t ck_add(uint32_t d, uint32_t c, uint32_t a)
{
    uint32_t r = ck_ch(d, 16) + (ck_ch(c, 16) * a >> 8), g = ck_ch(d, 8) + (ck_ch(c, 8) * a >> 8),
             b = ck_ch(d, 0) + (ck_ch(c, 0) * a >> 8);
    return (r > 255 ? 255u : r) << 16 | (g > 255 ? 255u : g) << 8 | (b > 255 ? 255u : b);
}

/* d toward c by a/256 */
static inline uint32_t ck_mix(uint32_t d, uint32_t c, uint32_t a)
{
    if (a >= 256)
        return c;
    return ck_scale(d, 256 - a) + ck_scale(c, a);
}

/* ---- easing: t and the result in 0..256 ---- */

static inline uint32_t ck_smooth(int32_t t) /* 3t^2 - 2t^3 */
{
    t = ck_clamp(t, 0, 256);
    return (uint32_t)(t * t * (768 - 2 * t) >> 16);
}

static inline uint32_t ck_ease_out(int32_t t) /* 1 - (1 - t)^3 */
{
    t = 256 - ck_clamp(t, 0, 256);
    return 256u - (uint32_t)(t * t * t >> 16);
}

/* 0..256 for how far `now` is from `from` to `to` (ms) */
static inline int32_t ck_prog(uint32_t now, uint32_t from, uint32_t to)
{
    if (now <= from)
        return 0;
    if (now >= to)
        return 256;
    return (int32_t)((uint64_t)(now - from) * 256u / (to - from));
}

/* ---- mathematics (paint.c) ---- */

int32_t ck_sin(uint32_t turn65536); /* scaled by 16384 */
int32_t ck_cos(uint32_t turn65536);
uint32_t ck_atan2(int32_t y, int32_t x); /* the angle of (x, y), 0..65535 */
uint32_t ck_exp_neg(uint32_t x256);      /* exp(-x/256), scaled by 65536 */
uint32_t ck_log2_q16(uint64_t v);        /* log2(v) * 65536, v >= 1 */
uint64_t ck_isqrt(uint64_t v);
uint32_t ck_hash(int32_t x, int32_t y, uint32_t seed);
uint32_t ck_noise(int32_t x256, int32_t y256, uint32_t seed); /* smooth value noise, 0..65535 */
uint32_t ck_fbm(int32_t x256, int32_t y256, uint32_t seed, int octaves); /* 0..65535 */

/* ---- shapes (paint.c); each draws only inside the clip rectangle ---- */

/* A soft round light at (cx, cy) with radius r (16ths of a pixel): colour
 * added with weight a * (1 - d^2/r^2)^3. */
void ck_glow(ck_canvas *cv, const ck_rect *clip, int32_t cx, int32_t cy, int32_t r, uint32_t color, uint32_t a);
/* A thread one pixel wide from (x0, y0) to (x1, y1) (16ths of a pixel),
 * anti-aliased, added as light whose weight runs from a0 to a1 along it. */
void ck_thread(ck_canvas *cv, const ck_rect *clip, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t color,
               uint32_t a0, uint32_t a1);
/* Signed distance from a pixel's centre to a rounded rectangle, 16ths of a
 * pixel, negative inside; and the coverage of a pixel at such a distance. */
int32_t ck_sd_rrect(int32_t x, int32_t y, const ck_rect *r, int32_t radius);
static inline uint32_t ck_cover(int32_t d16)
{
    int32_t v = 8 - d16;
    return v <= 0 ? 0u : v >= 16 ? 256u : (uint32_t)(v * 16);
}
/* One letter at pen (1/256 px) on baseline y, mixed in with weight a
 * (additive light when add is set); returns the pen after it. */
uint64_t ck_letter(ck_canvas *cv, const ck_rect *clip, ck_af_face *f, uint64_t pen, int32_t y, char c,
                   uint32_t color, uint32_t a, int add);
#endif
