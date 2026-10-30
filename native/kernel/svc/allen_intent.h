/* allen_intent.h -- How Allen reads a line he cannot answer by heart and
 * writes the job he hands to a far-off worker (a language model reached
 * through a courier, svc/courier.h). Allen decides what is wanted, something
 * made, something explained, something done, or just talk, and composes the
 * instruction himself: who he is, who asked, and the shape the answer must
 * take, so the worker speaks in his voice and the person only ever hears
 * Allen. Pure: no I/O, no model here. Host tested in ui/tests/allen_ui_test.c. */
#ifndef AIENOS_ALLEN_INTENT_H
#define AIENOS_ALLEN_INTENT_H
#include <stdint.h>
#include "allen_reply.h"

typedef enum { CK_WANT_CHAT, CK_WANT_MAKE, CK_WANT_EXPLAIN, CK_WANT_DO } ck_allen_want;

/* What a folded line (ck_allen_fold) asks for, from its first word and its verbs. */
ck_allen_want ck_allen_want_of(const char *folded);
/* "chat", "make", "explain" or "do". */
const char *ck_allen_want_name(ck_allen_want w);
/* The job Allen sends, one line: "JOB <seq> <want> <instruction>\t<line>"
 * (tabs, newlines and control bytes in line become spaces; no trailing
 * newline). Returns the length, 0 when cap is too small. */
unsigned ck_allen_job(const ck_allen_mind *m, uint32_t seq, const char *line, char *out, unsigned cap);
/* Reads an answer "SAY <seq> <text>" (the worker turns newlines into "\n"):
 * 1 with the seq and the text (at most cap - 1 bytes, "\n" back to a space),
 * 0 when the line is not an answer. */
int ck_allen_answer_line(const char *in, uint32_t *seq, char *text, unsigned cap);
/* Allen's memory for the worker, appended to a job line as tab-separated
 * fields: "self=<his name>", "name=<the person's>", "starts=<n>", one
 * "fact <key> is|are <val>" per taught fact, one "note <text>" per live note
 * among the newest `notes_max`. Returns the bytes written at out (no leading
 * tab is missing: the first byte is a tab), 0 when cap is too small. */
unsigned ck_allen_job_memory(const ck_allen_mind *m, unsigned notes_max, char *out, unsigned cap);
/* Reads one answer part "SAY <seq> <part>/<total> <text>" (parts count from
 * 1; total is 0 until the last part, which carries its own number as the
 * total) or a whole answer "SAY <seq> <text>" (part 1 of 1): 1 with seq,
 * part, total and the text folded as ck_allen_answer_line does; 0 when the
 * line is not an answer. "WAIT <seq>" (the worker is waiting on a busy
 * model) is 1 with part 0, total 0 and an empty text. */
int ck_allen_say_line(const char *in, uint32_t *seq, unsigned *part, unsigned *total, char *text, unsigned cap);

/* Code asked of the Spark (svc/allen_code.h). folded as ck_allen_want_of's.
 * 1 when the line asks for code to be written ("make me a calculator",
 * "write a snake game in python"); 0 for anything else, prose included. */
int ck_allen_wants_code(const char *folded);
/* 1 when the line reads as a change to code just shown ("now add a square
 * root key"); the caller asks only while code is fresh. */
int ck_allen_code_more(const char *folded);
/* The courier job for a code request: "JOB <seq> code|code-more <instruction>\t<line>"
 * (want "code" starts afresh, "code-more" goes on with the code before).
 * Returns its length, 0 when it does not fit in cap. */
unsigned ck_allen_code_job(const ck_allen_mind *m, uint32_t seq, const char *line, int more, char *out, unsigned cap);

#endif
