/* flow.h -- Allen's ink: a fluid that fills the screen around him.
 *
 * The whole screen is a shallow tank of water with a little luminous ink in
 * it. What Allen does moves the water, and the ink shows the movement:
 *   idle       a slow warm current turns round him, the ink drifts and thins.
 *   listening  every key typed drops ink at the bar and lifts it toward him;
 *              the line you type flows up into him before he answers.
 *   thinking   the water round him turns into a vortex, ink streams out of
 *              his heart along it; the longer he thinks, the deeper it turns.
 *   speaking   each word pushes a ring of water out from the heart.
 *   asleep     nothing pushes; the water stills and the ink fades.
 * No state is ever cut to: forces ease in and out, and the water carries
 * everything that was moving on until it settles.
 *
 * The solver is Stam's stable fluid (Jos Stam, "Real-Time Fluid Dynamics for
 * Games", GDC 2003; "Stable Fluids", SIGGRAPH 1999): add sources, diffuse,
 * move along the flow (semi-Lagrangian), make the velocity divergence free
 * (a Gauss-Seidel pressure solve). It never blows up whatever the time step,
 * which is what a screen that must never stutter needs. Everything is
 * integer: cells, velocities and ink in 16.16 fixed point, on a coarse grid
 * (CK_FLOW_W x CK_FLOW_H cells over the screen), drawn with bilinear
 * interpolation so the ink has no edges. Freestanding, no allocation (the
 * fields live in the struct), host tested in ui/tests/allen_ui_test.c.
 * ck_flow_at moves it on (one caller); ck_flow_draw only reads, so several
 * cores can draw different parts of the screen at once. */
#ifndef AIENOS_FLOW_H
#define AIENOS_FLOW_H
#include <stddef.h>
#include <stdint.h>
#include "paint.h"

#define CK_FLOW_W 64u
#define CK_FLOW_H 36u
#define CK_FLOW_CELLS ((CK_FLOW_W + 2u) * (CK_FLOW_H + 2u)) /* with the border cells */

enum { CK_FLOW_IDLE, CK_FLOW_LISTEN, CK_FLOW_THINK, CK_FLOW_SPEAK, CK_FLOW_SLEEP };

typedef struct {
    int32_t w, h;          /* the screen, px */
    int32_t cw, ch;        /* one cell, 1/256 px (the grid covers the screen exactly) */
    int32_t cx, cy, R;     /* Allen's centre and radius, px */
    int32_t bx, by;        /* where typed lines start from (the bar), px */
    uint32_t t, stepped;   /* now, and the time of the last solver step (ms) */
    int mode;
    uint32_t weight[5];    /* how far each mode's pull has eased in, 0..256; they add up */
    uint32_t think_t, word_t, key_t; /* when thinking began (0: not), the last word, the last key */
    uint32_t rng;
    /* the fields, 16.16: velocity in cells per second, ink 0..1 */
    int32_t u[CK_FLOW_CELLS], v[CK_FLOW_CELLS], d[CK_FLOW_CELLS];
    int32_t u0[CK_FLOW_CELLS], v0[CK_FLOW_CELLS], d0[CK_FLOW_CELLS];
    int32_t shown[CK_FLOW_CELLS]; /* the ink each cell showed at the last mark */
    uint16_t warm[CK_FLOW_CELLS]; /* how fast the water moves in each cell, 0..256, for the colour */
    uint16_t shown_warm[CK_FLOW_CELLS];
    uint32_t steps;
} ck_flow;

/* The screen w x h, Allen at (cx, cy) with radius R, the bar's mouth at (bx, by). */
void ck_flow_init(ck_flow *f, int32_t w, int32_t h, int32_t cx, int32_t cy, int32_t R, int32_t bx, int32_t by,
                  uint32_t t);
void ck_flow_mode(ck_flow *f, int mode, uint32_t t);
void ck_flow_key(ck_flow *f, uint32_t t);  /* a key was typed: a drop at the bar, lifted toward Allen */
void ck_flow_word(ck_flow *f, uint32_t t); /* a word was spoken: a ring from the heart */
/* A push at (px, py): velocity (vx, vy) in px per second and ink (0..65536) over radius r px. */
void ck_flow_touch(ck_flow *f, int32_t px, int32_t py, int32_t vx, int32_t vy, int32_t ink, int32_t r);
/* Moves the water to time t (ms); the solver steps at most every CK_FLOW_STEP_MS). */
#define CK_FLOW_STEP_MS 16u
void ck_flow_at(ck_flow *f, uint32_t t);
/* The water's velocity at a point, px per second (for things that float on it). */
void ck_flow_velocity(const ck_flow *f, int32_t px, int32_t py, int32_t *vx, int32_t *vy);
/* Publishes each cell whose ink changed enough to redraw, calling mark(rect)
 * for its interpolation footprint. Call after ck_flow_at, before drawing. */
void ck_flow_mark(ck_flow *f, void (*mark)(ck_rect));
/* The published ink snapshot into r of out (light added). gain scales
 * brightness (256 = as is); full and incremental draws see the same state. */
void ck_flow_draw(const ck_flow *f, ck_canvas *out, const ck_rect *r, uint32_t gain);
/* Total ink in the tank, 16.16 (for tests: it rises when touched and fades when left). */
int64_t ck_flow_ink(const ck_flow *f);
#endif
