/* mind.c -- Allen's presence; see mind.h. */
#include "mind.h"

#define S(v) ((int32_t)(((int64_t)(v) * (int64_t)m->k + 128) / 256))
#define INNER 24u /* the last INNER points sit inside the sphere */
#define TILT 3641u /* the upright axis leans toward the viewer by 20 degrees, in 65536ths */

static uint32_t rnd(ck_mind *m)
{
    m->rng ^= m->rng << 13;
    m->rng ^= m->rng >> 17;
    m->rng ^= m->rng << 5;
    return m->rng;
}

static int64_t dist2(const int16_t *a, const int16_t *b)
{
    int64_t dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return dx * dx + dy * dy + dz * dz;
}

static int has_edge(const ck_mind *m, unsigned a, unsigned b)
{
    for (unsigned i = 0; i < m->ne; i++)
        if ((m->ea[i] == a && m->eb[i] == b) || (m->ea[i] == b && m->eb[i] == a))
            return 1;
    return 0;
}

void ck_mind_init(ck_mind *m, int32_t cx, int32_t cy, uint32_t k, uint32_t born)
{
    *m = (ck_mind){0};
    m->cx = cx, m->cy = cy, m->k = k, m->born = born;
    m->R = S(118);
    m->rng = 0x9e3779b9u;
    m->spin = 349; /* a turn in 48 s */
    unsigned outer = CK_MIND_NODES - INNER;
    for (unsigned i = 0; i < CK_MIND_NODES; i++) {
        int64_t y, rr, x, z, len;
        if (i < outer) { /* evenly spread over the sphere: a Fibonacci lattice */
            y = 16384 - (int64_t)(2 * i + 1) * 16384 / outer;
            rr = (int64_t)ck_isqrt((uint64_t)(16384 * 16384 - y * y));
            uint32_t th = (uint32_t)(i * 25033u); /* the golden angle */
            x = rr * ck_cos(th) >> 14, z = rr * ck_sin(th) >> 14;
            len = 16384 - 2950 + (int64_t)(ck_hash((int32_t)i, 1, 31) % 5900u); /* +-18%: a cloud, not a ball */
        } else {
            uint32_t h1 = ck_hash((int32_t)i, 2, 31), h2 = ck_hash((int32_t)i, 3, 31);
            y = 16384 - (int64_t)(h1 % 32768u);
            rr = (int64_t)ck_isqrt((uint64_t)(16384 * 16384 - y * y));
            x = rr * ck_cos(h2) >> 14, z = rr * ck_sin(h2) >> 14;
            len = 4915 + (int64_t)(ck_hash((int32_t)i, 4, 31) % 5734u); /* 0.30 .. 0.65 */
        }
        m->home[i][0] = (int16_t)(x * len >> 14);
        m->home[i][1] = (int16_t)(y * len >> 14);
        m->home[i][2] = (int16_t)(z * len >> 14);
    }
    /* each point joined to its nearest two (the inner ones to three) */
    for (unsigned i = 0; i < CK_MIND_NODES; i++) {
        unsigned want = i < outer ? 2 : 3;
        for (unsigned w = 0; w < want; w++) {
            int best = -1;
            int64_t bd = 0;
            for (unsigned j = 0; j < CK_MIND_NODES; j++) {
                if (j == i || has_edge(m, i, j))
                    continue;
                int64_t d = dist2(m->home[i], m->home[j]);
                if (best < 0 || d < bd)
                    best = (int)j, bd = d;
            }
            if (best >= 0 && m->ne < CK_MIND_EDGES)
                m->ea[m->ne] = (uint8_t)i, m->eb[m->ne] = (uint8_t)best, m->ne++;
        }
    }
}

void ck_mind_mode(ck_mind *m, int mode, uint32_t t)
{
    (void)t;
    m->mode = mode;
}

static void push(uint32_t *ring, uint32_t t)
{
    for (unsigned i = 3; i > 0; i--)
        ring[i] = ring[i - 1];
    ring[0] = t ? t : 1;
}

void ck_mind_word(ck_mind *m, uint32_t t) { push(m->words, t); }
void ck_mind_key(ck_mind *m, uint32_t t) { push(m->keys, t); }
void ck_mind_voice(ck_mind *m, uint32_t level) { m->voice = level > 256 ? 256 : level; }

static void spark(ck_mind *m, unsigned from)
{
    if (m->nspark >= CK_MIND_SPARKS || !m->ne)
        return;
    /* a thread leaving `from`, or any thread when from is out of range */
    unsigned e = rnd(m) % m->ne, tries = 0;
    if (from < CK_MIND_NODES)
        while (m->ea[e] != from && m->eb[e] != from && tries++ < m->ne)
            e = (e + 1) % m->ne;
    unsigned s = m->nspark++;
    m->spark[s].e = (uint16_t)e;
    m->spark[s].back = (uint8_t)(from < CK_MIND_NODES ? m->eb[e] == from : rnd(m) & 1);
    m->spark[s].t0 = m->t;
    m->spark[s].dur = (uint16_t)(m->mode == CK_MIND_THINK ? 240 + rnd(m) % 260 : 420 + rnd(m) % 480);
}

static uint32_t toward(uint32_t cur, uint32_t want, uint32_t dt, uint32_t ms)
{
    int64_t d = (int64_t)want - cur, step = d * (int64_t)(dt < ms ? dt : ms) / (int64_t)ms;
    if (step == 0 && d && dt)
        step = d > 0 ? 1 : -1;
    return (uint32_t)((int64_t)cur + step);
}

static uint32_t pulse(int32_t off, int32_t width) /* (1 - (off/width)^2)^2, 0..256 */
{
    if (off <= -width || off >= width)
        return 0;
    int64_t q = 256 - (int64_t)off * off * 256 / ((int64_t)width * width);
    return (uint32_t)(q * q >> 8);
}

void ck_mind_at(ck_mind *m, uint32_t t)
{
    uint32_t dt = t > m->t ? t - m->t : 0;
    if (dt > 250)
        dt = 250; /* a long pause is not a jump */
    m->t = t;
    m->was = m->box;
    if (t < m->born) {
        m->alpha = 0;
        m->box = (ck_rect){0, 0, 0, 0};
        return;
    }
    static const uint32_t spins[4] = {349, 520, 1400, 600}, squeezes[4] = {0, 4, 16, 0}, hearts[4] = {50, 70, 120, 80},
                          rates[4] = {2, 6, 25, 12};
    int md = m->mode & 3;
    m->spin = toward(m->spin, spins[md], dt, 700);
    m->squeeze = toward(m->squeeze, squeezes[md], dt, 500);
    uint32_t want_heart = hearts[md] + (md == CK_MIND_SPEAK ? m->voice * 120 / 256 : 0);
    if (md == CK_MIND_THINK)
        want_heart += (uint32_t)(16384 + ck_sin(t * 65536u / 900u)) * 30 >> 15;
    m->heart = toward(m->heart, want_heart, dt, md == CK_MIND_SPEAK ? 80 : 400);
    m->angle += m->spin * dt;

    /* sparks: born at the mode's rate; each that arrives lights its point, and while thinking may go on */
    uint32_t form = (uint32_t)ck_prog(t, m->born, m->born + CK_MIND_FORM_MS);
    if (form >= 192) {
        m->spawn += rates[md] * dt;
        while (m->spawn >= 1000) {
            m->spawn -= 1000;
            spark(m, CK_MIND_NODES);
        }
    }
    for (unsigned i = 0; i < m->nspark;) {
        uint32_t age = t - m->spark[i].t0;
        if (age < m->spark[i].dur) {
            i++;
            continue;
        }
        unsigned e = m->spark[i].e, to = m->spark[i].back ? m->ea[e] : m->eb[e];
        m->act[to] = 256;
        m->spark[i] = m->spark[--m->nspark];
        if (md == CK_MIND_THINK ? rnd(m) % 100 < 70 : rnd(m) % 100 < 18)
            spark(m, to);
    }
    for (unsigned i = 0; i < CK_MIND_NODES; i++)
        m->act[i] = (uint16_t)(m->act[i] * (uint32_t)ck_exp_neg(dt * 256 / 260) >> 16);

    /* the shape: breathing, drawn in while thinking, and while being born gathered in from far out */
    int64_t scale = 65536 + ((int64_t)ck_sin(t * 65536u / 5500u) * 1966 >> 14) - (int64_t)m->squeeze * 65536 / 256;
    uint32_t gather = ck_ease_out((int32_t)form);
    scale = scale * (256 + (int64_t)(256 - gather) * 560 / 256) >> 8; /* 3.2 times out at the start */
    uint32_t swirl = (256 - gather) * (256 - gather) >> 8; /* an extra spiral turn that unwinds as it gathers */
    m->alpha = ck_smooth((int32_t)form * 3);
    uint32_t a = (m->angle >> 8) + swirl * 256;
    int64_t ca = ck_cos(a), sa = ck_sin(a), ct = ck_cos(TILT), st = ck_sin(TILT);
    int64_t R16 = (int64_t)m->R * 16;
    int32_t x0 = m->cx * 16, x1 = x0, y0 = m->cy * 16, y1 = y0;
    for (unsigned i = 0; i < CK_MIND_NODES; i++) {
        int64_t x = m->home[i][0], y = m->home[i][1], z = m->home[i][2];
        int64_t rx = (x * ca - z * sa) >> 14, rz = (x * sa + z * ca) >> 14; /* turn about the upright axis */
        int64_t ry = (y * ct - rz * st) >> 14, fz = (y * st + rz * ct) >> 14; /* lean toward the viewer */
        int64_t persp = 16384 + fz * 3 / 16; /* nearer is a little larger */
        m->px[i] = (int32_t)(m->cx * 16 + 8 + (rx * persp >> 14) * R16 / 16384 * scale / 65536);
        m->py[i] = (int32_t)(m->cy * 16 + 8 + (ry * persp >> 14) * R16 / 16384 * scale / 65536);
        uint32_t nr = (uint32_t)((fz + 16384) * 256 / 32768);
        m->near[i] = (uint16_t)(nr > 256 ? 256 : nr);
        /* light from the pulses: words outward from the heart, keys upward from below */
        int64_t rad = (int64_t)ck_isqrt((uint64_t)(x * x + y * y + z * z)) * 256 / 16384;
        uint32_t extra = 0;
        for (unsigned w = 0; w < 4; w++) {
            if (m->words[w] && t - m->words[w] < 900) {
                uint32_t age = t - m->words[w];
                extra += pulse((int32_t)rad - (int32_t)(age * 300 / 600), 70) * (900 - age) / 900 * 150 >> 8;
            }
            if (m->keys[w] && t - m->keys[w] < 600) {
                uint32_t age = t - m->keys[w];
                int32_t ky = 300 - (int32_t)(age * 600 / 500), ny = (int32_t)(y * 256 / 16384);
                extra += pulse(ny - ky, 60) * (600 - age) / 600 * 90 >> 8;
            }
        }
        uint32_t l = 34 + m->near[i] * m->near[i] / 256 * 180 / 256 + extra + m->act[i] * 120 / 256;
        m->lum[i] = (uint16_t)(l > 256 ? 256 : l);
        x0 = ck_min(x0, m->px[i]), x1 = ck_max(x1, m->px[i]);
        y0 = ck_min(y0, m->py[i]), y1 = ck_max(y1, m->py[i]);
    }
    m->hx = m->cx * 16 + 8, m->hy = m->cy * 16 + 8;
    m->hr = (int32_t)(R16 * 52 / 100 * scale / 65536);
    int32_t halo = (int32_t)(R16 * 17 / 10 * ck_min((int32_t)(scale >> 8), 256) / 256);
    int32_t pad = S(8) * 16;
    x0 = ck_min(x0 - pad, m->hx - halo), x1 = ck_max(x1 + pad, m->hx + halo);
    y0 = ck_min(y0 - pad, m->hy - halo), y1 = ck_max(y1 + pad, m->hy + halo);
    m->box = (ck_rect){(x0 >> 4) - 1, (y0 >> 4) - 1, ((x1 - x0) >> 4) + 3, ((y1 - y0) >> 4) + 3};
}

ck_rect ck_mind_dirty(const ck_mind *m) { return ck_union(m->was, m->box); }

void ck_mind_draw(const ck_mind *m, ck_canvas *out, const ck_rect *r)
{
    if (!m->alpha || !ck_meets(m->box, r))
        return;
    uint32_t A = m->alpha, form = (uint32_t)ck_prog(m->t, m->born, m->born + CK_MIND_FORM_MS);
    int64_t R16 = (int64_t)m->R * 16;
    /* the halo: a faint blue light round the whole mind, swelling a little with the heart */
    ck_glow(out, r, m->hx, m->hy, (int32_t)(R16 * 17 / 10), CK_RGB(60, 74, 210), (52 + m->heart / 8) * A >> 8);
    ck_glow(out, r, m->hx, m->hy, (int32_t)R16, CK_RGB(112, 92, 255), (30 + m->heart / 8) * A >> 8);
    /* the threads, fading in once the points have mostly gathered */
    uint32_t ta = ck_smooth(((int32_t)form - 128) * 2) * A >> 8;
    if (ta)
        for (unsigned i = 0; i < m->ne; i++) {
            unsigned a = m->ea[i], b = m->eb[i];
            uint32_t a0 = (m->lum[a] * (16u + m->near[a] * 60u / 256u) >> 8) * ta >> 8,
                     a1 = (m->lum[b] * (16u + m->near[b] * 60u / 256u) >> 8) * ta >> 8;
            ck_thread(out, r, m->px[a], m->py[a], m->px[b], m->py[b], CK_RGB(120, 140, 255), a0, a1);
            if (m->k > 320) /* a second strand on big screens, so the thread keeps its weight */
                ck_thread(out, r, m->px[a], m->py[a] + 16, m->px[b], m->py[b] + 16, CK_RGB(120, 140, 255), a0 / 2,
                          a1 / 2);
        }
    /* the points: pale blue-white, warming as they light */
    for (unsigned i = 0; i < CK_MIND_NODES; i++) {
        int32_t rad = (int32_t)((int64_t)S(3) * 16 * (160 + m->near[i] * 120 / 256) >> 8);
        uint32_t c = ck_mix(CK_RGB(206, 222, 255), CK_RGB(255, 214, 160), m->act[i]);
        ck_glow(out, r, m->px[i], m->py[i], rad * 3, c, (m->lum[i] * 50 >> 8) * A >> 8);
        ck_glow(out, r, m->px[i], m->py[i], rad, c, m->lum[i] * A >> 8);
    }
    /* the sparks, each with a short fading tail */
    for (unsigned i = 0; i < m->nspark; i++) {
        unsigned e = m->spark[i].e, a = m->ea[e], b = m->eb[e];
        if (m->spark[i].back)
            a = m->eb[e], b = m->ea[e];
        int32_t f = (int32_t)((m->t - m->spark[i].t0) * 256u / m->spark[i].dur);
        for (int k = 0; k < 3; k++) {
            int32_t g = ck_clamp(f - k * 22, 0, 256);
            int32_t x = m->px[a] + (int32_t)((int64_t)(m->px[b] - m->px[a]) * g >> 8);
            int32_t y = m->py[a] + (int32_t)((int64_t)(m->py[b] - m->py[a]) * g >> 8);
            ck_glow(out, r, x, y, S(5) * 16 - k * S(1) * 16, CK_RGB(255, 222, 176), (200 >> k) * A >> 8);
        }
    }
    /* the heart: a warm light at the centre, and a small bright core */
    ck_glow(out, r, m->hx, m->hy, m->hr, CK_RGB(255, 210, 170), m->heart * 7 / 10 * A >> 8);
    ck_glow(out, r, m->hx, m->hy, m->hr * 2 / 5, CK_RGB(255, 236, 214), (90 + m->heart) * A >> 8);
    ck_glow(out, r, m->hx, m->hy, m->hr / 8, CK_RGB(255, 250, 240), 230 * A >> 8);
}
