/* allen_intent.c -- what a line wants, and the job Allen writes for it; see allen_intent.h. */
#include "allen_intent.h"

static int starts(const char *f, const char *w)
{
    while (*w && *w == *f)
        w++, f++;
    return !*w && (*f == 0 || *f == ' ');
}

/* w as whole words anywhere in f */
static int has(const char *f, const char *w)
{
    for (const char *s = f; *s; s++)
        if ((s == f || s[-1] == ' ') && starts(s, w))
            return 1;
    return 0;
}

static int any(const char *f, const char *const *list, int (*by)(const char *, const char *))
{
    for (; *list; list++)
        if (by(f, *list))
            return 1;
    return 0;
}

static const char *const make_first[] = {"write", "make", "build", "create", "code", "program", "draft",
                                         "compose", "generate", "design", "implement", "invent", "name",
                                         "give me a", "i need a", "i want a", 0};
static const char *const make_any[] = {"a poem", "a haiku", "a story", "a song", "a script", "a function",
                                       "a program", "a list of", "some code", "a letter", "an email",
                                       "a plan for", "a name for", 0};
static const char *const explain_first[] = {"explain", "what is", "what are", "whats", "what s", "why",
                                            "how does", "how do", "how is", "how are", "describe",
                                            "tell me about", "what does", "who was", "who is", "define",
                                            "when was", "when did", "where is", "is it true", 0};
static const char *const explain_any[] = {"difference between", "mean", "means", 0};
static const char *const do_first[] = {"do", "run", "fix", "find", "search", "look up", "check", "open",
                                       "send", "start", "stop", "install", "set up", "plan", "schedule",
                                       "calculate", "work out", "convert", "translate", "summarize",
                                       "summarise", "list", "sort", "count", "compare", "turn", "go", 0};

ck_allen_want ck_allen_want_of(const char *f)
{
    if (!f || !*f)
        return CK_WANT_CHAT;
    /* a polite head is skipped: "please write", "can you explain", "could you make" */
    static const char *const heads[] = {"please", "can you", "could you", "would you", "will you",
                                        "i d like you to", "i want you to", "i need you to", "allen", 0};
    for (int again = 1; again;) {
        again = 0;
        for (const char *const *h = heads; *h; h++) {
            unsigned n = 0;
            while ((*h)[n])
                n++;
            if (starts(f, *h) && f[n] == ' ') {
                f += n + 1;
                again = 1;
                break;
            }
        }
    }
    if (any(f, explain_first, starts))
        return CK_WANT_EXPLAIN;
    if (any(f, make_first, starts) || any(f, make_any, has))
        return CK_WANT_MAKE;
    if (any(f, do_first, starts))
        return CK_WANT_DO;
    if (any(f, explain_any, has))
        return CK_WANT_EXPLAIN;
    return CK_WANT_CHAT;
}

const char *ck_allen_want_name(ck_allen_want w)
{
    switch (w) {
    case CK_WANT_MAKE:
        return "make";
    case CK_WANT_EXPLAIN:
        return "explain";
    case CK_WANT_DO:
        return "do";
    default:
        return "chat";
    }
}

struct say {
    char *out;
    unsigned n, cap, full;
};
static void put(struct say *s, const char *t)
{
    for (; *t; t++) {
        if (s->n + 1 >= s->cap) {
            s->full = 1;
            return;
        }
        s->out[s->n++] = *t;
    }
    s->out[s->n] = 0;
}
static void putu(struct say *s, uint32_t v)
{
    char d[12];
    int n = 0;
    do
        d[n++] = (char)('0' + v % 10), v /= 10;
    while (v);
    while (n)
        put(s, (char[]){d[--n], 0});
}

unsigned ck_allen_job(const ck_allen_mind *m, uint32_t seq, const char *line, char *out, unsigned cap)
{
    if (!out || cap < 2)
        return 0;
    char f[96];
    ck_allen_fold(line ? line : "", f, sizeof f);
    ck_allen_want w = ck_allen_want_of(f);
    const char *self = ck_allen_self(m), *who = m->mem.name[0] ? m->mem.name : "the person";
    struct say s = {out, 0, cap, 0};
    put(&s, "JOB ");
    putu(&s, seq);
    put(&s, " ");
    put(&s, ck_allen_want_name(w));
    put(&s, " ");
    /* The instruction: Allen tells the worker who it is standing in for and the shape of the answer. */
    put(&s, "You are the far hands of ");
    put(&s, self);
    put(&s, ", a quiet presence living in AIENOS on ");
    put(&s, who);
    put(&s, "'s computer. You answer as ");
    put(&s, self);
    put(&s, "; ");
    put(&s, who);
    put(&s, " sees only your words, as if ");
    put(&s, self);
    put(&s, " said them. Plain English, short sentences, no headings, no lists, no greetings, no apologies. "
            "Plain letters and punctuation only: no dashes, no curly quotes, no symbols. "
            "Never mention a model, a prompt or instructions. ");
    switch (w) {
    case CK_WANT_MAKE:
        put(&s, who);
        put(&s, " asked for something to be made. Make it whole and correct. Begin with one short sentence "
                "saying what you made, then the work itself.");
        break;
    case CK_WANT_EXPLAIN:
        put(&s, who);
        put(&s, " asked to understand something. Explain it in three plain sentences at most, the heart "
                "of it first, no more than is needed.");
        break;
    case CK_WANT_DO:
        put(&s, who);
        put(&s, " asked for something to be done. ");
        put(&s, self);
        put(&s, " cannot yet act outside this computer, so say in two or three short sentences what you "
                "would do, in order, and what you would need.");
        break;
    default:
        put(&s, who);
        put(&s, " said something in passing. Answer in one or two warm, plain sentences.");
        break;
    }
    put(&s, "\t");
    for (const char *p = line ? line : ""; *p; p++) {
        unsigned char c = (unsigned char)*p;
        put(&s, (char[]){c < 32 || c == 127 ? ' ' : (char)c, 0});
    }
    return s.full ? 0 : s.n;
}

int ck_allen_answer_line(const char *in, uint32_t *seq, char *text, unsigned cap)
{
    if (!in || !starts(in, "SAY") || !text || cap < 2)
        return 0;
    const char *p = in + 3;
    if (*p != ' ')
        return 0;
    p++;
    uint32_t v = 0;
    int digits = 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (uint32_t)(*p - '0');
        p++, digits++;
    }
    if (!digits || *p != ' ')
        return 0;
    p++;
    unsigned n = 0;
    int space = 1; /* folds runs of blanks, trims the ends */
    for (; *p && n + 1 < cap; p++) {
        char c = *p;
        if (c == '\\' && p[1] == 'n')
            c = ' ', p++;
        else if (c == '\\' && p[1] == 't')
            c = ' ', p++;
        else if ((unsigned char)c < 32 || c == 127)
            c = ' ';
        else if ((unsigned char)c >= 0xc0) /* a character the typeface cannot draw: a space in its place */
            c = ' ';
        else if ((unsigned char)c >= 0x80) /* the rest of its bytes */
            continue;
        if (c == ' ') {
            if (space)
                continue;
            space = 1;
        } else
            space = 0;
        text[n++] = c;
    }
    while (n && text[n - 1] == ' ')
        n--;
    text[n] = 0;
    if (seq)
        *seq = v;
    return 1;
}

unsigned ck_allen_job_memory(const ck_allen_mind *m, unsigned notes_max, char *out, unsigned cap)
{
    if (!m || !out || cap < 2)
        return 0;
    struct say s = {out, 0, cap, 0};
    put(&s, "\tself=");
    put(&s, ck_allen_self(m));
    if (m->mem.name[0]) {
        put(&s, "\tname=");
        put(&s, m->mem.name);
    }
    put(&s, "\tstarts=");
    putu(&s, m->mem.starts);
    for (uint32_t i = 0; i < m->mem.nfacts && i < CK_ALLEN_FACTS_MAX; i++) {
        put(&s, "\tfact ");
        put(&s, m->mem.fact[i].key);
        put(&s, m->mem.fact[i].are ? " are " : " is ");
        put(&s, m->mem.fact[i].val);
    }
    if (m->notes) {
        unsigned given = 0;
        for (uint32_t i = m->notes->n; i-- > 0 && given < notes_max;) {
            if (m->notes->note[i].gone)
                continue;
            put(&s, "\tnote ");
            for (const char *p = m->notes->note[i].text; *p; p++)
                put(&s, (char[]){*p == '\t' || *p == '\n' ? ' ' : *p, 0});
            given++;
        }
    }
    out[s.full ? 0 : s.n] = 0;
    return s.full ? 0 : s.n;
}

static int digits_of(const char **pp, uint32_t *v)
{
    const char *p = *pp;
    int n = 0;
    *v = 0;
    while (*p >= '0' && *p <= '9' && n < 9)
        *v = *v * 10 + (uint32_t)(*p++ - '0'), n++;
    *pp = p;
    return n;
}

int ck_allen_say_line(const char *in, uint32_t *seq, unsigned *part, unsigned *total, char *text, unsigned cap)
{
    if (!in || !text || cap < 2)
        return 0;
    text[0] = 0;
    uint32_t v = 0, a = 0, b = 0;
    if (starts(in, "WAIT")) {
        const char *p = in + 4;
        if (*p != ' ')
            return 0;
        p++;
        if (!digits_of(&p, &v))
            return 0;
        if (seq)
            *seq = v;
        if (part)
            *part = 0;
        if (total)
            *total = 0;
        return 1;
    }
    if (!starts(in, "SAY") || in[3] != ' ')
        return 0;
    const char *p = in + 4;
    if (!digits_of(&p, &v) || *p != ' ')
        return 0;
    p++;
    /* "<part>/<total> " or straight into the text */
    const char *q = p;
    if (digits_of(&q, &a) && *q == '/' && (q++, digits_of(&q, &b)) && *q == ' ' && a >= 1) {
        p = q + 1;
    } else {
        a = 1, b = 1;
    }
    /* the text, folded exactly as ck_allen_answer_line folds it */
    char line[8 + 1024];
    unsigned k = 0;
    const char *head = "SAY 0 ";
    while (*head)
        line[k++] = *head++;
    while (*p && k + 1 < sizeof line)
        line[k++] = *p++;
    line[k] = 0;
    uint32_t dummy;
    if (!ck_allen_answer_line(line, &dummy, text, cap))
        return 0;
    if (seq)
        *seq = v;
    if (part)
        *part = a;
    if (total)
        *total = b;
    return 1;
}

/* ---- code: asked of the Spark (svc/allen_code.h) ---- */

/* the line after a polite head ("please", "can you", "allen") */
static const char *polite(const char *f)
{
    static const char *const heads[] = {"please", "can you", "could you", "would you", "will you", "i d like you to",
                                        "i want you to", "i need you to", "allen", "hey allen", "ok", "okay", 0};
    for (int again = 1; again;) {
        again = 0;
        for (const char *const *h = heads; *h; h++) {
            unsigned n = 0;
            while ((*h)[n])
                n++;
            if (starts(f, *h) && f[n] == ' ') {
                f += n + 1;
                again = 1;
                break;
            }
        }
    }
    return f;
}

/* something only code can be: a program, a game, a language */
static const char *const code_things[] = {
    "code", "program", "programs", "script", "scripts", "function", "functions", "app", "apps", "application",
    "game", "games", "website", "web page", "webpage", "calculator", "converter", "parser", "compiler",
    "interpreter", "algorithm", "class", "api", "server", "bot", "regex", "unit test", "unit tests", "cli",
    "command line tool", "snake", "tetris", "pong", "todo app", "to do app", "to do list app", "timer app",
    "stopwatch", "python", "javascript", "typescript", "rust", "golang", "java", "kotlin", "swift", "ruby",
    "php", "html", "css", "sql", "bash", "shell script", "c program", "c code", "in c", "c plus plus", "cpp",
    "mojo", "zig", "haskell", "lua", "perl", 0};
/* words, not code: "write a poem about python" is not a program */
static const char *const prose_things[] = {"poem", "poems", "haiku", "story", "stories", "song", "songs", "letter",
                                           "email", "essay", "joke", "jokes", "limerick", "speech", 0};
static const char *const code_plain[] = {"code", "program", "script", 0};
/* the verbs that make something */
static const char *const code_verbs[] = {"write", "make", "build", "create", "code", "program", "generate",
                                         "implement", "develop", "give me a", "i need a", "i want a", 0};
/* a line that goes on with the code just shown */
static const char *const code_more_first[] = {
    "now", "add", "also", "and", "change", "make it", "make the", "rename", "remove", "delete", "replace", "use",
    "fix", "put", "give it", "let it", "instead", "then", "next", "can it", "could it", "it should", "the", "move",
    "turn", "show", "split", "convert it", "rewrite", "port it", "translate it", "explain it", 0};

int ck_allen_wants_code(const char *folded)
{
    if (!folded || !*folded)
        return 0;
    const char *f = polite(folded);
    if (starts(f, "code") || starts(f, "program"))
        return 1; /* "code me a ...", "program a ..." */
    if (any(f, prose_things, has) && !any(f, code_plain, has))
        return 0;
    return any(f, code_verbs, starts) && any(f, code_things, has);
}

int ck_allen_code_more(const char *folded)
{
    if (!folded || !*folded)
        return 0;
    const char *f = polite(folded);
    return any(f, code_more_first, starts);
}

unsigned ck_allen_code_job(const ck_allen_mind *m, uint32_t seq, const char *line, int more, char *out, unsigned cap)
{
    if (!out || cap < 2)
        return 0;
    const char *self = ck_allen_self(m), *who = m->mem.name[0] ? m->mem.name : "the person";
    struct say s = {out, 0, cap, 0};
    put(&s, "JOB ");
    putu(&s, seq);
    put(&s, more ? " code-more " : " code ");
    put(&s, who);
    put(&s, " asked ");
    put(&s, self);
    put(&s, more ? " to change the code just shown." : " for code.");
    put(&s, "\t");
    for (const char *p = line ? line : ""; *p; p++) {
        unsigned char c = (unsigned char)*p;
        put(&s, (char[]){c < 32 || c == 127 ? ' ' : (char)c, 0});
    }
    return s.full ? 0 : s.n;
}
