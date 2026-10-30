/* boot_show.c -- the boot sequence on any screen, and its frame receipt
 * (boot_show.h). */
#include "boot_show.h"
#include "allen_ui.h"

static struct {
    int on, over;
    uint64_t hz, t0;
    uint64_t (*clock)(void);
    ck_boot_present_fn present;
    void *ctx;
    uint32_t frames, dropped, ms;
    int32_t last_slot; /* the slot of the last frame shown; -1 before the first */
    uint64_t busy, slowest;
} bs;

static uint32_t night_ms(uint64_t now) { return bs.on ? (uint32_t)((now - bs.t0) * 1000u / bs.hz) : 0; }

int ck_boot_show_start(volatile uint32_t *fb, uint32_t w, uint32_t h, uint32_t pitch, uint32_t format, void *mem,
                       size_t mem_len, uint64_t hz, uint64_t now, uint64_t (*clock)(void), ck_boot_present_fn present,
                       void *ctx)
{
    if (ck_allen_ui_start(fb, w, h, pitch, format, mem, mem_len, hz, now, clock))
        return -1;
    bs.on = 1, bs.over = 0, bs.hz = hz, bs.t0 = now, bs.clock = clock, bs.present = present, bs.ctx = ctx;
    bs.frames = bs.dropped = bs.ms = 0, bs.last_slot = -1, bs.busy = bs.slowest = 0;
    return 0;
}

uint32_t ck_boot_show_ms(uint64_t now) { return night_ms(now); }

uint32_t ck_boot_show_audio_frame(uint64_t now)
{
    return bs.on ? (uint32_t)((now - bs.t0) * CK_CHIRP_RATE / bs.hz) : 0;
}

/* The slots from the last frame shown up to (not including) slot s showed
 * nothing new. */
static void count_gap(int32_t s)
{
    if (s > bs.last_slot + 1)
        bs.dropped += (uint32_t)(s - bs.last_slot - 1);
}

int ck_boot_show_tick(uint64_t now)
{
    if (!bs.on || bs.over)
        return 0;
    uint32_t t = night_ms(now);
    if (t >= CK_BOOT_SHOW_MS) {
        count_gap((int32_t)CK_BOOT_SHOW_SLOTS);
        bs.over = 1, bs.ms = CK_BOOT_SHOW_MS;
        return 0;
    }
    bs.ms = t;
    uint64_t c0 = bs.clock ? bs.clock() : now;
    if (!ck_allen_ui_tick(now))
        return 1;
    if (bs.present)
        bs.present(bs.ctx);
    uint64_t c = (bs.clock ? bs.clock() : now) - c0;
    bs.busy += c;
    if (c > bs.slowest)
        bs.slowest = c;
    bs.frames++;
    int32_t s = (int32_t)(t * CK_BOOT_SHOW_HZ / 1000u);
    count_gap(s);
    if (s > bs.last_slot)
        bs.last_slot = s;
    return 1;
}

void ck_boot_show_stats(struct ck_boot_show_stats *s)
{
    uint32_t slots = bs.over ? CK_BOOT_SHOW_SLOTS : bs.ms * CK_BOOT_SHOW_HZ / 1000u;
    s->frames = bs.frames;
    s->slots = slots;
    s->dropped = bs.over ? bs.dropped : (slots > (uint32_t)(bs.last_slot + 1) ? bs.dropped + slots - (uint32_t)(bs.last_slot + 1) : bs.dropped);
    s->average_us = bs.frames && bs.hz ? (uint32_t)(bs.busy * 1000000u / bs.hz / bs.frames) : 0;
    s->slowest_us = bs.hz ? (uint32_t)(bs.slowest * 1000000u / bs.hz) : 0;
    s->ms = bs.ms;
}
