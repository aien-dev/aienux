/* allen_code_test.c -- host test of code from the Spark (svc/allen_code.c)
 * and Allen's coding requests (svc/allen_intent.c): recorded replies turned
 * into wire lines by the courier itself (allen-courier --split, made by
 * scripts/ck_allen_code_test.sh) are taken whole, in any order, with a piece
 * lost and asked again, with a piece spoiled, and against hostile lines; the
 * wait for a slow or silent Spark; the rows and the scroll the screen uses.
 *
 * usage: allen_code_test <fixtures dir> <wire dir>
 *   fixtures: <name>.txt, a model reply; wire: <name>.wire, its lines. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "allen_code.h"
#include "allen_intent.h"
#include "allen_reply.h"

static int failed, checks;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(c)) {                                                                                                    \
            failed++;                                                                                                  \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while (0)

static char *slurp(const char *dir, const char *name, const char *ext)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s%s", dir, name, ext);
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("FAIL cannot open %s\n", path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    if (fread(b, 1, (size_t)n, f) != (size_t)n)
        exit(1);
    b[n] = 0;
    fclose(f);
    return b;
}

/* the wire's lines, split in place */
static unsigned lines_of(char *s, char **out, unsigned max)
{
    unsigned n = 0;
    while (*s && n < max) {
        out[n++] = s;
        char *e = strchr(s, '\n');
        if (!e)
            break;
        *e = 0;
        s = e + 1;
    }
    return n;
}

/* The code in a fixture reply as the courier keeps it: the first fenced
 * block, a newline at its end (the fixtures hold no tabs or odd bytes). */
static unsigned code_of(const char *reply, char *out, unsigned cap)
{
    const char *a = strstr(reply, "```");
    if (!a)
        return 0;
    a = strchr(a, '\n') + 1;
    const char *e = a;
    while (*e && !(e[0] == '`' && e[1] == '`' && e[2] == '`' && (e == a || e[-1] == '\n')))
        e++;
    while (e > a && e[-1] == '\n')
        e--;
    unsigned n = (unsigned)(e - a);
    if (n + 2 > cap)
        return 0;
    memcpy(out, a, n);
    out[n++] = '\n';
    out[n] = 0;
    return n;
}

/* the kernel's own CODE lines for text, for the hostile cases (the same shape as the courier's) */
static unsigned make_lines(uint32_t seq, const char *text, uint32_t bytes, const char *lang, char lines[][320],
                           unsigned max)
{
    uint32_t n = (bytes + CK_CODE_PIECE - 1) / CK_CODE_PIECE, crc = ck_code_crc32(text, bytes);
    for (uint32_t i = 0; i < n && i < max; i++) {
        int k = snprintf(lines[i], 320, "CODE %u %u/%u %u %08x %s ", seq, i + 1, n, bytes, crc, lang);
        for (uint32_t j = i * CK_CODE_PIECE; j < bytes && j < (i + 1) * CK_CODE_PIECE; j++) {
            char c = text[j];
            if (c == '\n')
                lines[i][k++] = '\\', lines[i][k++] = 'n';
            else if (c == '\\')
                lines[i][k++] = '\\', lines[i][k++] = '\\';
            else
                lines[i][k++] = c;
        }
        lines[i][k] = 0;
    }
    return n;
}

static ck_code_rx rx;

/* a recorded reply taken whole: in order, then backwards; the closing SAY read as the kernel does */
static void test_whole(const char *fix, const char *wire_dir, const char *name, unsigned min_pieces,
                       const char *lang)
{
    char *reply = slurp(fix, name, ".txt"), *wire = slurp(wire_dir, name, ".wire");
    static char want[CK_CODE_MAX + 2];
    unsigned wn = code_of(reply, want, sizeof want);
    static char *l[CK_CODE_PIECES + 4];
    unsigned n = lines_of(wire, l, CK_CODE_PIECES + 4);
    CHECK(n >= min_pieces + 1, "%s: %u lines", name, n);
    for (int pass = 0; pass < 2; pass++) {
        ck_code_begin(&rx, 1);
        for (unsigned k = 0; k + 1 < n; k++) {
            unsigned i = pass ? n - 2 - k : k;
            CHECK(ck_code_take(&rx, l[i]) == 1, "%s: line %u refused", name, i);
        }
        CHECK(ck_code_state(&rx) == CK_CODE_WHOLE, "%s pass %d: state %d", name, pass, ck_code_state(&rx));
        CHECK(rx.bytes == wn && !memcmp(rx.text, want, wn), "%s: the code differs (%u vs %u bytes)", name, rx.bytes,
              wn);
        CHECK(!strcmp(rx.lang, lang), "%s: language %s", name, rx.lang);
    }
    /* the closing SAY: Allen's sentence */
    uint32_t seq;
    unsigned part, total;
    char text[160];
    CHECK(ck_code_take(&rx, l[n - 1]) == 0, "%s: the SAY is not a CODE line", name);
    CHECK(ck_allen_say_line(l[n - 1], &seq, &part, &total, text, sizeof text) && seq == 1 && part == 1 && total == 1 &&
              text[0],
          "%s: closing line %s", name, l[n - 1]);
    printf("ok      %s: %u pieces, %u bytes, %u lines, crc %08x, \"%s\"\n", name, rx.n, rx.bytes,
           ck_code_lines(rx.text, rx.bytes), rx.crc, text);
    /* what the courier sent is what the kernel would send: the same lines from the same code */
    static char mine[CK_CODE_PIECES][320];
    unsigned m = make_lines(1, want, wn, lang[0] ? lang : "-", mine, CK_CODE_PIECES);
    int same = m == n - 1;
    for (unsigned i = 0; same && i < m; i++)
        same = !strcmp(mine[i], l[i]);
    CHECK(same, "%s: the courier's lines and the kernel's shape differ", name);
    free(reply);
    free(wire);
}

/* a piece lost on the way: the state says which, AGAIN brings the whole set again */
static void test_lost(const char *wire_dir)
{
    char *wire = slurp(wire_dir, "long", ".wire");
    static char *l[CK_CODE_PIECES + 4];
    unsigned n = lines_of(wire, l, CK_CODE_PIECES + 4), lost = (n - 1) / 2 + 1;
    ck_code_begin(&rx, 1);
    for (unsigned i = 0; i + 1 < n; i++)
        if (i + 1 != lost)
            ck_code_take(&rx, l[i]);
    CHECK(ck_code_state(&rx) == CK_CODE_PART, "lost: state %d", ck_code_state(&rx));
    CHECK(ck_code_missing(&rx) == lost, "lost: missing %u, want %u", ck_code_missing(&rx), lost);
    /* AGAIN: the kernel starts the pieces over and the courier sends them all */
    ck_code_begin(&rx, 1);
    for (unsigned i = 0; i + 1 < n; i++)
        ck_code_take(&rx, l[i]);
    CHECK(ck_code_state(&rx) == CK_CODE_WHOLE, "lost then again: state %d", ck_code_state(&rx));
    printf("ok      lost piece %u of %u: missing said, whole after AGAIN\n", lost, n - 1);
    /* a piece spoiled on the way (its length kept): the checksum catches it */
    char *spoil = l[lost - 1] + strlen(l[lost - 1]) - 1; /* a letter of the code, not an escape's */
    while (!(*spoil >= 'a' && *spoil <= 'z' && spoil[-1] != '\\'))
        spoil--;
    char was = *spoil;
    *spoil = was == 'x' ? 'y' : 'x';
    ck_code_begin(&rx, 1);
    for (unsigned i = 0; i + 1 < n; i++)
        ck_code_take(&rx, l[i]);
    CHECK(ck_code_state(&rx) == CK_CODE_BAD, "spoiled: state %d", ck_code_state(&rx));
    *spoil = was;
    printf("ok      a spoiled piece: the checksum refuses the whole\n");
    free(wire);
}

static void test_hostile(void)
{
    static char text[CK_CODE_MAX + 1], lines[CK_CODE_PIECES][320];
    for (unsigned i = 0; i < 300; i++)
        text[i] = (char)(i % 10 == 9 ? '\n' : 'a' + i % 26);
    unsigned n = make_lines(9, text, 300, "c", lines, CK_CODE_PIECES);
    struct {
        const char *line, *why;
    } bad[] = {
        {"CODE 8 1/3 300 00000000 c x", "another job"},
        {"CODE 9 0/3 300 00000000 c x", "piece 0"},
        {"CODE 9 4/3 300 00000000 c x", "piece past the count"},
        {"CODE 9 1/2 300 00000000 c x", "a count that does not fit the length"},
        {"CODE 9 1/1 0 00000000 c ", "no bytes"},
        {"CODE 9 1/257 32769 00000000 c x", "past the limit"},
        {"CODE 9 1/3 300 0000000g c x", "a checksum that is not hex"},
        {"CODE 9 1/3 300 00000000 C x", "a language in capitals"},
        {"CODE 9 1/3 300 00000000 c", "no piece"},
        {"CODE 9 3/3 300 00000000 c ab\\tcd", "an unknown escape"},
        {"CODE 9 3/3 300 00000000 c ab\x01", "a control byte"},
        {"CODE 9 3/3 300 00000000 c ab", "a last piece too short"},
        {"CODE 9 1/3 300 00000000 c xyz", "a piece too short"},
        {"CODE 9 3/3 300 00000000 averyveryverylonglanguagename x", "a language too long"},
        {"CODE 9999999999 1/3 300 00000000 c x", "a seq past 32 bits"},
        {"CODE 00000000009 1/3 300 00000000 c x", "a seq of eleven digits"},
        {"CODE  9 1/3 300 00000000 c x", "two spaces"},
    };
    ck_code_begin(&rx, 9);
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++)
        CHECK(ck_code_take(&rx, bad[i].line) == -1, "hostile line taken: %s", bad[i].why);
    CHECK(rx.refused == sizeof bad / sizeof bad[0] && rx.got == 0 && ck_code_state(&rx) == CK_CODE_NONE,
          "refused %u got %u", rx.refused, rx.got);
    CHECK(ck_code_take(&rx, "SAY 9 1/1 hello") == 0 && ck_code_take(&rx, "CODEX 9") == 0 && ck_code_take(&rx, 0) == 0,
          "lines that are not CODE are 0");
    /* a head that disagrees with the first piece's */
    ck_code_take(&rx, lines[0]);
    char other[320];
    snprintf(other, sizeof other, "%s", lines[1]);
    char *crc = strstr(other, " 300 ") + 5;
    crc[0] = crc[0] == '0' ? '1' : '0';
    CHECK(ck_code_take(&rx, other) == -1, "a disagreeing head is refused");
    for (unsigned i = 1; i < n; i++)
        ck_code_take(&rx, lines[i]);
    ck_code_take(&rx, lines[1]); /* a piece twice: no harm */
    CHECK(ck_code_state(&rx) == CK_CODE_WHOLE && rx.got == n, "whole after the hostile ones: state %d got %u",
          ck_code_state(&rx), rx.got);
    /* more lines than the limit: refused whole even with a right checksum */
    unsigned b = 0;
    for (unsigned i = 0; i <= CK_CODE_LINES; i++)
        text[b++] = 'x', text[b++] = '\n';
    n = make_lines(10, text, b, "-", lines, CK_CODE_PIECES);
    ck_code_begin(&rx, 10);
    for (unsigned i = 0; i < n; i++)
        ck_code_take(&rx, lines[i]);
    CHECK(ck_code_state(&rx) == CK_CODE_BAD, "801 lines: state %d", ck_code_state(&rx));
    CHECK(rx.lang[0] == 0, "\"-\" is no language");
    /* the most code there may be: 32768 bytes, all backslashes, every line within the kernel's 320 */
    memset(text, '\\', CK_CODE_MAX);
    for (unsigned i = 63; i < CK_CODE_MAX; i += 64)
        text[i] = '\n';
    n = make_lines(4000000000u, text, CK_CODE_MAX, "objective-c++", lines, CK_CODE_PIECES);
    ck_code_begin(&rx, 4000000000u);
    int fit = 1;
    for (unsigned i = 0; i < n; i++)
        fit &= strlen(lines[i]) < 320 && ck_code_take(&rx, lines[i]) == 1;
    CHECK(fit && n == CK_CODE_PIECES && ck_code_state(&rx) == CK_CODE_WHOLE, "the largest code: %u pieces state %d", n,
          ck_code_state(&rx));
    printf("ok      %u hostile lines refused, a disagreeing head refused, 801 lines refused, 32768 bytes taken\n",
           (unsigned)(sizeof bad / sizeof bad[0]));
}

static void test_wait(void)
{
    ck_code_wait w;
    ck_code_wait_begin(&w, 1000);
    CHECK(ck_code_wait_tick(&w, 1000 + 19999) == CK_CODE_WAIT_NOTHING, "quiet before 20 s");
    CHECK(ck_code_wait_tick(&w, 1000 + 20000) == CK_CODE_WAIT_SLOW, "slow at 20 s");
    CHECK(ck_code_wait_tick(&w, 1000 + 20001) == CK_CODE_WAIT_NOTHING, "slow said once");
    CHECK(ck_code_wait_tick(&w, 1000 + 30000) == CK_CODE_WAIT_GONE, "silent 30 s: gone (the courier is down)");
    /* a busy Spark keeps the job: WAIT every 2 s */
    ck_code_wait_begin(&w, 0);
    int gone = 0, slow = 0;
    for (uint64_t t = 0; t <= 600000; t += 100) {
        if (t % 2000 == 0)
            ck_code_wait_heard(&w, t);
        int k = ck_code_wait_tick(&w, t);
        gone += k == CK_CODE_WAIT_GONE;
        slow += k == CK_CODE_WAIT_SLOW;
    }
    CHECK(!gone && slow == 1, "ten busy minutes: gone %d slow %d", gone, slow);
    printf("ok      the wait: slow once at 20 s, gone after 30 s of silence, kept while the Spark says WAIT\n");
}

static void test_rows(void)
{
    const char *t = "short\nthis line is longer than ten\n\nabcdefghijklmnopqrstuvwxyz\nend";
    uint32_t st[32];
    uint16_t ln[32], no[32];
    uint32_t r = ck_code_rows(t, (uint32_t)strlen(t), 10, st, ln, no, 32);
    /* short | this line | is longer | than ten | (blank) | abcdefghij | klmnopqrst | uvwxyz | end */
    CHECK(r == 9, "rows %u", r);
    CHECK(no[0] == 1 && no[1] == 2 && no[2] == 0 && no[3] == 0 && no[4] == 3 && no[5] == 4 && no[6] == 0 &&
              no[8] == 5,
          "line numbers %u %u %u %u %u %u %u %u", no[0], no[1], no[2], no[3], no[4], no[5], no[6], no[8]);
    CHECK(ln[1] == 10 && !memcmp(t + st[1], "this line ", 10) && ln[4] == 0 && ln[5] == 10, "cuts");
    for (uint32_t i = 0; i < r; i++)
        CHECK(ln[i] <= 10, "row %u too long", i);
    CHECK(ck_code_rows(t, (uint32_t)strlen(t), 10, st, ln, no, 3) == 3, "rows stop at max");
    CHECK(ck_code_scroll(0, -1, 100, 30) == 0 && ck_code_scroll(0, 29, 100, 30) == 29 &&
              ck_code_scroll(60, 29, 100, 30) == 70 && ck_code_scroll(5, 1, 20, 30) == 0 &&
              ck_code_scroll(70, -100000, 100, 30) == 0,
          "scroll clamps");
    printf("ok      rows wrap at a space within reach, numbered by line; the scroll stays in the code\n");
}

static void test_intent(void)
{
    static const char *const yes[] = {"make me a calculator", "Write a snake game in Python.",
                                      "please write a python script that renames my photos",
                                      "can you build me a todo app", "code me a timer", "Allen, make a pong game in C",
                                      "I need a program that sorts numbers", "create a website for my bakery", 0};
    static const char *const no[] = {"what is my name", "write a poem about python", "make me laugh", "hello",
                                     "remember my favorite color is blue", "tell me a story about a snake",
                                     "write me a letter to my mother", "what is a calculator", "make it quieter", 0};
    char f[256];
    for (const char *const *s = yes; *s; s++) {
        ck_allen_fold(*s, f, sizeof f);
        CHECK(ck_allen_wants_code(f), "a coding request missed: %s", *s);
    }
    for (const char *const *s = no; *s; s++) {
        ck_allen_fold(*s, f, sizeof f);
        CHECK(!ck_allen_wants_code(f), "not a coding request: %s", *s);
    }
    static const char *const more[] = {"now add a square root key", "add a reset button", "make it use colors",
                                       "also show the history", "change the title to Sums", 0};
    static const char *const not_more[] = {"what time is it", "who are you", "goodbye", 0};
    for (const char *const *s = more; *s; s++) {
        ck_allen_fold(*s, f, sizeof f);
        CHECK(ck_allen_code_more(f), "a change missed: %s", *s);
    }
    for (const char *const *s = not_more; *s; s++) {
        ck_allen_fold(*s, f, sizeof f);
        CHECK(!ck_allen_code_more(f), "not a change: %s", *s);
    }
    static ck_allen_mind m;
    char job[512];
    unsigned n = ck_allen_code_job(&m, 5, "make me a calculator", 0, job, sizeof job);
    CHECK(n && !strncmp(job, "JOB 5 code ", 11) && strstr(job, " for code.\tmake me a calculator") &&
              strlen(job) == n,
          "job line: %s", job);
    n = ck_allen_code_job(&m, 6, "now add a square root key\x01", 1, job, sizeof job);
    CHECK(n && !strncmp(job, "JOB 6 code-more ", 16) && strstr(job, "\tnow add a square root key ") &&
              !strchr(job, '\x01'),
          "code-more job line: %s", job);
    CHECK(ck_allen_code_job(&m, 7, "make me a calculator", 0, job, 20) == 0, "a job that does not fit is 0");
    printf("ok      coding requests told from talk (%u kinds), changes to shown code, the job lines\n",
           (unsigned)(sizeof yes / sizeof yes[0] + sizeof no / sizeof no[0] - 2));
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <fixtures dir> <wire dir>\n", argv[0]);
        return 2;
    }
    test_whole(argv[1], argv[2], "short", 1, "python");
    test_whole(argv[1], argv[2], "long", 100, "c");
    test_lost(argv[2]);
    test_hostile();
    test_wait();
    test_rows();
    test_intent();
    /* courier down is the wait's silence (test_wait); the kernel says so before it asks when no Spark said hello */
    printf("%s (%d checks, %d failed)\n", failed ? "AIENOS_ALLEN_CODE: FAIL" : "AIENOS_ALLEN_CODE: PASS", checks, failed);
    return failed != 0;
}
