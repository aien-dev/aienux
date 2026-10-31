/* allen_ui_test.c -- host test for Allen's home (ui/allen_ui.c with the night
 * sky, ui/night.c, and his presence, ui/mind.c), his typeface
 * (ui/allen_font.c) and his first answers (svc/allen_reply.c). Runs the
 * startup at 3840 x 2160 BGRX on a hand-moved clock, wakes Allen, types
 * "Hello Allen", lets him answer, leaves him alone, saves a note; checks the
 * pixels at each step, checks that drawing only the changed parts gives the
 * same picture as drawing everything, runs 1280 x 800 RGBX with and without
 * three helper threads (the frames must match exactly), and writes PNG
 * pictures to the directory given as argv[1] (if any) for a person to look at.
 * Final line: ALLEN_UI_TEST: PASS|FAIL (N checks). */
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include "../../svc/allen_reply.h"
#include "../allen_font.h"
#include "../allen_ui.h"
#include "../night.h"

static int checks, fails;
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
                    uint32_t c = px[((size_t)y0 + (size_t)y * step + j) * pitch + x0 + (size_t)x * step + i];
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
    /* 0644, not fopen's 0666 & ~umask: a picture for a person, never world-writable. */
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    FILE *f = fd < 0 ? NULL : fdopen(fd, "wb");
    if (fd >= 0 && !f)
        close(fd);
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
enum { BRIGHT, BLACK, VIOLET, LIT };
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
            else if (kind == VIOLET)
                n += b > 120 && b > r + 30;
            else
                n += r > 60 || g > 60 || b > 90;
        }
    return n;
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

static uint64_t real_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
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
static int bare_init(uint32_t w, uint32_t h, void *scratch)
{
    uint32_t k = w * 256u / 1920u;
    size_t nb = ck_night_mem_bytes(w, h, k);
    return ck_night_init(&bare_night, w, h, k, (int32_t)w / 2, (int32_t)(h * 40 / 100), malloc(nb), nb, scratch);
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

static volatile int helpers_run;
static void *helper(void *arg)
{
    (void)arg;
    while (helpers_run)
        ck_allen_ui_help();
    return 0;
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : 0;
    char path[512];
    crc_init();

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
            {"help", "I learn what you teach me (remember my car is red) and keep notes", 3666},
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

    /* ---- Allen's home at 3840 x 2160, BGRX ---- */
    uint32_t W = 3840, H = 2160;
    uint32_t *fb = calloc((size_t)W * H, 4), *copy = malloc((size_t)W * H * 4), *sky = malloc((size_t)W * H * 4);
    size_t need = ck_allen_ui_mem_bytes(W, H);
    void *mem = malloc(need);
    struct pic P = {fb, W, 1};
    CHECK(ck_allen_ui_start(fb, W, H, W, 1, mem, need - 1, 1000000, 0, real_us) == -1, "ui: short memory refused");
    CHECK(ck_allen_ui_start(fb, W, H, W, 2, mem, need, 1000000, 0, real_us) == -1, "ui: unknown pixel format refused");
    CHECK(bare_init(W, H, sky) == 0, "night: the bare sky for the checks");
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
    CHECK(count(&P, cx - R, cy - R, 2 * R, 2 * R, BRIGHT) > 1000 && count(&P, cx - R, cy - R, 2 * R, 2 * R, VIOLET) > 5000,
          "allen: formed, a bright violet mind in the middle");
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
    ck_allen_ui_wake("Type goodbye to restart this computer", now_us);
    run_to(17800);
    CHECK(ck_allen_ui_on(), "ui: on");
    unsigned bar0 = added(&P, sky, bx, by, bw, bh);
    CHECK(bar0 > 2500, "bar: up once he is awake, with its hint");
    CHECK(added(&P, sky, 0, by + bh + 10, W, H - by - bh - 10) > 1000, "bar: the footer under the empty bar");
    const char *typed = "Hello Allen";
    for (unsigned i = 1; i <= 11; i++) {
        ck_allen_ui_input(typed, i, now_us);
        run_to((uint32_t)(now_us / 1000) + 120);
    }
    CHECK(count(&P, bx, by, bw, bh, BRIGHT) > 500, "bar: what is typed appears in it");
    CHECK(added(&P, sky, 0, by + bh + 10, W, H - by - bh - 10) < 400, "bar: the footer goes while typing");
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
    /* left alone: his words fade once read, then the bar; only Allen is left, waiting */
    run_to(said + 24000);
    CHECK(added(&P, sky, W / 4, wy, W / 2, wh) < 400, "rest: his words fade once read");
    CHECK(added(&P, sky, 0, by - 40, W, H - by + 40) < 400, "rest: the bar fades when nothing is in use");
    CHECK(count(&P, cx - R, cy - R, 2 * R, 2 * R, BRIGHT) > 1000 && count(&P, cx - R, cy - R, 2 * R, 2 * R, VIOLET) > 5000,
          "rest: Allen stays, waiting");
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
    /* ---- sleep: dark but for a faint mark, nothing moves; waking brings the sky back ---- */
    {
        ck_allen_ui_sleep(1, now_us);
        uint32_t s0 = (uint32_t)(now_us / 1000);
        run_to(s0 + 200);
        CHECK(ck_allen_ui_asleep(), "sleep: asleep after ck_allen_ui_sleep(1)");
        unsigned lit = 0;
        for (uint32_t y = 0; y < H; y += 4)
            for (uint32_t x = 0; x < W; x += 4)
                lit += (fb[(size_t)y * W + x] & 0xffffffu) != 0;
        CHECK(lit > 3 && lit < 4000, "sleep: the screen is dark but for a faint mark (more than one pixel)");
        memcpy(copy, fb, (size_t)W * H * 4);
        run_to(s0 + 2000);
        CHECK(differ(fb, copy, W, W, H, 0) == 0, "sleep: nothing moves while asleep");
        ck_allen_ui_sleep(0, now_us);
        run_to(s0 + 2300);
        CHECK(!ck_allen_ui_asleep(), "sleep: awake after ck_allen_ui_sleep(0)");
        CHECK(differ(fb, copy, W, W, H, 0) > 100000, "sleep: the sky is back after waking");
    }
    uint32_t frames, slowest, average;
    ck_allen_ui_stats(&frames, &slowest, &average);
    printf("note: %u frames at 3840x2160 on one core of this host: average %u us, slowest %u us\n", frames, average,
           slowest);
    CHECK(frames > 1000, "ui: frames counted");

    /* ---- 1280 x 800 RGBX with a padded pitch (QEMU's usual mode); then the same with three helper cores ---- */
    uint32_t w2 = 1280, h2 = 800, pitch2 = 1344;
    uint32_t *fb2[2];
    size_t need2 = ck_allen_ui_mem_bytes(w2, h2);
    void *mem2 = malloc(need2);
    for (int run = 0; run < 2; run++) {
        fb2[run] = calloc((size_t)pitch2 * h2, 4);
        CHECK(ck_allen_ui_start(fb2[run], w2, h2, pitch2, 0, mem2, need2, 1000000, 0, 0) == 0, "ui: starts at 1280x800");
        pthread_t th[3];
        helpers_run = run;
        for (int i = 0; run && i < 3; i++)
            pthread_create(&th[i], 0, helper, 0);
        now_us = 0;
        run_to(15000);
        ck_allen_ui_wake("footer", now_us);
        ck_allen_ui_said("Hello Allen", "Hello Humanity.", now_us);
        run_to(18000);
        helpers_run = 0;
        for (int i = 0; run && i < 3; i++)
            pthread_join(th[i], 0);
    }
    struct pic P2 = {fb2[0], pitch2, 0};
    unsigned padding_clean = 1;
    for (uint32_t y = 0; y < h2; y++)
        for (uint32_t x = w2; x < pitch2; x++)
            padding_clean &= fb2[0][(size_t)y * pitch2 + x] == 0;
    uint32_t c2x = w2 / 2, c2y = h2 * 40 / 100;
    CHECK(count(&P2, c2x - 60, c2y - 60, 120, 120, VIOLET) > 2000, "ui: RGBX screens get red and blue swapped");
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
    free(fb), free(copy), free(sky), free(mem), free(fb2[0]), free(fb2[1]), free(mem2);
    printf("ALLEN_UI_TEST: %s (%d checks%s)\n", fails ? "FAIL" : "PASS", checks, fails ? ", some failed" : "");
    return fails ? 1 : 0;
}
