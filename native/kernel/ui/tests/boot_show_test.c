/* boot_show_test.c -- host test of the boot sequence's frame receipt
 * (ui/boot_show.c) on Allen's home (ui/allen_ui.c). Run by
 * scripts/ck_boot_show_test.sh.
 *
 * 1. A fake clock that moves 1 ms a step: the night draws at its 16 ms cap,
 *    no 60 Hz slot goes without a frame (0 dropped of 912).
 * 2. The same clock moving 50 ms a step (a slow machine): 304 frames, every
 *    frame followed by two empty slots, 608 dropped of 912.
 * 3. The present hook runs once per drawn frame.
 * 4. The chirp position follows the night's clock (48 frames a ms).
 * 5. Optional (argv[1] = WxH): the real clock of this host, one core, the
 *    whole night at that size; prints the receipt line (a host fixture, not a
 *    claim about any machine). */
#include "../allen_ui.h"
#include "../boot_show.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int fails, checks;
#define CHECK(c, what)                                                                                           \
    do {                                                                                                         \
        checks++;                                                                                                \
        if (!(c)) {                                                                                              \
            fails++;                                                                                             \
            printf("FAIL %s\n", what);                                                                           \
        }                                                                                                        \
    } while (0)

static unsigned presents;
static void present(void *ctx)
{
    (void)ctx;
    presents++;
}

static uint64_t real_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static void run_fake(uint32_t w, uint32_t h, uint32_t step_ms, struct ck_boot_show_stats *st, unsigned *pres)
{
    size_t mem_len = ck_allen_ui_mem_bytes(w, h);
    uint32_t *fb = calloc((size_t)w * h, 4);
    void *mem = calloc(1, mem_len);
    presents = 0;
    /* clock in ms; NULL clock: drawing time is not timed */
    if (!fb || !mem || ck_boot_show_start(fb, w, h, w, 1, mem, mem_len, 1000, 0, NULL, present, NULL)) {
        CHECK(0, "start");
        exit(1);
    }
    uint64_t now = 0;
    while (ck_boot_show_tick(now))
        now += step_ms;
    ck_boot_show_stats(st);
    *pres = presents;
    free(fb);
    free(mem);
}

int main(int argc, char **argv)
{
    struct ck_boot_show_stats st;
    unsigned pres;

    CHECK(CK_BOOT_SHOW_SLOTS == 912u, "912 slots of 60 Hz in the 15.2 s night");

    run_fake(1280, 800, 1, &st, &pres);
    printf("fake 1 ms steps: " CK_BOOT_SHOW_LINE, CK_BOOT_SHOW_ARGS(st, 1280u, 800u, "fake"));
    CHECK(st.slots == 912u, "1 ms: slots");
    CHECK(st.dropped == 0u, "1 ms: nothing dropped");
    CHECK(st.frames == 950u, "1 ms: one frame every 16 ms (0..15184)");
    CHECK(st.ms == CK_BOOT_SHOW_MS, "1 ms: clock at the end");
    CHECK(pres == st.frames, "1 ms: one present per frame");

    run_fake(1280, 800, 50, &st, &pres);
    printf("fake 50 ms steps: " CK_BOOT_SHOW_LINE, CK_BOOT_SHOW_ARGS(st, 1280u, 800u, "fake"));
    CHECK(st.frames == 304u, "50 ms: 304 frames");
    CHECK(st.dropped == 608u, "50 ms: two empty slots after every frame");
    CHECK(pres == 304u, "50 ms: one present per frame");

    CHECK(ck_boot_show_audio_frame(1000) == 48000u, "chirp position: 48000 frames at 1 s");
    CHECK(ck_boot_show_ms(12000) == CK_NIGHT_MERGE_MS, "night clock: the merger at 12 s");

    if (argc > 1) {
        unsigned w = 0, h = 0;
        if (sscanf(argv[1], "%ux%u", &w, &h) != 2) {
            CHECK(0, "size argument WxH");
        } else {
            size_t mem_len = ck_allen_ui_mem_bytes(w, h);
            uint32_t *fb = calloc((size_t)w * h, 4);
            void *mem = calloc(1, mem_len);
            CHECK(fb && mem && !ck_boot_show_start(fb, w, h, w, 1, mem, mem_len, 1000000000u, real_ns(), real_ns,
                                                     NULL, NULL),
                  "host start");
            while (ck_boot_show_tick(real_ns()))
                ;
            ck_boot_show_stats(&st);
            printf("host one core: " CK_BOOT_SHOW_LINE, CK_BOOT_SHOW_ARGS(st, w, h, "host-memory"));
            free(fb);
            free(mem);
        }
    }
    printf("BOOT_SHOW_TEST: %s (%d checks)\n", fails ? "FAIL" : "PASS", checks);
    return fails ? 1 : 0;
}
