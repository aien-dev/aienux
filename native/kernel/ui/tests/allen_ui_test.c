/* allen_ui_test.c -- host test for Allen's home (ui/allen_ui.c with the night
 * sky, ui/night.c, and his presence, ui/mind.c), his typeface
 * (ui/allen_font.c) and his first answers (svc/allen_reply.c). Runs the
 * startup at 3840 x 2160 BGRX on a hand-moved clock, wakes Allen, types
 * "Hello Allen", lets him answer, leaves him alone, saves a note, says goodbye
 * (the dusk: everything falls into the black hole, the disk glows, the horizon
 * closes to black) and wakes him; checks the pixels at each step, checks that drawing only the changed parts gives the
 * same picture as drawing everything, runs 1280 x 800 RGBX with and without
 * three helper threads (the frames must match exactly), and writes PNG
 * pictures to the directory given as argv[1] (if any) for a person to look at.
 * Then the setup wizard (svc/setup_wizard.c) at 1920 x 1080: its choices,
 * the lit one moving, the scan lines easing in, words held with a question.
 * Final line: ALLEN_UI_TEST: PASS|FAIL (N checks). */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../../svc/allen_intent.h"
#include "../../svc/allen_reply.h"
#include "../allen_font.h"
#include "../allen_ui.h"
#include "../flow.h"
#include "../mind.h"
#include "../night.h"
#include "../setup_view.h"

static int checks, fails;
static const char help_reply[] =
    "I remember what you teach me and keep notes. Goodbye turns the display off, "
    "or powers off if display sleep is unavailable.";
static unsigned flow_marks;
static void flow_mark(ck_rect r)
{
    if (r.w > 0 && r.h > 0)
        flow_marks++;
}
#define CHECK(cond, what)                                                                                     \
    do {                                                                                                     \
        checks++;                                                                                            \
        if (!(cond)) {                                                                                       \
            fails++;                                                                                         \
            printf("FAIL  %s\n", what);                                                                      \
        }                                                                                                    \
    } while (0)

/* ---- a minimal PNG writer (stored deflate blocks), host only ---- */
static uint32_t crc_table[256];
static void crc_init(void)
{
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++)
            c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}
static uint32_t crc(uint32_t c, const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++)
        c = crc_table[(c ^ b[i]) & 255] ^ (c >> 8);
    return c;
}
static void be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v;
}
static void chunk(FILE *f, const char *type, const uint8_t *data, size_t n)
{
    uint8_t h[8];
    be32(h, (uint32_t)n);
    memcpy(h + 4, type, 4);
    fwrite(h, 1, 8, f);
    if (n)
        fwrite(data, 1, n, f);
    uint32_t c = crc(0xffffffffu, (const uint8_t *)type, 4);
    c = crc(c, data, n) ^ 0xffffffffu;
    be32(h, c);
    fwrite(h, 1, 4, f);
}
/* pixels: 0x00RRGGBB (format 1) rows of `pitch`; region x,y,w,h; every `step`th pixel */
static void png(const char *path, const uint32_t *px, uint32_t pitch, int fmt, uint32_t x0, uint32_t y0, uint32_t w,
                uint32_t h, uint32_t step)
{
    uint32_t ow = w / step, oh = h / step;
    size_t raw_n = (size_t)oh * (1 + (size_t)ow * 3);
    uint8_t *raw = malloc(raw_n);
    size_t k = 0;
    for (uint32_t y = 0; y < oh; y++) {
        raw[k++] = 0;
        for (uint32_t x = 0; x < ow; x++) {
            uint32_t sr = 0, sg = 0, sb = 0;
            for (uint32_t j = 0; j < step; j++)
                for (uint32_t i = 0; i < step; i++) {
                    uint32_t c = px[(size_t)(y0 + y * step + j) * pitch + x0 + x * step + i];
                    sr += c >> 16 & 255, sg += c >> 8 & 255, sb += c & 255;
                }
            uint8_t r = (uint8_t)(sr / (step * step)), g = (uint8_t)(sg / (step * step)), b = (uint8_t)(sb / (step * step));
            if (fmt == 0) {
                uint8_t t = r;
                r = b;
                b = t;
            }
            raw[k++] = r, raw[k++] = g, raw[k++] = b;
        }
    }
    size_t blocks = raw_n / 65535 + 1;
    size_t z_n = 2 + raw_n + blocks * 5 + 4;
    uint8_t *z = malloc(z_n);
    size_t o = 0;
    z[o++] = 0x78, z[o++] = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_n;) {
        size_t n = raw_n - i > 65535 ? 65535 : raw_n - i;
        z[o++] = i + n == raw_n;
        z[o++] = (uint8_t)n, z[o++] = (uint8_t)(n >> 8), z[o++] = (uint8_t)~n, z[o++] = (uint8_t)(~n >> 8);
        memcpy(z + o, raw + i, n);
        for (size_t j = 0; j < n; j++) {
            a = (a + raw[i + j]) % 65521;
            b = (b + a) % 65521;
        }
        o += n;
        i += n;
    }
    be32(z + o, b << 16 | a);
    o += 4;
    FILE *f = fopen(path, "wb");
    if (!f) {
        printf("note: cannot write %s\n", path);
        free(raw), free(z);
        return;
    }
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    uint8_t ihdr[13];
    be32(ihdr, ow), be32(ihdr + 4, oh);
    ihdr[8] = 8, ihdr[9] = 2, ihdr[10] = 0, ihdr[11] = 0, ihdr[12] = 0;
    chunk(f, "IHDR", ihdr, 13);
    chunk(f, "IDAT", z, o);
    chunk(f, "IEND", 0, 0);
    fclose(f);
    free(raw), free(z);
    printf("note: wrote %s (%ux%u)\n", path, ow, oh);
}

/* ---- looking at a picture: counts over a rectangle of 0x00RRGGBB pixels ---- */
struct pic {
    const uint32_t *px;
    uint32_t pitch;
    int fmt; /* 0: red in the low byte (RGBX) */
};
static void rgb(const struct pic *p, uint32_t x, uint32_t y, int *r, int *g, int *b)
{
    uint32_t c = p->px[(size_t)y * p->pitch + x];
    *r = c >> 16 & 255, *g = c >> 8 & 255, *b = c & 255;
    if (p->fmt == 0) {
        int t = *r;
        *r = *b;
        *b = t;
    }
}
enum { BRIGHT, BLACK, LIT };
static unsigned count(const struct pic *p, uint32_t x0, uint32_t y0, uint32_t w, uint32_t h, int kind)
{
    unsigned n = 0;
    for (uint32_t y = y0; y < y0 + h; y++)
        for (uint32_t x = x0; x < x0 + w; x++) {
            int r, g, b;
            rgb(p, x, y, &r, &g, &b);
            if (kind == BRIGHT)
                n += r > 150 && g > 150 && b > 150;
            else if (kind == BLACK)
                n += r == 0 && g == 0 && b == 0;
            else
                n += r > 60 || g > 60 || b > 90;
        }
    return n;
}
/* the light in a rectangle: r + g + b summed over every fourth pixel each way */
static uint64_t light(const struct pic *p, uint32_t x0, uint32_t y0, uint32_t w, uint32_t h)
{
    uint64_t s = 0;
    for (uint32_t y = y0; y < y0 + h; y += 4)
        for (uint32_t x = x0; x < x0 + w; x += 4) {
            int r, g, b;
            rgb(p, x, y, &r, &g, &b);
            s += (uint64_t)(r + g + b);
        }
    return s;
}
/* pixels of a and b (w x h, rows `pitch`) that differ by more than tol in any channel */
static unsigned differ(const uint32_t *a, const uint32_t *b, uint32_t pitch, uint32_t w, uint32_t h, int tol)
{
    unsigned n = 0;
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint32_t c = a[(size_t)y * pitch + x], d = b[(size_t)y * pitch + x];
            for (int s = 0; s < 24; s += 8) {
                int e = (int)(c >> s & 255) - (int)(d >> s & 255);
                if (e > tol || e < -tol) {
                    n++;
                    break;
                }
            }
        }
    return n;
}

/* the frames are timed by the wall clock; ALLEN_UI_CLOCK=cpu times them by this thread's CPU time instead,
 * which is the fairer figure on a busy host */
static int cpu_clock;
/* The injected frame clock runs at begin and after helpers/presentation,
 * before the completed frame is published. No test-only renderer hook. */
static unsigned black_probe, black_probe_calls, black_probe_early, wake_during_frame;
static uint64_t real_us(void)
{
    if (black_probe) {
        black_probe_calls++;
        black_probe_early += ck_allen_ui_asleep() != 0;
    }
    if (wake_during_frame && !--wake_during_frame)
        ck_allen_ui_sleep(0, 0);
    struct timespec ts;
    clock_gettime(cpu_clock ? CLOCK_THREAD_CPUTIME_ID : CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

/* the screen's clock in these checks: microseconds, moved by hand */
static uint64_t now_us;
static void run_to(uint32_t ms)
{
    for (; now_us <= (uint64_t)ms * 1000u; now_us += 16000)
        ck_allen_ui_tick(now_us);
}

/* the frame drawn by changed parts against the whole screen drawn again at the same moment */
static unsigned stale(uint32_t *fb, uint32_t *copy, uint32_t pitch, uint32_t w, uint32_t h)
{
    memcpy(copy, fb, (size_t)pitch * h * 4);
    ck_allen_ui_refresh();
    ck_allen_ui_tick(now_us - 16000);
    return differ(fb, copy, pitch, w, h, 6);
}

/* The bare night sky as Allen's screen draws it (same scale and centre) with
 * nothing in front of it, and the pixels in a rectangle of the screen that
 * differ from it at the moment of the last frame: only what the screen added. */
static ck_night bare_night;
static int bare_init(uint32_t w, uint32_t h)
{
    uint32_t k = w * 256u / 1920u;
    size_t nb = ck_night_mem_bytes(w, h, k);
    return ck_night_init(&bare_night, w, h, k, (int32_t)w / 2, (int32_t)(h * 40 / 100), malloc(nb), nb);
}
static unsigned added(const struct pic *p, uint32_t *sky, uint32_t x0, uint32_t y0, uint32_t w, uint32_t h)
{
    ck_canvas cv = {sky, (int32_t)p->pitch, bare_night.h};
    ck_rect r = {(int32_t)x0, (int32_t)y0, (int32_t)w, (int32_t)h};
    ck_night_at(&bare_night, (uint32_t)((now_us - 16000) / 1000));
    ck_night_base(&bare_night, &cv, &r);
    ck_night_lights(&bare_night, &cv, &r, 0, 0);
    return differ(p->px + (size_t)y0 * p->pitch + x0, sky + (size_t)y0 * p->pitch + x0, p->pitch, w, h, 24);
}
/* Visibility must come from the being, not unrelated stars or the retired
 * warm/white broad discs. Use the same current native-sky baseline as added. */
static unsigned cool_added(const struct pic *p, uint32_t *sky, uint32_t x0, uint32_t y0, uint32_t w, uint32_t h)
{
    ck_canvas cv = {sky, (int32_t)p->pitch, bare_night.h};
    ck_rect area = {(int32_t)x0, (int32_t)y0, (int32_t)w, (int32_t)h};
    ck_night_at(&bare_night, (uint32_t)((now_us - 16000) / 1000));
    ck_night_base(&bare_night, &cv, &area);
    ck_night_lights(&bare_night, &cv, &area, 0, 0);
    unsigned visible = 0;
    for (uint32_t y = y0; y < y0 + h; y++)
        for (uint32_t x = x0; x < x0 + w; x++) {
            int r, g, b;
            rgb(p, x, y, &r, &g, &b);
            unsigned base_blue = sky[(size_t)y * p->pitch + x] & 255u;
            visible += b > g && g > r && b > (int)base_blue + 24;
        }
    return visible;
}

static volatile int helpers_run;
static void *helper(void *arg)
{
    (void)arg;
    while (helpers_run)
        ck_allen_ui_help();
    return 0;
}

/* ---- the setup wizard on the screen (svc/setup_wizard.c through ui/setup_view.h), 1920 x 1080 ---- */
#define CHECKF(cond, ...)                                                                                      \
    do {                                                                                                       \
        checks++;                                                                                              \
        if (!(cond)) {                                                                                         \
            fails++;                                                                                           \
            printf("FAIL  ");                                                                                  \
            printf(__VA_ARGS__);                                                                               \
            printf("\n");                                                                                     \
        }                                                                                                      \
    } while (0)
static sw_wizard wiz;
static unsigned wiz_said;
static void wiz_show(void) { ck_setup_show(sw_view_of(&wiz), &wiz_said, sw_step_of(&wiz) == SW_DONE, now_us); }
static void wiz_key(int k, char c)
{
    sw_key(&wiz, k, c, (uint32_t)(now_us / 1000));
    wiz_show();
}
static void wiz_run(uint32_t ms) /* the wizard's clock and the screen's together */
{
    uint32_t end = (uint32_t)(now_us / 1000) + ms;
    while (now_us / 1000 < end) { /* as run_to: the last frame is 16 ms before now_us */
        if (sw_tick(&wiz, (uint32_t)(now_us / 1000)))
            wiz_show();
        ck_allen_ui_tick(now_us);
        now_us += 16000;
    }
}

static void wizard_screens(const char *dir)
{
    char path[512];
    static char text[4096];
    FILE *f = fopen("native/kernel/svc/tests/fixtures/setup/games-pc.txt", "rb");
    CHECKF(f != 0, "wizard: the games PC fixture opens (run from the repository root)");
    if (!f)
        return;
    unsigned n = (unsigned)fread(text, 1, sizeof text, f);
    fclose(f);
    static si_inv inv;
    si_init(&inv);
    si_parse(&inv, text, n);
    uint32_t W = 1920, H = 1080;
    uint32_t *fb = calloc((size_t)W * H, 4), *copy = malloc((size_t)W * H * 4);
    size_t need = ck_allen_ui_mem_bytes(W, H);
    void *mem = malloc(need);
    struct pic P = {fb, W, 1};
    now_us = 0;
    CHECKF(ck_allen_ui_start(fb, W, H, W, 1, mem, need, 1000000, 0, 0) == 0, "wizard: the window starts at 1920x1080");
    run_to(9000);
    ck_allen_ui_wake(0, now_us);
    wiz_said = ~0u;
    sw_start(&wiz, &inv, 1, 0, 0, (uint32_t)(now_us / 1000));
    wiz_show();
    wiz_run(7000);
    CHECKF(sw_step_of(&wiz) == SW_INTENT, "wizard: hello, then the one question");
    wiz_run(5000);
    /* the row of choices sits where the bar is: three pills, the first lit */
    uint32_t ry = H - 150 - 4, rh = 62;
    unsigned row = count(&P, 0, ry, W, rh, BRIGHT);
    CHECKF(row > 400, "wizard: the choices are drawn where the bar sits (%u bright)", row);
    unsigned left = count(&P, 0, ry, W / 2 - 200, rh, LIT), right = count(&P, W / 2 + 200, ry, W / 2 - 200, rh, LIT);
    CHECKF(left > right, "wizard: the first choice is the lit one (%u against %u)", left, right);
    unsigned hint = count(&P, 400, H - 150 + 54 + 6, W - 800, 40, LIT);
    CHECKF(hint > 50, "wizard: the lit choice's line is under the row (%u)", hint);
    unsigned st = stale(fb, copy, W, W, H);
    CHECKF(st < 40, "wizard: the question drawn by changed parts is the whole picture (%u differ)", st);
    if (dir) {
        snprintf(path, sizeof path, "%s/wizard-1-intent.png", dir);
        png(path, fb, W, 1, 0, 0, W, H, 1);
    }
    wiz_key(SW_K_RIGHT, 0);
    wiz_run(600);
    unsigned left2 = count(&P, 0, ry, W / 2 - 200, rh, LIT), mid = count(&P, W / 2 - 200, ry, 400, rh, LIT);
    CHECKF(left2 < left && mid > 0, "wizard: the light moves to the next choice (%u -> %u)", left, left2);
    wiz_key(SW_K_LEFT, 0);
    wiz_key(SW_K_ENTER, 0);
    /* getting to know the computer: the lines come one after another */
    wiz_run(1200);
    uint32_t py = (uint32_t)(H * 40 / 100 + 250 + 40), ph = 220;
    unsigned early = count(&P, 200, py, W - 400, ph, BRIGHT);
    wiz_run(3000);
    unsigned later = count(&P, 200, py, W - 400, ph, BRIGHT);
    CHECKF(later > early + 200, "wizard: the scan's lines ease in one at a time (%u then %u)", early, later);
    if (dir) {
        snprintf(path, sizeof path, "%s/wizard-2-scan.png", dir);
        png(path, fb, W, 1, 0, 0, W, H, 1);
    }
    wiz_run(4000);
    CHECKF(sw_step_of(&wiz) == SW_CONSENT, "wizard: the consent comes by itself");
    wiz_run(4000);
    st = stale(fb, copy, W, W, H);
    CHECKF(st < 40, "wizard: the consent drawn by changed parts is the whole picture (%u differ)", st);
    if (dir) {
        snprintf(path, sizeof path, "%s/wizard-3-consent.png", dir);
        png(path, fb, W, 1, 0, 0, W, H, 1);
    }
    wiz_key(SW_K_ENTER, 0);
    wiz_run(5000);
    if (dir) {
        snprintf(path, sizeof path, "%s/wizard-4-games.png", dir);
        png(path, fb, W, 1, 0, 0, W, H, 1);
    }
    /* his words stay while the question is up, however long they wait */
    unsigned words = count(&P, 300, (uint32_t)(H * 40 / 100 + 200), W - 600, 70, BRIGHT);
    wiz_run(20000);
    unsigned words_later = count(&P, 300, (uint32_t)(H * 40 / 100 + 200), W - 600, 70, BRIGHT);
    CHECKF(words > 100 && words_later * 10 > words * 9, "wizard: Allen's words stay with the question (%u, %u)", words,
          words_later);
    wiz_key(SW_K_ENTER, 0);
    wiz_key(SW_K_CHAR, '2'); /* choose another */
    wiz_key(SW_K_CHAR, '2'); /* connect my own */
    wiz_key(SW_K_CHAR, '1'); /* Claude */
    for (const char *k = "sk-not-a-real-key"; *k; k++)
        wiz_key(SW_K_CHAR, *k);
    wiz_run(3000);
    CHECKF(sw_step_of(&wiz) == SW_KEY, "wizard: at the key");
    unsigned bar = count(&P, W / 2 - 360, H - 150, 720, 54, BRIGHT);
    CHECKF(bar > 30, "wizard: the bar is back for typing, the key as dots (%u)", bar);
    if (dir) {
        snprintf(path, sizeof path, "%s/wizard-5-key.png", dir);
        png(path, fb, W, 1, 0, 0, W, H, 1);
    }
    wiz_key(SW_K_ESC, 0);
    wiz_key(SW_K_ESC, 0);
    wiz_key(SW_K_ESC, 0);
    wiz_run(4000);
    CHECKF(sw_step_of(&wiz) == SW_READY, "wizard: ready");
    if (dir) {
        snprintf(path, sizeof path, "%s/wizard-6-ready.png", dir);
        png(path, fb, W, 1, 0, 0, W, H, 1);
    }
    wiz_key(SW_K_ENTER, 0);
    wiz_run(3000);
    CHECKF(count(&P, 0, ry, W, rh, BRIGHT) < row / 4, "wizard: done, the choices have gone");
    st = stale(fb, copy, W, W, H);
    CHECKF(st < 40, "wizard: after the panel went, changed parts are the whole picture (%u differ)", st);
    free(fb), free(copy), free(mem);
}

/* Compare the exact same native galaxy pixels, not an unrelated star patch.
 * The annulus starts beyond every projected node plus the largest sharp
 * primitive and its AA edge. Threads and travelling sparks stay between nodes. */
static void mind_no_halo(const char *dir)
{
    enum { W = 3840, H = 2160, K = 512, CX = W / 2, CY = H * 40 / 100 };
    size_t bytes = (size_t)W * H * sizeof(uint32_t);
    uint32_t *background = malloc(bytes), *image = malloc(bytes);
    size_t nb = ck_night_mem_bytes(W, H, K);
    void *storage = malloc(nb);
    static ck_night night;
    CHECK(background && image && storage, "mind: native frame allocation");
    if (!background || !image || !storage) {
        free(background), free(image), free(storage);
        return;
    }
    CHECK(!ck_night_init(&night, W, H, K, CX, CY, storage, nb), "mind: native galaxy init");
    ck_canvas bg = {background, W, H}, cv = {image, W, H};
    ck_rect clip = {0, 0, W, H};
    ck_night_at(&night, 30000);
    ck_night_base(&night, &bg, &clip);
    ck_night_lights(&night, &bg, &clip, 0, 0);
    for (int mode = CK_MIND_IDLE; mode <= CK_MIND_SPEAK; mode++) {
        ck_mind mind;
        ck_mind_init(&mind, CX, CY, K, 0);
        ck_mind_mode(&mind, mode, 0);
        ck_mind_voice(&mind, 256);
        for (uint32_t t = 0; t <= 6000; t += 100)
            ck_mind_at(&mind, t);
        ck_mind_word(&mind, 6000);
        ck_mind_key(&mind, 6000);
        ck_mind_at(&mind, 6100);
        int64_t radius = 0;
        for (unsigned i = 0; i < CK_MIND_NODES; i++) {
            int64_t dx = mind.px[i] - mind.hx, dy = mind.py[i] - mind.hy;
            int64_t d = (int64_t)ck_isqrt((uint64_t)(dx * dx + dy * dy));
            if (d > radius)
                radius = d;
        }
        /* S(5) is the largest spark radius; two pixels cover rounding/AA. */
        radius += 5 * K / 256 * 16 + 32;
        int64_t outer = (int64_t)mind.R * 16 * 2;
        memcpy(image, background, bytes);
        ck_mind_draw(&mind, &cv, &clip);
        unsigned annulus = 0, being = 0, control = 0;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                int64_t dx = x * 16 + 8 - mind.hx, dy = y * 16 + 8 - mind.hy;
                int64_t d2 = dx * dx + dy * dy;
                size_t p = (size_t)y * W + x;
                int positive = 0;
                for (int s = 0; s < 24; s += 8)
                    positive |= (image[p] >> s & 255) > (background[p] >> s & 255);
                if (d2 > radius * radius && d2 < outer * outer)
                    annulus += positive;
                if (d2 <= radius * radius)
                    being += positive;
            }
        CHECK(!annulus, "mind: no positive difference outside true being on identical galaxy");
        CHECK(being > 0, "mind: sharp being still present");
        if (dir) {
            char path[512];
            snprintf(path, sizeof path, "%s/mind-no-halo-%d.png", dir, mode);
            png(path, image, W, 1, 0, 0, W, H, 1);
        }
        /* Historical whole-mind glow, kept only as a detector control. */
        memcpy(image, background, bytes);
        ck_glow(&cv, &clip, mind.hx, mind.hy, mind.R * 16 * 17 / 10,
                CK_RGB(60, 74, 210), (52 + mind.heart / 8) * mind.alpha >> 8);
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                int64_t dx = x * 16 + 8 - mind.hx, dy = y * 16 + 8 - mind.hy;
                int64_t d2 = dx * dx + dy * dy;
                size_t p = (size_t)y * W + x;
                if (d2 > radius * radius && d2 < outer * outer)
                    control += image[p] != background[p];
            }
        CHECK(control > 0, "mind: historical glow positive control lights the annulus");
        printf("MIND_NO_HALO mode=%d center=%d,%d inner16=%lld outer16=%lld annulus_added=%u being_added=%u old_glow_added=%u box=%d,%d,%d,%d\n",
               mode, CX, CY, (long long)radius, (long long)outer, annulus, being, control,
               mind.box.x, mind.box.y, mind.box.w, mind.box.h);
    }
    /* Isolate an actual native-sized cool point, not a broad flat disc.
     * All other geometry is dark; this still calls the production renderer. */
    ck_mind point;
    ck_mind_init(&point, CX, CY, K, 0);
    ck_mind_at(&point, 6000);
    point.ne = point.nspark = 0;
    point.hr = 0;
    memset(point.lum, 0, sizeof point.lum);
    point.px[0] = CX * 16 + 8;
    point.py[0] = CY * 16 + 8;
    point.near[0] = 0;
    point.lum[0] = 256;
    point.act[0] = 0;
    point.box = clip;
    memset(image, 0, bytes);
    ck_mind_draw(&point, &cv, &clip);
    uint32_t centre = image[(size_t)CY * W + CX];
    CHECK(centre != 0, "mind: isolated point is present");
    CHECK(centre == CK_RGB(32, 72, 144), "mind: unactivated point retains its full cool core");
    unsigned footprint = 0;
    for (size_t i = 0; i < (size_t)W * H; i++)
        footprint += image[i] != 0;
    CHECK(footprint > 0 && footprint <= 25, "mind: isolated point has bounded native support");
    CHECK(image[(size_t)CY * W + CX + 1] == image[(size_t)CY * W + CX - 1] &&
              image[(size_t)CY * W + CX + 1] == image[(size_t)(CY + 1) * W + CX],
          "mind: native point edge coverage is symmetric");
    CHECK(!image[(size_t)CY * W + CX + 2], "mind: no point light outside the native edge");
    free(background), free(image), free(storage);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : 0;
    char path[512];
    crc_init();
    if (argc > 1 && !strcmp(argv[1], "--mind-no-halo")) {
        mind_no_halo(argc > 2 ? argv[2] : 0);
        printf("MIND_NO_HALO: %s (%d checks)\n", fails ? "FAIL" : "PASS", checks);
        return fails ? 1 : 0;
    }
    mind_no_halo(0);
    cpu_clock = getenv("ALLEN_UI_CLOCK") && !strcmp(getenv("ALLEN_UI_CLOCK"), "cpu");

    /* ---- answers ---- */
    CHECK(!strcmp(ck_allen_reply("Hello Allen"), "Hello Humanity."), "reply: Hello Allen");
    CHECK(!strcmp(ck_allen_reply("  hello   ALLEN!! "), "Hello Humanity."), "reply: case, spaces, punctuation folded");
    CHECK(!strcmp(ck_allen_reply("hello allen."), "Hello Humanity."), "reply: trailing period");
    CHECK(strcmp(ck_allen_reply("hello allenx"), "Hello Humanity."), "reply: near miss is not the first greeting");
    CHECK(!strncmp(ck_allen_reply(""), "I don't know that yet.", 22), "reply: empty line");
    CHECK(!strncmp(ck_allen_reply(0), "I don't know that yet.", 22), "reply: no line");
    CHECK(!strncmp(ck_allen_reply("xz"), "I don't know that yet.", 22), "reply: unknown line");
    CHECK(!strcmp(ck_allen_reply("who are you"), "I am Allen. I live in AIENOS."), "reply: who are you");
    CHECK(ck_allen_reply("Goodbye, Allen!") == ck_allen_goodbye && ck_allen_reply("bye") == ck_allen_goodbye &&
              ck_allen_reply("good bye") == ck_allen_goodbye && ck_allen_reply("goodbye now") != ck_allen_goodbye,
          "reply: goodbye ends the session (and only goodbye)");
    {
        /* A conversation: what Allen is told stays for the session. */
        ck_allen_facts facts = {4, 1, 1, 3840, 2160};
        ck_allen_mind m;
        ck_allen_mind_init(&m, &facts);
        static const struct {
            const char *say, *want; /* want: the answer starts with this */
            uint64_t awake;
        } talk[] = {
            {"Hello Allen", "Hello Humanity.", 5},
            {"what's my name?", "You haven't told me yet.", 6},
            {"My name is drake", "Nice to meet you, Drake.", 7},
            {"hi", "Hello, Drake. Good to see you.", 8},
            {"what is my name", "You're Drake.", 9},
            {"how are you?", "I'm running well, Drake. 4 cores awake", 10},
            {"good thanks", "Glad to hear it.", 11},
            {"i am tired", "Sorry to hear that.", 12},
            {"what is 12 times 4", "12 times 4 is 48.", 13},
            {"nine divided by two", "9 divided by 2 is 4 remainder 1.", 14},
            {"3 minus 10", "3 minus 10 is minus 7.", 15},
            {"what is 5 divided by zero", "Nobody can divide by zero", 16},
            {"tell me about this computer", "This computer has 4 cores, a 3840 by 2160 screen, 1 hard drive and 1 SSD.",
             17},
            {"how long have you been awake?", "I've been awake for 2 minutes and 5 seconds.", 125},
            {"what time is it", "I can't read the clock yet. I've been awake for 1 hour and 1 minute.", 3660},
            {"who made you", "Drake built me", 3661},
            {"what is aienos", "AIENOS is the operating system I live in", 3662},
            {"tell me a joke", "", 3663},
            {"thank you", "You're welcome, Drake.", 3664},
            {"can you remember things", "Teach me like this: remember my favorite color is blue.", 3665},
            {"help", help_reply, 3666},
            {"i'm fine", "Glad to hear it.", 3667},
            {"blorp", "I don't know that yet.", 3668},
        };
        int ok = 1;
        for (size_t i = 0; i < sizeof talk / sizeof talk[0]; i++) {
            const char *a = ck_allen_answer(&m, talk[i].say, talk[i].awake);
            if (strncmp(a, talk[i].want, strlen(talk[i].want)) || strlen(a) >= 160) {
                printf("  conversation line %zu: \"%s\" -> \"%s\"\n", i, talk[i].say, a);
                ok = 0;
            }
        }
        CHECK(ok, "reply: a whole conversation (name, how are you, sums, machine facts, awake time, help)");
    }
    {
        /* His own name: asked for at the first meeting, kept, changed on request. */
        ck_allen_mind m;
        ck_allen_mind_init(&m, 0);
        CHECK(!strcmp(ck_allen_welcome(&m), "Hello. I'm new here, and I have no name yet. What will you call me?"),
              "self: a first meeting asks for his name");
        CHECK(!strcmp(ck_allen_answer(&m, "Nova", 2), "Then I'm Nova. Hello Humanity.") && !strcmp(m.mem.self, "Nova") &&
                  m.changed && !m.naming,
              "self: the next word is his name, saved");
        m.changed = 0;
        CHECK(!strcmp(ck_allen_answer(&m, "hello nova", 3), "Hello Humanity."), "self: greeted by his new name");
        CHECK(!strcmp(ck_allen_answer(&m, "what is your name?", 4), "My name is Nova."), "self: says his name");
        CHECK(!strcmp(ck_allen_answer(&m, "who are you", 5), "I am Nova. I live in AIENOS."), "self: who are you");
        CHECK(!strcmp(ck_allen_answer(&m, "call yourself Sky", 6), "I'm Sky now.") && !strcmp(m.mem.self, "Sky") &&
                  m.changed,
              "self: renamed on request");
        CHECK(!strncmp(ck_allen_answer(&m, "your name is the best", 7), "One word, please", 16) &&
                  !strcmp(m.mem.self, "Sky"),
              "self: a rename needs one word");
        CHECK(!strncmp(ck_allen_answer(&m, "my name is sky", 8), "You haven't told me yet", 23) && !m.mem.name[0],
              "self: the person cannot take his name");
        /* no sky mode chosen yet, so the welcome also offers the two sky choices (allen_reply.c) */
        CHECK(!strcmp(ck_allen_welcome(&m), "Hello. I'm Sky. The sky behind me can be the real sky above you right now, or a different part of the universe each time. Tell me which you'd like."),
              "self: the next window opening says who he is, and offers the sky");
        CHECK(!strcmp(ck_allen_answer(&m, "forget everything", 9),
                      "Done. I've forgotten everything you taught me, and your name.") &&
                  !strcmp(m.mem.self, "Sky"),
              "self: forgetting what he was taught keeps his name");
        ck_allen_mind_init(&m, 0);
        ck_allen_welcome(&m);
        CHECK(!strcmp(ck_allen_answer(&m, "what can you do", 1), help_reply) && !m.naming && !m.mem.self[0],
              "self: any other line drops the question, he stays Allen");
        CHECK(!strcmp(ck_allen_answer(&m, "what's your name", 2),
                      "My name is Allen. You can give me another: call yourself, then a name."),
              "self: Allen by default, and how to change it");
        ck_allen_mind_init(&m, 0);
        ck_allen_welcome(&m);
        CHECK(!strcmp(ck_allen_answer(&m, "allen", 1), "Allen it is. Hello Humanity.") && !m.mem.self[0] && !m.naming,
              "self: Allen chosen stays the default");
        ck_allen_mind_init(&m, 0);
        ck_allen_welcome(&m);
        CHECK(!strcmp(ck_allen_answer(&m, "Hello Allen", 1), "Hello Humanity.") && !m.naming,
              "self: greeting him as Allen settles it");
        CHECK(!strncmp(ck_allen_answer(&m, "xz", 2), "I don't know that yet.", 22) && !m.mem.self[0],
              "self: after that a stray word is not a name");
    }
    {
        /* What a line wants, and the job Allen writes for the far side. */
        static const struct {
            const char *say;
            ck_allen_want want;
        } wants[] = {
            {"write a haiku about rain", CK_WANT_MAKE},
            {"please make me a list of three birds", CK_WANT_MAKE},
            {"can you code a function that adds two numbers", CK_WANT_MAKE},
            {"i need a name for my boat", CK_WANT_MAKE},
            {"explain how rain forms", CK_WANT_EXPLAIN},
            {"what is a prime number", CK_WANT_EXPLAIN},
            {"why is the sky blue", CK_WANT_EXPLAIN},
            {"allen tell me about the moon", CK_WANT_EXPLAIN},
            {"what does ephemeral mean", CK_WANT_EXPLAIN},
            {"find my keys", CK_WANT_DO},
            {"could you translate good morning to french", CK_WANT_DO},
            {"summarize the news", CK_WANT_DO},
            {"i had a long day", CK_WANT_CHAT},
            {"tell me a joke", CK_WANT_CHAT},
            {"", CK_WANT_CHAT},
        };
        int ok = 1;
        for (size_t i = 0; i < sizeof wants / sizeof wants[0]; i++) {
            char f[96];
            ck_allen_fold(wants[i].say, f, sizeof f);
            ck_allen_want got = ck_allen_want_of(f);
            if (got != wants[i].want) {
                printf("  want of \"%s\": %s, wanted %s\n", wants[i].say, ck_allen_want_name(got),
                       ck_allen_want_name(wants[i].want));
                ok = 0;
            }
        }
        CHECK(ok, "intent: make, explain, do and chat told apart");
        ck_allen_mind m;
        ck_allen_mind_init(&m, 0);
        char job[1024];
        unsigned n = ck_allen_job(&m, 7, "Write a haiku\tabout\nrain", job, sizeof job);
        CHECK(n && n == strlen(job) && !strncmp(job, "JOB 7 make You are the far hands of Allen, a quiet presence", 59),
              "intent: the job names the seq, the want and who he is");
        CHECK(strstr(job, "the person's computer") && strstr(job, "Begin with one short sentence") &&
                  !strchr(job, '\n') && strstr(job, "\tWrite a haiku about rain") == job + n - 25,
              "intent: the instruction fits the want; the line follows a tab, cleaned");
        strcpy(m.mem.name, "Drake");
        strcpy(m.mem.self, "Nova");
        n = ck_allen_job(&m, 8, "why is the sky blue?", job, sizeof job);
        CHECK(n && !strncmp(job, "JOB 8 explain You are the far hands of Nova", 43) && strstr(job, "on Drake's computer") &&
                  strstr(job, "three plain sentences"),
              "intent: his name and the person's name go into the instruction");
        CHECK(ck_allen_job(&m, 9, "hello", job, 100) == 0, "intent: too small a buffer is refused, not cut");
        uint32_t seq = 0;
        char text[160];
        CHECK(ck_allen_answer_line("SAY 8 Sunlight scatters\\nin the air,  blue most of all.  ", &seq, text, sizeof text) &&
                  seq == 8 && !strcmp(text, "Sunlight scatters in the air, blue most of all."),
              "intent: an answer line is read back, newlines and runs of blanks folded");
        CHECK(!ck_allen_answer_line("HELLO courier", &seq, text, sizeof text) &&
                  !ck_allen_answer_line("SAY x nope", &seq, text, sizeof text) &&
                  !ck_allen_answer_line("SAYING 3 no", &seq, text, sizeof text),
              "intent: other lines are not answers");
        CHECK(ck_allen_answer_line("SAY 12 0123456789abcdef0123456789abcdef", &seq, text, 10) && !strcmp(text, "012345678"),
              "intent: a long answer is cut to the buffer");
    }

    {
        /* A fluid strip must sample the same global grid as a full frame,
         * and a refresh cannot expose solver changes that were not marked. */
        const int32_t w = 1920, h = 1080;
        size_t bytes = (size_t)w * h * sizeof(uint32_t);
        uint32_t *whole = calloc(1, bytes), *parts = calloc(1, bytes);
        static ck_flow water;
        ck_flow_init(&water, w, h, w / 2, h / 2, 118, w / 2, h, 0);
        for (unsigned i = 0; i < CK_FLOW_CELLS; i++) {
            water.d[i] = 65536 / 2 + 255;
            water.warm[i] = i & 1 ? 256 : 0;
        }
        flow_marks = 0;
        ck_flow_mark(&water, flow_mark);
        CHECK(flow_marks > 0, "flow: changed solver cells publish their rendered snapshot");
        ck_canvas full = {whole, w, h}, tiled = {parts, w, h};
        ck_rect all = {0, 0, w, h};
        ck_flow_draw(&water, &full, &all, 256);
        for (int32_t y = 0; y < h; y += CK_FLOW_H)
            for (int32_t x = 0; x < w; x += CK_FLOW_W) {
                ck_rect r = {x, y, (int32_t)CK_FLOW_W, (int32_t)CK_FLOW_H};
                ck_flow_draw(&water, &tiled, &r, 256);
            }
        CHECK(!memcmp(whole, parts, bytes),
              "flow: exact grid remainder makes full and clipped draws byte-identical");
        unsigned marks = flow_marks;
        for (unsigned i = 0; i < CK_FLOW_CELLS; i++)
            water.d[i]++;
        ck_flow_mark(&water, flow_mark);
        memset(parts, 0, bytes);
        ck_flow_draw(&water, &tiled, &all, 256);
        CHECK(flow_marks == marks && !memcmp(whole, parts, bytes),
              "flow: below-threshold changes cannot leak into full refreshes");
        for (unsigned i = 0; i < CK_FLOW_CELLS; i++)
            water.d[i] = 0;
        ck_flow_mark(&water, flow_mark);
        memset(parts, 0, bytes);
        ck_flow_draw(&water, &tiled, &all, 256);
        int black = 1;
        for (size_t i = 0; i < (size_t)w * h; i++)
            black &= parts[i] == 0;
        CHECK(flow_marks > marks && black, "flow: a published disappearance leaves no stale ink");
        free(whole);
        free(parts);
    }

    {
        /* The ink is a smooth field: on the dark sky it must not show as
         * flat 8-bit rings, and its colour must not step along grid cells
         * (the concentric bands and squares seen on the iMac). */
        const int32_t w = 1920, h = 1080;
        uint32_t *px = calloc((size_t)w * h, sizeof(uint32_t));
        static ck_flow water;
        ck_canvas cv = {px, w, h};
        ck_rect all = {0, 0, w, h};
        ck_flow_init(&water, w, h, w / 2, h / 2, 118, w / 2, h, 0);
        for (unsigned j = 0; j < CK_FLOW_H + 2; j++)
            for (unsigned i = 0; i < CK_FLOW_W + 2; i++) {
                water.shown[i + (CK_FLOW_W + 2) * j] = (int32_t)(65536u * 55 / 100 * i / (CK_FLOW_W + 1));
                water.shown_warm[i + (CK_FLOW_W + 2) * j] = 0;
            }
        ck_flow_draw(&water, &cv, &all, 256);
        unsigned flat = 0, longest = 0;
        for (int32_t y = 100; y < h; y += 97) {
            unsigned run = 1;
            for (int32_t x = w / 3 + 1; x < w; x++) { /* where the ink is bright enough to step whole levels */
                run = px[(size_t)y * w + x] == px[(size_t)y * w + x - 1] ? run + 1 : 1;
                if (run > longest)
                    longest = run;
                flat += run == 12;
            }
        }
        /* undithered, a level is one flat band (28 px here, 803 runs of 12); the dither leaves only chance runs */
        CHECK(flat < 60 && longest < 20, "flow: a smooth ink ramp on black is dithered, not ringed into flat 8-bit bands");
        printf("note: flow ramp longest equal run %u px, runs reaching 12: %u\n", longest, flat);
        for (unsigned j = 0; j < CK_FLOW_H + 2; j++)
            for (unsigned i = 0; i < CK_FLOW_W + 2; i++) {
                water.shown[i + (CK_FLOW_W + 2) * j] = 65536 / 2;
                water.shown_warm[i + (CK_FLOW_W + 2) * j] = (i + j) & 1 ? 256 : 0;
            }
        memset(px, 0, (size_t)w * h * sizeof(uint32_t));
        ck_flow_draw(&water, &cv, &all, 256);
        unsigned step = 0;
        for (int32_t y = 100; y < h; y += 97)
            for (int32_t x = 200; x < w - 200; x++) {
                int32_t a = (int32_t)(px[(size_t)y * w + x] >> 16 & 255u), b = (int32_t)(px[(size_t)y * w + x - 1] >> 16 & 255u);
                step = (unsigned)(a > b ? a - b : b - a) > step ? (unsigned)(a > b ? a - b : b - a) : step;
            }
        CHECK(step < 24, "flow: warm and cool water blend between cells, no hard colour step");
        printf("note: flow colour largest neighbour step %u levels\n", step);
        free(px);
    }

    /* Display bytes are sRGB; coverage must weigh decoded light. These
     * checks distinguish the transfer-table path from encoded-byte scaling. */
    {
        uint32_t ink = CK_RGB(234, 238, 252), white = CK_RGB(255, 255, 255);
        uint32_t half = (ck_sky_srgb16[ck_sky_lin[255] * 128u >> 11] + 8u) >> 4;
        uint32_t expected = CK_RGB(half, half, half);
        CHECK(ck_scale(white, 128) == expected && ck_mix(0, white, 128) == expected &&
                  ck_add(0, white, 128) == expected && half > 128,
              "paint: half light is encoded after weighting, not half an sRGB byte");
        int endpoints = 1, stable = 1, monotone = 1;
        uint32_t previous = 0;
        for (uint32_t c = 0; c <= 255; c++) {
            uint32_t color = CK_RGB(c, c, c);
            endpoints &= ck_scale(color, 0) == 0 && ck_scale(color, 256) == color &&
                         ck_mix(ink, color, 0) == ink && ck_mix(ink, color, 256) == color &&
                         ck_add(0, color, 256) == color && ck_add(color, ink, 0) == color;
            for (uint32_t a = 0; a <= 256; a++)
                stable &= ck_mix(color, color, a) == color &&
                          ck_ch(ck_add(color, CK_RGB(0, 0, 255), a), 16) == c;
        }
        for (uint32_t a = 0; a <= 256; a++) {
            uint32_t value = ck_scale(white, a);
            monotone &= value >= previous;
            previous = value;
        }
        CHECK(endpoints && stable, "paint: opaque/black endpoints and untouched channels remain exact");
        CHECK(monotone, "paint: light fades monotonically all the way to pure black");

        uint32_t hpx[64 * 64] = {0}, vpx[64 * 64] = {0}, thin[64 * 64] = {0};
        ck_canvas hc = {hpx, 64, 64}, vc = {vpx, 64, 64}, tc = {thin, 64, 64};
        ck_rect clip = {0, 0, 64, 64};
        ck_thread(&hc, &clip, 4 * 16, 16 * 16 + 8, 48 * 16, 16 * 16 + 8, 32, white, 256, 256);
        ck_thread(&vc, &clip, 16 * 16 + 8, 4 * 16, 16 * 16 + 8, 48 * 16, 32, white, 256, 256);
        ck_thread(&tc, &clip, 4 * 16, 16 * 16 + 8, 48 * 16, 16 * 16 + 8, 16, white, 256, 256);
        int transpose = 1;
        for (unsigned y = 0; y < 64; y++)
            for (unsigned x = 0; x < 64; x++)
                transpose &= hpx[y * 64 + x] == vpx[x * 64 + y];
        CHECK(transpose && hpx[16 * 64 + 20] == white && hpx[15 * 64 + 20] == expected &&
                  hpx[17 * 64 + 20] == expected && thin[16 * 64 + 20] == white &&
                  thin[15 * 64 + 20] == 0 && thin[17 * 64 + 20] == 0,
              "paint: native-scaled threads widen symmetrically, including vertical edges");
    }

    /* ---- typeface ---- */
    CHECK(ck_af_isqrt(0) == 0 && ck_af_isqrt(15) == 3 && ck_af_isqrt(16) == 4 &&
              ck_af_isqrt(1000000007ull * 1000000007ull) == 1000000007ull,
          "font: integer square root");
    CHECK(ck_af_sin(0) == 0 && ck_af_sin(90) == 16384 && ck_af_sin(-90) == -16384 && ck_af_cos(180) == -16384 &&
              ck_af_sin(450) == 16384,
          "font: sine table");
    static uint8_t pool[4 << 20];
    ck_af_face face;
    ck_af_face_init(&face, 30, 1250, pool, sizeof pool);
    int all = 1, sym = 1;
    for (int c = 33; c <= 126; c++) {
        const ck_af_glyph *g = ck_af_glyph_get(&face, (char)c);
        unsigned ink = 0;
        if (!g || !g->alpha) {
            all = 0;
            continue;
        }
        for (size_t i = 0; i < (size_t)g->w * g->h; i++)
            ink += g->alpha[i] > 128;
        if (ink < 4)
            all = 0;
    }
    const ck_af_glyph *o = ck_af_glyph_get(&face, 'o');
    for (int y = 0; o && y < o->h; y++)
        for (int x = 0; x < o->w; x++) {
            int d = (int)o->alpha[y * o->w + x] - (int)o->alpha[y * o->w + (o->w - 1 - x)];
            if (d > 40 || d < -40)
                sym = 0;
        }
    CHECK(all, "font: every printable character rasterizes with ink");
    CHECK(sym, "font: 'o' is left-right symmetric");
    CHECK(ck_af_glyph_get(&face, ' ') != 0 && ck_af_advance(&face, ' ') > 0, "font: space advances");
    CHECK(ck_af_measure(&face, "Hello Humanity.", 15) > ck_af_measure(&face, "Hello", 5), "font: measure grows");
    uint8_t tiny[64];
    ck_af_face small;
    ck_af_face_init(&small, 30, 1250, tiny, sizeof tiny);
    CHECK(ck_af_glyph_get(&small, 'W') == 0, "font: a full pool refuses instead of overrunning");

    {
        uint32_t pixels[64 * 64] = {0};
        ck_canvas cv = {pixels, 64, 64};
        ck_rect clip = {0, 0, 64, 64};
        ck_letter(&cv, &clip, &face, 16 * 256, 48, 'o', CK_RGB(255, 255, 255), 128, 0);
        int exact = 1;
        unsigned fractional = 0;
        for (int y = 0; o && y < o->h; y++)
            for (int x = 0; x < o->w; x++) {
                int px = 16 + o->ox + x, py = 48 + o->oy + y;
                uint32_t cov = o->alpha[y * o->w + x], weight = (cov + (cov >> 7)) * 128 >> 8;
                uint32_t level = (ck_sky_srgb16[ck_sky_lin[255] * weight >> 11] + 8u) >> 4;
                if (px >= 0 && py >= 0 && px < 64 && py < 64)
                    exact &= pixels[py * 64 + px] == CK_RGB(level, level, level);
                fractional += cov > 0 && cov < 255;
            }
        CHECK(exact && fractional, "font: real glyph AA coverage is composed as light");
        uint32_t ink = CK_RGB(234, 238, 252);
        for (unsigned i = 0; i < 64 * 64; i++)
            pixels[i] = ink;
        ck_letter(&cv, &clip, &face, 16 * 256, 48, 'o', ink, 256, 0);
        exact = 1;
        for (unsigned i = 0; i < 64 * 64; i++)
            exact &= pixels[i] == ink;
        CHECK(exact, "font: matching ink/background does not darken at AA edges");
    }

    /* ---- Allen's home at 3840 x 2160, BGRX ---- */
    uint32_t W = 3840, H = 2160;
    uint32_t *fb = calloc((size_t)W * H, 4), *copy = malloc((size_t)W * H * 4), *sky = malloc((size_t)W * H * 4);
    size_t need = ck_allen_ui_mem_bytes(W, H);
    void *mem = malloc(need);
    struct pic P = {fb, W, 1};
    CHECK(ck_allen_ui_start(fb, W, H, W, 1, mem, need - 1, 1000000, 0, real_us) == -1, "ui: short memory refused");
    CHECK(ck_allen_ui_start(fb, W, H, W, 2, mem, need, 1000000, 0, real_us) == -1, "ui: unknown pixel format refused");
    CHECK(bare_init(W, H) == 0, "night: the bare sky for the checks");
    {
        /* No pull and no shading must be the native kept picture, including
         * single-pixel contrast and its final row/column. Dimming must not
         * select a lower-resolution copy either. */
        static ck_night n;
        uint32_t source[64 * 64], background[64 * 64] = {0}, resolved[64 * 64], previous[64 * 64];
        n.w = n.h = 64;
        n.k = 256;
        n.cx = n.cy = -64;
        n.sky = background;
        for (unsigned y = 0; y < 64; y++)
            for (unsigned x = 0; x < 64; x++)
                source[y * 64 + x] = (x + y) & 1 ? CK_RGB(255, 255, 255) : 0;
        void *storage = malloc(ck_night_picture_bytes(64, 64));
        ck_night_picture kept;
        ck_night_picture_take(&n, source, storage, &kept);
        ck_canvas cv = {resolved, 64, 64};
        ck_rect clip = {0, 0, 64, 64};
        ck_night_dusk d = {.crowd = 256, .keep = 256};
        ck_night_fall(&n, &d);
        ck_night_base_from(&n, &kept, &cv, &clip);
        CHECK(!memcmp(source, resolved, sizeof source), "dusk: unwarped picture resolves every native pixel and edge");
        uint32_t half = (ck_sky_srgb16[ck_sky_lin[255] * 128u >> 11] + 8u) >> 4;
        CHECK(kept.level[1][0] == CK_RGB(half, half, half),
              "dusk: minification averages light instead of darkening encoded contrast");
        memcpy(previous, resolved, sizeof previous);
        int monotone = 1, detail = 1;
        for (uint32_t keep = 128; keep; keep >>= 1) {
            d.keep = keep;
            ck_night_fall(&n, &d);
            ck_night_base_from(&n, &kept, &cv, &clip);
            for (unsigned i = 0; i < 64 * 64; i++) {
                monotone &= resolved[i] <= previous[i];
                detail &= resolved[i] == ck_scale(source[i], keep);
            }
            memcpy(previous, resolved, sizeof previous);
        }
        CHECK(monotone && detail && resolved[1] != 0 && resolved[0] == 0,
              "dusk: faint picture stays pixel-resolved, nonblack and monotonically dimming");
        d.keep = 0;
        ck_night_fall(&n, &d);
        ck_night_base_from(&n, &kept, &cv, &clip);
        int black = 1;
        for (unsigned i = 0; i < 64 * 64; i++)
            black &= resolved[i] == 0;
        CHECK(black, "dusk: zero light resolves to pure black at every native pixel");
        free(storage);

        ck_night_at(&bare_night, CK_NIGHT_MERGE_MS + CK_NIGHT_RIPPLE_MS);
        ck_canvas native = {copy, (int32_t)W, (int32_t)H};
        ck_rect whole = {0, 0, (int32_t)W, (int32_t)H};
        ck_night_base(&bare_night, &native, &whole);
        CHECK(!memcmp(copy, bare_night.sky, (size_t)W * H * 4) &&
                  ck_night_faded(&bare_night, 0, 0, 0) == 256 &&
                  ck_night_faded(&bare_night, (int32_t)W - 1, (int32_t)H - 1, 255) == 256,
              "night: settled 4K stars and diffuse sky are native, unscaled and fully unfaded");
    }
    uint64_t t_init = real_us();
    CHECK(ck_allen_ui_start(fb, W, H, W, 1, mem, need, 1000000, 0, real_us) == 0, "ui: starts at 3840x2160");
    printf("note: the night made in %llu ms (this host)\n", (unsigned long long)((real_us() - t_init) / 1000));
    CHECK(ck_allen_ui_started() && !ck_allen_ui_on(), "ui: the night has begun; Allen is not awake yet");
    now_us = 0;
    run_to(0);
    CHECK(count(&P, 0, 0, W, H, LIT) == 0, "night: it opens in the dark");
    run_to(1800);
    unsigned early = count(&P, 0, 0, W, H, LIT);
    run_to(4200);
    unsigned later = count(&P, 0, 0, W, H, LIT);
    CHECK(early > 0 && later > early * 2, "night: the stars come out, the brightest first");
    CHECK(stale(fb, copy, W, W, H) == 0, "night: the fading sky is drawn whole");
    if (dir) {
        snprintf(path, sizeof path, "%s/night-1-stars.png", dir);
        png(path, fb, W, 1, 0, 0, W, H, 2);
    }
    uint32_t cx = W / 2, cy = H * 40 / 100, R = 236;
    run_to(8000);
    CHECK(count(&P, cx - 800, cy - 500, 1600, 1000, BLACK) > 2000, "night: two black holes, their shadows");
    CHECK(count(&P, 0, 0, W, H, BLACK) < 200000, "night: and only the holes are black");
    CHECK(stale(fb, copy, W, W, H) < 40, "night: the bent starlight round the holes is redrawn as they move");
    if (dir) {
        snprintf(path, sizeof path, "%s/night-2-holes.png", dir);
        png(path, fb, W, 1, 0, 0, W, H, 2);
        snprintf(path, sizeof path, "%s/night-2-holes-detail.png", dir);
        png(path, fb, W, 1, cx - 900, cy - 560, 1800, 1120, 1);
    }
    run_to(11600);
    unsigned before = count(&P, cx - 300, cy - 300, 600, 600, BRIGHT);
    run_to(12100);
    CHECK(count(&P, cx - 300, cy - 300, 600, 600, BRIGHT) > before + 1500, "night: the merger flashes");
    run_to(12900);
    CHECK(stale(fb, copy, W, W, H) < 40, "night: the ripple is redrawn wherever it runs");
    if (dir) {
        snprintf(path, sizeof path, "%s/night-3-ripple.png", dir);
        png(path, fb, W, 1, 0, 0, W, H, 2);
    }
    run_to(16500);
    CHECK(count(&P, 0, 0, W, H, BLACK) == 0, "night: the last black hole has dissolved");
    CHECK(cool_added(&P, sky, cx - R, cy - R, 2 * R, 2 * R) > 5000,
          "allen: formed, a visible cool network over the actual sky");
    uint32_t bx = W / 2 - 720, by = H - 300, bw = 1440, bh = 108;
    CHECK(added(&P, sky, 0, by - 40, W, H - by + 40) < 400, "allen: no bar before he is awake");
    CHECK(stale(fb, copy, W, W, H) < 40, "allen: drawn whole as he turns");
    if (dir) {
        snprintf(path, sizeof path, "%s/allen-1-formed.png", dir);
        png(path, fb, W, 1, 0, 0, W, H, 2);
        snprintf(path, sizeof path, "%s/allen-1-formed-detail.png", dir);
        png(path, fb, W, 1, cx - 600, cy - 500, 1200, 1000, 1);
    }
    /* awake: the bar comes up */
    const char *footer = "Type goodbye to turn the display off";
    ck_allen_ui_wake(footer, now_us);
    run_to(17800);
    CHECK(ck_allen_ui_on(), "ui: on");
    unsigned bar0 = added(&P, sky, bx, by, bw, bh);
    CHECK(bar0 > 2500, "bar: up once he is awake, with its hint");
    memcpy(copy, fb, (size_t)W * H * 4);
    ck_allen_ui_wake(0, now_us);
    ck_allen_ui_refresh();
    ck_allen_ui_tick(now_us - 16000);
    CHECK(memcmp(copy, fb, (size_t)W * H * 4) != 0, "bar: removing the footer changes the empty bar's picture");
    ck_allen_ui_wake(footer, now_us);
    ck_allen_ui_refresh();
    ck_allen_ui_tick(now_us - 16000);
    CHECK(!memcmp(copy, fb, (size_t)W * H * 4), "bar: restoring the footer restores exactly the empty bar's picture");
    const char *typed = "Hello Allen";
    for (unsigned i = 1; i <= 11; i++) {
        ck_allen_ui_input(typed, i, now_us);
        run_to((uint32_t)(now_us / 1000) + 120);
    }
    CHECK(count(&P, bx, by, bw, bh, BRIGHT) > 500, "bar: what is typed appears in it");
    memcpy(copy, fb, (size_t)W * H * 4);
    ck_allen_ui_wake(0, now_us);
    ck_allen_ui_refresh();
    ck_allen_ui_tick(now_us - 16000);
    CHECK(!memcmp(copy, fb, (size_t)W * H * 4), "bar: removing the footer changes no pixel while typing");
    ck_allen_ui_wake(footer, now_us);
    ck_allen_ui_refresh();
    ck_allen_ui_tick(now_us - 16000);
    CHECK(!memcmp(copy, fb, (size_t)W * H * 4), "bar: restoring the footer changes no pixel while typing");
    if (dir) {
        snprintf(path, sizeof path, "%s/allen-2-typing.png", dir);
        png(path, fb, W, 1, 0, 0, W, H, 2);
    }
    /* Enter: the line rises into him, he thinks, then answers under him */
    uint32_t wy = cy + 500 - 90, wh = 5 * 80 + 60;
    ck_allen_ui_said(typed, ck_allen_reply(typed), now_us);
    uint32_t said = (uint32_t)(now_us / 1000);
    run_to(said + 500);
    CHECK(stale(fb, copy, W, W, H) < 40, "talk: the rising line and the thoughts are redrawn");
    if (dir) {
        snprintf(path, sizeof path, "%s/allen-3-thinking.png", dir);
        png(path, fb, W, 1, 0, 0, W, H, 2);
    }
    run_to(said + 1300);
    CHECK(stale(fb, copy, W, W, H) < 40, "talk: the galaxies' shimmer is redrawn while he speaks");
    run_to(said + 2600);
    CHECK(added(&P, sky, W / 4, wy, W / 2, wh) > 3000, "talk: Allen's answer appears under him");
    CHECK(count(&P, bx, by, bw, bh, BRIGHT) < 500, "talk: the bar is cleared for the next line");
    if (dir) {
        snprintf(path, sizeof path, "%s/allen-4-answer.png", dir);
        png(path, fb, W, 1, 0, 0, W, H, 2);
        snprintf(path, sizeof path, "%s/allen-4-answer-detail.png", dir);
        png(path, fb, W, 1, W / 4, cy - 400, W / 2, 1300, 1);
    }
    /* Remote work keeps THINK active rather than immediately scheduling an
     * answer. Exercise repeated dirty frames and the residual activity at the
     * transition to SPEAK; isolated fresh-frame fixtures cannot catch trails. */
    ck_allen_ui_think("why do stars shine", now_us);
    {
        uint32_t pending = (uint32_t)(now_us / 1000);
        static const uint32_t checkpoints[] = {500, 900, 1500, 2700, 4500};
        for (unsigned i = 0; i < sizeof checkpoints / sizeof checkpoints[0]; i++) {
            run_to(pending + checkpoints[i]);
            CHECKF(stale(fb, copy, W, W, H) < 40,
                  "talk: sustained thinking leaves no stale light at checkpoint %u", checkpoints[i]);
        }
    }
    ck_allen_ui_said(0, "The rendering fixture is answering.", now_us);
    said = (uint32_t)(now_us / 1000);
    run_to(said + 1300);
    CHECK(stale(fb, copy, W, W, H) < 40, "talk: thinking-to-speaking leaves no stale interior light");
    /* left alone: his words fade once read, then the bar; only Allen is left, waiting */
    run_to(said + 24000);
    CHECK(added(&P, sky, W / 4, wy, W / 2, wh) < 400, "rest: his words fade once read");
    CHECK(added(&P, sky, 0, by - 40, W, H - by + 40) < 400, "rest: the bar fades when nothing is in use");
    CHECK(cool_added(&P, sky, cx - R, cy - R, 2 * R, 2 * R) > 5000,
          "rest: Allen stays visible as a cool network, not a bright lamp");
    CHECK(stale(fb, copy, W, W, H) < 40, "rest: nothing is left behind on the screen");
    if (dir) {
        snprintf(path, sizeof path, "%s/allen-5-waiting.png", dir);
        png(path, fb, W, 1, 0, 0, W, H, 2);
    }
    ck_allen_ui_input("h", 1, now_us);
    run_to((uint32_t)(now_us / 1000) + 400);
    CHECK(added(&P, sky, bx, by, bw, bh) > 2500, "rest: a key brings the bar back");
    ck_allen_ui_input("", 0, now_us);
    {
        /* the notes constellation: shown when a note is saved, then it goes */
        static const char *const notes[] = {"the gate code is 4471", "the garage code is 9012",
                                            "the wifi password is Blue-Fox-88", "the garage door opener is in the truck",
                                            "the dentist is on friday at 10", "the truck needs new tires before the winter"};
        static ck_allen_notes ns;
        ns.n = 6;
        for (unsigned i = 0; i < 6; i++)
            ck_allen_note_set(&ns.note[i], notes[i], 1);
        uint8_t ea[32], eb[32];
        unsigned ne = 0;
        for (unsigned a = 0; a < 6; a++) /* point 0 is the newest note */
            for (unsigned b = a + 1; b < 6; b++)
                if (ck_allen_note_links(&ns.note[5 - a], &ns.note[5 - b]))
                    ea[ne] = (uint8_t)a, eb[ne++] = (uint8_t)b;
        CHECK(ne >= 3, "map: the six notes link (garage, code, truck)");
        uint32_t mx = W * 84 / 100 - 380, my = H * 42 / 100 - 390, mw = 760, mh = 820;
        ck_allen_ui_map(6, ea, eb, ne, -1, 0, 6, ne);
        run_to((uint32_t)(now_us / 1000) + 1500);
        CHECK(added(&P, sky, mx, my, mw, mh) < 400, "map: not shown when nothing was saved");
        ck_allen_ui_map(6, ea, eb, ne, 0, ns.note[5].text, 6, ne);
        uint32_t m0 = (uint32_t)(now_us / 1000);
        run_to(m0 + 500);
        CHECK(stale(fb, copy, W, W, H) < 40, "map: the spark from Allen to the note is redrawn");
        run_to(m0 + 1500);
        CHECK(added(&P, sky, mx, my, mw, mh) > 3000, "map: the note just saved glows");
        if (dir) {
            snprintf(path, sizeof path, "%s/allen-6-notes.png", dir);
            png(path, fb, W, 1, 0, 0, W, H, 2);
            snprintf(path, sizeof path, "%s/allen-6-notes-detail.png", dir);
            png(path, fb, W, 1, mx, my, mw, mh, 1);
        }
        ck_allen_ui_map(6, ea, eb, ne, 3, "a very long note that cannot possibly fit on the one line under the map", 6,
                        ne);
        run_to(m0 + 3000);
        CHECK(added(&P, sky, mx, my, mw, mh) > 3000, "map: a long label is cut to fit, not overrun");
        run_to(m0 + 15000);
        CHECK(added(&P, sky, mx, my, mw, mh) < 400, "map: it goes again after a while");
    }
    /* ---- sleep: the dusk. Everything falls into the black hole, the disk glows and dims, the horizon closes
     * to pure black and nothing moves; waking brings the sky back calmly ---- */
    {
        ck_allen_ui_sleep(1, now_us);
        uint32_t s0 = (uint32_t)(now_us / 1000);
        static const uint32_t at[6] = {600, 1300, 2000, 2700, 3200, 3700}; /* the pictures, ms into the dusk */
        static uint64_t whole[320], middle[320];                            /* the light in each frame, and in the middle */
        static uint32_t when[320];
        unsigned nf = 0, saved = 0, lit600 = 0;
        int mid_ok = 1;
        for (; now_us <= (uint64_t)(s0 + 4300) * 1000u; now_us += 16000) {
            black_probe = !ck_allen_ui_asleep();
            int drawn = ck_allen_ui_tick(now_us);
            black_probe = 0;
            if (!drawn)
                continue;
            uint32_t d = (uint32_t)(now_us / 1000) - s0;
            if (nf < 320) {
                when[nf] = d;
                whole[nf] = light(&P, 0, 0, W, H);
                middle[nf] = light(&P, cx - 350, cy - 350, 700, 700);
                nf++;
            }
            if (saved < 6 && d >= at[saved]) {
                if (saved == 0)
                    for (uint32_t y = 0; y < H; y += 4)
                        for (uint32_t x = 0; x < W; x += 4)
                            lit600 += (fb[(size_t)y * W + x] & 0xffffffu) != 0;
                if (saved == 2 || saved == 3) { /* mid-dusk: the disk is there, brighter than the edges of the screen */
                    uint64_t c = light(&P, cx - 350, cy - 350, 700, 700) / (700 * 700 / 16);
                    uint64_t e = (light(&P, 0, 0, W, 200) + light(&P, 0, H - 200, W, 200) + light(&P, 0, 200, 200, H - 400) +
                                  light(&P, W - 200, 200, 200, H - 400)) /
                                 ((2 * (uint64_t)W * 200 + 2 * (uint64_t)(H - 400) * 200) / 16);
                    printf("note: dusk %u ms: the middle's light %llu, the edges' %llu (per pixel, r + g + b)\n", d,
                           (unsigned long long)c, (unsigned long long)e);
                    mid_ok &= c > 2 * e + 10;
                }
                if (dir) {
                    snprintf(path, sizeof path, "%s/allen-8-horizon-%u.png", dir, saved + 1);
                    png(path, fb, W, 1, 0, 0, W, H, 2);
                    if (saved == 3) {
                        snprintf(path, sizeof path, "%s/allen-8-horizon-%u-detail.png", dir, saved + 1);
                        png(path, fb, W, 1, cx - 800, cy - 500, 1600, 1000, 1);
                    }
                }
                saved++;
            }
        }
        CHECK(saved == 6 && nf >= 200, "dusk: six pictures and the light of every frame through it");
        CHECK(lit600 > 4000, "dusk: the sky is still on screen early in the dusk, falling, not cut to black");
        /* the disk's height: the most light in the middle once the disk has begun (from 700 ms; before that the
         * middle's light is Allen's own, going into the hole) */
        unsigned peak = 0;
        for (unsigned i = 0; i < nf; i++)
            if (when[i] >= 1000 && (!peak || middle[i] > middle[peak]))
                peak = i;
        printf("note: dusk: the disk is brightest %u ms in (frame %u of %u)\n", when[peak], peak, nf);
        CHECK(when[peak] > 1500 && when[peak] < 3600, "dusk: the disk comes to its height in the second half");
        unsigned rises = 0; /* a thousandth is the noise of summing every fourth pixel while the picture turns */
        for (unsigned i = peak; i + 1 < nf; i++)
            if (whole[i + 1] > whole[i] + whole[i] / 1000) {
                if (rises < 5)
                    printf("  the light rose again at %u ms: %llu after %llu\n", when[i + 1],
                           (unsigned long long)whole[i + 1], (unsigned long long)whole[i]);
                rises++;
            }
        CHECK(rises == 0, "dusk: after the disk's height the light only falls, frame by frame (within a thousandth)");
        CHECK(mid_ok, "dusk: in the middle of the dusk the middle of the screen is brighter than its edges");
        CHECK(black_probe_calls > 0 && black_probe_early == 0,
              "dusk: frame-begin/end observers cannot see unpublished black completion");
        CHECK(ck_allen_ui_asleep(), "dusk: asleep once the horizon has closed");
        CHECK(count(&P, 0, 0, W, H, BLACK) == W * H, "dusk: the closed horizon is pure black, every pixel");
        if (dir) {
            snprintf(path, sizeof path, "%s/allen-8-horizon-closed.png", dir);
            png(path, fb, W, 1, 0, 0, W, H, 2);
        }
        memcpy(copy, fb, (size_t)W * H * 4);
        run_to(s0 + 4600);
        CHECK(differ(fb, copy, W, W, H, 0) == 0, "sleep: nothing moves while asleep");
        uint32_t df, ds, da;
        ck_allen_ui_dusk_stats(&df, &ds, &da);
        printf("note: the dusk: %u frames at 3840x2160 on one core of this host: average %u us, slowest %u us (%s)\n",
               df, da, ds, cpu_clock ? "CPU time of this thread" : "wall clock");
        CHECK(df >= 200 && da > 0, "dusk: its frames are counted");
        /* the dawn: not a cut, and the sky is back in under a second, with nothing of the dusk left */
        ck_allen_ui_sleep(1, now_us);
        CHECK(!ck_allen_ui_asleep(), "sleep: a new request clears the previous black acknowledgement immediately");
        ck_allen_ui_refresh();
        ck_allen_ui_tick(now_us - 16000);
        CHECK(ck_allen_ui_asleep(), "sleep: a repeated request is acknowledged after a newly completed black frame");
        /* Queue a wake from the closing clock callback, after drawing but
         * before completion publication. The stale sleep must not win. */
        wake_during_frame = 2;
        ck_allen_ui_refresh();
        ck_allen_ui_tick(now_us - 16000);
        CHECK(!wake_during_frame && !ck_allen_ui_asleep(),
              "dawn: a wake queued before frame commit cannot be overwritten by that sleeping frame");
        uint32_t w0 = (uint32_t)(now_us / 1000);
        run_to(w0 + 300);
        CHECK(!ck_allen_ui_asleep(), "dawn: awake after ck_allen_ui_sleep(0)");
        uint64_t part = light(&P, 0, 0, W, H);
        CHECK(part > 0, "dawn: the sky is coming back a third of the way in");
        run_to(w0 + 1000);
        uint64_t full = light(&P, 0, 0, W, H);
        CHECK(full > part * 3 / 2, "dawn: the sky is back, brighter than a third of the way in");
        CHECK(stale(fb, copy, W, W, H) < 40, "dawn: nothing of the dusk is left behind on the screen");
        CHECK(cool_added(&P, sky, cx - R, cy - R, 2 * R, 2 * R) > 5000, "dawn: the visible cool network returns");
    }
    uint32_t frames, slowest, average;
    ck_allen_ui_stats(&frames, &slowest, &average);
    printf("note: %u frames at 3840x2160 on one core of this host: average %u us, slowest %u us (%s)\n", frames,
           average, slowest, cpu_clock ? "CPU time of this thread" : "wall clock");
    CHECK(frames > 1000, "ui: frames counted");

    /* Padded RGBX alone/with helpers, then the identical timeline in BGRX. */
    uint32_t w2 = 1280, h2 = 800, pitch2 = 1344;
    uint32_t *fb2[3];
    size_t need2 = ck_allen_ui_mem_bytes(w2, h2);
    void *mem2 = malloc(need2);
    for (int run = 0; run < 3; run++) {
        fb2[run] = calloc((size_t)pitch2 * h2, 4);
        CHECK(ck_allen_ui_start(fb2[run], w2, h2, pitch2, run == 2, mem2, need2, 1000000, 0, 0) == 0,
              "ui: starts at 1280x800");
        pthread_t th[3];
        helpers_run = run == 1;
        for (int i = 0; run == 1 && i < 3; i++)
            pthread_create(&th[i], 0, helper, 0);
        now_us = 0;
        run_to(15000);
        ck_allen_ui_wake("footer", now_us);
        ck_allen_ui_said("Hello Allen", "Hello Humanity.", now_us);
        run_to(18000);
        helpers_run = 0;
        for (int i = 0; run == 1 && i < 3; i++)
            pthread_join(th[i], 0);
    }
    struct pic P2 = {fb2[0], pitch2, 0};
    unsigned padding_clean = 1;
    for (uint32_t y = 0; y < h2; y++)
        for (uint32_t x = w2; x < pitch2; x++)
            padding_clean &= fb2[0][(size_t)y * pitch2 + x] == 0;
    unsigned format_exact = 1, colored = 0;
    for (uint32_t y = 0; y < h2; y++)
        for (uint32_t x = 0; x < w2; x++) {
            size_t p = (size_t)y * pitch2 + x;
            uint32_t bgr = fb2[2][p];
            uint32_t rgb = (bgr & 0xff00ff00u) | ((bgr & 0xffu) << 16) | ((bgr >> 16) & 0xffu);
            format_exact &= fb2[0][p] == rgb;
            colored |= (bgr & 0xffu) != ((bgr >> 16) & 0xffu);
        }
    CHECK(format_exact && colored, "ui: RGBX swaps red/blue exactly against an asymmetric BGRX frame");
    uint32_t c2y = h2 * 40 / 100;
    CHECK(count(&P2, w2 / 4, c2y + 120, w2 / 2, 200, BRIGHT) > 100, "ui: Allen answers at 1280x800");
    CHECK(padding_clean, "ui: nothing written past the visible width");
    CHECK(memcmp(fb2[0], fb2[1], (size_t)pitch2 * h2 * 4) == 0, "ui: three helper cores draw exactly the same frames");
    if (dir) {
        snprintf(path, sizeof path, "%s/allen-1280x800.png", dir);
        uint32_t *tmp = malloc((size_t)w2 * h2 * 4);
        for (uint32_t y = 0; y < h2; y++)
            memcpy(tmp + (size_t)y * w2, fb2[0] + (size_t)y * pitch2, w2 * 4);
        png(path, tmp, w2, 0, 0, 0, w2, h2, 1);
        free(tmp);
    }
    free(fb), free(copy), free(sky), free(mem), free(fb2[0]), free(fb2[1]), free(fb2[2]), free(mem2);
    wizard_screens(dir);
    printf("ALLEN_UI_TEST: %s (%d checks%s)\n", fails ? "FAIL" : "PASS", checks, fails ? ", some failed" : "");
    return fails ? 1 : 0;
}
