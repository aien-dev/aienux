/* allen_code.h -- Code from the Spark, the kernel side. A person asks Allen
 * for code ("make me a calculator", "write a snake game in Python"); Allen
 * says at once that he needs to code it and sends the request over the
 * courier (svc/courier.h) to the Spark, where crates/allen-courier asks the
 * language model with a coding prompt. The answer comes back as numbered
 * pieces and a closing sentence:
 *
 *   CODE <seq> <i>/<n> <bytes> <crc32> <lang> <piece>
 *   SAY <seq> 1/1 <Allen's sentence>
 *
 * Every piece but the last carries exactly CK_CODE_PIECE bytes of code (the
 * piece is escaped on the wire: "\n" a newline, "\\" a backslash), so piece i
 * lands at i * CK_CODE_PIECE whatever order the pieces come in. bytes is the
 * whole length and crc32 (IEEE, 8 hex digits) the checksum of the whole; the
 * kernel checks both before anything is shown. A missing piece or a bad
 * checksum is asked for again once ("AGAIN <seq>": the Spark sends every
 * piece and the sentence again); after that Allen says plainly that the code
 * got lost on the way.
 *
 * Limits, stated: at most CK_CODE_MAX bytes and CK_CODE_LINES lines (the
 * Spark cuts a longer answer at a line and says so), printable ASCII and
 * newlines only (the Spark turns tabs into spaces and anything else into
 * '?'). Pure and integer only: no I/O, host tested in
 * svc/tests/allen_code_test.c (scripts/ck_allen_code_test.sh). */
#ifndef AIENOS_ALLEN_CODE_H
#define AIENOS_ALLEN_CODE_H
#include <stddef.h>
#include <stdint.h>

#define CK_CODE_MAX 32768u  /* bytes of code at most */
#define CK_CODE_LINES 800u  /* lines of code at most */
#define CK_CODE_PIECE 128u  /* code bytes per piece (escaped, at most 256 on the wire) */
#define CK_CODE_PIECES (CK_CODE_MAX / CK_CODE_PIECE)
#define CK_CODE_LANG 16u

enum { /* where the pieces stand (ck_code_state) */
    CK_CODE_NONE = 0, /* no piece has come */
    CK_CODE_PART,     /* some pieces, not all */
    CK_CODE_WHOLE,    /* every piece, length and checksum right */
    CK_CODE_BAD,      /* every piece, but the length or checksum is wrong */
};

typedef struct {
    uint32_t seq;               /* the job the pieces belong to */
    uint32_t n;                 /* pieces announced (0: none yet) */
    uint32_t bytes, crc;        /* announced length and checksum */
    uint32_t got;               /* pieces taken */
    uint32_t refused;           /* lines that said CODE but did not fit (wrong job, malformed, past the limits) */
    char lang[CK_CODE_LANG];    /* the language the Spark named ("python", "c"; "" when it named none) */
    uint8_t have[CK_CODE_PIECES];
    char text[CK_CODE_MAX + 1]; /* the code, NUL-terminated once whole */
} ck_code_rx;

/* Empties rx for job seq. */
void ck_code_begin(ck_code_rx *rx, uint32_t seq);
/* Takes one courier line. 1 when it is a CODE line for rx's job and was kept
 * (a piece already held is kept again, no harm), 0 when the line is not a
 * CODE line at all, -1 when it is a CODE line that does not fit (another
 * job, a malformed head, a piece past the limits or of the wrong length, a
 * head that disagrees with the first piece's): counted in rx->refused. */
int ck_code_take(ck_code_rx *rx, const char *line);
/* CK_CODE_NONE, _PART, _WHOLE or _BAD. Checks the checksum once all pieces are in. */
int ck_code_state(ck_code_rx *rx);
/* The first piece still missing (1-based), 0 when none is. */
uint32_t ck_code_missing(const ck_code_rx *rx);
/* Lines in a whole code text (a last line without a newline counts). */
uint32_t ck_code_lines(const char *text, uint32_t bytes);
/* CRC-32 (IEEE 802.3, reflected, as zlib's crc32) of n bytes. */
uint32_t ck_code_crc32(const void *p, size_t n);

/* ---- what the screen shows: rows of at most cols characters ---- */

/* Breaks text into screen rows of at most cols characters: a line longer than
 * cols goes on in the next row (cut at the last space in reach when there is
 * one, else at cols). starts[i] is where row i begins in text, lens[i] its
 * length, line_of[i] the 1-based line it belongs to (0 for a row that goes on
 * from the one before). Returns the number of rows, at most max. */
uint32_t ck_code_rows(const char *text, uint32_t bytes, uint32_t cols, uint32_t *starts, uint16_t *lens,
                      uint16_t *line_of, uint32_t max);
/* The top row after a scroll: delta rows (negative up), kept within
 * [0, rows - page] (0 when everything fits). */
uint32_t ck_code_scroll(uint32_t top, int32_t delta, uint32_t rows, uint32_t page);

/* ---- the wait for the Spark ---- */

enum { /* what ck_code_wait_tick says should happen now */
    CK_CODE_WAIT_NOTHING = 0,
    CK_CODE_WAIT_SLOW,   /* still nothing after CK_CODE_SLOW_MS: say once that the Spark is still at it */
    CK_CODE_WAIT_GONE,   /* silence for CK_CODE_GONE_MS: the Spark stopped answering; give the job up */
};
#define CK_CODE_SLOW_MS 20000u
#define CK_CODE_GONE_MS 30000u
typedef struct {
    uint64_t asked_ms, heard_ms; /* when the job went, and the last word about it (WAIT, a piece, a part) */
    int slow_said;
} ck_code_wait;
void ck_code_wait_begin(ck_code_wait *w, uint64_t now_ms);
/* The Spark said something about the job (it is busy, or a piece came). */
void ck_code_wait_heard(ck_code_wait *w, uint64_t now_ms);
int ck_code_wait_tick(ck_code_wait *w, uint64_t now_ms);

#endif
