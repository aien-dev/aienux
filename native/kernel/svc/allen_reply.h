/* allen_reply.h -- Allen's local conversation on the iMac: greetings, small
 * talk, what it is and where it lives, facts about the machine it runs on,
 * how long it has been awake, simple arithmetic said in words, and what it
 * has been taught: your name and short facts ("remember my favorite color is
 * blue", then "what is my favorite color"). Lines are matched after folding
 * case, punctuation and spacing; anything else gets an honest "not yet". No
 * model, no network: every answer comes from the iMac alone. What Allen is
 * taught lives in ck_allen_memory, which the kernel keeps on the iMac's own
 * disk (svc/allen_memory.h) so it survives a restart. Pure, host tested in
 * ui/tests/allen_ui_test.c. */
#ifndef AIENOS_ALLEN_REPLY_H
#define AIENOS_ALLEN_REPLY_H
#include <stddef.h>
#include <stdint.h>
#include "allen_notes.h"

/* What Allen knows about the machine (filled by the kernel; 0 = unknown). */
typedef struct {
    unsigned cores, sata_disks, nvme_disks;
    unsigned screen_w, screen_h;
} ck_allen_facts;

#define CK_ALLEN_NAME_MAX 24u
#define CK_ALLEN_KEY_MAX 48u
#define CK_ALLEN_VAL_MAX 64u
#define CK_ALLEN_FACTS_MAX 32u

/* One taught fact, both halves folded (lowercase letters, digits, single
 * spaces) and NUL-terminated: "my favorite color" / "is" / "blue". */
typedef struct {
    char key[CK_ALLEN_KEY_MAX];
    char val[CK_ALLEN_VAL_MAX];
    uint8_t are; /* taught with "are" instead of "is" */
} ck_allen_fact;

/* Everything Allen keeps across restarts. */
typedef struct {
    uint32_t starts; /* times Allen has woken up with this memory */
    uint32_t lines;  /* lines answered, all starts together */
    char name[CK_ALLEN_NAME_MAX]; /* the person's name once told, "" before */
    char self[CK_ALLEN_NAME_MAX]; /* the name the person gave Allen, "" until the first meeting names him */
    uint32_t nfacts;
    uint32_t nnotes; /* notes saved, deleted ones included (svc/allen_notes.h) */
    ck_allen_fact fact[CK_ALLEN_FACTS_MAX];
    /* sky mode (persisted across restarts) */
    uint8_t sky_mode;     /* 0=unset, 1=above_me, 2=somewhere_new */
    uint8_t sky_region;   /* next region index for somewhere_new mode */
    uint8_t sky_have_loc; /* 1 if sky_lat_udeg/sky_lon_udeg were set */
    uint8_t sky_pad;      /* reserved, must be zero */
    int32_t sky_lat_udeg; /* latitude, millionths of a degree */
    int32_t sky_lon_udeg; /* east longitude, millionths of a degree */
} ck_allen_memory;

/* One conversation: the memory plus what only this session needs. */
typedef struct {
    ck_allen_facts facts;
    ck_allen_memory mem;
    int lasting;    /* 1: the kernel saves mem to disk; 0: it lasts until power off */
    int changed;    /* mem changed in a way worth saving (the kernel saves, then clears) */
    unsigned turns; /* lines answered this session */
    int asked_how;  /* Allen just asked "how are you?" */
    int naming;     /* Allen just asked "What will you call me?" (a first meeting) */
    int far;        /* 1: a far-off worker is listening (svc/courier.h): it answers first, and the
                     * answers below are only the offline fallback (goodbye, the naming, the greeting) */
    ck_allen_notes *notes; /* the notes (the kernel loads them), NULL: none */
    int note_dirty; /* index + 1 of a note to write to disk (the kernel writes, then clears) */
    int lit;        /* index + 1 of the note just saved or handed back, 0: none */
    int map_changed; /* the notes or the lit one changed: redraw the map */
    int sky_changed; /* sky_mode, location, or sky_region changed: rebuild the sky view */
    char out[160];  /* the last composed answer */
} ck_allen_mind;

void ck_allen_mind_init(ck_allen_mind *m, const ck_allen_facts *facts);
/* Allen's answer to one typed line (never NULL; valid until the next call).
 * awake_s: seconds since the machine started. */
const char *ck_allen_answer(ck_allen_mind *m, const char *line, uint64_t awake_s);
/* A place given as coordinates ("35 north 90 west", "35.1, -90.0"): 1 and the latitude and
 * longitude in millionths of a degree (east positive), or 0 when the line is no place. */
int ck_allen_parse_place(const char *s, int32_t *lat_udeg, int32_t *lon_udeg);
/* What Allen says first when the window opens: a welcome back by name when
 * it remembers someone; on a first meeting (nothing remembered, no name of
 * his own yet) he asks what to call him, and the next single word typed
 * becomes his name ("call yourself Nova" or "your name is Nova" renames him
 * any time). Never NULL. */
const char *ck_allen_welcome(ck_allen_mind *m);
/* The name Allen goes by: the one he was given, else "Allen". */
const char *ck_allen_self(const ck_allen_mind *m);
/* Stateless: the answer of a fresh conversation at 0 s awake. */
const char *ck_allen_reply(const char *line);
/* The answer to "goodbye" (and "bye", with or without "allen"); the caller
 * compares the pointer to end the session. */
extern const char ck_allen_goodbye[];
/* What Allen says instead when a save to disk failed. */
extern const char ck_allen_save_failed[];
/* The honest "not yet" for a line nothing here can answer; the kernel compares
 * the pointer to know a far-off worker may be asked (svc/allen_intent.h). */
extern const char ck_allen_not_yet[];
/* What Allen says when the far-off worker he was listening to falls silent
 * mid-answer (the kernel says it after 30 s without a word). */
extern const char ck_allen_lost_thread[];
/* Allen keeps a note of an exchange he had through the far-off worker (what
 * was asked, what he said; cut to fit a note): 0 and the note marked for the
 * kernel to write (note_dirty, changed), -1 when the notebook is full or the
 * text is empty. Pure; the kernel writes the disk. */
int ck_allen_note_exchange(ck_allen_mind *m, const char *asked, const char *said);
/* Folds a line for matching: lowercase letters and digits, single spaces,
 * NUL-terminated in out (cap bytes); returns the length. */
size_t ck_allen_fold(const char *s, char *out, size_t cap);

#endif