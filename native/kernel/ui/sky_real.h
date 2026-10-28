/* sky_real.h -- the real sky behind Allen: the catalogue's stars at their
 * true places, brightness and colour; the Milky Way from Gaia DR3 source
 * counts and integrated G-band flux (ESA/Gaia/DPAC, CC BY-NC 3.0 IGO),
 * reddened by extinction from gaiadr3.astrophysical_parameters; the
 * galaxies the eye can see. The data are in sky_data.c (generated, see
 * sky_data.h); this is the drawing, integer only.
 *
 * The view: a gnomonic projection (what a pinhole camera sees) of any piece
 * of sky, the view centre at (cx, cy) on the screen, a set width of sky
 * across the screen's width.
 *
 * The Milky Way is stored as a full-sphere equirectangular grid; rendering
 * inverse-projects each screen point to (RA, Dec) and bilinear-samples the
 * grid, so any view can be shown.
 *
 * Also here: the sidereal-time maths for "the sky above you now". */
#ifndef AIENOS_SKY_REAL_H
#define AIENOS_SKY_REAL_H
#include <stdint.h>

#include "night.h"
#include "sky_data.h"

/* Where the direction d (Q15) lands on a screen w px wide whose view
 * centre is at (cx, cy) px: (x16, y16) in 16ths of a pixel, the pixel i
 * spanning 16 i to 16 i + 16. 0 if it is more than 88 degrees from the
 * centre (behind the camera), else 1. */
int ck_sky_project(const ck_sky_view *v, const int16_t d[3], int32_t w, int32_t cx, int32_t cy, int32_t *x16,
                   int32_t *y16);

/* Inverse-project and sample the sourced Milky Way at every native pixel
 * centre, then encode sRGB with ordered quantization. No screen-grid
 * intermediate, runtime blur or enlargement. The Gaia-derived map has
 * 360 x 180 angular luma cells (stored 361 x 180 with a wrap column) and
 * 181 x 90 stored chroma samples. Native pixels cannot invent source detail. */
void ck_sky_draw_milkyway(ck_canvas *cv, int32_t cx, int32_t cy, const ck_sky_view *v);

/* Draws every catalogue star in the view into cv (a w x h sky whose view
 * centre is at (cx, cy)). UI scale does not affect a point source.
 * Adds linear light with at most 2 x 2 native-pixel support. The brightest
 * few (V <= 1.0) are also listed in tw, up to max, to twinkle; *ntw: count. */
void ck_sky_draw_stars(ck_canvas *cv, int32_t cx, int32_t cy, const ck_sky_view *v,
                       ck_night_star *tw, unsigned max, unsigned *ntw);
/* Dynamic light uses exactly the static point source's native footprint.
 * amount is additional linear light in 1/256 of its catalogue flux. */
void ck_sky_twinkle(ck_canvas *cv, const ck_rect *clip, const ck_night_star *s, uint32_t amount);

/* The largest box galaxy i can need on a w-wide screen (for memory). */
ck_rect ck_sky_galaxy_bound(uint32_t w, unsigned i);
/* Galaxy i's box on a w x h screen, view v, centre (cx, cy) (clipped; empty
 * if not in the view or no galaxy i). */
ck_rect ck_sky_galaxy_box(uint32_t w, uint32_t h, int32_t cx, int32_t cy, const ck_sky_view *v, unsigned i);
/* Fills g->light and g->phase over g->box. */
void ck_sky_galaxy_maps(ck_night_galaxy *g, unsigned i, uint32_t w, int32_t cx, int32_t cy,
                        const ck_sky_view *v);

/* ---- the sky above you now ---- */

/* Greenwich mean sidereal time at unix time t (seconds, UTC taken as UT1:
 * within a second), in turns / 2^32. IAU 1982 (Aoki et al. 1982; USNO). */
uint32_t ck_sky_gmst(int64_t unix_s);
/* The direction straight up (the zenith) at unix time t from latitude lat
 * and east longitude lon (millionths of a degree), in J2000 axes (the
 * local sidereal time, then the 1976 IAU precession back to J2000), Q15.
 * As RA and Dec in millionths of a degree too, if ra or dec are given. */
void ck_sky_zenith(int64_t unix_s, int32_t lat_udeg, int32_t lon_udeg, int16_t d[3], int32_t *ra_udeg,
                   int32_t *dec_udeg);
/* A view centred on (ra, dec) (millionths of a degree), fov degrees wide
 * (thousandths), north up turned by roll (thousandths), built with integer
 * trigonometry. */
void ck_sky_view_at(ck_sky_view *v, int32_t ra_udeg, int32_t dec_udeg, int32_t fov_mdeg, int32_t roll_mdeg);

/* the location setting: unset unless the build sets both (millionths of a degree) */
#if defined(CK_SKY_LAT_UDEG) && defined(CK_SKY_LON_UDEG)
#define CK_SKY_HAVE_LOCATION 1
#else
#define CK_SKY_HAVE_LOCATION 0
#endif
#endif
