/* allen_notes.h -- Allen's notes: short things the person saves in their own
 * words ("save the gate code is 4471") and gets handed back when they ask
 * ("what's the gate code"), without ever browsing for them. Each note keeps
 * its key words (folded, without small words like "the" or "is", plurals
 * cut to one form); two notes that share a key word are linked, which is the
 * map Allen's window draws. Finding a note scores every live note by the key
 * words of the question it holds and hands back the best one, the newest on
 * a tie. The rules are deliberately simple, to be taught better later.
 * Pure: no disk (the kernel keeps notes on disk through svc/allen_memory.h),
 * host tested in svc/tests/allen_memory_test.c and ui/tests/allen_ui_test.c. */
#ifndef AIENOS_ALLEN_NOTES_H
#define AIENOS_ALLEN_NOTES_H
#include <stdint.h>

#define CK_ALLEN_NOTE_MAX 128u  /* bytes of text, NUL included */
#define CK_ALLEN_NOTES_MAX 512u /* notes Allen can keep */
#define CK_ALLEN_NOTE_WORDS 12u /* key words kept per note */

typedef struct {
    uint32_t start; /* Allen's start number when it was saved */
    uint8_t gone;   /* deleted (or found damaged on disk): never handed back */
    uint8_t nword;
    uint16_t len;
    char text[CK_ALLEN_NOTE_MAX]; /* as typed, printable ASCII, NUL-terminated */
    uint32_t word[CK_ALLEN_NOTE_WORDS]; /* key words (hashes), derived from text */
} ck_allen_note;

typedef struct {
    uint32_t n; /* notes in use, deleted ones included (note i lives in slot i) */
    ck_allen_note note[CK_ALLEN_NOTES_MAX];
} ck_allen_notes;

/* Sets text (as typed, trimmed; at most CK_ALLEN_NOTE_MAX - 1 bytes) and its
 * key words. 0, or -1 when text is empty, too long or not printable ASCII. */
int ck_allen_note_set(ck_allen_note *n, const char *text, uint32_t start);
/* Recomputes the key words from n->text (after loading from disk). */
void ck_allen_note_index(ck_allen_note *n);
/* Key words of any text; returns how many (at most max). */
unsigned ck_allen_key_words(const char *text, uint32_t *word, unsigned max);
/* Key words two notes share (0: not linked). */
unsigned ck_allen_note_links(const ck_allen_note *a, const ck_allen_note *b);
/* Live notes linked to note i. */
unsigned ck_allen_note_degree(const ck_allen_notes *ns, uint32_t i);
/* The best live note for a question: its index, or -1. strict: every key
 * word of the question must be in the note; otherwise at least half of
 * them. *also: how many other live notes would have qualified. */
int ck_allen_notes_best(const ck_allen_notes *ns, const char *question, int strict, unsigned *also);
/* Live notes. */
unsigned ck_allen_notes_live(const ck_allen_notes *ns);

#endif
