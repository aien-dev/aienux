/* chirp.c -- the startup sound; see chirp.h.
 *
 * Every frame is worked out from where it sits on the night's timeline.
 * With L the share of the inspiral left (1 at CK_CHIRP_START_MS, 0 at the
 * merger), the holes' orbit goes as 1 - L^(5/8); so does the sound's
 * phase, scaled so it starts at CK_CHIRP_FROM_HZ, which makes its pitch go
 * as L^(-3/8). Once the pitch reaches CK_CHIRP_TO_HZ (the last few
 * milliseconds) it holds there through the merger and the fade. Loudness
 * goes as L^(-1/4), which is pitch^(2/3), reaching CK_CHIRP_PEAK where the
 * pitch stops rising. Phases are in 2^32ths of a cycle; L and its powers in
 * Q30. */
#include "chirp.h"

#define F0_FRAME (CK_CHIRP_RATE / 1000u * (CK_CHIRP_START_MS - CK_CHIRP_IN_MS)) /* the sound comes in */
#define F1_FRAME (CK_CHIRP_RATE / 1000u * CK_CHIRP_START_MS)                    /* the sweep begins */
#define F2_FRAME (CK_CHIRP_RATE / 1000u * CK_NIGHT_MERGE_MS)  /* the merger */
#define Q30 (1ull << 30)
#define DECAY 1072996429ull /* exp(-1 / (30 ms * 48 kHz)) in Q30: the fade's time constant is 30 ms */

static int16_t sin_t[4096]; /* Q14 */
static uint32_t cap_frame;  /* the pitch stops rising here */
static uint64_t cap_l14;    /* L^(1/4) there */
static uint64_t cap_phase;  /* the phase there */
static int ready;

static uint64_t isqrt(uint64_t x)
{
    uint64_t r = 0, bit = 1ull << 62;
    while (bit > x)
        bit >>= 2;
    while (bit) {
        if (x >= r + bit) {
            x -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return r;
}

/* L at a frame of the inspiral, and its eighth root (both Q30) */
static uint64_t left_q30(uint32_t f) { return ((uint64_t)(F2_FRAME - f) << 30) / (F2_FRAME - F1_FRAME); }

static uint64_t root8(uint64_t l)
{
    for (int i = 0; i < 3; i++)
        l = isqrt(l << 30);
    return l;
}

static uint64_t mul(uint64_t a, uint64_t b) { return a * b >> 30; }

/* phase over the inspiral before the cap: P1 + K (1 - L^(5/8)), K chosen so
 * the pitch starts at CK_CHIRP_FROM_HZ: K = FROM_HZ * 8/5 * inspiral seconds */
static uint64_t inspiral_phase(uint32_t f)
{
    uint64_t r = root8(left_q30(f)), l58 = mul(mul(mul(mul(r, r), r), r), r);
    uint64_t p1 = ((uint64_t)CK_CHIRP_FROM_HZ * (F1_FRAME - F0_FRAME) << 32) / CK_CHIRP_RATE;
    uint64_t k = (uint64_t)CK_CHIRP_FROM_HZ * 8 * (F2_FRAME - F1_FRAME); /* K * 5 * RATE */
    return p1 + ((k * (Q30 - l58) << 2) / (5ull * CK_CHIRP_RATE));
}

static void setup(void)
{
    const int64_t C = 1073740561, S = 1647099; /* cos, sin of a 4096th of a turn, Q30 */
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
    /* the first frame where FROM_HZ * L^(-3/8) reaches TO_HZ, i.e. where
     * L^(3/8) falls to FROM_HZ / TO_HZ (L^(3/8) only falls) */
    uint64_t want = ((uint64_t)CK_CHIRP_FROM_HZ << 30) / CK_CHIRP_TO_HZ;
    uint32_t lo = F1_FRAME, hi = F2_FRAME;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        uint64_t r = root8(left_q30(mid));
        if (mul(mul(r, r), r) <= want)
            hi = mid;
        else
            lo = mid + 1;
    }
    cap_frame = lo;
    uint64_t r = root8(left_q30(lo));
    cap_l14 = mul(r, r);
    cap_phase = inspiral_phase(lo);
    ready = 1;
}

static uint64_t phase_at(uint32_t f)
{
    if (f < F1_FRAME)
        return ((uint64_t)CK_CHIRP_FROM_HZ * (f - F0_FRAME) << 32) / CK_CHIRP_RATE;
    if (f < cap_frame)
        return inspiral_phase(f);
    return cap_phase + ((uint64_t)CK_CHIRP_TO_HZ * (f - cap_frame) << 32) / CK_CHIRP_RATE;
}

static uint64_t decay_pow(uint32_t k) /* DECAY^k, Q30 */
{
    uint64_t r = Q30, b = DECAY;
    while (k) {
        if (k & 1)
            r = mul(r, b);
        b = mul(b, b);
        k >>= 1;
    }
    return r;
}

uint32_t ck_chirp_level(uint32_t f)
{
    if (!ready)
        setup();
    if (f < F0_FRAME || f >= CK_CHIRP_FRAMES)
        return 0;
    uint64_t steady = (uint64_t)CK_CHIRP_PEAK * cap_l14 >> 30; /* the level at L = 1 */
    if (f < F1_FRAME) { /* comes in smoothly (smoothstep) */
        uint64_t x = ((uint64_t)(f - F0_FRAME) << 16) / (F1_FRAME - F0_FRAME), s = x * x * (3ull * 65536 - 2 * x);
        return (uint32_t)(steady * s >> 48);
    }
    if (f < cap_frame) {
        uint64_t r = root8(left_q30(f));
        return (uint32_t)((uint64_t)CK_CHIRP_PEAK * cap_l14 / mul(r, r));
    }
    if (f < F2_FRAME)
        return CK_CHIRP_PEAK;
    return (uint32_t)((uint64_t)CK_CHIRP_PEAK * decay_pow(f - F2_FRAME) >> 30);
}

uint32_t ck_chirp_hz(uint32_t f)
{
    if (!ready)
        setup();
    if (f < F0_FRAME || f >= CK_CHIRP_FRAMES)
        return 0;
    if (f < F1_FRAME)
        return CK_CHIRP_FROM_HZ;
    if (f >= cap_frame)
        return CK_CHIRP_TO_HZ;
    uint64_t r = root8(left_q30(f));
    return (uint32_t)(((uint64_t)CK_CHIRP_FROM_HZ << 30) / mul(mul(r, r), r));
}

void ck_chirp_fill(int16_t *out, uint32_t from, uint32_t n)
{
    if (!ready)
        setup();
    for (uint32_t i = 0; i < n; i++) {
        uint32_t f = from + i, a = ck_chirp_level(f);
        int32_t v = 0;
        if (a) {
            uint32_t ph = (uint32_t)phase_at(f), k = ph >> 20, frac = ph >> 4 & 0xffffu;
            int32_t s0 = sin_t[k], s1 = sin_t[(k + 1) & 4095];
            int32_t s = s0 + (int32_t)(((int64_t)(s1 - s0) * frac) >> 16); /* Q14 */
            v = (int32_t)(((int64_t)s * a) >> 14);
        }
        out[2 * i] = out[2 * i + 1] = (int16_t)v;
    }
}

/* The same chirp at 44.1 kHz for converters that take only that rate: frame
 * j sits at 48 kHz frame j * 160 / 147, drawn as a straight line between the
 * two 48 kHz frames either side (the chirp stays under 1000 Hz, far below
 * either rate's limit). Each sample goes in the top 16 bits of a 32-bit slot,
 * the layout a 24-bit converter reads (Linux: S32_LE with 24 bits used). */
void ck_chirp_fill_441(int32_t *out, uint32_t from, uint32_t n)
{
    int16_t src[2 * 144];
    while (n) {
        uint32_t m = n < 128u ? n : 128u, s0 = (uint32_t)((uint64_t)from * 160u / 147u);
        uint32_t s1 = (uint32_t)((uint64_t)(from + m - 1) * 160u / 147u) + 1;
        ck_chirp_fill(src, s0, s1 - s0 + 1);
        for (uint32_t i = 0; i < m; i++) {
            uint64_t at = (uint64_t)(from + i) * 160u;
            uint32_t s = (uint32_t)(at / 147u) - s0, frac = (uint32_t)(at % 147u);
            int32_t a = src[2 * s], b = src[2 * s + 2];
            int32_t v = (a * (int32_t)(147u - frac) + b * (int32_t)frac) / 147;
            out[2 * i] = out[2 * i + 1] = (int32_t)((uint32_t)v << 16);
        }
        out += 2 * m;
        from += m;
        n -= m;
    }
}
