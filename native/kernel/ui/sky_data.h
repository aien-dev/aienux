/* sky_data.h -- the real sky as tables: what native/kernel/tools/ck_sky_gen.c
 * writes into sky_data.c (regenerate with scripts/ck_sky_gen.sh; the inputs,
 * their addresses and sha256 are listed there and in NOTICE).
 *
 * Directions are unit vectors in the J2000 equatorial frame (x towards
 * RA 0 Dec 0, z towards the north celestial pole), Q15. Integer only.
 *
 * The Milky Way is stored as a full-sphere equirectangular grid (cols x rows):
 *   col 0..cols-1 covers RA 0..360 degrees (wraps);
 *   row 0 = Dec +90, row rows-1 = Dec -90;
 *   luma at full resolution, chroma at half (cols/2 x rows/2).
 * The generator queries Gaia DR3 (ESA/Gaia/DPAC, CC BY-NC 3.0 IGO) and
 * reddening from gaiadr3.astrophysical_parameters to fill the grid. */
#ifndef AIENOS_SKY_DATA_H
#define AIENOS_SKY_DATA_H
#include <stdint.h>

typedef struct {
    int32_t ra_mdeg, dec_mdeg, fov_mdeg, roll_mdeg; /* the settings, thousandths of a degree */
    int32_t tan_half_q30;                            /* tan(fov / 2): half the width, in tangent-plane units */
    int32_t rot_q30[3][3];                           /* rows: screen right, screen up, the view centre */
} ck_sky_view;

typedef struct {
    int16_t x, y, z; /* the direction, Q15 */
    uint8_t mag;     /* (V + 2) x 20 */
    uint8_t bv;      /* palette index: B-V = -0.40 + 0.02 bv */
} ck_sky_star;

typedef struct {
    uint16_t cols, rows; /* equirectangular grid: cols RA cols, rows Dec rows */
    uint16_t per_width;  /* = cols (kept for renderer: samples per 360 deg RA) */
    int32_t row0_q16;    /* unused (0); kept so the struct is unchanged */
} ck_sky_map;

typedef struct {
    int16_t dir[3];         /* the centre, Q15 */
    int32_t a_q16, b_q16;   /* half the long and short axes on screen, screen widths, Q16 */
    uint16_t angle;         /* the long axis on screen, turns / 65536 from right towards down */
    uint32_t tint;          /* its colour (from its B-V), sRGB */
} ck_sky_galaxy;

typedef struct {
    uint16_t star;     /* index into ck_sky_stars */
    int32_t x16, y16;  /* where the generator puts it on 3840 x 2160, 16ths of a pixel */
} ck_sky_check;

extern const ck_sky_view ck_sky_the_view;
extern const ck_sky_star ck_sky_stars[];
extern const unsigned ck_sky_nstars;
extern const uint32_t ck_sky_flux[256];
extern const uint16_t ck_sky_palette[128][3];
extern const uint16_t ck_sky_lin[256];
extern const uint16_t ck_sky_srgb16[4096];
extern const ck_sky_map ck_sky_milkyway;
extern const uint8_t ck_sky_luma[];
extern const int8_t ck_sky_chroma[][2];
extern const ck_sky_galaxy ck_sky_galaxies[];
extern const unsigned ck_sky_ngalaxies;
extern const ck_sky_check ck_sky_checks[];
extern const unsigned ck_sky_nchecks;
#endif
