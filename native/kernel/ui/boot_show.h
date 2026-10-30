/* boot_show.h -- the boot sequence on any screen: the night (ui/night.c, two
 * black holes circling and merging) drawn by Allen's home (ui/allen_ui.c),
 * with its chirp (ui/chirp.c) kept in time by the same clock. One module for
 * every image: the x86 kernel (iMac, PC, the aienos-run VM) and the aarch64
 * kernel (Spark, Pi 5, the aienos-run VM). The caller hands in the pixels
 * (a linear GOP framebuffer, or a virtio-gpu resource's backing) and,
 * where the screen needs one, a present hook that shows a drawn frame.
 *
 * It also keeps the receipt: frames drawn against the 60 Hz beat a screen
 * refreshes at. Every 1/60 s slot of the night that ends without a new frame
 * counts as one dropped frame (the screen showed the old picture again).
 * Integer only, freestanding; host tested in ui/tests/boot_show_test.c. */
#ifndef AIENOS_BOOT_SHOW_H
#define AIENOS_BOOT_SHOW_H
#include <stddef.h>
#include <stdint.h>
#include "chirp.h"
#include "night.h"

#define CK_BOOT_SHOW_HZ 60u             /* the beat frames are counted against */
#define CK_BOOT_SHOW_MS CK_NIGHT_GONE_MS /* the night is over: the last black hole has dissolved */
/* 60 Hz slots in the night: 912 */
#define CK_BOOT_SHOW_SLOTS (CK_BOOT_SHOW_MS * CK_BOOT_SHOW_HZ / 1000u)

struct ck_boot_show_stats {
    uint32_t frames;     /* frames drawn while the night played */
    uint32_t slots;      /* 60 Hz slots the night has run through so far */
    uint32_t dropped;    /* slots that ended with no new frame */
    uint32_t average_us; /* drawing time per frame (present included) */
    uint32_t slowest_us;
    uint32_t ms;         /* the night's clock at the last tick */
};

/* Called after every drawn frame with the frame's dirty area unknown: the
 * hook shows the whole picture (virtio-gpu: transfer and flush). NULL for a
 * linear framebuffer the screen scans out by itself. */
typedef void (*ck_boot_present_fn)(void *ctx);

/* Starts the night on fb (w x h, pitch in pixels, format 0 RGBX / 1 BGRX)
 * with mem = ck_allen_ui_mem_bytes(w, h) bytes; hz = ticks a second of clock,
 * now = clock() at the night's 0 ms. 0, or -1 (as ck_allen_ui_start). */
int ck_boot_show_start(volatile uint32_t *fb, uint32_t w, uint32_t h, uint32_t pitch, uint32_t format, void *mem,
                       size_t mem_len, uint64_t hz, uint64_t now, uint64_t (*clock)(void), ck_boot_present_fn present,
                       void *ctx);
/* Draws a frame when one is due. Returns 1 while the night plays, 0 once
 * CK_BOOT_SHOW_MS has passed (the counts are then final). */
int ck_boot_show_tick(uint64_t now);
/* Milliseconds on the night's clock (0 before start). */
uint32_t ck_boot_show_ms(uint64_t now);
/* The chirp frame (CK_CHIRP_RATE) the night has reached: the sound driver
 * starts its stream here so the sound lands on the merger. */
uint32_t ck_boot_show_audio_frame(uint64_t now);
void ck_boot_show_stats(struct ck_boot_show_stats *s);

/* One receipt line, the same on every machine (printed by the caller):
 *   ck_printf(CK_BOOT_SHOW_LINE, CK_BOOT_SHOW_ARGS(st, w, h, how)); */
#define CK_BOOT_SHOW_LINE                                                                                        \
    "boot-show: %ux%u via %s: %u frames in %u ms, %u of %u slots at 60 Hz dropped, %u.%u fps, draw avg %u us "   \
    "slowest %u us\n"
#define CK_BOOT_SHOW_ARGS(st, w, h, how)                                                                         \
    (w), (h), (how), (st).frames, (st).ms, (st).dropped, (st).slots,                                             \
        (st).ms ? (st).frames * 1000u / (st).ms : 0u, (st).ms ? (st).frames * 10000u / (st).ms % 10u : 0u,       \
        (st).average_us, (st).slowest_us
#endif
