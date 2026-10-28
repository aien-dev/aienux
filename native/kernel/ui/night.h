/* night.h -- the night sky AIENOS opens on, and the two black holes that
 * merge in it.
 *
 * The sky is made once, into RAM, and it is the real sky (ui/sky_real.h):
 * by default the piece of it GW150914, the first black-hole merger ever
 * heard, came from; the Milky Way from Gaia DR3 integrated light, the
 * catalogue's stars at their true places, brightness and colour, and
 * the galaxies in the view the eye can see (there: the Large and Small
 * Magellanic Clouds). Each galaxy also keeps, per pixel, how bright it is
 * and where along its long axis that pixel sits, so a band of light can
 * later sweep along it (while Allen answers) without the galaxy itself
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
 * The dusk (ck_night_fall): when Allen goes to sleep, one black hole opens
 * where the pair merged and everything on the screen falls into it. The
 * caller keeps the picture as it was (ck_night_picture_take) and the night
 * draws it pulled in: a pixel at distance r from the hole shows what was at
 * sqrt(r^2 + fallen), turned round the hole a set part of a turn for every
 * doubling of that distance; so the picture streams inward, fastest near the
 * hole, whipping round as it goes, and the light that crowds in there is
 * drawn brighter and warmer. Round the shadow an accretion disk glows, warm
 * white at its inner edge through gold and blue to the dark, brighter on the
 * side turning toward the eye, with a thin bright photon ring just outside
 * the shadow; then the whole picture is dimmed as the horizon closes.
 *
 * Every pixel is a pure function of its position and the time, so a frame
 * redraws only the regions that changed, in any order. Integer only,
 * freestanding, no heap. Host tested in ui/tests/allen_ui_test.c. */
#ifndef AIENOS_NIGHT_H
#define AIENOS_NIGHT_H
#include <stddef.h>
#include <stdint.h>

#include "paint.h"
#include "sky_data.h"

#define CK_NIGHT_FADE_END_MS 6800u
#define CK_NIGHT_HOLES_MS 2600u
#define CK_NIGHT_SPIRAL_MS 6000u
#define CK_NIGHT_MERGE_MS 12000u
#define CK_NIGHT_RIPPLE_MS 3600u /* the ripple's run across the screen */
#define CK_NIGHT_GONE_MS 15200u  /* the last black hole has dissolved */

#define CK_NIGHT_GALAXIES 6u
#define CK_NIGHT_TWINKLES 256u
#define CK_NIGHT_LEVELS 6u   /* a kept picture and its copies at 1/2, 1/4, down to 1/32 size */
#define CK_NIGHT_W_MAX 8192u /* the widest screen the dusk can draw (one row of 8-pixel blocks at a time) */

/* How the dusk stands at a frame; see ck_night_fall. All distances in 16ths of a pixel. */
typedef struct {
    int64_t fallen;  /* a pixel at distance r from the hole shows what was at sqrt(r^2 + fallen) */
    uint32_t swirl;  /* 65536ths of a turn the picture is turned for each doubling of that distance, close to the hole */
    int32_t calm;    /* the turning is weaker further out: by calm / (calm + r) */
    uint32_t crowd;  /* the most the crowded light near the hole is brightened: 256 not at all, 1024 four times */
    int32_t e, rs;   /* the hole: Einstein radius and shadow radius */
    uint32_t disk;   /* how bright the accretion disk and the photon ring are, 0..256 */
    uint32_t beam;   /* which way the disk's bright side faces (the side turning toward the eye), 65536ths of a turn */
    uint32_t keep;   /* how much of the picture's light is kept, 0..256; the horizon closing takes it to 0 */
    int32_t near, far; /* the edges of the screen go dark first: the light falls off from near out to far ... */
    uint32_t shade;    /* ... by this much at far, 0..256 */
} ck_night_dusk;

typedef struct {
    ck_rect box;
    uint8_t *light, *phase; /* box.w * box.h each */
    uint32_t tint;          /* the colour of the light that sweeps it */
    uint16_t period;        /* ms for the sweep to come round */
} ck_night_galaxy;

typedef struct {
    int32_t x, y; /* 16ths of a native pixel */
    uint32_t peak; /* catalogue flux, linear Q16 */
    uint16_t col[3]; /* linear colour, brightest channel 4096 */
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
    /* the dusk (ck_night_fall); falling 0: none */
    int falling;
    ck_night_dusk dusk;
    uint8_t disk_a[1024];   /* the disk's light by distance from the hole, 0..255 */
    uint32_t disk_c[1024];  /* and its colour there */
    int32_t disk_far;       /* the disk reaches this far, 16ths of a pixel */
    ck_sky_view view;     /* the current sky view (set by ck_night_init, updated by ck_night_view) */
} ck_night;

/* The picture kept for the dusk: the screen as it was, with smaller copies of it and of the bare sky.
 * Beyond the picture's edges the dusk shows the sky mirrored, so the stars seem to come from past the
 * screen and no edge is seen falling in. */
typedef struct {
    const uint32_t *level[CK_NIGHT_LEVELS]; /* the picture, then it at 1/2, 1/4 ... the width and height */
    const uint32_t *sky[CK_NIGHT_LEVELS];   /* the bare sky the same way (sky[0] is the night's own) */
} ck_night_picture;
/* Memory for a w x h picture kept this way. */
size_t ck_night_picture_bytes(uint32_t w, uint32_t h);
/* Copies the w x h picture `from` (rows w apart) into mem (ck_night_picture_bytes) and makes the smaller
 * copies of it and of n's sky, each a quarter of the one before by averaging 2 x 2 pixels' linear light. */
void ck_night_picture_take(const ck_night *n, const uint32_t *from, void *mem, ck_night_picture *kept);

/* Memory for a w x h night at scale k. */
size_t ck_night_mem_bytes(uint32_t w, uint32_t h, uint32_t k);
/* Makes the sky at native resolution into mem. (cx, cy): where the holes
 * merge. No screen-grid scratch buffer is needed. Returns 0, or -1. */
int ck_night_init(ck_night *n, uint32_t w, uint32_t h, uint32_t k, int32_t cx, int32_t cy, void *mem, size_t len);
/* Rebuilds the native sky for a new view (changing sky mode or location). */
void ck_night_view(ck_night *n, const ck_sky_view *v);
/* Moves the night to t ms: the holes, the lens region, the ripple. Ends any dusk. */
void ck_night_at(ck_night *n, uint32_t t);
/* The dusk, after ck_night_at: one black hole at (cx, cy) with everything falling into it as d says.
 * The picture is then drawn with ck_night_base_from. */
void ck_night_fall(ck_night *n, const ck_night_dusk *d);
/* The sky as seen at the current time (faded in, bent, rippled) into the
 * rectangle r of out. */
void ck_night_base(const ck_night *n, ck_canvas *out, const ck_rect *r);
/* The kept picture (ck_night_picture_take) as the dusk shows it now: pulled into the hole, the shadow,
 * the ring and the disk on it, the whole dimmed by keep; into the rectangle r of out. */
void ck_night_base_from(const ck_night *n, const ck_night_picture *kept, ck_canvas *out, const ck_rect *r);
/* Adds the twinkling and, with shimmer > 0 (0..256), the light sweeping the
 * galaxies; voice (0..256) brightens their cores. */
void ck_night_lights(const ck_night *n, ck_canvas *out, const ck_rect *r, uint32_t shimmer, uint32_t voice);
/* The rectangle a twinkling star needs redrawn. */
ck_rect ck_night_star_box(const ck_night *n, unsigned i);
/* How far the sky has come out of the dark at (x, y) for light of level l
 * (0..255): 0..256. */
uint32_t ck_night_faded(const ck_night *n, int32_t x, int32_t y, uint32_t l);
#endif