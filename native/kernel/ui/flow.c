/* flow.c -- Allen's ink; see flow.h. */
#include "flow.h"

#define W ((int32_t)CK_FLOW_W)
#define H ((int32_t)CK_FLOW_H)
#define IX(i, j) ((i) + (W + 2) * (j))
#define Q 65536 /* one, 16.16 */
#define HALF 32768

/* How the water behaves (16.16 unless said): a little thick, so a push
 * becomes a slow curl rather than a splash; ink spreads a little more than
 * the water does; both fade on their own so the tank always comes to rest. */
#define VISC_PER_MS 205  /* diffusion of velocity per ms (0.05 per 16 ms step) */
#define INK_PER_MS 328   /* diffusion of ink per ms (0.08 per 16 ms step) */
#define INK_FADE_PER_MS 33 /* 0.5 of the ink per second */
#define VEL_FADE_PER_MS 59 /* 0.9 of the velocity per second */
#define BLEND_MS 700u    /* a mode's pull eases in or out over this long */
#define SOLVE_STEPS 10   /* Gauss-Seidel sweeps for diffusion and pressure */

static int32_t mul(int32_t a, int32_t b) { return (int32_t)(((int64_t)a * b) >> 16); }

static uint32_t rnd(ck_flow *f)
{
    f->rng ^= f->rng << 13;
    f->rng ^= f->rng >> 17;
    f->rng ^= f->rng << 5;
    return f->rng;
}

/* ---- the grid and the screen ---- */

/* A screen position to a grid position (16.16 cells, the border cell being 0). */
static int32_t cell_x(const ck_flow *f, int32_t px) { return (int32_t)((((int64_t)px << 24) + f->cw / 2) / f->cw) + Q; }
static int32_t cell_y(const ck_flow *f, int32_t py) { return (int32_t)((((int64_t)py << 24) + f->ch / 2) / f->ch) + Q; }
/* px per second to cells per second */
static int32_t cells_x(const ck_flow *f, int32_t vx) { return (int32_t)(((int64_t)vx << 24) / f->cw); }
static int32_t cells_y(const ck_flow *f, int32_t vy) { return (int32_t)(((int64_t)vy << 24) / f->ch); }

/* The field sampled between cells (values sit at cell centres). */
static int32_t sample(const int32_t *x, int32_t gx, int32_t gy)
{
    gx = ck_clamp(gx - HALF, 0, (W + 1) * Q - 1);
    gy = ck_clamp(gy - HALF, 0, (H + 1) * Q - 1);
    int32_t i = gx >> 16, j = gy >> 16, s1 = gx & 0xffff, t1 = gy & 0xffff, s0 = Q - s1, t0 = Q - t1;
    int64_t a = (int64_t)x[IX(i, j)] * t0 + (int64_t)x[IX(i, j + 1)] * t1;
    int64_t b = (int64_t)x[IX(i + 1, j)] * t0 + (int64_t)x[IX(i + 1, j + 1)] * t1;
    return (int32_t)((a * s0 + b * s1) >> 32);
}

/* ---- Stam's steps ---- */

/* The border: walls for the velocity (the part across the wall mirrors), continuation for a scalar. */
static void set_bnd(int32_t *x, int which)
{
    for (int32_t i = 1; i <= W; i++) {
        x[IX(i, 0)] = which == 2 ? -x[IX(i, 1)] : x[IX(i, 1)];
        x[IX(i, H + 1)] = which == 2 ? -x[IX(i, H)] : x[IX(i, H)];
    }
    for (int32_t j = 1; j <= H; j++) {
        x[IX(0, j)] = which == 1 ? -x[IX(1, j)] : x[IX(1, j)];
        x[IX(W + 1, j)] = which == 1 ? -x[IX(W, j)] : x[IX(W, j)];
    }
    x[IX(0, 0)] = (x[IX(1, 0)] + x[IX(0, 1)]) / 2;
    x[IX(0, H + 1)] = (x[IX(1, H + 1)] + x[IX(0, H)]) / 2;
    x[IX(W + 1, 0)] = (x[IX(W, 0)] + x[IX(W + 1, 1)]) / 2;
    x[IX(W + 1, H + 1)] = (x[IX(W, H + 1)] + x[IX(W + 1, H)]) / 2;
}

/* x = (x0 + a * (the four neighbours)) / c, swept SOLVE_STEPS times. */
static void lin_solve(int32_t *x, const int32_t *x0, int32_t a, int32_t c, int which)
{
    int64_t inv = ((int64_t)1 << 46) / c; /* one division per solve instead of one per cell */
    for (int k = 0; k < SOLVE_STEPS; k++) {
        for (int32_t j = 1; j <= H; j++)
            for (int32_t i = 1; i <= W; i++) {
                int64_t sum = (int64_t)x[IX(i - 1, j)] + x[IX(i + 1, j)] + x[IX(i, j - 1)] + x[IX(i, j + 1)];
                int64_t num = (((int64_t)x0[IX(i, j)] << 16) + a * sum) >> 14;
                x[IX(i, j)] = (int32_t)((num * inv) >> 32);
            }
        set_bnd(x, which);
    }
}

static void diffuse(int32_t *x, const int32_t *x0, int32_t a, int which) { lin_solve(x, x0, a, Q + 4 * a, which); }

/* Each cell takes the value found where its water came from, dt seconds ago. */
static void advect(int32_t *d, const int32_t *d0, const int32_t *u, const int32_t *v, int32_t dt, int which)
{
    for (int32_t j = 1; j <= H; j++)
        for (int32_t i = 1; i <= W; i++) {
            int32_t x = (i << 16) - mul(dt, u[IX(i, j)]), y = (j << 16) - mul(dt, v[IX(i, j)]);
            x = ck_clamp(x, HALF, W * Q + HALF);
            y = ck_clamp(y, HALF, H * Q + HALF);
            int32_t i0 = x >> 16, j0 = y >> 16, s1 = x & 0xffff, t1 = y & 0xffff, s0 = Q - s1, t0 = Q - t1;
            int64_t a = (int64_t)d0[IX(i0, j0)] * t0 + (int64_t)d0[IX(i0, j0 + 1)] * t1;
            int64_t b = (int64_t)d0[IX(i0 + 1, j0)] * t0 + (int64_t)d0[IX(i0 + 1, j0 + 1)] * t1;
            d[IX(i, j)] = (int32_t)((a * s0 + b * s1) >> 32);
        }
    set_bnd(d, which);
}

/* Takes the divergence out of the velocity, so the water neither piles up nor
 * vanishes anywhere: p and div are scratch. */
static void project(int32_t *u, int32_t *v, int32_t *p, int32_t *div)
{
    for (int32_t j = 1; j <= H; j++)
        for (int32_t i = 1; i <= W; i++) {
            div[IX(i, j)] = -(u[IX(i + 1, j)] - u[IX(i - 1, j)] + v[IX(i, j + 1)] - v[IX(i, j - 1)]) / 2;
            p[IX(i, j)] = 0;
        }
    set_bnd(div, 0);
    set_bnd(p, 0);
    lin_solve(p, div, Q, 4 * Q, 0);
    for (int32_t j = 1; j <= H; j++)
        for (int32_t i = 1; i <= W; i++) {
            u[IX(i, j)] -= (p[IX(i + 1, j)] - p[IX(i - 1, j)]) / 2;
            v[IX(i, j)] -= (p[IX(i, j + 1)] - p[IX(i, j - 1)]) / 2;
        }
    set_bnd(u, 1);
    set_bnd(v, 2);
}

static void copy_field(int32_t *d, const int32_t *s)
{
    for (size_t i = 0; i < CK_FLOW_CELLS; i++)
        d[i] = s[i];
}

/* ---- what Allen does to the water ---- */

/* A push over a soft disc: velocity (vx, vy) and ink, both in grid units, strongest at the centre. */
static void push(ck_flow *f, int32_t gx, int32_t gy, int32_t vx, int32_t vy, int32_t ink, int32_t r)
{
    int32_t i0 = ck_max(1, (gx - r) >> 16), i1 = ck_min(W, (gx + r) >> 16);
    int32_t j0 = ck_max(1, (gy - r) >> 16), j1 = ck_min(H, (gy + r) >> 16);
    for (int32_t j = j0; j <= j1; j++)
        for (int32_t i = i0; i <= i1; i++) {
            int64_t dx = (i << 16) + HALF - gx, dy = (j << 16) + HALF - gy;
            int64_t d2 = (dx * dx + dy * dy) >> 16, r2 = ((int64_t)r * r) >> 16;
            if (d2 >= r2)
                continue;
            uint32_t w = ck_exp_neg((uint32_t)(d2 * 3 * 256 / r2)); /* exp(-3 d^2 / r^2), 16.16 */
            f->u[IX(i, j)] += (int32_t)(((int64_t)vx * w) >> 16);
            f->v[IX(i, j)] += (int32_t)(((int64_t)vy * w) >> 16);
            f->d[IX(i, j)] = ck_min(Q, f->d[IX(i, j)] + (int32_t)(((int64_t)ink * w) >> 16));
        }
}

/* The pull of each mode, each weighted by how far it has eased in (0..256),
 * all added: a change of mode is a change of weights, never a jump. */
static void forces(ck_flow *f, uint32_t dt_ms)
{
    int32_t cx = cell_x(f, f->cx), cy = cell_y(f, f->cy);
    int32_t R = ck_max(Q / 2, (int32_t)(((int64_t)f->R << 24) / f->cw)); /* Allen's radius in cells */
    int32_t bx = cell_x(f, f->bx), by = cell_y(f, f->by);
    uint32_t wi = f->weight[CK_FLOW_IDLE], wl = f->weight[CK_FLOW_LISTEN], wt = f->weight[CK_FLOW_THINK];
    uint32_t ws = f->weight[CK_FLOW_SPEAK];
    /* thinking deepens over the first four seconds */
    uint32_t deep = f->think_t ? 256 + ck_smooth(ck_prog(f->t, f->think_t, f->think_t + 4000)) * 3 / 4 : 256;
    int32_t breath = ck_sin(f->t * 65536 / 7000); /* a slow swell, +-16384 */
    for (int32_t j = 1; j <= H; j++)
        for (int32_t i = 1; i <= W; i++) {
            int64_t dx = (i << 16) + HALF - cx, dy = (j << 16) + HALF - cy;
            int64_t r = ck_isqrt((uint64_t)((dx * dx + dy * dy) >> 16)) << 8; /* 16.16 cells */
            if (r < Q / 8)
                r = Q / 8;
            int32_t tx = (int32_t)(-dy * Q / r), ty = (int32_t)(dx * Q / r); /* tangent, unit */
            int32_t rx = (int32_t)(dx * Q / r), ry = (int32_t)(dy * Q / r);   /* outward, unit */
            /* a bell over the ring R..3.5R where the water turns */
            int64_t ring = r - 3 * R / 2;
            uint32_t bell = r < 7 * R / 2 ? ck_exp_neg((uint32_t)(ring * ring * 512 / ((int64_t)R * R))) : 0;
            int64_t ax = 0, ay = 0; /* cells per second per second, 16.16 */
            if (wi && bell) { /* idle: a slow turn with the breath */
                int64_t k = (int64_t)bell * wi >> 8;
                int64_t turn = Q / 5 + (breath * (Q / 8) >> 14);
                ax += (tx * turn >> 16) * k >> 16;
                ay += (ty * turn >> 16) * k >> 16;
                ax += (rx * (int64_t)(breath * (Q / 10) >> 14) >> 16) * k >> 16;
                ay += (ry * (int64_t)(breath * (Q / 10) >> 14) >> 16) * k >> 16;
            }
            if (wt && bell) { /* thinking: the vortex, deepening, drawn a little inward */
                int64_t k = (int64_t)bell * wt * deep >> 16;
                int64_t turn = 3 * Q / 2;
                ax += (tx * turn >> 16) * k >> 16;
                ay += (ty * turn >> 16) * k >> 16;
                ax -= (rx * (int64_t)(Q / 4) >> 16) * k >> 16;
                ay -= (ry * (int64_t)(Q / 4) >> 16) * k >> 16;
            }
            if (wl) { /* listening: a soft current from the bar up to Allen */
                int64_t lx = cx - bx, ly = cy - by, len = ck_isqrt((uint64_t)((lx * lx + ly * ly) >> 16)) << 8;
                if (len > Q) {
                    int64_t px = (i << 16) + HALF - bx, py = (j << 16) + HALF - by;
                    int64_t along = (px * lx + py * ly) / len, across = (px * ly - py * lx) / len;
                    if (along > 0 && along < len) {
                        uint32_t near = ck_exp_neg((uint32_t)(across * across * 256 / ((int64_t)R * R)));
                        int64_t k = (int64_t)near * wl >> 8;
                        ax += (lx * Q / len * (Q / 2) >> 16) * k >> 16;
                        ay += (ly * Q / len * (Q / 2) >> 16) * k >> 16;
                    }
                }
            }
            (void)ws; /* speaking pushes in rings as the words come (ck_flow_word) */
            f->u[IX(i, j)] += (int32_t)(ax * (int64_t)dt_ms / 1000);
            f->v[IX(i, j)] += (int32_t)(ay * (int64_t)dt_ms / 1000);
        }
    /* ink from the heart: a trickle when idle, a stream when speaking */
    int32_t ink = (int32_t)(((int64_t)(wi * 6 + wl * 10 + ws * 60) * dt_ms) >> 8);
    if (ink)
        push(f, cx, cy, 0, 0, ink, R);
    /* thinking: the ink wells up round him, where the vortex carries it, not under his own light */
    int32_t think_ink = (int32_t)(((int64_t)wt * 120 * deep / 256 * dt_ms) >> 8);
    if (think_ink)
        for (int k = 0; k < 8; k++) {
            uint32_t a = (uint32_t)(f->t * 65536 / 9000) + (uint32_t)k * 8192;
            push(f, cx + (int32_t)((int64_t)ck_cos(a) * 3 * R / 2 >> 14), cy + (int32_t)((int64_t)ck_sin(a) * 3 * R / 2 >> 14), 0, 0,
                 think_ink, R / 2);
        }
    /* a few motes of ink drift in from nowhere while he is awake, so the water is never empty */
    if ((wi || wl) && (rnd(f) & 15) == 0)
        push(f, Q + (int32_t)(rnd(f) % (uint32_t)(W * Q)), Q + (int32_t)(rnd(f) % (uint32_t)(H * Q)), 0, 0,
             Q / 40, R / 2);
}

static void fade(int32_t *x, int32_t per_ms, uint32_t dt_ms)
{
    int32_t k = (int32_t)ck_min(Q, (int32_t)(per_ms * dt_ms));
    for (size_t i = 0; i < CK_FLOW_CELLS; i++)
        x[i] -= mul(x[i], k);
}

static void step(ck_flow *f, uint32_t dt_ms)
{
    int32_t dt = (int32_t)((dt_ms << 16) / 1000);
    for (int m = 0; m < 5; m++) { /* the modes' weights ease toward where they should be */
        uint32_t want = m == f->mode ? 256 : 0, cur = f->weight[m], stepw = 256 * dt_ms / BLEND_MS;
        stepw = stepw ? stepw : 1;
        f->weight[m] = cur < want ? ck_min((int32_t)want, (int32_t)(cur + stepw))
                                  : ck_max((int32_t)want, (int32_t)cur - (int32_t)stepw);
    }
    forces(f, dt_ms);
    /* velocity */
    copy_field(f->u0, f->u);
    copy_field(f->v0, f->v);
    diffuse(f->u, f->u0, (int32_t)(VISC_PER_MS * dt_ms), 1);
    diffuse(f->v, f->v0, (int32_t)(VISC_PER_MS * dt_ms), 2);
    project(f->u, f->v, f->u0, f->v0);
    copy_field(f->u0, f->u);
    copy_field(f->v0, f->v);
    advect(f->u, f->u0, f->u0, f->v0, dt, 1);
    advect(f->v, f->v0, f->u0, f->v0, dt, 2);
    project(f->u, f->v, f->u0, f->v0);
    /* ink */
    copy_field(f->d0, f->d);
    diffuse(f->d, f->d0, (int32_t)(INK_PER_MS * dt_ms), 0);
    copy_field(f->d0, f->d);
    advect(f->d, f->d0, f->u, f->v, dt, 0);
    uint32_t still = f->weight[CK_FLOW_SLEEP]; /* asleep the ink goes faster */
    fade(f->d, INK_FADE_PER_MS + (int32_t)(INK_FADE_PER_MS * 3 * still >> 8), dt_ms);
    fade(f->u, VEL_FADE_PER_MS, dt_ms);
    fade(f->v, VEL_FADE_PER_MS, dt_ms);
    /* how fast each cell moves, for the colour: 2 cells per second and up is fully warm */
    for (size_t i = 0; i < CK_FLOW_CELLS; i++) {
        int64_t s2 = ((int64_t)f->u[i] * f->u[i] + (int64_t)f->v[i] * f->v[i]) >> 16;
        uint32_t s = (uint32_t)ck_isqrt((uint64_t)s2) << 8; /* 16.16 cells per second */
        uint32_t warm = s >= 2 * Q ? 256 : s * 256 / (2 * Q);
        f->warm[i] = (uint16_t)(f->warm[i] + ((int32_t)warm - (int32_t)f->warm[i]) * (int32_t)dt_ms / 250);
    }
    f->steps++;
}

/* ---- the interface ---- */

void ck_flow_init(ck_flow *f, int32_t w, int32_t h, int32_t cx, int32_t cy, int32_t R, int32_t bx, int32_t by,
                  uint32_t t)
{
    *f = (ck_flow){0};
    f->w = w, f->h = h;
    f->cw = ck_max(256, w * 256 / W);
    f->ch = ck_max(256, h * 256 / H);
    f->cx = cx, f->cy = cy, f->R = R, f->bx = bx, f->by = by;
    f->t = f->stepped = t;
    f->mode = CK_FLOW_SLEEP; /* still water until he is awake */
    f->weight[CK_FLOW_SLEEP] = 256;
    f->rng = 0x9e3779b9u ^ (uint32_t)t;
}

void ck_flow_mode(ck_flow *f, int mode, uint32_t t)
{
    if (mode < 0 || mode > CK_FLOW_SLEEP || mode == f->mode)
        return;
    f->mode = mode;
    f->think_t = mode == CK_FLOW_THINK ? (t ? t : 1) : 0;
}

void ck_flow_key(ck_flow *f, uint32_t t)
{
    f->key_t = t;
    int32_t gx = cell_x(f, f->bx), gy = cell_y(f, f->by);
    int32_t lx = cell_x(f, f->cx) - gx, ly = cell_y(f, f->cy) - gy;
    int64_t len = ck_isqrt((uint64_t)(((int64_t)lx * lx + (int64_t)ly * ly) >> 16)) << 8;
    if (len < Q)
        len = Q;
    int32_t speed = 5 * Q / 2; /* cells per second, toward Allen */
    push(f, gx, gy, (int32_t)((int64_t)lx * speed / len), (int32_t)((int64_t)ly * speed / len), Q / 2, Q);
}

void ck_flow_word(ck_flow *f, uint32_t t)
{
    f->word_t = t;
    int32_t cx = cell_x(f, f->cx), cy = cell_y(f, f->cy);
    int32_t R = ck_max(Q / 2, (int32_t)(((int64_t)f->R << 24) / f->cw));
    for (unsigned k = 0; k < 12; k++) { /* a ring of twelve pushes, outward */
        uint32_t a = k * 65536 / 12 + (rnd(f) & 2047);
        int32_t ux = ck_cos(a) << 2, uy = ck_sin(a) << 2; /* unit, 16.16 */
        push(f, cx + (int32_t)((int64_t)ux * R / 2 >> 16), cy + (int32_t)((int64_t)uy * R / 2 >> 16),
             (int32_t)((int64_t)ux * (3 * Q / 2) >> 16), (int32_t)((int64_t)uy * (3 * Q / 2) >> 16), Q / 6, R / 2);
    }
}

void ck_flow_touch(ck_flow *f, int32_t px, int32_t py, int32_t vx, int32_t vy, int32_t ink, int32_t r)
{
    push(f, cell_x(f, px), cell_y(f, py), cells_x(f, vx), cells_y(f, vy), ink,
         ck_max(Q / 4, (int32_t)(((int64_t)r << 24) / f->cw)));
}

void ck_flow_at(ck_flow *f, uint32_t t)
{
    f->t = t;
    /* Catch up in fixed steps; after a long gap (the frame core was resting) start from now. */
    if (t - f->stepped > 8 * CK_FLOW_STEP_MS)
        f->stepped = t - 8 * CK_FLOW_STEP_MS;
    while (t - f->stepped >= CK_FLOW_STEP_MS) {
        f->stepped += CK_FLOW_STEP_MS;
        step(f, CK_FLOW_STEP_MS);
    }
}

void ck_flow_velocity(const ck_flow *f, int32_t px, int32_t py, int32_t *vx, int32_t *vy)
{
    int32_t gx = cell_x(f, px), gy = cell_y(f, py);
    *vx = (int32_t)(((int64_t)sample(f->u, gx, gy) * f->cw) >> 24);
    *vy = (int32_t)(((int64_t)sample(f->v, gx, gy) * f->ch) >> 24);
}

void ck_flow_mark(ck_flow *f, void (*mark)(ck_rect))
{
    for (int32_t j = 0; j <= H + 1; j++)
        for (int32_t i = 0; i <= W + 1; i++) {
            int32_t now = f->d[IX(i, j)], was = f->shown[IX(i, j)];
            int32_t warm = f->warm[IX(i, j)], warm_was = f->shown_warm[IX(i, j)];
            if (ck_abs(now - was) < Q / 400 && (now < Q / 200 || ck_abs(warm - warm_was) < 24))
                continue;
            f->shown[IX(i, j)] = now;
            f->shown_warm[IX(i, j)] = (uint16_t)warm;
            /* the cell and one cell round it (the interpolation reaches that far) */
            ck_rect r = {(int32_t)(((int64_t)(i - 2) * f->cw) >> 8), (int32_t)(((int64_t)(j - 2) * f->ch) >> 8),
                         (int32_t)((3 * (int64_t)f->cw >> 8) + 2), (int32_t)((3 * (int64_t)f->ch >> 8) + 2)};
            mark(r);
        }
}

void ck_flow_draw(const ck_flow *f, ck_canvas *out, const ck_rect *r, uint32_t gain)
{
    ck_rect c = *r;
    ck_rect all = {0, 0, out->w, out->h};
    if (!ck_clip(&c, &all))
        return;
    const uint32_t cool = CK_RGB(64, 86, 196), warm = CK_RGB(255, 228, 196);
    /* Render the snapshot published by ck_flow_mark, not sub-threshold
     * solver changes that did not invalidate their tiles. A full refresh
     * must see exactly the same ink as an incremental redraw. */
    int64_t dgx = ((int64_t)1 << 24) / f->cw, rgx = ((int64_t)1 << 24) % f->cw;
    int64_t numerator = ((int64_t)c.x << 24) + f->cw / 2;
    int64_t gx0 = numerator / f->cw + Q, rx0 = numerator % f->cw;
    for (int32_t y = c.y; y < c.y + c.h; y++) {
        int32_t gy = cell_y(f, y);
        uint32_t *px = out->px + (size_t)y * (size_t)out->w;
        int64_t gx = gx0, rx = rx0;
        for (int32_t x = c.x; x < c.x + c.w; x++, gx += dgx, rx += rgx) {
            /* Carry the division remainder: sampling is independent of
             * where a clipped strip starts, without per-pixel division. */
            if (rx >= f->cw)
                rx -= f->cw, gx++;
            int32_t ink = sample(f->shown, (int32_t)gx, gy);
            if (ink < Q / 512)
                continue;
            uint32_t a = (uint32_t)(((int64_t)ink * gain) >> 16);
            a = a > 230 ? 230 : a;
            /* the colour: still water deep blue, moving water lit from within */
            int32_t gxx = ck_clamp((int32_t)gx - HALF, 0, (W + 1) * Q - 1), gyy = ck_clamp(gy - HALF, 0, (H + 1) * Q - 1);
            uint32_t w = f->shown_warm[IX(gxx >> 16, gyy >> 16)];
            uint32_t col = ck_mix(cool, warm, w);
            px[x] = ck_add(px[x], col, a);
        }
    }
}

int64_t ck_flow_ink(const ck_flow *f)
{
    int64_t s = 0;
    for (size_t i = 0; i < CK_FLOW_CELLS; i++)
        s += f->d[i];
    return s;
}
