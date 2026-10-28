/* Production-path native-pixel regression. Include the renderer so the
 * independent reference can sample the source map before any screen grid.
 * The catalogue override isolates real catalogue stars, not a substitute
 * point primitive. The normal frame uses the complete production catalogue. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../sky_real.h"
static const ck_sky_star *catalogue;
static unsigned catalogue_count;
#define ck_sky_stars catalogue
#define ck_sky_nstars catalogue_count
#include "../sky_real.c"
#undef ck_sky_stars
#undef ck_sky_nstars
/* The pinned rejected oracle has the retired scale argument. There is no
 * production compatibility API; only this historical comparison adapts it. */
#ifdef CK_SKY_BASELINE
#define TEST_DRAW_STARS(cv, scale, ...) ck_sky_draw_stars(cv, scale, __VA_ARGS__)
#else
#define TEST_DRAW_STARS(cv, scale, ...) ck_sky_draw_stars(cv, __VA_ARGS__)
#endif

#define W 3840
#define H 2160
static int failures;
static void check(int ok, const char *name, unsigned value)
{
    printf("%s %s: %u\n", ok ? "PASS" : "FAIL", name, value);
    failures += !ok;
}
static void crop(const char *dir, const char *name, const uint32_t *px, int x, int y)
{
    if (!dir) return;
    char path[1024];
    snprintf(path, sizeof path, "%s/%s.ppm", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) exit(2);
    fprintf(f, "P6\n65 65\n255\n");
    for (int j = y - 32; j <= y + 32; j++)
        for (int i = x - 32; i <= x + 32; i++) {
            uint32_t c = px[(size_t)j * W + i];
            unsigned char rgb[3] = {c >> 16, c >> 8, c};
            fwrite(rgb, 1, 3, f);
        }
    fclose(f);
}
static unsigned footprint(const uint32_t *px, unsigned *height, unsigned *pixels)
{
    int x0 = W, x1 = -1, y0 = H, y1 = -1;
    *pixels = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            if (px[(size_t)y * W + x] & 0xffffff) {
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
                if (y < y0) y0 = y;
                if (y > y1) y1 = y;
                (*pixels)++;
            }
    *height = y1 < y0 ? 0 : (unsigned)(y1 - y0 + 1);
    return x1 < x0 ? 0 : (unsigned)(x1 - x0 + 1);
}
/* Independent display encoding of one direct source-map sample. No
 * production screen renderer, screen-grid cache or interpolator is used. */
static uint32_t reference(const ck_sky_view *v, int x, int y)
{
    static const unsigned char dither[16] = {0,8,2,10,12,4,14,6,3,11,1,9,15,7,13,5};
    int32_t rgb[3];
    milkyway_at(v, 2*x+1, 2*y+1, W, W/2, 864, rgb);
    uint32_t c = 0;
    for (int k = 0; k < 3; k++) {
        unsigned l = (unsigned)rgb[k];
        unsigned v16 = ck_sky_srgb16[l >= 32767 ? 4095 : l >> 3];
        c = c << 8 | ((v16 + dither[(y & 3)*4+(x & 3)]) >> 4);
    }
    return c;
}
int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : NULL;
    uint32_t *pixels = calloc((size_t)W*H, sizeof *pixels);
    uint32_t *expected = calloc((size_t)W*H, sizeof *expected);
    size_t bytes = ck_night_mem_bytes(W,H,512);
    void *mem = malloc(bytes);
    if (!pixels || !expected || !mem) return 2;
    ck_canvas cv = {pixels,W,H}, ref = {expected,W,H};
    ck_rect all = {0,0,W,H};
    ck_night n;
    catalogue = ck_sky_stars;
    catalogue_count = ck_sky_nstars;
#ifdef CK_SKY_BASELINE
    void *scratch = malloc((size_t)W*H*4);
    if (!scratch || ck_night_init(&n,W,H,512,W/2,864,mem,bytes,scratch)) return 2;
#else
    if (ck_night_init(&n,W,H,512,W/2,864,mem,bytes)) return 2;
#endif
    /* Every native pixel, including all off-quarter-grid positions. */
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) expected[(size_t)y*W+x] = reference(&n.view,x,y);
    ck_night_star tw[CK_NIGHT_TWINKLES];
    unsigned ntw;
    TEST_DRAW_STARS(&ref,512,W/2,864,&n.view,tw,CK_NIGHT_TWINKLES,&ntw);
    unsigned mismatch = 0;
    for (size_t i = 0; i < (size_t)W*H; i++) mismatch += expected[i] != n.sky[i];
    check(!mismatch,"native frame versus direct source samples (mismatched pixels)",mismatch);
    crop(dir,"native-map",n.sky,W/2,864);
    /* Pick the brightest real visible catalogue star, retaining its exact
     * subpixel projection, magnitude, colour and twinkle schedule. */
    unsigned selected = ck_sky_nstars;
    int32_t sx=0,sy=0;
    for (unsigned i=0;i<ck_sky_nstars;i++) {
        int16_t d[3] = {ck_sky_stars[i].x,ck_sky_stars[i].y,ck_sky_stars[i].z};
        int32_t x,y;
        if (!ck_sky_project(&n.view,d,W,W/2,864,&x,&y) || x<1024 || y<1024 || x>(W-64)*16 || y>(H-64)*16) continue;
        if (selected==ck_sky_nstars || ck_sky_stars[i].mag<ck_sky_stars[selected].mag) selected=i,sx=x,sy=y;
    }
    if (selected==ck_sky_nstars) return 2;
    printf("catalogue star index=%u mag_code=%u x16=%d y16=%d\n",selected,ck_sky_stars[selected].mag,sx,sy);
    catalogue = &ck_sky_stars[selected]; catalogue_count = 1;
    static const unsigned scales[] = {128,256,512,1024};
    for (unsigned s=0;s<sizeof scales/sizeof scales[0];s++) {
        memset(pixels,0,(size_t)W*H*4);
        TEST_DRAW_STARS(&cv,scales[s],W/2,864,&n.view,tw,CK_NIGHT_TWINKLES,&ntw);
        unsigned height,count,width=footprint(pixels,&height,&count);
        printf("static k=%u width=%u height=%u lit_pixels=%u\n",scales[s],width,height,count);
        check(width>=1 && width<=2 && height>=1 && height<=2,"static native footprint",width);
        check(count>0,"visible static star",count);
        if (scales[s]==512) crop(dir,"static-star",pixels,sx>>4,sy>>4);
        /* Actual night overlay, full native frame, both voice settings and
         * 16 phase samples. Galaxies disabled to isolate the catalogue
         * point source, not to bypass its dynamic renderer. */
        ck_night lights = {0};
        lights.w=W; lights.h=H; lights.k=scales[s]; lights.ntw=ntw;
        memcpy(lights.tw,tw,ntw*sizeof *tw);
        unsigned maxw=0,maxh=0,mincount=~0u;
        for (unsigned voice=0;voice<=256;voice+=256)
            for (unsigned phase=0;phase<16;phase++) {
                memset(pixels,0,(size_t)W*H*4);
                TEST_DRAW_STARS(&cv,scales[s],W/2,864,&n.view,tw,CK_NIGHT_TWINKLES,&ntw);
                lights.t=30000+phase*tw[0].period/16;
                ck_night_lights(&lights,&cv,&all,voice,voice);
                width=footprint(pixels,&height,&count);
                if(width>maxw) maxw=width;
                if(height>maxh) maxh=height;
                if(count<mincount) mincount=count;
                if(scales[s]==512 && voice==256 && phase==4) crop(dir,"twinkle-star",pixels,sx>>4,sy>>4);
            }
        printf("dynamic k=%u max_width=%u max_height=%u min_lit_pixels=%u phases=16 voice=0,256\n",scales[s],maxw,maxh,mincount);
        check(maxw<=2 && maxh<=2 && mincount>0,"twinkle and voice native footprint",maxw);
    }
    free(mem); free(expected); free(pixels);
#ifdef CK_SKY_BASELINE
    free(scratch);
#endif
    printf("SKY_SHARPNESS: %s\n",failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
