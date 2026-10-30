/* Host-only production renderer replay. Reuses the delayed-strip test from
 * imac/shutdown-timeline; no display, power, network or model access. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../allen_ui.c"

int main(void)
{
    enum { W = 1280, H = 800, P = 1296 };
    size_t bytes = ck_allen_ui_mem_bytes(W, H);
    uint32_t *fb = calloc((size_t)P * H, 4);
    uint32_t *copy = calloc((size_t)P * H, 4);
    void *mem = malloc(bytes);
    assert(mem && fb && copy);
    assert(!ck_allen_ui_start(fb, W, H, P, 1, mem, bytes, 1000000, 0, 0));
    ck_allen_ui_tick(16000000);
    ck_allen_ui_sleep(1, 17000000);
    ck_allen_ui_tick(17000000);

    /* Hold a real strip incomplete: no helper may expose half an exit frame. */
    ui.t = 17600;
    advance(ui.t);
    unsigned n = strips();
    assert(n > 1);
    memcpy(copy, fb, (size_t)P * H * 4);
    ui.done = 0;
    __atomic_store_n(&ui.ticket, (uint64_t)n << 16, __ATOMIC_RELEASE);
    assert(ck_allen_ui_help());
    assert(ui.done == 1 && !ck_allen_ui_asleep());
    assert(!memcmp(copy, fb, (size_t)P * H * 4));
    while (ck_allen_ui_help())
        ;
    assert(ui.done == n);
    present_exit(n);
    assert(memcmp(copy, fb, (size_t)P * H * 4));

    /* Irregular clocks must finish on elapsed time, not on frame count. */
    const unsigned at[] = {616, 632, 1000, 1600, 1616, 2400, 2700, 2800};
    for (unsigned i = 0; i < sizeof at / sizeof *at; i++) {
        ck_allen_ui_tick((17000ull + at[i]) * 1000);
        assert(ck_allen_ui_asleep() == (at[i] >= DUSK_MS));
        if (at[i] == 1616) {
            unsigned pixels = 0;
            /* ticket carries the actual job count, not stale entries. */
            unsigned jobs = (unsigned)(__atomic_load_n(&ui.ticket, __ATOMIC_ACQUIRE) >> 16);
            pixels = 0;
            for (unsigned j = 0; j < jobs; j++)
                pixels += (unsigned)(ui.jobs[j].w * ui.jobs[j].h);
            assert(pixels < W * H / 10);
        }
    }
    for (unsigned y = 0; y < H; y++)
        for (unsigned x = 0; x < P; x++)
            assert(!fb[y * P + x]);
    unsigned black_frames = ui.frames;
    assert(!ck_allen_ui_tick(19816000) && ui.frames == black_frames && ck_allen_ui_asleep());
    fb[0] = 1;
    ck_allen_ui_refresh();
    assert(ck_allen_ui_tick(19832000) && !fb[0] && ck_allen_ui_asleep());
    ck_allen_ui_sleep(0, 19900000);
    assert(!ck_allen_ui_asleep());
    assert(ck_allen_ui_tick(19900000));

    /* The UI epoch is a valid exit start, not an inactive sentinel. */
    assert(!ck_allen_ui_start(fb, W, H, P, 1, mem, bytes, 1000000, 1000000, 0));
    ck_allen_ui_sleep(1, 1000000);
    assert(ck_allen_ui_tick(1000000));
    assert(ui.dusk_active && ui.dusk_t == 0 && ui.dim == 0 && !ck_allen_ui_asleep());
    assert(ck_allen_ui_tick(1016000));
    assert(ui.dusk_active && ui.dusk_t == 0 && ui.dim < DIM_FULL && !ck_allen_ui_asleep());
    free(mem);
    free(copy);
    free(fb);
    puts("ALLEN_EXIT_TEST: PASS (delayed strip, elapsed timeline, small late redraw, padded black, idle, wake, zero epoch)");
    return 0;
}
