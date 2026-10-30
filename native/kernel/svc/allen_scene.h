/* allen_scene.h -- Allen's canvas, the words side: a scene is what the model
 * writes when it answers, a few plain lines that say WHAT Allen's screen
 * shows, never where the pixels go. The grammar and the reasons are in
 * docs/ALLEN_CANVAS.md; in short:
 *
 *   mood dawn lively
 *   say Seventeen percent of 240 is 40.8.
 *   number right warm: 40.8 | 17% of 240
 *   note left: How | 240 x 0.17 = 40.8
 *
 * One line each: `mood` (the sky's tone and the water), `say` (the one
 * sentence Allen speaks under him), and up to CK_SCENE_THINGS things placed
 * round him: note, list, number, quote, person, timer, picture, video. Words
 * before the first colon are how the thing is shown (where: left, right,
 * above; how much: warm, quiet; how it comes: bloom, rise, fade; a person's
 * message: to, from); after it, the fields, split by '|'.
 *
 * The parser is the kernel's guard: it reads at most CK_SCENE_BYTES bytes and
 * CK_SCENE_LINES lines, keeps printable ASCII only, cuts every field to its
 * room, and drops any line it does not know without a sound (it is counted,
 * never shown). Everything lands in one fixed struct: no allocation, no
 * pointers into the input. Integer only, freestanding; host tested and fuzzed
 * in ui/tests/canvas_test.c. */
#ifndef AIENOS_ALLEN_SCENE_H
#define AIENOS_ALLEN_SCENE_H
#include <stddef.h>
#include <stdint.h>

#define CK_SCENE_BYTES 2048u /* read at most this much of a reply */
#define CK_SCENE_LINES 12u   /* lines that count (blank lines, fences and thinking do not) */
#define CK_SCENE_THINGS 4u   /* things on the screen at once: more than this is not calm */
#define CK_SCENE_FIELDS 6u   /* fields of one thing (a list: its title and five items) */
#define CK_SCENE_TEXT 2304u  /* every kept field, NUL-terminated, back to back */

enum { /* what a thing is */
    CK_THING_NOTE = 1,
    CK_THING_LIST,
    CK_THING_NUMBER,
    CK_THING_QUOTE,
    CK_THING_PERSON,
    CK_THING_TIMER,
    CK_THING_PICTURE,
    CK_THING_VIDEO,
};
enum { CK_PLACE_ANY, CK_PLACE_LEFT, CK_PLACE_RIGHT, CK_PLACE_ABOVE };
enum { CK_ARRIVE_BLOOM, CK_ARRIVE_RISE, CK_ARRIVE_FADE };
enum { CK_TONE_NIGHT, CK_TONE_DAWN, CK_TONE_DEEP, CK_TONE_CLEAR };
enum { CK_WATER_SLOW, CK_WATER_STILL, CK_WATER_LIVELY };
enum { CK_WAY_NONE, CK_WAY_TO, CK_WAY_FROM }; /* a person's message: going to them, or come from them */

typedef struct {
    uint8_t kind, place, arrive, way;
    uint8_t warm, quiet; /* emphasis: lit warm, or held back */
    uint8_t nf;          /* fields kept */
    uint16_t f[CK_SCENE_FIELDS]; /* offsets into ck_scene.text */
} ck_scene_thing;

typedef struct {
    uint8_t tone, water;
    uint8_t has_say, nthings;
    uint16_t say; /* offset into text */
    ck_scene_thing thing[CK_SCENE_THINGS];
    uint16_t used;
    char text[CK_SCENE_TEXT];
} ck_scene;

/* What the parser met, for tests and for the model's report card. */
typedef struct {
    unsigned lines;    /* lines that counted */
    unsigned kept;     /* of those, understood and kept */
    unsigned rejected; /* not understood (unknown word first, or a thing with nothing in it): dropped */
    unsigned tidied;   /* understood after taking off a list mark or quotes the grammar does not use */
    unsigned cut;      /* fields cut to fit their room, or bytes outside printable ASCII left out */
    unsigned over;     /* lines past CK_SCENE_LINES, things past CK_SCENE_THINGS, a second say or mood */
    unsigned unknown;  /* words before the colon that are not a place, emphasis or arrival (ignored) */
    unsigned truncated; /* 1 when the reply was longer than CK_SCENE_BYTES */
} ck_scene_stats;

/* Reads a reply (len bytes; NUL ends it early) into s. Returns 1 when there
 * is something to show (a say or a thing), else 0. A reply with no scene
 * lines at all (the model answered in plain words) is not lost: its first
 * sentences become the say, and stats->rejected counts its lines. st may be
 * NULL. */
int ck_scene_parse(const char *in, size_t len, ck_scene *s, ck_scene_stats *st);
/* Field i of thing t (an empty string when it has none). */
const char *ck_scene_field(const ck_scene *s, const ck_scene_thing *t, unsigned i);
const char *ck_scene_say(const ck_scene *s);
/* A timer's "when" as a length of time: "25m", "1h 30m", "90 seconds", "10 min" -> ms (at most 24 h).
 * 0 when it is not a length (a clock time like "6:00 pm" is shown as written). */
uint32_t ck_scene_duration_ms(const char *when);
const char *ck_scene_kind_name(unsigned kind);
/* A clean reply as the model should write it: every counted line understood, nothing tidied or over, a say. */
int ck_scene_clean(const ck_scene_stats *st, const ck_scene *s);
/* The courier's wire form (one line, newlines as backslash-n) back into lines, in place; returns the new length. */
size_t ck_scene_unwire(char *buf, size_t len);
#endif
