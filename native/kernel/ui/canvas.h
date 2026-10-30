/* canvas.h -- Allen's canvas, the light side: what a scene (svc/allen_scene.h)
 * says is shown, painted into the night round Allen in his own style.
 *
 * Nothing here is a window. A thing is light and type on the sky: a soft
 * pool of shade behind its words (no edge, no frame), a thread of light down
 * its side, Allen's stroke typeface; a number is drawn large in thin strokes,
 * a timer is a ring of light that empties, a person is a ring round their
 * initial, a picture or a video still on its way is a place of gathering
 * light. Allen stays alone in the centre; things sit to his left and right
 * or above him, at most CK_SCENE_THINGS at once.
 *
 * Every change eases in: by default a spark leaves Allen's heart, flies to
 * where the thing will be, touches the water there, and the thing opens line
 * by line (bloom); `rise` floats up, `fade` simply comes. A new scene sends
 * the old things away over 0.7 s while the new ones come; the sky's tone
 * blends over 0.7 s. Nothing cuts.
 *
 * Integer only (16.16 and 1/256 steps), freestanding, no allocation (one
 * static pool for the large digits). ck_allen_canvas_at moves it on (one
 * caller, between frames); ck_allen_canvas_draw only reads, so several
 * cores can draw different parts of the screen at once. Host tested in
 * ui/tests/canvas_test.c. */
#ifndef AIENOS_CANVAS_H
#define AIENOS_CANVAS_H
#include <stddef.h>
#include <stdint.h>

#include "../svc/allen_scene.h"
#include "allen_font.h"
#include "flow.h"
#include "paint.h"

#define CK_CANVAS_SLOTS (2u * CK_SCENE_THINGS) /* the things here now, and those on their way out */
#define CK_CANVAS_TEXT 512u
#define CK_CANVAS_ROWS 12u
#define CK_CANVAS_HOLD_MS 90000u /* a thing left alone this long goes on its own (a running timer stays) */

/* One line of type in a thing: which face and colour, where its baseline sits. */
typedef struct {
    uint16_t off, len;  /* in the thing's text */
    int32_t dx, dy;     /* from the thing's top left to the line's start and baseline, px */
    uint8_t face, role; /* face: 0 voice, 1 body, 2 small, 3 large digits; role: 0 text, 1 mist, 2 hint, 3 warm */
    uint8_t dot;        /* a point of light before it (a list's items) */
} ck_canvas_row;

typedef struct {
    uint8_t live, kind, warm, quiet, arrive, way, nf, nrows, landed;
    uint16_t f[CK_SCENE_FIELDS];
    char text[CK_CANVAS_TEXT];
    uint16_t used, base; /* text used; where the fields end (a lay-out adds after them) */
    ck_canvas_row row[CK_CANVAS_ROWS];
    int32_t x, y, w, h; /* its own area, px */
    int32_t rx, ry, rr; /* a ring (timer, person) or the picture's place (rx, ry, w x rr), px; rr 0: none */
    ck_rect box;        /* all it may light, px */
    ck_rect spark_was;  /* where its spark was drawn last frame */
    int32_t ax, ay;     /* where its spark lands, 16ths of a pixel */
    uint32_t born, gone; /* ms; gone 0 while it stays */
    uint32_t dur;        /* a timer's length, ms (0: a clock time, shown as written) */
    uint32_t tq;         /* the moment its moving part (a timer's ring, a picture's light) is drawn at, ms */
} ck_canvas_item;

typedef struct {
    int32_t w, h, cx, cy;
    uint32_t k;
    ck_af_face *voice, *body, *small;
    ck_af_face big;
    ck_canvas_item item[CK_CANVAS_SLOTS];
    uint8_t tone_from, tone_to, water;
    uint32_t tone_t, tone_w; /* the tone's blend began at tone_t; how far it is, 0..256 */
    uint32_t stir_t, t;
    int32_t hx, hy; /* Allen's heart now, 16ths of a pixel */
    uint32_t shown, dropped;
} ck_allen_canvas;

/* A w x h screen at scale k (1/256 px per design px on 1920 x 1080), Allen at
 * (cx, cy); his three faces are borrowed (already rasterized: drawing never
 * writes to them). 0, or -1 when the large digits do not fit their pool. */
int ck_allen_canvas_init(ck_allen_canvas *c, int32_t w, int32_t h, uint32_t k, int32_t cx, int32_t cy,
                         ck_af_face *voice, ck_af_face *body, ck_af_face *small);
/* A new scene from time t: what is here goes, its things come. */
void ck_allen_canvas_show(ck_allen_canvas *c, const ck_scene *s, uint32_t t);
/* Everything goes (the guest started a new line). */
void ck_allen_canvas_clear(ck_allen_canvas *c, uint32_t t);
/* Moves to t; (hx, hy) is Allen's heart (16ths of a pixel); touches the water
 * (flow may be NULL); calls mark for every part of the screen that changes. */
void ck_allen_canvas_at(ck_allen_canvas *c, uint32_t t, int32_t hx, int32_t hy, ck_flow *flow, void (*mark)(ck_rect));
/* The sky's tone and the things, into the part r of cv (after the sky and the water, before Allen). */
void ck_allen_canvas_draw(const ck_allen_canvas *c, ck_canvas *cv, const ck_rect *r);
/* Things on the screen now (arriving or staying, not leaving). */
unsigned ck_allen_canvas_count(const ck_allen_canvas *c);
#endif
