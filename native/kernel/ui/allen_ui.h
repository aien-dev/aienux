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

/* RAM for a w x h screen (two full-screen layers, the picture kept for the dusk with its smaller copies, and
 * the glyph masks). */
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
/* 1 once Allen has gathered out of the night and can speak (his first words
 * wait for it): what waits on his words, like the setup wizard, starts then. */
int ck_allen_ui_formed(uint64_t now);
/* The line being typed changed (buf holds len characters). */
void ck_allen_ui_input(const char *buf, unsigned len, uint64_t now);
/* A line was entered and Allen answered: the line rises from the bar into
 * him, he thinks, then speaks the answer. line NULL: Allen speaks first (the
 * welcome after a restart). */
void ck_allen_ui_said(const char *line, const char *reply, uint64_t now);
/* A line was entered and the answer is coming from further off: the line
 * rises into him and he keeps thinking (the water round him turns, deeper
 * the longer it takes) until ck_allen_ui_said(NULL, reply) brings the words. */
void ck_allen_ui_think(const char *line, uint64_t now);
/* The name Allen goes by, for the bar's "Speak to ..." hint ("Allen" until called). */
void ck_allen_ui_named(const char *name);
/* Draws a frame when one is due (at most 60 a second) and returns 1, else
 * 0. One caller at a time; ck_allen_ui_help may run on other cores. */
int ck_allen_ui_tick(uint64_t now);
/* Another core's share of the frame in progress: draws strips while there
 * are any; 1 if it drew one. */
int ck_allen_ui_help(void);
/* The whole screen is drawn again by the next frame, at once (after
 * something else wrote to it). */
void ck_allen_ui_refresh(void);
/* Allen sleeps (asleep = 1): the dusk. Everything on the screen (the stars, the galaxies, the water's
 * light, Allen, his words) falls in a spiral into a black hole where he was; an accretion disk glows as the
 * light arrives, then dims, and the horizon closes to pure black over about four seconds, after which nothing
 * moves and ck_allen_ui_asleep() is 1, only after all drawing cores have presented the final black frame
 * (the panel can then be switched off). A new sleep/wake request clears that acknowledgement immediately.
 * asleep = 0 brings the sky back, calmly, in under a second. */
void ck_allen_ui_sleep(int asleep, uint64_t now);
int ck_allen_ui_asleep(void);
/* Frames drawn, the slowest and the average (microseconds). */
void ck_allen_ui_stats(uint32_t *frames, uint32_t *slowest_us, uint32_t *average_us);
/* The same for the frames of the dusk alone (the whole screen moves in each of them). */
void ck_allen_ui_dusk_stats(uint32_t *frames, uint32_t *slowest_us, uint32_t *average_us);

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
/* The model answered with a scene (svc/allen_scene.h, docs/ALLEN_CANVAS.md: len bytes, at most
 * CK_SCENE_BYTES are read): Allen speaks its say line, as ck_allen_ui_said(NULL, say) would, and the things
 * it asks for come round him as he starts to speak (ui/canvas.c); what was shown before goes. A reply in
 * plain words, with no scene lines, is simply spoken. */
void ck_allen_ui_scene(const char *scene, unsigned len, uint64_t now);

/* ---- sky view and real-time sky ---- */
void ck_allen_ui_set_unix_time(int64_t unix_s, uint64_t tsc_at_read);
void ck_allen_ui_sky_mode(uint8_t mode, uint8_t region, int32_t lat_udeg, int32_t lon_udeg);

/* The setup wizard's panel (svc/setup_wizard.h): up to CK_ALLEN_UI_PANEL_LINES
 * lines under Allen's words, each easing in a moment after the one before
 * (a line already shown stays as it is when the panel is set again), and up
 * to CK_ALLEN_UI_CHOICES choices in a row where the bar sits, the lit one
 * bright, with hint under the row and aside small under that. A prompt
 * (not NULL or "") puts the bar back for typing instead of the choices, with
 * the prompt as its hint. While the panel is up Allen's words stay until he
 * says something else. NULL: the panel fades and the window is as before.
 * Strings are copied. */
#define CK_ALLEN_UI_PANEL_LINES 8u
#define CK_ALLEN_UI_CHOICES 5u
typedef struct {
    const char *line[CK_ALLEN_UI_PANEL_LINES];
    unsigned nline;
    const char *choice[CK_ALLEN_UI_CHOICES];
    unsigned nchoice;
    int lit;
    const char *hint, *aside, *prompt;
} ck_allen_ui_panel_view;
void ck_allen_ui_panel(const ck_allen_ui_panel_view *p, uint64_t now);

/* Code from the Spark (svc/allen_code.h): text (bytes long, printable ASCII
 * and newlines; at most CK_ALLEN_UI_CODE_MAX bytes are kept) shown to the left
 * of Allen in his own type set at one width per letter, numbered by line,
 * long lines going on in the next row, on a soft pool of shade (no window,
 * no edge). lang names the language ("" for none). While it is up Allen's
 * words keep to a narrower column. NULL text: the code fades away. */
#define CK_ALLEN_UI_CODE_MAX 32768u
void ck_allen_ui_code(const char *text, unsigned bytes, const char *lang, uint64_t now);
/* Scrolls the code by rows (negative: up) or by pages (rows a screenful:
 * CK_ALLEN_UI_CODE_PAGE). Returns the top row after it, or -1 with no code up.
 * The first row shown and the rows in all are given back for the record. */
#define CK_ALLEN_UI_CODE_PAGE 100000
int ck_allen_ui_code_scroll(int rows, unsigned *top, unsigned *total, unsigned *page);
/* 1 while code is shown. */
int ck_allen_ui_code_on(void);
/* Allen is working on something far off: after his words he goes on
 * thinking (the water turns) until this is 0 again. */
void ck_allen_ui_working(int on);

#endif
