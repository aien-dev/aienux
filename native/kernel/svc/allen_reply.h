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
    uint32_t nfacts;
    uint32_t nnotes; /* notes saved, deleted ones included (svc/allen_notes.h) */
    ck_allen_fact fact[CK_ALLEN_FACTS_MAX];
} ck_allen_memory;

/* One conversation: the memory plus what only this session needs. */
typedef struct {
    ck_allen_facts facts;
    ck_allen_memory mem;
    int lasting;    /* 1: the kernel saves mem to disk; 0: it lasts until power off */
    int changed;    /* mem changed in a way worth saving (the kernel saves, then clears) */
    unsigned turns; /* lines answered this session */
    int asked_how;  /* Allen just asked "how are you?" */
    ck_allen_notes *notes; /* the notes (the kernel loads them), NULL: none */
    int note_dirty; /* index + 1 of a note to write to disk (the kernel writes, then clears) */
    int lit;        /* index + 1 of the note just saved or handed back, 0: none */
    int map_changed; /* the notes or the lit one changed: redraw the map */
    char out[160];  /* the last composed answer */
} ck_allen_mind;

void ck_allen_mind_init(ck_allen_mind *m, const ck_allen_facts *facts);
/* Allen's answer to one typed line (never NULL; valid until the next call).
 * awake_s: seconds since the machine started. */
const char *ck_allen_answer(ck_allen_mind *m, const char *line, uint64_t awake_s);
/* What Allen says first when the window opens: a welcome back by name when
 * it remembers someone, NULL for a first meeting. */
const char *ck_allen_welcome(ck_allen_mind *m);
/* Stateless: the answer of a fresh conversation at 0 s awake. */
const char *ck_allen_reply(const char *line);
/* The answer to "goodbye" (and "bye", with or without "allen"); the caller
 * compares the pointer to end the session. */
extern const char ck_allen_goodbye[];
/* What Allen says instead when a save to disk failed. */
extern const char ck_allen_save_failed[];

#endif
