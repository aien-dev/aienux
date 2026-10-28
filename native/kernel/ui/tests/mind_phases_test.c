/* Production-renderer regression: native pixel crops, not rescaled previews.
 * A hotspot is an Allen-changed final pixel with both red and green >=192
 * (the upper sRGB quarter; white includes blue>=192, yellow has lower blue).
 * On black, any red/green-dominant warm pixel also fails at any brightness.
 * On sky, count new/worsened hotspots, not an unchanged background star.
 * Cool points must fit a 5x5 native footprint including coverage; the heart
 * fits the same footprint. These are geometric contracts, not area allowances.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../mind.h"
#include "../night.h"

enum { W=3840, H=2160, K=512, CX=1920, CY=864, CROP=768 };
static uint32_t *sky, *image;
static unsigned failures, frames, yellow_pixels, white_pixels;
static const char *directory;
static int halo_control;
static void check(int good, const char *name) { if (!good) { failures++; if (failures<20) fprintf(stderr,"FAIL %s\n",name); } }
static int hot(uint32_t c) { return ck_ch(c,16)>=192 && ck_ch(c,8)>=192; }
static void save(const char *name, const uint32_t *p) {
    if (!directory) return;
    char path[1024]; snprintf(path,sizeof path,"%s/%s.ppm",directory,name);
    FILE *f=fopen(path,"wb"); if (!f) { check(0,"frame open"); return; }
    fprintf(f,"P6\n%d %d\n255\n",CROP,CROP);
    for(int y=CY-CROP/2;y<CY+CROP/2;y++) for(int x=CX-CROP/2;x<CX+CROP/2;x++) {
        uint32_t c=p[(size_t)y*W+x]; unsigned char rgb[3]={c>>16,c>>8,c}; fwrite(rgb,1,3,f);
    }
    fclose(f);
}
static unsigned measure(const ck_mind *m, const uint32_t *base, unsigned *lit, unsigned *annulus) {
    unsigned bad=0; *lit=*annulus=0;
    int64_t radius=0;
    for(unsigned i=0;i<CK_MIND_NODES;i++) {
        int64_t dx=m->px[i]-m->hx,dy=m->py[i]-m->hy;
        int64_t d=ck_isqrt((uint64_t)(dx*dx+dy*dy)); if(d>radius) radius=d;
    }
    radius+=192; /* historical largest point plus coverage: annulus control only */
    ck_rect box=ck_grow(m->box,16), canvas={0,0,W,H}; ck_clip(&box,&canvas);
    for(int y=box.y;y<box.y+box.h;y++) for(int x=box.x;x<box.x+box.w;x++) {
        size_t p=(size_t)y*W+x; uint32_t a=image[p],b=base?base[p]:0;
        if(a==b) continue;
        (*lit)++;
        int64_t dx=x*16+8-m->hx,dy=y*16+8-m->hy;
        if(dx*dx+dy*dy>radius*radius) (*annulus)++;
        /* Inspect final composite, not merely the subtracted light's colour. */
        int warm_black=!base && ck_ch(a,16)>ck_ch(a,0) && ck_ch(a,8)>ck_ch(a,0);
        if(warm_black || (hot(a) && (!hot(b) || ck_ch(a,16)>ck_ch(b,16) || ck_ch(a,8)>ck_ch(b,8)))) {
            bad++;
            if(ck_ch(a,0)>=192) white_pixels++; else yellow_pixels++;
        }
    }
    return bad;
}
static void render(const ck_mind *m,const uint32_t *base) {
    ck_rect box=ck_grow(m->box,16),canvas={0,0,W,H}; ck_clip(&box,&canvas);
    for(int y=box.y;y<box.y+box.h;y++) {
        uint32_t *p=image+(size_t)y*W+box.x;
        if(base) memcpy(p,base+(size_t)y*W+box.x,(size_t)box.w*4); else memset(p,0,(size_t)box.w*4);
    }
    ck_canvas cv={image,W,H}; ck_mind_draw(m,&cv,&canvas);
    if(halo_control)
        ck_glow(&cv,&canvas,m->hx,m->hy,m->R*16*2,CK_RGB(60,74,210),64);
}
static void isolated(int control) {
    ck_mind m; ck_mind_init(&m,CX,CY,K,0); ck_mind_at(&m,6000);
    int32_t heart_radius=m.hr;
    m.ne=m.nspark=0; memset(m.lum,0,sizeof m.lum); m.hr=0;
    for(unsigned i=0;i<CK_MIND_NODES;i++) {m.px[i]=CX*16+8;m.py[i]=CY*16+8;}
    m.lum[0]=256; m.near[0]=256; m.act[0]=0;
    render(&m,0);
    unsigned lit,ann; unsigned bad=measure(&m,0,&lit,&ann);
    check(!bad,"isolated whole-point brightness"); check(lit>0 && lit<=25,"native point footprint <=5x5, visible");
    printf("POINT lit=%u hot=%u\n",lit,bad);
    m.lum[0]=0; m.hr=control==2?160:heart_radius; m.heart=256; render(&m,0);
    bad=measure(&m,0,&lit,&ann); check(!bad,"isolated white heart"); check(lit>0 && lit<=25,"native heart footprint <=5x5, visible");
    printf("HEART lit=%u hot=%u\n",lit,bad);
    /* Isolate real moving spark head/tail geometry as well as heart/nodes. */
    m.hr=0; m.nspark=1; m.spark[0].e=0; m.spark[0].dur=240; m.spark[0].t0=m.t;
    render(&m,0); bad=measure(&m,0,&lit,&ann);
    check(!bad && lit>0 && lit<=25,"native cool spark/tail footprint <=5x5");
    printf("SPARK lit=%u hot=%u\n",lit,bad);
    m.nspark=0; m.hr=heart_radius;
    /* Synthetic maximum-overlap raster stress, not a reachable-history claim. */
    for(unsigned i=0;i<CK_MIND_NODES;i++) m.lum[i]=m.act[i]=256;
    render(&m,0); bad=measure(&m,0,&lit,&ann);
    check(!bad && lit<=25,"overlapping points do not accumulate white or broaden");
    if(control==1) {
        ck_canvas cv={image,W,H}; ck_rect clip={0,0,W,H};
        ck_glow(&cv,&clip,m.hx,m.hy,160,CK_RGB(255,250,240),256);
        bad=measure(&m,0,&lit,&ann); check(!bad,"explicit warm/white injected control (must fail)");
    }
}
int main(int argc,char **argv) {
    directory=argc>1 && strcmp(argv[1],"-")?argv[1]:0;
    int control=argc>2 && !strcmp(argv[2],"--warm-control")?1:
        argc>2 && !strcmp(argv[2],"--large-disc-control")?2:0;
    int suppressed=argc>2 && !strcmp(argv[2],"--suppressed-control");
    halo_control=argc>2 && !strcmp(argv[2],"--halo-control");
    size_t bytes=(size_t)W*H*4;
    sky=calloc(1,bytes); image=calloc(1,bytes);
    size_t nb=ck_night_mem_bytes(W,H,K); void *storage=malloc(nb); static ck_night n;
    if(!sky||!image||!storage) return 2;
    check(!ck_night_init(&n,W,H,K,CX,CY,storage,nb),"galaxy init");
    ck_canvas bg={sky,W,H}; ck_rect clip={0,0,W,H}; ck_night_at(&n,30000); ck_night_base(&n,&bg,&clip); ck_night_lights(&n,&bg,&clip,0,0);
    isolated(control);
    for(unsigned seed=0;seed<2;seed++) for(int initial=0;initial<4;initial++) {
        ck_mind m; ck_mind_init(&m,CX,CY,K,0); m.rng^=seed; ck_mind_mode(&m,initial,0);
        unsigned badblack=0,badsky=0,annuli=0,visible=0,sparks=0;
        for(uint32_t t=0;t<=12000;t+=30) {
            /* Formation, settled cadence, then each transition with live residual sparks. */
            if(t>=7500 && t%1500==0) ck_mind_mode(&m,(m.mode+1)&3,t);
            ck_mind_voice(&m,(t/300)%3*128);
            if(t%300==0) {ck_mind_word(&m,t);ck_mind_key(&m,t);}
            ck_mind_at(&m,t); sparks+=m.nspark;
            if(suppressed) m.alpha=0;
            unsigned lit,ann; render(&m,0); unsigned b=measure(&m,0,&lit,&ann); badblack+=b; annuli+=ann;
            if(t>=3000) {check(lit>CK_MIND_NODES,"meaningful visible network, not alpha zero"); visible+=lit;}
            render(&m,sky); badsky+=measure(&m,sky,&lit,&ann); annuli+=ann;
            if(seed==0 && t==6000) {
                char name[64];
                memcpy(image,sky,bytes); render(&m,sky);
                snprintf(name,sizeof name,"mode-%d-galaxy-native",initial);save(name,image);
                memset(image,0,bytes);render(&m,0);
                snprintf(name,sizeof name,"mode-%d-black-native",initial);save(name,image);
            }
            frames++;
        }
        printf("PHASE seed=%u initial=%d black_hot=%u sky_hot=%u annulus=%u visible=%u sparks=%u\n",seed,initial,badblack,badsky,annuli,visible,sparks);
        check(!badblack,"no interior warm/white on black");check(!badsky,"no new/worsened final-composite warm/white on identical sky");check(!annuli,"no exterior light");check(sparks>0 && visible>0,"live sparks and network retained");
    }
    printf("MIND_PHASES %s frames=%u yellow_pixels=%u white_pixels=%u failures=%u\n",
           failures?"FAIL":"PASS",frames,yellow_pixels,white_pixels,failures);
    free(storage);free(sky);free(image);return failures?1:0;
}
