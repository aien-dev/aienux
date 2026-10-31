/* allen_ui.h -- Allen's home on the display.
 *
 * The screen opens on a dark night sky (ui/night.c): the stars come out, two
 * black holes circle and merge, and from the light of the merger Allen
 * gathers (ui/mind.c): a mind drawn as light, in the middle of the screen.
 * The person is his guest. A quiet bar at the bottom takes what they type;
 * Allen's answers appear under him, word by word, while the galaxies in the
 * sky shimmer with his voice. Nothing else is on the screen unless it is in
 * use: the bar fades away when the conversation rests (a key brings it
 * back), Allen's words fade once they have been read, and the map of his
 * notes (a small constellation) shows only when a note is saved or handed
 * back.
 *
 * Drawing: every pixel is worked out from its position and the time, so a
 * frame redraws only the parts of the screen that change, in strips that
 * several processor cores can draw at once (ck_allen_ui_help). Integer
 * math, no MMIO of its own (the caller hands in the framebuffer), host
 * tested in ui/tests/allen_ui_test.c. Text uses Allen's own typeface
 * (allen_font.h). */
#ifndef AIENOS_ALLEN_UI_H
#define AIENOS_ALLEN_UI_H
#include <stddef.h>
#include <stdint.h>

/* RAM for a w x h screen (two full-screen layers and the glyph masks). */
size_t ck_allen_ui_mem_bytes(uint32_t w, uint32_t h);
/* Takes over the screen and starts the night: fb = the framebuffer (pitch in
 * pixels; format 0 RGBX, 1 BGRX), mem = ck_allen_ui_mem_bytes(w, h) bytes,
 * hz = ticks per second of the clock `now` counts in, clock = that clock
 * (read to time each frame; NULL: frames are not timed). 0, or -1 (bad
 * size or format, short mem). */
int ck_allen_ui_start(volatile uint32_t *fb, uint32_t w, uint32_t h, uint32_t pitch, uint32_t format, void *mem,
                      size_t mem_len, uint64_t hz, uint64_t now, uint64_t (*clock)(void));
int ck_allen_ui_started(void);
/* Allen is ready to talk: the bar comes up once he has formed. footer: one
 * short line shown under the empty bar. */
void ck_allen_ui_wake(const char *footer, uint64_t now);
int ck_allen_ui_on(void);
/* The line being typed changed (buf holds len characters). */
void ck_allen_ui_input(const char *buf, unsigned len, uint64_t now);
/* A line was entered and Allen answered: the line rises from the bar into
 * him, he thinks, then speaks the answer. line NULL: Allen speaks first (the
 * welcome after a restart). */
void ck_allen_ui_said(const char *line, const char *reply, uint64_t now);
/* Draws a frame when one is due (at most 60 a second) and returns 1, else
 * 0. One caller at a time; ck_allen_ui_help may run on other cores. */
int ck_allen_ui_tick(uint64_t now);
/* Another core's share of the frame in progress: draws strips while there
 * are any; 1 if it drew one. */
int ck_allen_ui_help(void);
/* The whole screen is drawn again by the next frame, at once (after
 * something else wrote to it). */
void ck_allen_ui_refresh(void);
/* Allen sleeps (asleep = 1): the screen goes dark but for a faint mark where he
 * was, nothing moves; asleep = 0 brings the sky back, at once. */
void ck_allen_ui_sleep(int asleep, uint64_t now);
int ck_allen_ui_asleep(void);
/* Frames drawn, the slowest and the average (microseconds). */
void ck_allen_ui_stats(uint32_t *frames, uint32_t *slowest_us, uint32_t *average_us);

/* The notes map, a small constellation to the right of Allen: n notes as
 * stars, 0 the newest at the centre, the older ones spiralling out (at most
 * CK_ALLEN_UI_MAP_NODES); ne links, star ea[i] to star eb[i] (notes that
 * share a key word; at most CK_ALLEN_UI_MAP_LINKS); lit the star of the note
 * just saved or handed back (-1: none), with label its text; total and links
 * count every live note and link. Shown for a while when lit >= 0: a spark
 * flies from Allen to the star. */
#define CK_ALLEN_UI_MAP_NODES 48u
#define CK_ALLEN_UI_MAP_LINKS 160u
void ck_allen_ui_map(unsigned n, const uint8_t *ea, const uint8_t *eb, unsigned ne, int lit, const char *label,
                     unsigned total, unsigned links);

#endif
