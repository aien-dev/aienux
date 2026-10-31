/* night.h -- the night sky AIENOS opens on, and the two black holes that
 * merge in it.
 *
 * The sky is made once, into RAM: a near-black field, a faint Milky Way with
 * dust lanes across the middle, faint coloured nebulae, some ten thousand
 * stars and six galaxies. Each galaxy also keeps, per pixel, how bright it is
 * and where along its spiral that pixel sits, so a band of light can later
 * sweep along its arms (while Allen answers) without the galaxy itself
 * changing.
 *
 * The startup, on the clock from ck_night_init (ms):
 *   0 to CK_NIGHT_FADE_END_MS  the eyes adjust to the dark: the brightest
 *                              stars first, then the fainter ones, then the
 *                              Milky Way, in slow patches across the sky.
 *   CK_NIGHT_HOLES_MS          two black holes start to bend the starlight
 *                              (gravitational lensing: stars behind them are
 *                              pulled into arcs), then show their shadows,
 *                              circling on a tilted orbit;
 *   CK_NIGHT_SPIRAL_MS         they spiral in, faster and closer, the way a
 *                              real pair does (separation as t^(1/4), angle
 *                              as t^(5/8), t the time left);
 *   CK_NIGHT_MERGE_MS          they merge; a ripple (the gravitational wave,
 *                              stretching one way and squeezing the other)
 *                              runs out through the stars; the one black
 *                              hole left rings, then dissolves by
 *                              CK_NIGHT_GONE_MS.
 *
 * Every pixel is a pure function of its position and the time, so a frame
 * redraws only the regions that changed, in any order. Integer only,
 * freestanding, no heap. Host tested in ui/tests/allen_ui_test.c. */
#ifndef AIENOS_NIGHT_H
#define AIENOS_NIGHT_H
#include <stddef.h>
#include <stdint.h>

#include "paint.h"

#define CK_NIGHT_FADE_END_MS 6800u
#define CK_NIGHT_HOLES_MS 2600u
#define CK_NIGHT_SPIRAL_MS 6000u
#define CK_NIGHT_MERGE_MS 12000u
#define CK_NIGHT_RIPPLE_MS 3600u /* the ripple's run across the screen */
#define CK_NIGHT_GONE_MS 15200u  /* the last black hole has dissolved */

#define CK_NIGHT_GALAXIES 6u
#define CK_NIGHT_TWINKLES 256u

typedef struct {
    ck_rect box;
    uint8_t *light, *phase; /* box.w * box.h each */
    uint32_t tint;          /* the colour of the light that sweeps it */
    uint16_t period;        /* ms for the sweep to come round */
} ck_night_galaxy;

typedef struct {
    int32_t x, y, r; /* 16ths of a pixel */
    uint32_t color;
    uint16_t period, phase; /* ms */
} ck_night_star;

typedef struct {
    int32_t w, h;
    uint32_t k;      /* 1/256 px per design px (1920 x 1080 design) */
    int32_t cx, cy;  /* where the black holes merge, px */
    uint32_t *sky;   /* the night, w * h */
    uint8_t *appear; /* when each 4 x 4 cell comes out of the dark (0..255) */
    int32_t aw;
    ck_night_galaxy gal[CK_NIGHT_GALAXIES];
    ck_night_star tw[CK_NIGHT_TWINKLES];
    unsigned ntw;
    /* state at the time ck_night_at was last given */
    uint32_t t;
    unsigned holes;
    struct {
        int32_t x, y, e, rs; /* 16ths of a pixel: centre, Einstein radius, shadow radius */
    } hole[2];
    ck_rect lens; /* where the light is bent (empty: nowhere) */
    int32_t ripple_r, ripple_w; /* px; ripple_w 0: no ripple */
    int32_t ripple_disp[1024];  /* 16ths of a pixel, by distance from the crest */
    uint8_t ripple_light[1024];
    int32_t ripple_c2, ripple_s2; /* the stretch direction, Q14 */
} ck_night;

/* Memory for a w x h night at scale k. */
size_t ck_night_mem_bytes(uint32_t w, uint32_t h, uint32_t k);
/* Makes the sky into mem; scratch: at least w * h * 4 bytes of working
 * memory, overwritten. (cx, cy): where the holes merge. 0, or -1. */
int ck_night_init(ck_night *n, uint32_t w, uint32_t h, uint32_t k, int32_t cx, int32_t cy, void *mem, size_t len,
                  void *scratch);
/* Moves the night to t ms: the holes, the lens region, the ripple. */
void ck_night_at(ck_night *n, uint32_t t);
/* The sky as seen at the current time (faded in, bent, rippled) into the
 * rectangle r of out. */
void ck_night_base(const ck_night *n, ck_canvas *out, const ck_rect *r);
/* Adds the twinkling and, with shimmer > 0 (0..256), the light sweeping the
 * galaxies; voice (0..256) brightens their cores. */
void ck_night_lights(const ck_night *n, ck_canvas *out, const ck_rect *r, uint32_t shimmer, uint32_t voice);
/* The rectangle a twinkling star needs redrawn. */
ck_rect ck_night_star_box(const ck_night *n, unsigned i);
/* How far the sky has come out of the dark at (x, y) for light of level l
 * (0..255): 0..256. */
uint32_t ck_night_faded(const ck_night *n, int32_t x, int32_t y, uint32_t l);
#endif
