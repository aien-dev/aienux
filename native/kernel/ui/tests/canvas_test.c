/* canvas_test.c -- host test for Allen's canvas: the scene parser
 * (svc/allen_scene.c) and the renderer (ui/canvas.c) inside his home
 * (ui/allen_ui.c).
 *   1. The parser on fixed scenes: every kind, the colon and no-colon forms,
 *      the limits (lines, things, bytes, field room), unknown lines dropped
 *      quietly, plain words still spoken, thinking and fences ignored.
 *   2. A fuzz pass: random bytes, mutated scenes and word soup, each in a
 *      buffer of exactly its length (so a sanitizer sees any overrun); the
 *      parsed scene must stay inside its limits, printable, NUL-terminated;
 *      some of them are also laid out and drawn.
 *   3. Frames at 3840 x 2160: Allen forms, each sample scene arrives after a
 *      line; drawing only what changed must match drawing everything; the
 *      things light the sky beside him; pictures to argv[1].
 *   4. With --replies DIR: every NN-slug.txt file there (raw replies from the model) is
 *      parsed, counted and drawn the same way (live-*.png).
 * Frame times (this host, one core) are printed for the plain answers and
 * the scenes. Final line: CANVAS_TEST: PASS|FAIL (N checks). */
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../../svc/allen_scene.h"
#include "../allen_font.h"
#include "../allen_ui.h"
#include "../canvas.h"
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
static void png(const char *path, const uint32_t *px, uint32_t pitch, uint32_t x0, uint32_t y0, uint32_t w, uint32_t h,
                uint32_t step)
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
            raw[k++] = (uint8_t)(sr / (step * step)), raw[k++] = (uint8_t)(sg / (step * step)),
            raw[k++] = (uint8_t)(sb / (step * step));
        }
    }
    size_t blocks = raw_n / 65535 + 1, o = 0;
    uint8_t *z = malloc(2 + raw_n + blocks * 5 + 4);
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
    if (f) {
        fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
        uint8_t ihdr[13];
        be32(ihdr, ow), be32(ihdr + 4, oh);
        ihdr[8] = 8, ihdr[9] = 2, ihdr[10] = 0, ihdr[11] = 0, ihdr[12] = 0;
        chunk(f, "IHDR", ihdr, 13);
        chunk(f, "IDAT", z, o);
        chunk(f, "IEND", 0, 0);
        fclose(f);
        printf("note: wrote %s (%ux%u)\n", path, ow, oh);
    }
    free(raw), free(z);
}

/* ---- the parser's promises, checked on any scene ---- */
static int sane(const ck_scene *s)
{
    if (s->nthings > CK_SCENE_THINGS || s->used > CK_SCENE_TEXT)
        return 0;
    for (unsigned i = 0; i < s->used; i++)
        if (s->text[i] && (s->text[i] < 32 || s->text[i] > 126))
            return 0;
    if (s->has_say && (s->say >= s->used || strlen(s->text + s->say) > 159))
        return 0;
    for (unsigned t = 0; t < s->nthings; t++) {
        const ck_scene_thing *th = &s->thing[t];
        if (th->kind < CK_THING_NOTE || th->kind > CK_THING_VIDEO || th->nf == 0 || th->nf > CK_SCENE_FIELDS)
            return 0;
        for (unsigned f = 0; f < th->nf; f++) {
            if (th->f[f] >= s->used || !memchr(s->text + th->f[f], 0, s->used - th->f[f]))
                return 0;
            if (strlen(s->text + th->f[f]) > 243)
                return 0;
        }
    }
    return 1;
}

static int parse_str(const char *in, ck_scene *s, ck_scene_stats *st) { return ck_scene_parse(in, strlen(in), s, st); }

static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint32_t rnd(void)
{
    rng ^= rng << 13, rng ^= rng >> 7, rng ^= rng << 17;
    return (uint32_t)(rng >> 16);
}

/* ---- the screen ---- */
static uint64_t real_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}
static uint64_t now_us;
struct timing {
    uint64_t sum, n, worst;
};
static struct timing *timing;
static void run_to(uint32_t ms)
{
    for (; now_us <= (uint64_t)ms * 1000u; now_us += 16000) {
        uint64_t a = real_us();
        int drew = ck_allen_ui_tick(now_us);
        uint64_t d = real_us() - a;
        if (drew && timing) {
            timing->sum += d, timing->n++;
            if (d > timing->worst)
                timing->worst = d;
        }
    }
}
static unsigned differ(const uint32_t *a, const uint32_t *b, size_t n, int tol)
{
    unsigned c = 0;
    for (size_t i = 0; i < n; i++)
        for (int s = 0; s < 24; s += 8) {
            int e = (int)(a[i] >> s & 255) - (int)(b[i] >> s & 255);
            if (e > tol || e < -tol) {
                c++;
                break;
            }
        }
    return c;
}
static unsigned stale(uint32_t *fb, uint32_t *copy, size_t n)
{
    memcpy(copy, fb, n * 4);
    ck_allen_ui_refresh();
    struct timing *keep = timing;
    timing = 0;
    ck_allen_ui_tick(now_us - 16000);
    timing = keep;
    return differ(fb, copy, n, 6);
}
/* pixels in a rectangle brighter than the night by more than tol in any channel */
static unsigned lit(const uint32_t *fb, uint32_t pitch, uint32_t x0, uint32_t y0, uint32_t w, uint32_t h, int tol)
{
    unsigned n = 0;
    for (uint32_t y = y0; y < y0 + h; y++)
        for (uint32_t x = x0; x < x0 + w; x++) {
            uint32_t c = fb[(size_t)y * pitch + x];
            n += (int)(c >> 16 & 255) > 6 + tol || (int)(c >> 8 & 255) > 8 + tol || (int)(c & 255) > 18 + tol;
        }
    return n;
}
/* text-bright pixels (type) in a rectangle */
static unsigned bright(const uint32_t *fb, uint32_t pitch, uint32_t x0, uint32_t y0, uint32_t w, uint32_t h)
{
    unsigned n = 0;
    for (uint32_t y = y0; y < y0 + h; y++)
        for (uint32_t x = x0; x < x0 + w; x++) {
            uint32_t c = fb[(size_t)y * pitch + x];
            n += (c >> 16 & 255) > 150 && (c >> 8 & 255) > 150 && (c & 255) > 150;
        }
    return n;
}

struct sample {
    const char *name, *line, *scene;
};
static const struct sample samples[] = {
    {"sum", "what's 17% of 240",
     "mood calm\nsay Seventeen percent of 240 is 40.8.\nnumber right: 40.8 | 17% of 240\n"
     "note left quiet: How | 240 x 0.17 = 40.8, a tenth (24) plus seven hundredths (16.8)."},
    {"reminder", "remind me to call mom at 6",
     "mood dawn\nsay I'll keep this in front of you: call Mom at six.\ntimer right warm: 6:00 pm | Call Mom"},
    {"friend", "tell my friend Sam I'm running late",
     "say I'll tell Sam you're running late.\nperson to right: Sam | Running late, I'll be there soon."},
    {"video", "play a video about black holes",
     "mood deep\nsay A film about black holes will play here once I can reach one.\n"
     "video right: Black holes, explained | a short film\n"
     "note left: While it comes | A black hole is gravity so strong that not even light gets out."},
    {"beautiful", "show me something beautiful",
     "mood clear lively\nsay Look up a moment. Here is something old and quiet.\n"
     "quote left: We are made of star-stuff. | Carl Sagan\npicture right: The Pleiades rising at midnight"},
    {"walk", "what's the weather like for a walk",
     "mood dawn\nsay I can't see outside yet. Here is what to check before you go.\n"
     "list right: Before a walk | Look out of a window | A layer for the wind | Water and your keys"},
    {"focus", "give me 25 minutes to focus",
     "say Twenty-five minutes, starting now. I'll keep the time.\ntimer above: 25m | Focus"},
};
#define NSAMPLES (sizeof samples / sizeof samples[0])

static uint32_t W = 3840, H = 2160;
static unsigned base_lit, base_type; /* the same regions with no things, after a plain answer */
static uint32_t *fb, *cpy;

/* A line, the model's reply to it, and what it paints: checks, picture, the frame times in *tm. */
static void show(const char *tag, const char *line, const char *reply, size_t len, const char *dir, int expect_things,
                 struct timing *tm)
{
    char path[512], what[160];
    uint32_t t0 = (uint32_t)(now_us / 1000);
    timing = tm;
    ck_allen_ui_think(line, now_us);
    run_to(t0 + 1400); /* he thinks; the old things go */
    ck_allen_ui_scene(reply, (unsigned)len, now_us);
    uint32_t t1 = (uint32_t)(now_us / 1000);
    run_to(t1 + 900); /* the spark flies, the first thing opens */
    snprintf(what, sizeof what, "%s: drawn whole while things arrive", tag);
    CHECK(stale(fb, cpy, (size_t)W * H) < 60, what);
    run_to(t1 + 3600);
    snprintf(what, sizeof what, "%s: drawn whole once they rest", tag);
    CHECK(stale(fb, cpy, (size_t)W * H) < 60, what);
    run_to(t1 + 5400); /* and Allen has finished speaking */
    timing = 0;
    if (expect_things) {
        uint32_t cx = W / 2, cy = H * 40 / 100;
        unsigned side = lit(fb, W, 200, cy - 700, cx - 200 - 400, 1300, 30) + lit(fb, W, cx + 400, cy - 700, W - cx - 600, 1300, 30) +
                        lit(fb, W, cx - 600, 80, 1200, cy - 80 - 280, 30);
        unsigned type = bright(fb, W, 200, 80, cx - 600, 1800) + bright(fb, W, cx + 400, 80, W - cx - 600, 1800) +
                        bright(fb, W, cx - 600, 80, 1200, cy - 80 - 280);
        printf("note: %s: %u lit, %u type (no things: %u, %u)\n", tag, side, type, base_lit, base_type);
        snprintf(what, sizeof what, "%s: things light the sky beside or above Allen (%u lit, %u type; no things: %u, %u)", tag,
                 side, type, base_lit, base_type);
        CHECK(type > base_type + 2500, what);
        (void)side;
    } else if (!strncmp(tag, "plain", 5)) {
        uint32_t cx = W / 2, cy = H * 40 / 100;
        base_lit = lit(fb, W, 200, cy - 700, cx - 200 - 400, 1300, 30) + lit(fb, W, cx + 400, cy - 700, W - cx - 600, 1300, 30) +
                   lit(fb, W, cx - 600, 80, 1200, cy - 80 - 280, 30);
        base_type = bright(fb, W, 200, 80, cx - 600, 1800) + bright(fb, W, cx + 400, 80, W - cx - 600, 1800) +
                    bright(fb, W, cx - 600, 80, 1200, cy - 80 - 280);
    }
    if (dir) {
        snprintf(path, sizeof path, "%s/%s.png", dir, tag);
        png(path, fb, W, 0, 0, W, H, 2);
    }
}

static char *slurp(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    long m = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)m + 1);
    *n = fread(b, 1, (size_t)m, f);
    b[*n] = 0;
    fclose(f);
    return b;
}

static void no_mark(ck_rect r) { (void)r; }

static int by_name(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

int main(int argc, char **argv)
{
    const char *dir = 0, *replies = 0;
    unsigned fuzz_n = 300000;
    int parse_only = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--replies") && i + 1 < argc)
            replies = argv[++i];
        else if (!strcmp(argv[i], "--parse-only"))
            parse_only = 1;
        else if (!strcmp(argv[i], "--fuzz") && i + 1 < argc)
            fuzz_n = (unsigned)strtoul(argv[++i], 0, 10);
        else
            dir = argv[i];
    }
    crc_init();
    static ck_scene s;
    ck_scene_stats st;

    /* ---- 1. the parser on fixed scenes ---- */
    CHECK(parse_str("mood dawn lively\nsay Seventeen percent of 240 is 40.8.\nnumber right warm: 40.8 | 17% of 240\n"
                    "note left: How | 240 x 0.17 = 40.8\n",
                    &s, &st) &&
              s.tone == CK_TONE_DAWN && s.water == CK_WATER_LIVELY && !strcmp(ck_scene_say(&s), "Seventeen percent of 240 is 40.8.") &&
              s.nthings == 2 && s.thing[0].kind == CK_THING_NUMBER && s.thing[0].place == CK_PLACE_RIGHT && s.thing[0].warm &&
              !strcmp(ck_scene_field(&s, &s.thing[0], 0), "40.8") && !strcmp(ck_scene_field(&s, &s.thing[0], 1), "17% of 240") &&
              s.thing[1].kind == CK_THING_NOTE && s.thing[1].place == CK_PLACE_LEFT && ck_scene_clean(&st, &s) && st.kept == 4,
          "scene: mood, say and two things, read whole");
    {
        static const char *const kinds[] = {"note: a | b",          "list: Title | one | two", "number: 42",
                                            "quote: Be here. | Ram", "person: Sam | late",      "timer: 25m | Focus",
                                            "picture: the sea",      "video: rain | 3 min"};
        int ok = 1;
        for (unsigned k = 0; k < 8; k++) {
            char b[96];
            snprintf(b, sizeof b, "say ok\n%s", kinds[k]);
            if (!parse_str(b, &s, &st) || s.nthings != 1 || s.thing[0].kind != k + 1 || !ck_scene_clean(&st, &s)) {
                printf("  kind %u: \"%s\" things %u rejected %u\n", k + 1, kinds[k], s.nthings, st.rejected);
                ok = 0;
            }
        }
        CHECK(ok, "scene: every kind of thing");
    }
    CHECK(parse_str("Say: Hi there.\nNUMBER 6:00 pm | dinner\ntimer 6:00 pm | call", &s, &st) && s.nthings == 2 &&
              !strcmp(ck_scene_say(&s), "Hi there.") && !strcmp(ck_scene_field(&s, &s.thing[0], 0), "6:00 pm") &&
              !strcmp(ck_scene_field(&s, &s.thing[1], 1), "call") && ck_scene_clean(&st, &s),
          "scene: any case, and the forms without a colon after the words");
    CHECK(parse_str("person from: Ana | Landed safe, see you soon\nsay Ana wrote.", &s, &st) && s.thing[0].way == CK_WAY_FROM &&
              parse_str("note to: x | y\nsay a", &s, &st) && st.unknown == 1 && s.nthings == 1,
          "scene: to and from are a person's; elsewhere an unknown word, ignored");
    CHECK(parse_str("say hi\nwindow left: a | b\nbutton: OK\nnote: fine", &s, &st) && s.nthings == 1 && st.rejected == 2 &&
              !ck_scene_clean(&st, &s),
          "scene: lines it does not know are dropped and counted, never shown");
    CHECK(parse_str("Sunlight scatters in the air, blue most of all. That is why the sky is blue.", &s, &st) &&
              !strcmp(ck_scene_say(&s), "Sunlight scatters in the air, blue most of all. That is why the sky is blue.") &&
              !s.nthings && st.rejected == 1,
          "scene: plain words, no scene: Allen still says them");
    CHECK(parse_str("<think>\nthe user wants a sum\nnote: nope\n</think>\n```\nsay 2.\nnumber: 2\n```", &s, &st) &&
              s.nthings == 1 && !strcmp(ck_scene_say(&s), "2.") && st.lines == 2 && st.tidied == 2,
          "scene: thinking is skipped, fences taken off (and counted)");
    CHECK(parse_str("- say a\n* note: b\n1. list: c | d\n**number**: 5", &s, &st) && s.nthings == 3 && s.has_say &&
              st.tidied == 4 && !st.rejected,
          "scene: list marks and bold taken off, counted as tidied");
    {
        char b[4096];
        unsigned n = 0;
        n += (unsigned)snprintf(b + n, sizeof b - n, "say first\n");
        for (int i = 0; i < 15; i++)
            n += (unsigned)snprintf(b + n, sizeof b - n, "note: line %d\n", i);
        CHECK(parse_str(b, &s, &st) && s.nthings == CK_SCENE_THINGS && st.lines == 16 && st.over == 4 + 7,
              "scene: at most 12 lines and 4 things");
        memset(b, 'a', sizeof b);
        memcpy(b, "say ", 4);
        b[sizeof b - 1] = 0;
        CHECK(!parse_str(b, &s, &st) && st.truncated && !s.has_say, "scene: past 2 KiB is not read; the cut line is dropped");
        {
            static const char nul[] = "say hi\nnote: x\0note: hidden";
            CHECK(ck_scene_parse(nul, sizeof nul - 1, &s, &st) == 1 && s.nthings == 1, "scene: a NUL ends the reply");
        }
        char lng[600];
        memset(lng, 0, sizeof lng);
        strcpy(lng, "say ok\nnumber: 1 | ");
        for (int i = 0; i < 40; i++)
            strcat(lng, "word ");
        parse_str(lng, &s, &st);
        const char *cap = ck_scene_field(&s, &s.thing[0], 1);
        CHECK(strlen(cap) <= 64 && !strcmp(cap + strlen(cap) - 3, "...") && st.cut == 1, "scene: a field too long is cut at a word, with ...");
    }
    CHECK(parse_str("say caf\xc3\xa9 \x01\x7f ok\tthen", &s, &st) && !strcmp(ck_scene_say(&s), "caf ok then") && st.cut == 1,
          "scene: only printable ASCII is kept");
    CHECK(parse_str("say a\nnumber: lots | of things\nperson: Sam", &s, &st) && !s.nthings && st.rejected == 2,
          "scene: a number with no digits, a person with no message: dropped");
    CHECK(parse_str("say one\nsay two\nmood deep\nmood dawn", &s, &st) && !strcmp(ck_scene_say(&s), "one") && s.tone == CK_TONE_DEEP &&
              st.over == 2,
          "scene: the first say and mood count");
    CHECK(ck_scene_duration_ms("25m") == 1500000 && ck_scene_duration_ms("1h 30m") == 5400000 &&
              ck_scene_duration_ms("90 seconds") == 90000 && ck_scene_duration_ms("in 10 minutes") == 600000 &&
              ck_scene_duration_ms("1.5 hours") == 5400000 && ck_scene_duration_ms("6:00 pm") == 0 &&
              ck_scene_duration_ms("soon") == 0 && ck_scene_duration_ms("25 parsecs") == 0 &&
              ck_scene_duration_ms("30 hours") == 0 && ck_scene_duration_ms("") == 0 &&
              ck_scene_duration_ms("99999999999 s") == 0,
          "scene: a timer's length (and what is not one)");
    {
        char w[] = "say hi\\nnote: a \\\\ b";
        size_t n = ck_scene_unwire(w, strlen(w));
        CHECK(n == strlen("say hi\nnote: a \\ b") && !strcmp(w, "say hi\nnote: a \\ b"), "scene: the courier's one-line form unwired");
    }
    CHECK(!ck_scene_parse(0, 10, &s, &st) && !ck_scene_parse("", 0, &s, 0) && !ck_scene_parse("\n\n  \n", 5, &s, &st),
          "scene: nothing in, nothing to show");

    /* ---- 2. fuzz ---- */
    {
        static const char *const soup[] = {"say", "mood", "note", "list", "number", "quote", "person", "timer",
                                           "picture", "video", ":", "|", " ", "\n", "left", "right", "above", "warm",
                                           "quiet", "to", "from", "bloom", "rise", "fade", "dawn", "deep", "lively",
                                           "12", "6:00", "25m", "```", "<think>", "</think>", "-", "**", "\xe2\x80\x94",
                                           "\t", "\r", "||||", "::::", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"};
        unsigned nsoup = sizeof soup / sizeof soup[0], bad = 0, shown = 0, things = 0, rejected = 0, lines = 0;
        unsigned drawn = 0;
        char *buf = malloc(4096);
        /* a small screen for drawing fuzzed scenes */
        static uint8_t pv[2 << 20], pb[2 << 20], ps[2 << 20];
        static ck_af_face fv, fbd, fs;
        uint32_t k = 170, cw = 1280, ch = 800;
        ck_af_face_init(&fv, 19 * k / 256, 1000, pv, sizeof pv);
        ck_af_face_init(&fbd, 15 * k / 256, 1150, pb, sizeof pb);
        ck_af_face_init(&fs, 11 * k / 256, 1250, ps, sizeof ps);
        for (int c = CK_AF_FIRST; c <= CK_AF_LAST; c++)
            ck_af_glyph_get(&fv, (char)c), ck_af_glyph_get(&fbd, (char)c), ck_af_glyph_get(&fs, (char)c);
        static ck_allen_canvas cv0;
        uint32_t *px = calloc((size_t)cw * ch, 4);
        ck_canvas pic = {px, (int32_t)cw, (int32_t)ch};
        CHECK(ck_allen_canvas_init(&cv0, (int32_t)cw, (int32_t)ch, k, (int32_t)cw / 2, (int32_t)ch * 40 / 100, &fv, &fbd, &fs) == 0,
              "fuzz: a small canvas to draw on");
        uint32_t tt = 1000;
        for (unsigned it = 0; it < fuzz_n; it++) {
            size_t n = 0;
            unsigned mode = rnd() % 3;
            if (mode == 0) { /* random bytes */
                n = rnd() % 3000;
                if (n > 4096)
                    n = 4096;
                for (size_t i = 0; i < n; i++)
                    buf[i] = (char)(rnd() & 0xff);
            } else if (mode == 1) { /* a sample, mutated */
                const char *base = samples[rnd() % NSAMPLES].scene;
                n = strlen(base);
                memcpy(buf, base, n);
                for (unsigned m = rnd() % 12; m; m--) {
                    unsigned op = rnd() % 4, at = n ? rnd() % (unsigned)n : 0;
                    if (op == 0 && n)
                        buf[at] = (char)(rnd() & 0xff);
                    else if (op == 1 && n < 4000)
                        memmove(buf + at + 1, buf + at, n - at), buf[at] = "|:\n \x00"[rnd() % 5], n++;
                    else if (op == 2 && n)
                        memmove(buf + at, buf + at + 1, n - at - 1), n--;
                    else if (n && n * 2 < 4000) /* the whole thing again */
                        memcpy(buf + n, buf, n), n *= 2;
                }
            } else { /* word soup */
                unsigned words = rnd() % 200;
                for (unsigned w = 0; w < words; w++) {
                    const char *x = soup[rnd() % nsoup];
                    size_t xl = strlen(x);
                    if (n + xl >= 4096)
                        break;
                    memcpy(buf + n, x, xl), n += xl;
                }
            }
            char *exact = malloc(n ? n : 1); /* exactly n bytes: reading past them is caught */
            memcpy(exact, buf, n);
            int r = ck_scene_parse(exact, n, &s, &st);
            free(exact);
            if (!sane(&s))
                bad++;
            shown += r != 0, things += s.nthings, rejected += st.rejected, lines += st.lines;
            if (s.nthings && (it % 97 == 0)) { /* drawn too */
                ck_allen_canvas_show(&cv0, &s, tt);
                for (uint32_t d = 0; d <= 3200; d += 400) {
                    ck_allen_canvas_at(&cv0, tt + d, (int32_t)cw * 8, (int32_t)ch * 6, 0, no_mark);
                }
                ck_rect all = {0, 0, (int32_t)cw, (int32_t)ch};
                ck_allen_canvas_draw(&cv0, &pic, &all);
                drawn++;
                tt += 5000;
            }
        }
        free(buf);
        free(px);
        printf("note: fuzz %u replies: %u gave something to show, %u things, %u lines read, %u lines dropped, %u drawn, %u broke a limit\n",
               fuzz_n, shown, things, lines, rejected, drawn, bad);
        CHECK(bad == 0, "fuzz: every parsed scene stays within its limits, printable and NUL-terminated");
        CHECK(fuzz_n < 1000 || (shown > fuzz_n / 10 && drawn > fuzz_n / 400), "fuzz: the fuzz reached real scenes and drew them");
    }

    if (parse_only) {
        printf("CANVAS_TEST: %s (%d checks%s, parser and fuzz only)\n", fails ? "FAIL" : "PASS", checks,
               fails ? ", some failed" : "");
        return fails ? 1 : 0;
    }
    /* ---- 3. frames at 3840 x 2160 ---- */
    fb = calloc((size_t)W * H, 4), cpy = malloc((size_t)W * H * 4);
    size_t need = ck_allen_ui_mem_bytes(W, H);
    void *mem = malloc(need);
    CHECK(ck_allen_ui_start(fb, W, H, W, 1, mem, need, 1000000, 0, real_us) == 0, "ui: starts at 3840x2160 with the canvas");
    struct timing t_start = {0}, t_plain = {0}, t_scene = {0}, t_live = {0};
    timing = &t_start;
    now_us = 0;
    run_to(16500);
    ck_allen_ui_wake("Type goodbye and this computer rests; any key wakes it", now_us);
    run_to(17800);
    timing = 0;
    /* plain answers first: the cost of the frames without things */
    static const char *const plain[][2] = {{"hello allen", "Hello Humanity."},
                                           {"how are you", "I'm running well. 4 cores awake."},
                                           {"what is 12 times 4", "12 times 4 is 48."}};
    for (unsigned i = 0; i < 3; i++) {
        char tag[32];
        snprintf(tag, sizeof tag, "plain-%u", i + 1);
        show(tag, plain[i][0], plain[i][1], strlen(plain[i][1]), 0, 0, &t_plain);
    }
    for (unsigned i = 0; i < NSAMPLES; i++) {
        char tag[48];
        snprintf(tag, sizeof tag, "canvas-%u-%s", i + 1, samples[i].name);
        show(tag, samples[i].line, samples[i].scene, strlen(samples[i].scene), dir, 1, &t_scene);
        if (i == 0 && dir) { /* the arrival itself, a moment in */
            ck_allen_ui_think("what's 17% of 240", now_us);
            run_to((uint32_t)(now_us / 1000) + 1400);
            ck_allen_ui_scene(samples[0].scene, (unsigned)strlen(samples[0].scene), now_us);
            uint32_t ts = (uint32_t)(now_us / 1000);
            char path[512];
            static const uint32_t at[3] = {700, 850, 1050};
            for (unsigned q = 0; q < 3; q++) { /* the spark on its way from his heart, at full size */
                run_to(ts + at[q]);
                snprintf(path, sizeof path, "%s/canvas-0-arriving-%u.png", dir, q + 1);
                png(path, fb, W, W / 2 - 200, H * 40 / 100 - 520, 1800, 800, 1);
            }
            snprintf(path, sizeof path, "%s/canvas-0-arriving.png", dir);
            png(path, fb, W, 0, 0, W, H, 2);
            run_to((uint32_t)(now_us / 1000) + 2800);
        }
    }
    /* left alone, the things stay a while and then go; Allen is alone again */
    {
        uint32_t t0 = (uint32_t)(now_us / 1000);
        run_to(t0 + 30000);
        CHECK(ck_allen_ui_started(), "rest: still running");
        ck_allen_ui_think("thanks", now_us);
        run_to(t0 + 30000 + 1200);
        CHECK(stale(fb, cpy, (size_t)W * H) < 60, "rest: things leaving are drawn whole");
        ck_allen_ui_said(0, "You're welcome.", now_us);
        run_to(t0 + 30000 + 4000);
        uint32_t cx = W / 2;
        unsigned left = bright(fb, W, 200, 80, cx - 600, 1800) + bright(fb, W, cx + 400, 80, W - cx - 600, 1800) +
                        bright(fb, W, cx - 600, 80, 1200, H * 40 / 100 - 80 - 280);
        char what[160];
        snprintf(what, sizeof what, "rest: a new line sends the things away; only Allen and his words stay (%u type, %u with none)",
                 left, base_type);
        CHECK(left < base_type + 2500, what);
        if (dir) {
            char path[512];
            snprintf(path, sizeof path, "%s/canvas-9-gone.png", dir);
            png(path, fb, W, 0, 0, W, H, 2);
        }
    }

    /* ---- 4. real replies from the model ---- */
    if (replies) {
        DIR *d = opendir(replies);
        char *names[64];
        unsigned nn = 0, clean = 0, used = 0, parsed = 0;
        struct dirent *e;
        while (d && (e = readdir(d)) && nn < 64) {
            size_t l = strlen(e->d_name);
            if (l > 4 && !strcmp(e->d_name + l - 4, ".txt") && e->d_name[0] >= '0' && e->d_name[0] <= '9') /* NN-slug.txt */
                names[nn++] = strdup(e->d_name);
        }
        if (d)
            closedir(d);
        qsort(names, nn, sizeof names[0], by_name);
        CHECK(nn > 0, "live: replies to read");
        for (unsigned i = 0; i < nn; i++) {
            char path[1024], ask_path[1024], tag[128];
            snprintf(path, sizeof path, "%s/%s", replies, names[i]);
            snprintf(ask_path, sizeof ask_path, "%s/ask-%s", replies, names[i]);
            size_t n = 0, an = 0;
            char *b = slurp(path, &n), *ask = slurp(ask_path, &an);
            if (!b)
                continue;
            int r = ck_scene_parse(b, n, &s, &st);
            int ok = ck_scene_clean(&st, &s);
            parsed += r != 0, clean += ok, used += s.nthings > 0;
            printf("live %s: %s; lines %u kept %u dropped %u tidied %u over %u cut %u; things %u:", names[i],
                   ok ? "CLEAN" : "not clean", st.lines, st.kept, st.rejected, st.tidied, st.over, st.cut, s.nthings);
            for (unsigned t = 0; t < s.nthings; t++)
                printf(" %s", ck_scene_kind_name(s.thing[t].kind));
            printf("\n");
            CHECK(sane(&s), "live: the reply's scene stays within its limits");
            snprintf(tag, sizeof tag, "live-%.*s", (int)(strlen(names[i]) - 4), names[i]);
            if (ask) {
                char *nl = strchr(ask, '\n');
                if (nl)
                    *nl = 0;
            }
            show(tag, ask ? ask : "a question", b, n, dir, 0, &t_live);
            free(b), free(ask), free(names[i]);
        }
        printf("LIVE: %u replies, %u parsed cleanly, %u used the canvas (at least one thing), %u gave something to show\n",
               nn, clean, used, parsed);
    }
    uint32_t frames, slowest, average;
    ck_allen_ui_stats(&frames, &slowest, &average);
    printf("note: frames at 3840x2160 on one core of this host (us): whole run avg %u worst %u over %u frames\n", average,
           slowest, frames);
    printf("note: startup avg %llu worst %llu (%llu frames); plain answers avg %llu worst %llu (%llu); "
           "scenes avg %llu worst %llu (%llu)",
           (unsigned long long)(t_start.n ? t_start.sum / t_start.n : 0), (unsigned long long)t_start.worst,
           (unsigned long long)t_start.n, (unsigned long long)(t_plain.n ? t_plain.sum / t_plain.n : 0),
           (unsigned long long)t_plain.worst, (unsigned long long)t_plain.n,
           (unsigned long long)(t_scene.n ? t_scene.sum / t_scene.n : 0), (unsigned long long)t_scene.worst,
           (unsigned long long)t_scene.n);
    if (t_live.n)
        printf("; live avg %llu worst %llu (%llu)", (unsigned long long)(t_live.sum / t_live.n),
               (unsigned long long)t_live.worst, (unsigned long long)t_live.n);
    printf("\n");
    free(fb), free(cpy), free(mem);
    printf("CANVAS_TEST: %s (%d checks%s)\n", fails ? "FAIL" : "PASS", checks, fails ? ", some failed" : "");
    return fails ? 1 : 0;
}
