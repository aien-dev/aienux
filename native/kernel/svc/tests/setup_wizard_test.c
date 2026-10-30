/* setup_wizard_test.c -- host test of the setup inventory (svc/setup_inv.c)
 * and Allen's setup wizard (svc/setup_wizard.c) against the three machine
 * fixtures in fixtures/setup: a games PC, an AI PC and the Spark. Every path
 * of FIRST_RUN_UX.md screens 3 to 9 is walked with keys and the clock: both
 * intents and their own words, Set it up / Show me each one / Not now, the
 * three ways to an AI (found here, found nearby, connect my own with a key
 * test), a failed key, no wire for a key, a restart in the middle, and
 * Escape everywhere. Run: setup_wizard_test <fixtures dir>. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "setup_inv.h"
#include "setup_wizard.h"
#include "setup_store.h"
#include "disk_file.h"

static int fails;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            fails++;                                                                                                   \
            printf("FAIL %s:%d ", __FILE__, __LINE__);                                                                 \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while (0)

static char fx[3][4096];
static unsigned fxn[3];

static void load(const char *dir, const char *name, int i)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("FAIL cannot open %s\n", path);
        exit(1);
    }
    fxn[i] = (unsigned)fread(fx[i], 1, sizeof fx[i], f);
    fclose(f);
}

/* every string in the view fits, is plain printable ASCII, and names none of our own parts (spec rule 3) */
static void view_ok(const sw_wizard *w, const char *where)
{
    const sw_view *v = sw_view_of(w);
    const char *texts[SW_LINES + SW_CHOICES + 5];
    unsigned n = 0;
    texts[n++] = v->say, texts[n++] = v->hint, texts[n++] = v->aside, texts[n++] = v->prompt, texts[n++] = v->typed;
    for (unsigned i = 0; i < v->nline; i++)
        texts[n++] = v->line[i];
    for (unsigned i = 0; i < v->nchoice; i++)
        texts[n++] = v->choice[i];
    static const char *const banned[] = {"Aienux", "Interplane", "Store", "contract", "kernel", "!", "blazing",
                                         "seamless", "magic", 0};
    for (unsigned i = 0; i < n; i++) {
        for (const char *p = texts[i]; *p; p++)
            CHECK(*p >= ' ' && *p < 127, "%s: unprintable byte in \"%s\"", where, texts[i]);
        for (unsigned b = 0; banned[b]; b++)
            CHECK(!strstr(texts[i], banned[b]), "%s: \"%s\" says %s", where, texts[i], banned[b]);
    }
    CHECK(strlen(v->say) < SW_SAY - 1, "%s: say may be cut: %s", where, v->say);
    for (unsigned i = 0; i < v->nline; i++)
        CHECK(strlen(v->line[i]) < SW_LINE - 1, "%s: line may be cut: %s", where, v->line[i]);
    for (unsigned i = 0; i < v->nchoice; i++)
        CHECK(strlen(v->choice[i]) < SW_CHOICE - 1, "%s: choice may be cut: %s", where, v->choice[i]);
    CHECK(!v->nchoice || (v->lit >= 0 && (unsigned)v->lit < v->nchoice), "%s: lit %d of %u", where, v->lit, v->nchoice);
}

static int has_line(const sw_wizard *w, const char *s)
{
    const sw_view *v = sw_view_of(w);
    for (unsigned i = 0; i < v->nline; i++)
        if (strstr(v->line[i], s))
            return 1;
    return 0;
}

static int has_choice(const sw_wizard *w, const char *s)
{
    const sw_view *v = sw_view_of(w);
    for (unsigned i = 0; i < v->nchoice; i++)
        if (!strcmp(v->choice[i], s))
            return 1;
    return 0;
}

/* the log so far, one line each, appended to log */
static char log_text[16384];
static void drain(sw_wizard *w)
{
    char e[SW_LINE];
    while (sw_event(w, e, sizeof e)) {
        strncat(log_text, e, sizeof log_text - strlen(log_text) - 2);
        strncat(log_text, "\n", sizeof log_text - strlen(log_text) - 1);
    }
}

static uint32_t now;
static void key(sw_wizard *w, int k, char c)
{
    sw_key(w, k, c, now);
    view_ok(w, sw_step_name(sw_step_of(w)));
    drain(w);
}
static void enter(sw_wizard *w) { key(w, SW_K_ENTER, 0); }
static void esc(sw_wizard *w) { key(w, SW_K_ESC, 0); }
static void type(sw_wizard *w, const char *s)
{
    while (*s)
        key(w, SW_K_CHAR, *s++);
}
static void wait_ms(sw_wizard *w, uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += 100) {
        now += 100;
        sw_tick(w, now);
        view_ok(w, sw_step_name(sw_step_of(w)));
        drain(w);
    }
}
static void until(sw_wizard *w, int step, uint32_t max_ms, const char *what)
{
    for (uint32_t t = 0; t < max_ms && sw_step_of(w) != step; t += 100) {
        now += 100;
        sw_tick(w, now);
        drain(w);
    }
    CHECK(sw_step_of(w) == step, "%s: at %s, wanted %s", what, sw_step_name(sw_step_of(w)), sw_step_name(step));
    view_ok(w, what);
}

static void test_inventory(void)
{
    si_inv v;
    si_init(&v);
    unsigned n = si_parse(&v, fx[0], fxn[0]);
    CHECK(n == 13, "games pc items %u", n);
    CHECK(v.host && v.ngpu == 1 && v.gpu[0].vendor == 0x10de && v.gpu[0].device == 0x2786, "games gpu");
    CHECK(v.gpu[0].vram_mib == 12282 && !strcmp(v.gpu[0].driver, "NVIDIA 560.94"), "games gpu memory/driver");
    CHECK(v.ndisplay == 1 && v.display[0].hz == 60 && v.display[0].max_hz == 165 && v.display[0].vrr, "games display");
    CHECK(v.cpu.fast == 8 && v.cpu.threads == 16 && v.ram_mib == 32768 && v.ndisk == 2 && v.nusb == 3, "games cpu etc");
    CHECK(v.npeer == 1 && v.peer[0].kind == SI_PEER_AIENOS && v.peer[0].params_b10 == 270, "games peer");
    CHECK(!strcmp(v.gateway, "192.168.1.1") && v.skipped == 0, "games gateway, skipped %u", v.skipped);
    char out[8][96];
    unsigned k = si_summary(&v, out, 8);
    CHECK(k == 7, "games summary %u lines", k);
    CHECK(!strcmp(out[0], "Screen: ASUS VG27AQ, 2560 by 1440, 60 times a second (it can do 165)"), "%s", out[0]);
    CHECK(!strcmp(out[1], "Graphics: NVIDIA GeForce RTX 4070, 12 GB of its own memory"), "%s", out[1]);
    CHECK(!strcmp(out[2], "Processor: AMD Ryzen 7 7800X3D, 8 cores"), "%s", out[2]);
    CHECK(!strcmp(out[3], "Memory: 32 GB"), "%s", out[3]);
    CHECK(!strcmp(out[4], "Storage: 931 GB fast drive; 1.9 TB hard disk"), "%s", out[4]);
    CHECK(!strcmp(out[5], "Nearby: 1 other AienOS computer (spark), and your router"), "%s", out[5]);
    CHECK(!strcmp(out[6], "Also: 3 USB devices"), "%s", out[6]);

    si_init(&v);
    n = si_parse(&v, fx[1], fxn[1]);
    CHECK(n == 12 && v.ngpu == 2 && v.nmodel == 2, "ai pc items %u gpus %u models %u", n, v.ngpu, v.nmodel);
    CHECK(si_best_gpu(&v) == 0 && v.gpu[1].kind == SI_GPU_INTEGRATED, "ai pc best gpu");
    CHECK(v.model[0].src == SI_SRC_OLLAMA && !v.model[0].running && v.model[0].mib == 4700, "ollama model");
    CHECK(v.model[1].src == SI_SRC_LMSTUDIO && v.model[1].running && !strcmp(v.model[1].where, "http://127.0.0.1:1234"),
          "lmstudio server");
    k = si_summary(&v, out, 8);
    CHECK(!strcmp(out[1], "Graphics: NVIDIA GeForce RTX 4090, 24 GB of its own memory, and 1 more"), "%s", out[1]);
    CHECK(!strcmp(out[2], "Processor: Intel Core i9-13900K, 24 cores (8 fast, 16 efficient)"), "%s",
          out[2]);

    si_init(&v);
    n = si_parse(&v, fx[2], fxn[2]);
    CHECK(n == 10 && v.gpu[0].kind == SI_GPU_UNIFIED && v.gpu[0].vram_mib == 124607, "spark items %u", n);
    CHECK(v.nmodel == 1 && v.model[0].running && v.model[0].mib == 29460, "spark server");
    k = si_summary(&v, out, 8);
    CHECK(!strcmp(out[1], "Graphics: NVIDIA GB10, 122 GB of shared memory"), "%s", out[1]);

    /* what a newer launcher or a broken one may hand in */
    static const char odd[] = "# comment\n\n  gpu vendor=1002 name=\"AMD Radeon\" future=1\nhologram depth=3\n"
                              "display name=\"never closed\nram mib=99999999999999\nusb noequals name=\"Pad\"\n"
                              "model params_b10=10\nmachine name=\"x\" from=host\r\n";
    si_init(&v);
    n = si_parse(&v, odd, sizeof odd - 1);
    CHECK(n == 4, "odd items taken %u", n);
    CHECK(v.ngpu == 1 && v.gpu[0].vendor == 0x1002 && v.ndisplay == 0 && v.nmodel == 0, "odd kept");
    CHECK(v.ram_mib == 0xffffffffu && v.nusb == 1 && !strcmp(v.usb[0], "Pad"), "odd values");
    CHECK(v.skipped == 5, "odd skipped %u", v.skipped);

    /* what the kernel itself finds, with no host */
    si_init(&v);
    si_add_gpu(&v, 0x1234, 0x1111, 0x030000);
    si_add_display(&v, 1280, 800);
    si_add_cpus(&v, 2, "QEMU Virtual CPU");
    si_add_ram(&v, 1024ull << 20);
    si_add_disk(&v, "QEMU HARDDISK", 64ull << 20, SI_DISK_VIRTUAL);
    v.npci = 7;
    CHECK(si_best_gpu(&v) == -1, "a virtual display runs no model");
    k = si_summary(&v, out, 8);
    CHECK(!strcmp(out[0], "Screen: this screen, 1280 by 800"), "%s", out[0]);
    CHECK(!strcmp(out[1], "Graphics: only a simple virtual display"), "%s", out[1]);
    CHECK(!strcmp(out[2], "Processor: 2 cores") && !strcmp(out[3], "Memory: 1 GB"), "%s / %s", out[2], out[3]);
    CHECK(!strcmp(out[k - 1], "Also: 7 parts on the main board"), "%s", out[k - 1]);
}

static si_inv games, ai, spark, empty;

static void parse_all(void)
{
    si_init(&games);
    si_parse(&games, fx[0], fxn[0]);
    si_init(&ai);
    si_parse(&ai, fx[1], fxn[1]);
    si_init(&spark);
    si_parse(&spark, fx[2], fxn[2]);
    si_init(&empty);
}

/* games PC: games, Set it up, the Spark's model nearby used after 10 s */
static void test_games(void)
{
    static sw_wizard w;
    char rec[512];
    now = 1000;
    log_text[0] = 0;
    sw_start(&w, &games, 1, 0, 0, now);
    drain(&w);
    CHECK(!strcmp(w.v.say, "Hello. I'm Allen. I'm going to look after this computer with you."), "%s", w.v.say);
    until(&w, SW_INTENT, 6100, "hello moves on");
    CHECK(w.v.nchoice == 3 && w.v.lit == 0 && !strcmp(w.v.choice[0], "Play games smoothly"), "intent choices");
    key(&w, SW_K_RIGHT, 0);
    CHECK(w.v.lit == 1 && strstr(w.v.hint, "run an AI"), "right lights the next: %s", w.v.hint);
    key(&w, SW_K_LEFT, 0);
    enter(&w);
    CHECK(sw_step_of(&w) == SW_SCAN && !strcmp(w.v.say, "Getting to know this computer."), "scan %s", w.v.say);
    CHECK(w.v.nline == 7, "scan lines %u", w.v.nline);
    until(&w, SW_CONSENT, 10000, "scan moves on");
    CHECK(w.v.nline == 5 && !strcmp(w.v.line[1], "Set your screen to 165 times a second"), "consent: %s", w.v.line[1]);
    CHECK(has_line(&w, "fast processor cores") && has_line(&w, "Build drivers") && !has_line(&w, "Run the AI"),
          "games consent list");
    enter(&w);
    CHECK(sw_step_of(&w) == SW_GAMES, "games path, at %s", sw_step_name(sw_step_of(&w)));
    CHECK(!strcmp(w.v.line[0], "Screen: 60 times a second now, it can do 165. My graphics driver will set it."), "%s",
          w.v.line[0]);
    CHECK(!strcmp(w.v.line[1], "Graphics: NVIDIA GeForce RTX 4070, driven by NVIDIA 560.94"), "%s", w.v.line[1]);
    CHECK(has_line(&w, "I'll measure them the first time a game runs"), "frames are not invented");
    for (unsigned i = 0; i < w.v.nline; i++)
        if (strstr(w.v.line[i], "Frames"))
            CHECK(!strpbrk(w.v.line[i], "0123456789"), "a frame rate with no measurement: %s", w.v.line[i]);
    enter(&w);
    CHECK(sw_step_of(&w) == SW_LLM, "llm");
    CHECK(!strcmp(w.v.say, "I build drivers with an AI. I found The 27B model nearby. I'll use it."), "%s", w.v.say);
    CHECK(has_choice(&w, "Use it") && has_choice(&w, "Choose another") && has_line(&w, "On spark, nearby"), "one found");
    wait_ms(&w, 9800);
    CHECK(sw_step_of(&w) == SW_LLM, "not before 10 s");
    until(&w, SW_READY, 400, "auto-picked");
    CHECK(has_line(&w, "Games will use your screen at 165 times a second") &&
              has_line(&w, "Your AI is qwen3.8-27b-fp8, on a computer nearby.") && has_line(&w, "build drivers"),
          "ready lines");
    unsigned n = sw_llm_conf(&w, rec, sizeof rec);
    CHECK(n && !strcmp(rec, "kind openai\nbase_url http://127.0.0.1:18091\nmodel qwen3.8-27b-fp8\nkey_env none\n"),
          "llm.conf: %s", rec);
    sw_consent(&w, rec, sizeof rec);
    CHECK(!strcmp(rec, "aienos-consent v1\ndrivers-own yes\nscreen-refresh yes\ncpu-games yes\ndrivers-build yes\n"),
          "consent: %s", rec);
    CHECK(sw_record(&w, rec, sizeof rec) && strstr(rec, "\nkind openai\nbase_url http://127.0.0.1:18091\n") &&
              strstr(rec, "\naienos-consent v1\ndrivers-own yes\n"),
          "the disk record carries llm.conf and consent: %s", rec);
    enter(&w);
    CHECK(sw_step_of(&w) == SW_DONE && w.v.nchoice == 0 && w.v.nline == 0, "done clears the panel");
    n = sw_save(&w, rec, sizeof rec);
    CHECK(n && sw_saved_done(rec, n), "a finished setup is saved as done: %s", rec);
    CHECK(strstr(log_text, "intent games\n") && strstr(log_text, "consent all\n") &&
              strstr(log_text, "llm auto-picked after 10 s\n") && strstr(log_text, "llm picked peer qwen3.8-27b-fp8\n") &&
              strstr(log_text, "step done\n"),
          "games log:\n%s", log_text);
}

/* AI PC: the AI by its number, Show me each one, two models here (the running one first) */
static void test_ai(void)
{
    static sw_wizard w;
    char rec[512];
    now = 1000;
    log_text[0] = 0;
    sw_start(&w, &ai, 1, 0, 0, now);
    enter(&w); /* hello: Enter moves on */
    CHECK(sw_step_of(&w) == SW_INTENT, "enter skips hello");
    key(&w, SW_K_CHAR, '2');
    CHECK(sw_step_of(&w) == SW_SCAN, "2 picks the AI");
    enter(&w);
    CHECK(sw_step_of(&w) == SW_CONSENT && has_line(&w, "Run the AI on your NVIDIA GeForce RTX 4090"), "ai consent");
    CHECK(!has_line(&w, "screen to"), "no screen change for an AI");
    key(&w, SW_K_DOWN, 0);
    enter(&w);
    CHECK(sw_step_of(&w) == SW_EACH && has_line(&w, "Use my own drivers"), "each 1");
    enter(&w);                    /* yes: drivers */
    key(&w, SW_K_CHAR, '2');      /* skip: ai on the gpu */
    CHECK(has_line(&w, "Build drivers"), "each 3");
    enter(&w);                    /* yes: build */
    CHECK(sw_step_of(&w) == SW_AI, "ai path, at %s", sw_step_name(sw_step_of(&w)));
    CHECK(!strcmp(w.v.line[0], "NVIDIA GeForce RTX 4090 has 24 GB of its own memory."), "%s", w.v.line[0]);
    CHECK(!strcmp(w.v.line[1], "The 27B model needs 29 GB for its weights: it fits if I share main memory, slower."),
          "%s", w.v.line[1]);
    CHECK(!strcmp(w.v.line[3], "A small 4B model needs 8 GB for its weights: it fits on the graphics card."), "%s",
          w.v.line[3]);
    CHECK(has_line(&w, "not measured yet"), "working memory not invented");
    enter(&w);
    CHECK(sw_step_of(&w) == SW_LLM && !strcmp(w.v.say, "I found 2 AI models. The first one fits best."), "%s",
          w.v.say);
    CHECK(!strcmp(w.v.choice[0], "qwen2.5-7b-instruct") && !strcmp(w.v.choice[1], "llama3.1:8b") &&
              has_choice(&w, "Connect my own"),
          "running first, then installed, then connect");
    CHECK(!strcmp(w.v.hint, "Already running on this computer"), "%s", w.v.hint);
    key(&w, SW_K_TAB, 0);
    CHECK(!strcmp(w.v.hint, "Installed with Ollama on this computer"), "%s", w.v.hint);
    wait_ms(&w, 12000);
    CHECK(sw_step_of(&w) == SW_LLM, "two found: nothing picked by itself");
    key(&w, SW_K_CHAR, '1');
    CHECK(sw_step_of(&w) == SW_READY && has_line(&w, "Your AI is qwen2.5-7b-instruct, on this computer."), "picked");
    sw_llm_conf(&w, rec, sizeof rec);
    CHECK(!strcmp(rec, "kind openai\nbase_url http://127.0.0.1:1234\nmodel qwen2.5-7b-instruct\nkey_env none\n"), "%s",
          rec);
    sw_consent(&w, rec, sizeof rec);
    CHECK(!strcmp(rec, "aienos-consent v1\ndrivers-own yes\nai-gpu no\ndrivers-build yes\n"), "consent: %s", rec);
    CHECK(strstr(log_text, "consent yes drivers-own\nconsent skip ai-gpu\nconsent yes drivers-build\n") != 0,
          "each log:\n%s", log_text);
    until(&w, SW_DONE, 15100, "ready moves on by itself");

    /* an installed model nothing serves yet: chosen, but nothing to call */
    sw_start(&w, &ai, 1, 0, 0, now);
    w.want = SW_WANT_ASKED | SW_WANT_AI;
    sw_key(&w, SW_K_ESC, 0, now);
    until(&w, SW_INTENT, 100, "escape on hello");
}

/* the Spark: their own words, Not now, choose another, connect another service, the key test */
static void test_spark(void)
{
    static sw_wizard w;
    char rec[512];
    const char *kind = "", *url = "", *model = "", *k = "";
    now = 1000;
    log_text[0] = 0;
    sw_start(&w, &spark, 1, 0, 0, now);
    until(&w, SW_INTENT, 6100, "hello");
    type(&w, "Run my own AI and play games");
    CHECK(sw_step_of(&w) == SW_OTHER && !strcmp(w.v.typed, "Run my own AI and play games"), "typing on the question");
    enter(&w);
    CHECK(sw_step_of(&w) == SW_OTHER_CHECK && !strcmp(w.v.say, "You want smoother games and an AI here. I'll set up both."),
          "%s", w.v.say);
    key(&w, SW_K_RIGHT, 0);
    enter(&w); /* not quite */
    CHECK(sw_step_of(&w) == SW_OTHER && w.v.ntyped == 0, "not quite: the bar again");
    type(&w, "write letters quietly");
    enter(&w);
    CHECK(!strcmp(w.v.say, "You'd like this: write letters quietly. I'll keep everything else simple."), "%s", w.v.say);
    key(&w, SW_K_ESC, 0);
    CHECK(sw_step_of(&w) == SW_INTENT, "escape goes back to the question");
    type(&w, "both gaming and an ai model");
    enter(&w);
    enter(&w); /* that's right */
    CHECK(strstr(log_text, "intent games+ai\n") != 0, "both intents");
    until(&w, SW_CONSENT, 10000, "scan");
    key(&w, SW_K_CHAR, '3'); /* not now */
    CHECK(sw_step_of(&w) == SW_GAMES, "not now still shows the path, at %s", sw_step_name(sw_step_of(&w)));
    CHECK(!strcmp(w.v.line[0], "Screen: 60 times a second, the most it can do."), "%s", w.v.line[0]);
    CHECK(has_line(&w, "the 10 fast cores stay free for the game, the rest on the 10 efficient ones."), "cores");
    enter(&w);
    CHECK(sw_step_of(&w) == SW_AI && !strcmp(w.v.line[0], "NVIDIA GB10 shares 122 GB of memory with the processor."),
          "%s", w.v.line[0]);
    CHECK(!strcmp(w.v.line[1], "The 27B model needs 29 GB for its weights: it fits."), "%s", w.v.line[1]);
    enter(&w);
    CHECK(!strcmp(w.v.say, "I found The 27B model on this computer. I'll use it."), "%s", w.v.say);
    key(&w, SW_K_CHAR, '2'); /* choose another */
    CHECK(sw_step_of(&w) == SW_LLM && !strcmp(w.v.say, "Here's what I found.") && has_choice(&w, "Connect my own"),
          "the full list: %s", w.v.say);
    wait_ms(&w, 11000);
    CHECK(sw_step_of(&w) == SW_LLM, "the list is never auto-picked");
    key(&w, SW_K_CHAR, '2');
    CHECK(sw_step_of(&w) == SW_SERVICE, "connect");
    key(&w, SW_K_CHAR, '3');
    CHECK(sw_step_of(&w) == SW_URL, "another service asks its address");
    type(&w, "localhost:18091");
    enter(&w);
    CHECK(sw_step_of(&w) == SW_URL && has_line(&w, "http:// or https://"), "a bad address is said plainly");
    for (int i = 0; i < 15; i++)
        key(&w, SW_K_BACK, 0);
    type(&w, "http://127.0.0.1:18091");
    enter(&w);
    CHECK(sw_step_of(&w) == SW_KEY && strstr(w.v.say, "or press Enter if it needs none"), "key: %s", w.v.say);
    type(&w, "sk-test-123");
    CHECK(!strcmp(w.v.typed, "***********") && !strstr(w.v.say, "sk-test"), "the key shows as dots");
    enter(&w);
    CHECK(sw_step_of(&w) == SW_TESTING, "testing");
    CHECK(sw_test_wanted(&w, &kind, &url, &model, &k) == 1, "a test is wanted");
    CHECK(!strcmp(kind, "openai") && !strcmp(url, "http://127.0.0.1:18091") && !strcmp(model, "auto") &&
              !strcmp(k, "sk-test-123"),
          "test request");
    CHECK(sw_test_wanted(&w, &kind, &url, &model, &k) == 0, "asked once");
    sw_test_done(&w, 1, 0, now);
    drain(&w);
    CHECK(sw_step_of(&w) == SW_READY && has_line(&w, "Your AI is your own service, through your key. It works."),
          "ready after the test");
    CHECK(has_line(&w, "Nothing was changed"), "not now is said at the end");
    int clean = 1;
    for (unsigned i = 0; i < sizeof w.key; i++)
        clean &= w.key[i] == 0;
    CHECK(clean && w.nkey == 0, "the key is wiped after the test");
    sw_llm_conf(&w, rec, sizeof rec);
    CHECK(!strcmp(rec, "kind openai\nbase_url http://127.0.0.1:18091\nmodel auto\nkey_env AIENOS_LLM_KEY\n"), "%s",
          rec);
    sw_save(&w, rec, sizeof rec);
    CHECK(!strstr(rec, "sk-test") && !strstr(rec, "letters"), "no key and no wish in the saved record: %s", rec);
    CHECK(!strstr(log_text, "sk-test") && !strstr(log_text, "127.0.0.1") && !strstr(log_text, "letters"),
          "nothing typed reaches the log:\n%s", log_text);
}

/* a key that fails, Claude without a key, Escape out of the key screens */
static void test_key_fails(void)
{
    static sw_wizard w;
    const char *kind = "", *url = "", *model = "", *k = "";
    now = 1000;
    sw_start(&w, &spark, 1, 0, 0, now);
    enter(&w);
    key(&w, SW_K_CHAR, '2');
    enter(&w);
    enter(&w);
    enter(&w);
    CHECK(sw_step_of(&w) == SW_LLM, "at the pick, at %s", sw_step_name(sw_step_of(&w)));
    key(&w, SW_K_CHAR, '2');
    key(&w, SW_K_CHAR, '2');
    key(&w, SW_K_CHAR, '1'); /* Claude */
    CHECK(sw_step_of(&w) == SW_KEY && strstr(w.v.say, "Type your key for Claude."), "%s", w.v.say);
    enter(&w);
    CHECK(sw_step_of(&w) == SW_KEY && has_line(&w, "I need the key"), "Claude needs a key");
    type(&w, "bad-key");
    enter(&w);
    CHECK(sw_test_wanted(&w, &kind, &url, &model, &k) && !strcmp(kind, "anthropic") &&
              !strcmp(url, "https://api.anthropic.com"),
          "claude test");
    sw_test_done(&w, 0, "key not valid", now);
    CHECK(sw_step_of(&w) == SW_TEST_FAILED && !strcmp(w.v.say, "That didn't work. The service said: key not valid. Try again?"),
          "%s", w.v.say);
    CHECK(w.nkey == 0 && !w.key[0], "a failed key is wiped too");
    enter(&w);
    CHECK(sw_step_of(&w) == SW_KEY && w.v.ntyped == 0, "try again: an empty key");
    type(&w, "x");
    esc(&w);
    CHECK(sw_step_of(&w) == SW_SERVICE && w.nkey == 0, "escape wipes the key");
    key(&w, SW_K_CHAR, '2');
    type(&w, "y");
    enter(&w);
    CHECK(sw_step_of(&w) == SW_TESTING, "chatgpt testing");
    CHECK(sw_test_wanted(&w, &kind, &url, &model, &k) && !strcmp(url, "https://api.openai.com"), "chatgpt url");
    wait_ms(&w, 29800);
    CHECK(sw_step_of(&w) == SW_TESTING, "still waiting");
    wait_ms(&w, 400);
    CHECK(sw_step_of(&w) == SW_TEST_FAILED && strstr(w.v.say, "no answer came back"), "a silent service: %s", w.v.say);
    key(&w, SW_K_CHAR, '2');
    CHECK(sw_step_of(&w) == SW_LLM && has_choice(&w, "Connect my own"), "choose another goes to the list");
}

/* no wire for a key (native, no launcher), nothing found anywhere */
static void test_alone(void)
{
    static sw_wizard w;
    char rec[256];
    now = 1000;
    log_text[0] = 0;
    sw_start(&w, &empty, 0, 0, 0, now);
    enter(&w);
    esc(&w);
    CHECK(sw_step_of(&w) == SW_SCAN && strstr(w.v.say, "That's fine. I'll set up the basics first."), "%s", w.v.say);
    CHECK(has_line(&w, "I couldn't find much yet"), "nothing found is said");
    enter(&w);
    CHECK(sw_step_of(&w) == SW_CONSENT && w.v.nline == 3, "consent with no intent: %u", w.v.nline);
    enter(&w);
    CHECK(sw_step_of(&w) == SW_LLM && strstr(w.v.say, "I didn't find an AI model"), "%s", w.v.say);
    CHECK(!strcmp(w.v.hint, "The small 4B model, an 8 GB download"), "fetch the size that fits: %s", w.v.hint);
    key(&w, SW_K_CHAR, '2'); /* connect my own, with no wire */
    CHECK(sw_step_of(&w) == SW_LLM && strstr(w.v.say, "I can't keep a key safely here yet") &&
              !has_choice(&w, "Connect my own"),
          "no wire: %s", w.v.say);
    enter(&w);
    CHECK(sw_step_of(&w) == SW_READY && has_line(&w, "I'll get the small 4B model when this computer can download it."),
          "fetch");
    CHECK(sw_llm_conf(&w, rec, sizeof rec) == 0, "nothing to call yet");
    CHECK(strstr(log_text, "connect refused: no wire to keep a key\n") != 0, "log:\n%s", log_text);

    /* Escape on every question */
    sw_start(&w, &games, 1, 0, 0, now);
    esc(&w);
    esc(&w);
    esc(&w);
    esc(&w);
    CHECK(sw_step_of(&w) == SW_LLM, "escape through, at %s", sw_step_name(sw_step_of(&w)));
    esc(&w);
    CHECK(sw_step_of(&w) == SW_READY && has_line(&w, "No AI yet") && has_line(&w, "Nothing was changed"), "ready");
    sw_consent(&w, rec, sizeof rec);
    CHECK(!strcmp(rec, "aienos-consent v1\ndrivers-own no\ndrivers-build no\n"), "%s", rec);
    esc(&w);
    CHECK(sw_step_of(&w) == SW_DONE, "done");
    CHECK(sw_key(&w, SW_K_ENTER, 0, now) == 0, "done takes no keys");
}

/* a restart in the middle carries on where it stopped */
static void test_resume(void)
{
    static sw_wizard w, again;
    char rec[512];
    now = 1000;
    sw_start(&w, &games, 1, 0, 0, now);
    enter(&w);
    enter(&w);
    enter(&w);
    CHECK(sw_step_of(&w) == SW_CONSENT, "at consent");
    /* the disk record before an AI is picked: the step and the consent, no llm.conf yet */
    unsigned r = sw_record(&w, rec, sizeof rec);
    CHECK(r && strstr(rec, "aienos-setup v1\nstep 5\n") && strstr(rec, "aienos-consent v1\n") && !strstr(rec, "base_url"),
          "record before a pick: %s", rec);
    CHECK(sw_record(&w, rec, 40) == 0 && rec[0] == 0, "a short buffer gets no record");
    unsigned n = sw_save(&w, rec, sizeof rec);
    CHECK(n && !sw_saved_done(rec, n), "saved mid-way: %s", rec);
    log_text[0] = 0;
    sw_start(&again, &games, 1, rec, n, now);
    drain(&again);
    CHECK(!strcmp(again.v.say, "Welcome back. Let's carry on where we stopped."), "%s", again.v.say);
    CHECK(strstr(log_text, "resumed at consent\n") != 0, "log:\n%s", log_text);
    until(&again, SW_CONSENT, 4100, "back at consent");
    CHECK(again.v.nline == 5 && has_line(&again, "165 times a second"), "the same list after a restart");
    /* inside the key screens a restart goes back to the pick */
    sw_start(&w, &spark, 1, 0, 0, now);
    enter(&w);
    key(&w, SW_K_CHAR, '2');
    enter(&w);
    enter(&w);
    enter(&w);
    key(&w, SW_K_CHAR, '2');
    key(&w, SW_K_CHAR, '2');
    key(&w, SW_K_CHAR, '1');
    type(&w, "secret");
    n = sw_save(&w, rec, sizeof rec);
    CHECK(!strstr(rec, "secret"), "the key is not saved");
    sw_start(&again, &spark, 1, rec, n, now);
    until(&again, SW_LLM, 4100, "a restart in the key screen goes back to the pick");
    /* a broken record is a first start */
    sw_start(&again, &games, 1, "aienos-setup v1\nstep x\n", 22, now);
    CHECK(!strcmp(again.v.say, "Hello. I'm Allen. I'm going to look after this computer with you."), "broken record");
    CHECK(sw_save(&again, rec, 20) == 0 && rec[0] == 0, "a short buffer gets nothing, not half a record");
}

/* the records on disk: two copies, a power cut in a save, a spoiled copy */
static void test_store(const char *dir)
{
    char path[512], text[CK_SETUP_TEXT + 1];
    unsigned n;
    snprintf(path, sizeof path, "%s/setup-store.img", dir);
    static disk_file f;
    disk_dev d;
    uint64_t blocks = (uint64_t)(CK_SETUP_FIRST + CK_SETUP_UNITS) * (CK_ALLEN_MEM_UNIT / 512);
    CHECK(disk_file_open(&f, &d, path, 512, blocks, 1) == 0, "store image");
    ck_setup_store st;
    CHECK(ck_setup_load(&st, &d, text, sizeof text, &n) == 1 && n == 0, "a blank section is a first start");
    CHECK(ck_setup_save(&st, "aienos-setup v1\nstep 5\n", 23) == 0 && st.gen == 1 && st.newest == 0, "first save in A");
    CHECK(ck_setup_save(&st, "aienos-setup v1\nstep 9\n", 23) == 0 && st.newest == 1, "second save in B");
    CHECK(ck_setup_load(&st, &d, text, sizeof text, &n) == 0 && !strcmp(text, "aienos-setup v1\nstep 9\n") && st.gen == 2,
          "the newest comes back: %s", text);
    disk_file_arm_cut(&f, 0); /* power lost before the next save (into copy A) reached the disk */
    CHECK(ck_setup_save(&st, "aienos-setup v1\nstep 16\n", 24) != 0, "a cut save fails");
    disk_file_disarm(&f);
    CHECK(ck_setup_load(&st, &d, text, sizeof text, &n) == 0 && !strcmp(text, "aienos-setup v1\nstep 9\n"),
          "after the cut the last whole save is there: %s", text);
    uint8_t junk[512];
    memset(junk, 0xa5, sizeof junk);
    disk_write(&d, (uint64_t)(CK_SETUP_FIRST + 1) * 8, 1, junk); /* spoil copy B, the newest */
    CHECK(ck_setup_load(&st, &d, text, sizeof text, &n) == 0 && !strcmp(text, "aienos-setup v1\nstep 5\n"),
          "a spoiled newest copy: the older one is used: %s", text);
    CHECK(ck_setup_save(&st, "x\x01y", 3) == 0, "a save of any bytes");
    CHECK(ck_setup_load(&st, &d, text, sizeof text, &n) == 0 && !strcmp(text, "aienos-setup v1\nstep 5\n"),
          "a copy with a control byte is not taken: %s", text);
    disk_write(&d, (uint64_t)CK_SETUP_FIRST * 8, 1, junk);
    CHECK(ck_setup_load(&st, &d, text, sizeof text, &n) == 1 && n == 0, "no valid copy: a first start, not junk");
    char big[CK_SETUP_TEXT + 2];
    memset(big, 'a', sizeof big);
    CHECK(ck_setup_save(&st, big, CK_SETUP_TEXT + 1) != 0, "too long is refused");
    disk_dev small = d;
    small.block_count = blocks - 1;
    CHECK(ck_setup_load(&st, &small, text, sizeof text, &n) < 0, "a section too small is refused");
    disk_file_close(&f);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, 0, _IONBF, 0);
    const char *dir = argc > 1 ? argv[1] : "native/kernel/svc/tests/fixtures/setup";
    load(dir, "games-pc.txt", 0);
    load(dir, "ai-pc.txt", 1);
    load(dir, "spark.txt", 2);
    test_inventory();
    parse_all();
    test_games();
    test_ai();
    test_spark();
    test_key_fails();
    test_alone();
    test_resume();
    test_store(argc > 2 ? argv[2] : "/tmp");
    if (fails) {
        printf("%d checks failed\n", fails);
        return 1;
    }
    printf("all setup checks passed\n");
    return 0;
}
