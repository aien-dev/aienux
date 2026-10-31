/* allen_font.h -- Allen's typeface: every printable ASCII character drawn as
 * round-capped pen strokes (lines and elliptical arcs) on a design grid, and
 * rasterized on demand into anti-aliased alpha masks at any size. Integer math
 * only (the kernel builds -mgeneral-regs-only); no font files, no outside
 * data. Pure: no MMIO, host tested in ui/tests/allen_ui_test.c.
 *
 * Design grid: cap height and ascenders at y=0, lowercase x-height at y=8,
 * baseline at y=20, descenders to y=27; x grows right, y grows down. */
#ifndef AIENOS_ALLEN_FONT_H
#define AIENOS_ALLEN_FONT_H
#include <stddef.h>
#include <stdint.h>

#define CK_AF_FIRST 32
#define CK_AF_LAST 126
#define CK_AF_COUNT (CK_AF_LAST - CK_AF_FIRST + 1)

typedef struct {
    uint8_t *alpha; /* w*h coverage, 0..255; NULL when not rasterized yet */
    uint16_t w, h;
    int16_t ox, oy; /* mask top-left relative to the pen on the baseline, px */
} ck_af_glyph;

typedef struct {
    uint32_t s256;  /* 1/256 px per design unit */
    uint32_t hw256; /* half the stroke width, 1/256 px */
    uint8_t *pool;  /* mask storage, filled as glyphs are first used */
    size_t pool_len, pool_used;
    ck_af_glyph g[CK_AF_COUNT];
} ck_af_face;

/* cap_px: cap height in pixels; weight: half the stroke width in
 * thousandths of a design unit (1250 = regular). pool holds the masks. */
void ck_af_face_init(ck_af_face *f, unsigned cap_px, unsigned weight, uint8_t *pool, size_t pool_len);
/* The mask for c (anything outside 32..126 draws as '?'); NULL only when the
 * pool is full. Rasterized on first use. */
const ck_af_glyph *ck_af_glyph_get(ck_af_face *f, char c);
/* Advance after c, 1/256 px. */
uint32_t ck_af_advance(const ck_af_face *f, char c);
/* Width of the first n characters of s, whole px (rounded up). */
uint32_t ck_af_measure(const ck_af_face *f, const char *s, size_t n);
/* Integer square root (floor). */
uint64_t ck_af_isqrt(uint64_t v);
/* sin and cos of whole degrees (any sign), scaled by 16384. */
int32_t ck_af_sin(int32_t deg);
int32_t ck_af_cos(int32_t deg);

#endif
