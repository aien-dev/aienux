/* chirp.h -- the startup sound: a quick "blirp", the last moment of the chirp
 * two bodies make as they spiral into each other and merge (a gravitational
 * wave), raised into hearing and timed to the night sky of the startup
 * (ui/night.h). Silent until CK_CHIRP_RISE_MS before the merger; then it
 * comes in over CK_CHIRP_IN_MS and sweeps up in pitch and loudness the way
 * the real wave does (pitch as t^(-3/8), loudness as pitch^(2/3), t the time
 * left), with its peak at the merger (CK_NIGHT_MERGE_MS); then a short
 * ringing fade. Under a second in all. Made from that formula, no recorded
 * sound. Integer only (the kernel builds with -mgeneral-regs-only),
 * freestanding, no memory of its own beyond a sine table. Host tested in
 * ui/tests/chirp_test.c. */
#ifndef AIENOS_CHIRP_H
#define AIENOS_CHIRP_H
#include <stdint.h>
#include "night.h"

#define CK_CHIRP_RATE 48000u   /* frames a second; each frame is a left and a right sample, 16 bits */
#define CK_CHIRP_FROM_HZ 200u  /* the pitch the sweep starts at */
#define CK_CHIRP_TO_HZ 1000u   /* the pitch at the merger, held through the fade */
#define CK_CHIRP_PEAK 22937    /* 70% of full scale */
#define CK_CHIRP_RISE_MS 600u  /* the sweep up to the merger */
#define CK_CHIRP_IN_MS 40u     /* the sound comes in over this, just before the sweep */
#define CK_CHIRP_START_MS (CK_NIGHT_MERGE_MS - CK_CHIRP_RISE_MS)
#define CK_CHIRP_END_MS (CK_NIGHT_MERGE_MS + 300u) /* the ring has died away */
#define CK_CHIRP_FRAMES (CK_CHIRP_RATE / 1000u * CK_CHIRP_END_MS)

/* Writes frames [from, from + n) as left, right pairs into out (2n samples),
 * frame 0 being the night's 0 ms. Silent past CK_CHIRP_FRAMES. Any frame
 * can be asked for in any order: each is worked out from the formula. */
void ck_chirp_fill(int16_t *out, uint32_t from, uint32_t n);

/* The pitch in Hz and the loudness (0..CK_CHIRP_PEAK) at a frame. */
uint32_t ck_chirp_hz(uint32_t frame);
uint32_t ck_chirp_level(uint32_t frame);

/* The chirp at 44.1 kHz instead: frames [from, from + n) of that rate, frame
 * 0 the night's 0 ms, each sample in the top 16 bits of a 32-bit slot (a
 * 24-bit converter's layout), left and right. */
#define CK_CHIRP_RATE_441 44100u
#define CK_CHIRP_FRAMES_441 (CK_CHIRP_RATE_441 / 100u * CK_CHIRP_END_MS / 10u)
void ck_chirp_fill_441(int32_t *out, uint32_t from, uint32_t n);
#endif
