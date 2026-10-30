/* mind.h -- Allen's presence: no face and no body, a crisp network.
 *
 * About a hundred sharp points sit on and inside a sphere, joined by fine
 * threads into a network, turning slowly in three dimensions round a compact
 * heart. Cool points and heart have native-pixel-bounded coverage, never bloom.
 * Small sparks run along the threads from point to point; each one that
 * arrives lights its point for a moment. How it moves is how Allen is:
 *   idle       it breathes (a slow swell) and turns slowly; a few sparks.
 *   listening  each key typed sends a soft wave of light up through it.
 *   thinking   it draws in a little, turns faster, the cool heart pulses and
 *              sparks cascade through the whole network.
 *   speaking   every word sends a pulse out from the heart, and the heart
 *              follows the voice.
 * It is born from the light of the black holes' merger: the points start
 * scattered wide and gather, spiralling, into the network.
 *
 * Integer only, freestanding. ck_mind_at moves it to a time (one caller);
 * ck_mind_draw then only reads, so several processor cores can draw
 * different parts of the screen at once. Host tested in
 * ui/tests/allen_ui_test.c and ui/tests/mind_phases_test.c. */
#ifndef AIENOS_MIND_H
#define AIENOS_MIND_H
#include <stddef.h>
#include <stdint.h>

#include "paint.h"

#define CK_MIND_NODES 120u
#define CK_MIND_EDGES 300u
#define CK_MIND_SPARKS 64u
#define CK_MIND_FORM_MS 3000u /* from born to fully gathered */

enum { CK_MIND_IDLE, CK_MIND_LISTEN, CK_MIND_THINK, CK_MIND_SPEAK };

typedef struct {
    /* the shape, fixed at init */
    int32_t cx, cy, R; /* px */
    uint32_t k;
    int16_t home[CK_MIND_NODES][3]; /* unit sphere, Q14 */
    uint8_t ea[CK_MIND_EDGES], eb[CK_MIND_EDGES];
    unsigned ne;
    uint32_t born; /* ms */
    /* how it is, moved on by ck_mind_at */
    uint32_t t, rng, spawn;
    int mode;
    uint32_t angle;          /* the turn so far, 1/256 of a 65536th */
    uint32_t spin, squeeze;  /* eased toward the mode's: per-ms turn (same units), and 0..256 drawn in */
    uint32_t heart, voice;   /* 0..256 */
    uint32_t words[4], keys[4]; /* when the last pulses began (ms; 0 none) */
    uint16_t act[CK_MIND_NODES]; /* how lit each point is, 0..256 */
    struct {
        uint16_t e, dur;
        uint8_t back; /* runs eb to ea */
        uint32_t t0;
    } spark[CK_MIND_SPARKS];
    unsigned nspark;
    /* the frame, worked out by ck_mind_at for ck_mind_draw */
    int32_t px[CK_MIND_NODES], py[CK_MIND_NODES]; /* 16ths of a pixel */
    uint16_t lum[CK_MIND_NODES], near[CK_MIND_NODES]; /* brightness 0..256; nearness 0..256 */
    uint32_t alpha; /* the whole figure, 0..256 */
    int32_t hx, hy, hr; /* compact heart centre and radius: 16ths of a pixel */
    ck_rect box, was;   /* where it is now, and was the frame before */
} ck_mind;

/* (cx, cy) its centre, px; k the scale (1/256 px per design px); born the
 * time it starts to gather (ms). */
void ck_mind_init(ck_mind *m, int32_t cx, int32_t cy, uint32_t k, uint32_t born);
void ck_mind_mode(ck_mind *m, int mode, uint32_t t);
void ck_mind_word(ck_mind *m, uint32_t t); /* a word was spoken */
void ck_mind_key(ck_mind *m, uint32_t t);  /* a key was typed */
void ck_mind_voice(ck_mind *m, uint32_t level); /* 0..256 */
void ck_mind_at(ck_mind *m, uint32_t t);
/* The figure into the part r of out (cool threads and coverage-mixed points). */
void ck_mind_draw(const ck_mind *m, ck_canvas *out, const ck_rect *r);
/* What needs redrawing for this frame: where it was and where it is. */
ck_rect ck_mind_dirty(const ck_mind *m);
#endif
