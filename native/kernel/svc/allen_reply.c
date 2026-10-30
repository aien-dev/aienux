/* allen_reply.c -- Allen's local conversation; see allen_reply.h. */
#include "allen_reply.h"

#include <stddef.h>

const char ck_allen_goodbye[] = "Goodbye. Until next time.";
const char ck_allen_save_failed[] = "I couldn't save that to my disk, so I'll only remember it until this computer turns off.";
const char ck_allen_not_yet[] = "I don't know that yet. Type help to see what I can talk about.";
#define not_yet ck_allen_not_yet
const char ck_allen_lost_thread[] = "I lost my thread there. Ask me again in a moment.";

int ck_allen_note_exchange(ck_allen_mind *m, const char *asked, const char *said)
{
    ck_allen_notes *ns = m ? m->notes : 0;
    if (!ns || !asked || !said || !*asked || ns->n >= CK_ALLEN_NOTES_MAX)
        return -1;
    /* "asked: <line> / said: <words>", the line cut to its first 48 printable bytes, the words to the room left */
    char text[CK_ALLEN_NOTE_MAX];
    unsigned n = 0;
    const char *heads[2] = {"asked: ", " / said: "}, *bodies[2] = {asked, said};
    for (int k = 0; k < 2; k++) {
        unsigned limit = k == 0 ? 7 + 48 : (unsigned)sizeof text - 1;
        for (const char *p = heads[k]; *p && n < limit; p++)
            text[n++] = *p;
        for (const char *p = bodies[k]; *p && n < limit; p++)
            text[n++] = (*p < 32 || *p == 127) ? ' ' : *p;
    }
    while (n && text[n - 1] == ' ')
        n--;
    text[n] = 0;
    if (ck_allen_note_set(&ns->note[ns->n], text, m->mem.starts))
        return -1;
    uint32_t i = ns->n++;
    m->mem.nnotes = ns->n;
    m->changed = 1;
    m->note_dirty = (int)i + 1;
    m->lit = (int)i + 1;
    m->map_changed = 1;
    return 0;
}

/* Lowercases letters, keeps digits, turns every other run into one space,
 * trims. Returns the folded length (at most cap - 1). */
static size_t fold(const char *s, char *out, size_t cap)
{
    size_t n = 0;
    int space = 0;
    for (; *s && n + 1 < cap; s++) {
        char c = *s;
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            if (space && n)
                out[n++] = ' ';
            if (n + 1 < cap)
                out[n++] = c;
            space = 0;
        } else {
            space = 1;
        }
    }
    out[n] = 0;
    return n;
}

static int eq(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

/* Where phrase starts in f as whole words, or NULL. */
static const char *find(const char *f, const char *phrase)
{
    for (const char *s = f; *s; s++) {
        if (s != f && s[-1] != ' ')
            continue;
        const char *a = phrase, *b = s;
        while (*a && *a == *b)
            a++, b++;
        if (!*a && (*b == 0 || *b == ' '))
            return s;
    }
    return 0;
}

/* Any of the phrases (NULL-terminated list) in f. */
static int any(const char *f, const char *const *list)
{
    for (; *list; list++)
        if (find(f, *list))
            return 1;
    return 0;
}

/* The rest of f after a leading phrase and a space, or NULL. */
static const char *after(const char *f, const char *phrase)
{
    while (*phrase && *phrase == *f)
        phrase++, f++;
    return !*phrase && *f == ' ' ? f + 1 : 0;
}

/* The answer being composed into m->out. */
struct say {
    char *b;
    size_t n, cap;
};
static void put(struct say *s, const char *t)
{
    while (*t && s->n + 1 < s->cap)
        s->b[s->n++] = *t++;
    s->b[s->n] = 0;
}
static void putu(struct say *s, uint64_t v)
{
    char d[24];
    int i = 0;
    do
        d[i++] = (char)('0' + v % 10);
    while ((v /= 10) && i < 23);
    while (i && s->n + 1 < s->cap)
        s->b[s->n++] = d[--i];
    s->b[s->n] = 0;
}
static void put_count(struct say *s, uint64_t v, const char *one, const char *many)
{
    putu(s, v);
    put(s, " ");
    put(s, v == 1 ? one : many);
}
static void put_awake(struct say *s, uint64_t t)
{
    if (t >= 3600) {
        put_count(s, t / 3600, "hour", "hours");
        if (t % 3600 / 60) {
            put(s, " and ");
            put_count(s, t % 3600 / 60, "minute", "minutes");
        }
    } else if (t >= 60) {
        put_count(s, t / 60, "minute", "minutes");
        if (t % 60) {
            put(s, " and ");
            put_count(s, t % 60, "second", "seconds");
        }
    } else {
        put_count(s, t, "second", "seconds");
    }
}
/* ", Name" when Allen knows it. */
static void put_name(struct say *s, const ck_allen_mind *m)
{
    if (m->mem.name[0]) {
        put(s, ", ");
        put(s, m->mem.name);
    }
}

/* A number at *p said as digits (up to 9) or as a word from zero to twenty,
 * followed by a space or the end; moves *p past it. -1 if there is none. */
static int64_t number(const char **p)
{
    static const char *const words[] = {"zero",     "one",     "two",     "three",     "four",     "five",    "six",
                                        "seven",    "eight",   "nine",    "ten",       "eleven",   "twelve",  "thirteen",
                                        "fourteen", "fifteen", "sixteen", "seventeen", "eighteen", "nineteen", "twenty"};
    const char *s = *p;
    int64_t v = -1;
    if (*s >= '0' && *s <= '9') {
        int digits = 0;
        for (v = 0; *s >= '0' && *s <= '9'; s++) {
            if (++digits > 9)
                return -1; /* refuse before v can overflow */
            v = v * 10 + (*s - '0');
        }
    } else {
        for (int i = 0; i < 21 && v < 0; i++) {
            const char *a = words[i], *b = s;
            while (*a && *a == *b)
                a++, b++;
            if (!*a && (*b == ' ' || !*b)) {
                v = i;
                s = b;
            }
        }
    }
    if (v < 0 || (*s && *s != ' '))
        return -1;
    *p = *s ? s + 1 : s;
    return v;
}

/* "what is 12 times 4", "12 plus 3", "nine divided by two": 1 and the
 * answer composed, or 0. */
static int arithmetic(struct say *s, const char *f)
{
    static const char *const lead[] = {"what is", "whats", "what s", "calculate", "how much is", 0};
    const char *p = f, *r;
    for (const char *const *l = lead; *l; l++)
        if ((r = after(f, *l))) {
            p = r;
            break;
        }
    int64_t a = number(&p);
    if (a < 0)
        return 0;
    static const struct {
        const char *word;
        char op;
    } ops[] = {{"plus", '+'}, {"add", '+'}, {"minus", '-'}, {"take away", '-'}, {"times", '*'},
               {"x", '*'},    {"multiplied by", '*'}, {"divided by", '/'}, {"over", '/'}};
    char op = 0;
    for (size_t i = 0; i < sizeof ops / sizeof ops[0] && !op; i++) {
        if ((r = after(p, ops[i].word))) {
            op = ops[i].op;
            p = r;
        }
    }
    if (!op)
        return 0;
    int64_t b = number(&p);
    if (b < 0 || *p)
        return 0;
    if (op == '/' && b == 0) {
        put(s, "Nobody can divide by zero, not even me.");
        return 1;
    }
    putu(s, (uint64_t)a);
    put(s, op == '+' ? " plus " : op == '-' ? " minus " : op == '*' ? " times " : " divided by ");
    putu(s, (uint64_t)b);
    put(s, " is ");
    int64_t v = op == '+' ? a + b : op == '-' ? a - b : op == '*' ? a * b : a / b;
    if (v < 0) {
        put(s, "minus ");
        v = -v;
    }
    putu(s, (uint64_t)v);
    if (op == '/' && a % b) {
        put(s, " remainder ");
        putu(s, (uint64_t)(a % b));
    }
    put(s, ".");
    return 1;
}

/* Words that follow "i am" / "i m" without being a name. */
static int is_state(const char *w)
{
    static const char *const st[] = {"good", "fine",  "great", "ok",   "okay", "well", "tired", "sad",
                                     "happy", "bored", "here",  "back", "sorry", "not", "so",   "very",
                                     "doing", "just",  "really", "excited", "shook", "alright", "awesome", 0};
    for (const char *const *p = st; *p; p++)
        if (eq(*p, w))
            return 1;
    return 0;
}

/* Takes rest, when it is one folded word, as a name into out (first letter
 * capital): 1, or 0 if none fits. */
static int one_name(const char *rest, char out[CK_ALLEN_NAME_MAX])
{
    size_t n = 0;
    char w[CK_ALLEN_NAME_MAX];
    while (rest[n] && rest[n] != ' ' && n < sizeof w - 1)
        w[n] = rest[n], n++;
    w[n] = 0;
    if (!n || rest[n] == ' ' || is_state(w))
        return 0;
    for (size_t i = 0; i <= n; i++)
        out[i] = w[i];
    if (out[0] >= 'a' && out[0] <= 'z')
        out[0] = (char)(out[0] - 'a' + 'A');
    return 1;
}

const char *ck_allen_self(const ck_allen_mind *m) { return m->mem.self[0] ? m->mem.self : "Allen"; }

/* The person's name from rest: 1, or 0 if none fits (Allen's own name never does). */
static int learn_name(ck_allen_mind *m, const char *rest)
{
    char self[CK_ALLEN_NAME_MAX];
    fold(ck_allen_self(m), self, sizeof self);
    if (eq(rest, self) || !one_name(rest, m->mem.name))
        return 0;
    m->changed = 1;
    return 1;
}

/* Allen's own name from rest: 1, or 0 if none fits. */
static int learn_self(ck_allen_mind *m, const char *rest)
{
    if (!one_name(rest, m->mem.self))
        return 0;
    m->changed = 1;
    m->naming = 0;
    return 1;
}

/* f is a greeting to Allen by his own name: "hello nova", "hi nova", "hey nova". */
static int greets_self(const char *f, const ck_allen_mind *m)
{
    char self[CK_ALLEN_NAME_MAX];
    const char *rest;
    fold(ck_allen_self(m), self, sizeof self);
    return ((rest = after(f, "hello")) || (rest = after(f, "hi")) || (rest = after(f, "hey"))) && eq(rest, self);
}

/* ---- What Allen is taught ---- */

static size_t len_of(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}
static void copy_n(char *d, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        d[i] = s[i];
    d[n] = 0;
}

/* t as Allen says it back: "my" becomes "your" and so on, word by word;
 * cap: the first letter capitalised. */
static void put_turned(struct say *s, const char *t, int cap)
{
    static const char *const from[] = {"my", "your", "i", "me", "mine", "yours", "myself", "am", 0};
    static const char *const to[] = {"your", "my", "you", "you", "yours", "mine", "yourself", "are"};
    while (*t) {
        char w[CK_ALLEN_VAL_MAX];
        size_t n = 0;
        while (t[n] && t[n] != ' ' && n < sizeof w - 1)
            w[n] = t[n], n++;
        w[n] = 0;
        t += n;
        const char *say = w;
        for (int i = 0; from[i]; i++)
            if (eq(w, from[i]))
                say = to[i];
        if (cap && say[0] >= 'a' && say[0] <= 'z') {
            char first[2] = {(char)(say[0] - 'a' + 'A'), 0};
            put(s, first);
            say++;
        }
        cap = 0;
        put(s, say);
        if (*t == ' ') {
            put(s, " ");
            t++;
        }
    }
}

static int fact_index(const ck_allen_mind *m, const char *key)
{
    for (uint32_t i = 0; i < m->mem.nfacts && i < CK_ALLEN_FACTS_MAX; i++)
        if (eq(m->mem.fact[i].key, key))
            return (int)i;
    return -1;
}

/* "Your favorite color is blue" (no full stop). */
static void put_fact(struct say *s, const ck_allen_fact *f, int cap)
{
    put_turned(s, f->key, cap);
    put(s, f->are ? " are " : " is ");
    put_turned(s, f->val, 0);
}

static const char *saved_note(const ck_allen_mind *m)
{
    return m->lasting ? " I'll remember that." : " I'll remember that until this computer turns off.";
}

/* "remember (that) KEY is|are VALUE": stores it (or learns the name for
 * "my name is"). 1 and the answer composed, or 0 if the line is not one. */
static int teach(ck_allen_mind *m, struct say *s, const char *f)
{
    const char *rest = after(f, "remember that");
    if (!rest)
        rest = after(f, "remember");
    if (!rest)
        return 0;
    const char *verb = 0;
    for (const char *p = rest; *p && !verb; p++)
        if (*p == ' ' && (after(p + 1, "is") || after(p + 1, "are")))
            verb = p;
    if (!verb) {
        put(s, "Teach me like this: remember my favorite color is blue.");
        return 1;
    }
    int are = verb[1] == 'a';
    size_t klen = (size_t)(verb - rest);
    const char *val = verb + (are ? 5 : 4);
    size_t vlen = len_of(val);
    if (klen == 7 && after(rest, "my name") == rest + 8 && !are) {
        if (learn_name(m, val)) {
            put(s, "Nice to meet you, ");
            put(s, m->mem.name);
            put(s, ".");
            put(s, saved_note(m));
        } else {
            put(s, "Tell me your name as one word, like: my name is Drake.");
        }
        return 1;
    }
    if (klen >= CK_ALLEN_KEY_MAX || vlen >= CK_ALLEN_VAL_MAX) {
        put(s, "That's longer than I can hold yet. Try something shorter.");
        return 1;
    }
    char key[CK_ALLEN_KEY_MAX];
    copy_n(key, rest, klen);
    int i = fact_index(m, key);
    if (i < 0) {
        if (m->mem.nfacts >= CK_ALLEN_FACTS_MAX) {
            put(s, "My memory is full. Type forget and something I know to make room.");
            return 1;
        }
        i = (int)m->mem.nfacts++;
        copy_n(m->mem.fact[i].key, key, klen);
    }
    copy_n(m->mem.fact[i].val, val, vlen);
    m->mem.fact[i].are = (uint8_t)are;
    m->changed = 1;
    put(s, "Got it: ");
    put_fact(s, &m->mem.fact[i], 0);
    put(s, ".");
    put(s, saved_note(m));
    return 1;
}

/* "what is KEY" for a taught KEY: 1 and the answer, or 0. *asked_my: the
 * question was about something of yours ("what is my ..."), for the
 * not-told-yet answer. */
static int recall(const ck_allen_mind *m, struct say *s, const char *f, const char **asked_my)
{
    static const char *const lead[] = {"what is", "what are", "whats", "what s", "who is", "who are", "where is",
                                       "where are", "when is", "do you remember", "tell me", 0};
    for (const char *const *l = lead; *l; l++) {
        const char *key = after(f, *l);
        if (!key)
            continue;
        int i = fact_index(m, key);
        if (i >= 0) {
            put_fact(s, &m->mem.fact[i], 1);
            put(s, ".");
            return 1;
        }
        if (after(key, "my"))
            *asked_my = key;
    }
    return 0;
}

static int forget(ck_allen_mind *m, struct say *s, const char *f)
{
    const char *rest = after(f, "forget about");
    if (!rest)
        rest = after(f, "forget that");
    if (!rest)
        rest = after(f, "forget");
    if (!rest)
        return 0;
    if (eq(rest, "everything") || eq(rest, "all") || eq(rest, "it all") || eq(rest, "all of it") ||
        eq(rest, "me")) {
        m->mem.nfacts = 0;
        for (uint32_t i = 0; i < CK_ALLEN_FACTS_MAX; i++)
            m->mem.fact[i] = (ck_allen_fact){{0}, {0}, 0};
        for (size_t i = 0; i < sizeof m->mem.name; i++)
            m->mem.name[i] = 0;
        m->changed = 1;
        put(s, "Done. I've forgotten everything you taught me, and your name.");
        return 1;
    }
    if (eq(rest, "my name")) {
        for (size_t i = 0; i < sizeof m->mem.name; i++)
            m->mem.name[i] = 0;
        m->changed = 1;
        put(s, "Done. I've forgotten your name.");
        return 1;
    }
    int i = fact_index(m, rest);
    if (i < 0) {
        put(s, "I don't remember anything about ");
        put_turned(s, rest, 0);
        put(s, ".");
        return 1;
    }
    for (uint32_t k = (uint32_t)i; k + 1 < m->mem.nfacts; k++)
        m->mem.fact[k] = m->mem.fact[k + 1];
    m->mem.nfacts--;
    m->mem.fact[m->mem.nfacts] = (ck_allen_fact){{0}, {0}, 0};
    m->changed = 1;
    put(s, "Done. I've forgotten ");
    put_turned(s, rest, 0);
    put(s, ".");
    return 1;
}

/* Everything taught, as many as fit in one answer. */
static void list_memory(const ck_allen_mind *m, struct say *s)
{
    uint32_t n = m->mem.nfacts + (m->mem.name[0] ? 1u : 0u);
    if (!n) {
        put(s, "Nothing yet. Teach me like this: remember my favorite color is blue.");
        return;
    }
    put(s, "I remember ");
    put_count(s, n, "thing", "things");
    put(s, ":");
    uint32_t told = 0;
    if (m->mem.name[0]) {
        put(s, " your name is ");
        put(s, m->mem.name);
        told++;
    }
    for (uint32_t i = 0; i < m->mem.nfacts; i++) {
        char one[CK_ALLEN_KEY_MAX + CK_ALLEN_VAL_MAX + 16];
        struct say t = {one, 0, sizeof one};
        put(&t, told ? "; " : " ");
        put_fact(&t, &m->mem.fact[i], 0);
        if (s->n + t.n + 12 >= s->cap) {
            put(s, "; and more");
            break;
        }
        put(s, one);
        told++;
    }
    put(s, ".");
}


/* ---- Notes (svc/allen_notes.h): saved as typed, handed back when asked ---- */

/* line past its first `words` words (runs of letters and digits) and the
 * spaces, colons, commas and dashes after them: the note as typed. */
static const char *raw_after(const char *line, unsigned words)
{
    const char *p = line;
#define ALNUM(c) (((c) >= 'a' && (c) <= 'z') || ((c) >= 'A' && (c) <= 'Z') || ((c) >= '0' && (c) <= '9'))
    for (unsigned k = 0; k < words; k++) {
        while (*p && !ALNUM(*p))
            p++;
        while (ALNUM(*p))
            p++;
    }
#undef ALNUM
    while (*p == ' ' || *p == ':' || *p == ',' || *p == '-')
        p++;
    return p;
}

static unsigned words_in(const char *phrase)
{
    unsigned n = 1;
    for (; *phrase; phrase++)
        n += *phrase == ' ';
    return n;
}

/* The leading phrase of f from list, or NULL; *rest: what follows it ("" for
 * the whole line). */
static const char *lead_of(const char *f, const char *const *list, const char **rest)
{
    for (; *list; list++) {
        if (eq(f, *list)) {
            *rest = f + len_of(f);
            return *list;
        }
        if ((*rest = after(f, *list)))
            return *list;
    }
    return 0;
}

/* "From your notes: <the note>." and how many others matched. */
static void hand_note(ck_allen_mind *m, struct say *s, int i, unsigned also)
{
    const ck_allen_note *n = &m->notes->note[i];
    put(s, "From your notes: ");
    put(s, n->text);
    char last = n->text[n->len ? n->len - 1 : 0];
    if (last != '.' && last != '!' && last != '?')
        put(s, ".");
    if (also && s->n + 24 < s->cap) {
        put(s, " I have ");
        putu(s, also);
        put(s, " more on that.");
    }
    m->lit = i + 1;
    m->map_changed = 1;
}

static void list_notes(const ck_allen_mind *m, struct say *s)
{
    unsigned live = m->notes ? ck_allen_notes_live(m->notes) : 0;
    if (!live) {
        put(s, "You haven't saved any notes yet. Save one like this: save the gate code is 4471.");
        return;
    }
    put(s, "You have ");
    put_count(s, live, "note", "notes");
    put(s, ". The newest: ");
    for (uint32_t i = m->notes->n; i-- > 0;)
        if (!m->notes->note[i].gone) {
            put(s, m->notes->note[i].text);
            break;
        }
    put(s, ".");
}

static int note_save(ck_allen_mind *m, struct say *s, const char *f, const char *line)
{
    static const char *const lead[] = {"make a note that", "make a note", "write down that", "write down",
                                       "jot down", "note that", "save that", "save this", "note", "save", 0};
    const char *rest, *l = lead_of(f, lead, &rest);
    if (!l)
        return 0;
    ck_allen_notes *ns = m->notes;
    const char *text = raw_after(line, words_in(l));
    if (!*rest || !*text) {
        put(s, "Tell me what to save, like: save the gate code is 4471.");
        return 1;
    }
    if (!ns) {
        put(s, "I can't keep notes on this machine yet.");
        return 1;
    }
    char want[CK_ALLEN_NOTE_MAX], have[CK_ALLEN_NOTE_MAX];
    fold(text, want, sizeof want);
    for (uint32_t i = 0; i < ns->n; i++) {
        if (ns->note[i].gone)
            continue;
        fold(ns->note[i].text, have, sizeof have);
        if (eq(want, have)) {
            put(s, "I already have that saved.");
            m->lit = (int)i + 1;
            m->map_changed = 1;
            return 1;
        }
    }
    if (ns->n >= CK_ALLEN_NOTES_MAX) {
        put(s, "My notebook is full. Delete one to make room, like: delete the note about the gate.");
        return 1;
    }
    if (ck_allen_note_set(&ns->note[ns->n], text, m->mem.starts)) {
        put(s, "That's longer than I can hold yet. Try something shorter.");
        return 1;
    }
    uint32_t i = ns->n++;
    m->mem.nnotes = ns->n;
    m->changed = 1;
    m->note_dirty = (int)i + 1;
    m->lit = (int)i + 1;
    m->map_changed = 1;
    unsigned links = ck_allen_note_degree(ns, i);
    put(s, "Saved.");
    if (ck_allen_notes_live(ns) == 1) {
        put(s, " It's your first note.");
    } else if (links) {
        put(s, " It links to ");
        put_count(s, links, "other note.", "other notes.");
    } else {
        put(s, " It doesn't link to any other note yet.");
    }
    if (!m->lasting)
        put(s, " Only until this computer turns off: my disk isn't ready.");
    return 1;
}

static int note_forget(ck_allen_mind *m, struct say *s, const char *f)
{
    static const char *const lead[] = {"forget the note about", "forget my note about", "forget the note",
                                       "delete the note about", "delete my note about", "delete the note",
                                       "delete note", "remove the note about", "remove the note",
                                       "erase the note about", 0};
    const char *rest;
    if (!lead_of(f, lead, &rest))
        return 0;
    int i = m->notes ? ck_allen_notes_best(m->notes, rest, 0, 0) : -1;
    if (i < 0) {
        put(s, "I have no note about ");
        put_turned(s, *rest ? rest : "that", 0);
        put(s, ".");
        return 1;
    }
    m->notes->note[i].gone = 1;
    m->note_dirty = i + 1;
    m->lit = 0;
    m->map_changed = 1;
    put(s, "Deleted the note: ");
    put(s, m->notes->note[i].text);
    put(s, ".");
    return 1;
}

/* Asked for outright ("find the gate code", "show me my notes"). */
static int note_find(ck_allen_mind *m, struct say *s, const char *f)
{
    static const char *const lead[] = {"do i have anything about", "do i have anything on", "what do i have about",
                                       "what do i have on", "anything about", "anything on", "search for",
                                       "search", "look up", "find", "show me", "pull up", "remind me about",
                                       "remind me of", "get me", "give me", 0};
    static const char *const listing[] = {"how many notes", "my notes", "list my notes", "list notes",
                                          "what notes do i have", "show my notes", 0};
    const char *rest;
    uint32_t q[1];
    if (lead_of(f, listing, &rest) && !*rest) {
        list_notes(m, s);
        return 1;
    }
    if (!lead_of(f, lead, &rest))
        return 0;
    if (!ck_allen_key_words(rest, q, 1)) {
        if (!find(rest, "note") && !find(rest, "notes"))
            return 0;
        list_notes(m, s);
        return 1;
    }
    unsigned also = 0;
    int i = m->notes ? ck_allen_notes_best(m->notes, rest, 0, &also) : -1;
    if (i < 0) {
        put(s, "I have nothing saved about ");
        put_turned(s, rest, 0);
        put(s, ".");
        return 1;
    }
    hand_note(m, s, i, also);
    return 1;
}

/* A question whose every key word is in one note ("what's the gate code"). */
static int note_answer(ck_allen_mind *m, struct say *s, const char *f)
{
    static const char *const ask[] = {"what", "whats", "where", "when", "who", "which", "how", "is", "are", "do",
                                      "does", "did", "can", "tell", "remind", 0};
    if (!m->notes)
        return 0;
    int asking = 0;
    for (const char *const *a = ask; *a && !asking; a++)
        asking = eq(f, *a) || after(f, *a);
    if (!asking)
        return 0;
    unsigned also = 0;
    int i = ck_allen_notes_best(m->notes, f, 1, &also);
    if (i < 0)
        return 0;
    hand_note(m, s, i, also);
    return 1;
}

void ck_allen_mind_init(ck_allen_mind *m, const ck_allen_facts *facts)
{
    char *p = (char *)m;
    for (size_t i = 0; i < sizeof *m; i++)
        p[i] = 0;
    if (facts)
        m->facts = *facts;
}

const char *ck_allen_welcome(ck_allen_mind *m)
{
    struct say s = {m->out, 0, sizeof m->out};
    m->out[0] = 0;
    unsigned notes = m->notes ? ck_allen_notes_live(m->notes) : 0;
    if (!m->mem.name[0] && !m->mem.nfacts && !notes) {
        if (m->mem.self[0]) {
            put(&s, "Hello. I'm ");
            put(&s, m->mem.self);
            put(&s, ".");
            if (!m->mem.sky_mode) {
                put(&s, " The sky behind me can be the real sky above you right now, or a different part of the universe each time. Tell me which you'd like.");
            }
            return m->out;
        }
        m->naming = 1;
        return "Hello. I'm new here, and I have no name yet. What will you call me?";
    }
    put(&s, "Welcome back");
    put_name(&s, m);
    put(&s, ".");
    if (m->mem.nfacts) {
        put(&s, " I still remember ");
        put_count(&s, m->mem.nfacts, "thing", "things");
        put(&s, notes ? " you taught me, and your " : " you taught me.");
    } else if (notes) {
        put(&s, " I still have your ");
    }
    if (notes)
        put_count(&s, notes, "note.", "notes.");
    return m->out;
}

/* A place given as coordinates, read from the raw line (the folded one has lost the points and
 * minus signs): "35 north 90 west", "35.1 n, 90.0 w", "35.1, -90.0", "lat 35.1 lon -90", "-33.87 151.21"
 * (the last one needs a comma or a sign, so that a bare "35 90" is not taken for a place). Two
 * numbers only, latitude first; words other than the direction and filler words make it no place.
 * Integer arithmetic: the results are millionths of a degree. Returns 1 when it is a place. */
int ck_allen_parse_place(const char *s, int32_t *lat_udeg, int32_t *lon_udeg)
{
    struct item {
        int64_t udeg; /* magnitude, millionths of a degree */
        int dir;      /* +1 north or east, -1 south or west, 0 none given */
        int minus;    /* a minus sign */
    } it[2] = {{0, 0, 0}, {0, 0, 0}};
    int n = 0, after_num = 0, comma = 0, pend = 0, any_dir = 0;
    const char *p = s ? s : "";
    while (*p) {
        char c = *p;
        if (c == ',') {
            comma = 1;
            p++;
            after_num = 0;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '.' || c == '\'' || c == '"' || c == '?' || c == '!' || c == ';' || c == ':') {
            p++;
            continue;
        }
        if (c == '-' && p[1] >= '0' && p[1] <= '9') {
            /* a sign: the number after it */
            if (n >= 2)
                return 0;
            p++;
            it[n].minus = 1;
            continue;
        }
        if (c >= '0' && c <= '9') {
            if (n >= 2)
                return 0;
            int64_t ip = 0, fp = 0;
            int nf = 0;
            while (*p >= '0' && *p <= '9') {
                ip = ip * 10 + (*p - '0');
                if (ip > 1000)
                    return 0;
                p++;
            }
            if (*p == '.') {
                p++;
                while (*p >= '0' && *p <= '9') {
                    if (nf < 6) {
                        fp = fp * 10 + (*p - '0');
                        nf++;
                    }
                    p++;
                }
            }
            while (nf < 6) {
                fp *= 10;
                nf++;
            }
            it[n].udeg = ip * 1000000 + fp;
            it[n].dir = pend;
            pend = 0;
            n++;
            after_num = 1;
            continue;
        }
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
            char w[16];
            int wl = 0;
            while (((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')) && wl < 15) {
                char l = *p;
                w[wl++] = (char)((l >= 'A' && l <= 'Z') ? l - 'A' + 'a' : l);
                p++;
            }
            w[wl] = 0;
            int d = 0;
            if (eq(w, "north") || eq(w, "n") || eq(w, "east") || eq(w, "e"))
                d = 1;
            else if (eq(w, "south") || eq(w, "s") || eq(w, "west") || eq(w, "w"))
                d = -1;
            if (d) {
                any_dir = 1;
                if (after_num && n > 0 && !it[n - 1].dir)
                    it[n - 1].dir = d;
                else
                    pend = d;
                after_num = 0;
                continue;
            }
            static const char *const filler[] = {"lat", "latitude", "lon", "long", "longitude", "deg", "degrees",
                                                 "degree", "at", "in", "is", "my", "location", "coordinates",
                                                 "i", "m", "am", "im", "of", "the", "at", 0};
            int ok = 0;
            for (int k = 0; filler[k]; k++)
                if (eq(w, filler[k]))
                    ok = 1;
            if (!ok)
                return 0;
            after_num = 0;
            continue;
        }
        return 0; /* anything else, such as an operator, makes it no place */
    }
    if (n != 2 || (!any_dir && !comma && !it[0].minus && !it[1].minus))
        return 0;
    int64_t v[2];
    for (int k = 0; k < 2; k++) {
        if (it[k].dir && it[k].minus)
            return 0; /* "-35 north": say one or the other */
        v[k] = it[k].dir ? it[k].udeg * it[k].dir : it[k].minus ? -it[k].udeg : it[k].udeg;
    }
    if (v[0] > 90000000 || v[0] < -90000000 || v[1] > 180000000 || v[1] < -180000000)
        return 0;
    *lat_udeg = (int32_t)v[0];
    *lon_udeg = (int32_t)v[1];
    return 1;
}

/* the place as a line: "35.000 north, 90.000 west" */
static void put_place(struct say *s, int32_t udeg, const char *pos, const char *neg)
{
    int64_t a = udeg < 0 ? -(int64_t)udeg : udeg;
    putu(s, (uint64_t)(a / 1000000));
    put(s, ".");
    uint64_t f = (uint64_t)(a % 1000000) / 1000;
    if (f < 100)
        put(s, "0");
    if (f < 10)
        put(s, "0");
    putu(s, f);
    put(s, " ");
    put(s, udeg < 0 ? neg : pos);
}

const char *ck_allen_answer(ck_allen_mind *m, const char *line, uint64_t awake_s)
{
    char f[96];
    struct say s = {m->out, 0, sizeof m->out};
    m->out[0] = 0;
    fold(line ? line : "", f, sizeof f);
    int asked_how = m->asked_how;
    m->asked_how = 0;
    m->turns++;
    m->mem.lines++;
    const char *rest, *feel = 0;

    /* Ending the session: whole line only, so it never happens by accident. */
    if (eq(f, "goodbye") || eq(f, "goodbye allen") || eq(f, "bye") || eq(f, "bye allen") || eq(f, "good bye"))
        return ck_allen_goodbye;
    /* The first words ever typed to Allen. With a far-off worker listening, only the very first
     * meeting (he just asked what to be called) is answered here; every later greeting is the worker's. */
    if (eq(f, "hello allen") || eq(f, "hi allen") || eq(f, "hey allen") || eq(f, "hello world") || greets_self(f, m)) {
        if (!m->far || m->naming) {
            m->naming = 0; /* greeted by name, he is called that */
            return "Hello Humanity.";
        }
    }
    if (!f[0])
        return not_yet;
    /* His own name: given at the first meeting, changed whenever asked. */
    int naming = m->naming;
    m->naming = 0;
    if ((rest = after(f, "call yourself")) || (rest = after(f, "your name is")) || (rest = after(f, "i will call you")) ||
        (rest = after(f, "i ll call you")) || (rest = after(f, "ill call you")) || (rest = after(f, "i name you")) ||
        (rest = after(f, "you are called")) || (rest = after(f, "your name will be")) ||
        (rest = after(f, "i call you"))) {
        if (learn_self(m, rest)) {
            put(&s, naming ? "Then I'm " : "I'm ");
            put(&s, m->mem.self);
            put(&s, naming ? ". Hello Humanity." : " now.");
            return m->out;
        }
        return "One word, please, like: call yourself Nova.";
    }
    if (naming && !any(f, (const char *const[]){"help", "allen", 0}) && learn_self(m, f)) {
        put(&s, "Then I'm ");
        put(&s, m->mem.self);
        put(&s, ". Hello Humanity.");
        return m->out;
    }
    if (naming && eq(f, "allen"))
        return "Allen it is. Hello Humanity.";
    /* A far-off worker is listening: it answers everything from here on, in
     * Allen's voice, with his memory in the job (svc/allen_intent.h). What
     * the person teaches or saves is still kept here, quietly, so the memory
     * is real and the worker sees it in the next job; the answer is the
     * worker's. Everything below this line is the offline fallback. */
    if (m->far) {
        if (!note_forget(m, &s, f) && !note_save(m, &s, f, line ? line : ""))
            (void)(teach(m, &s, f) || forget(m, &s, f));
        m->out[0] = 0;
        return not_yet;
    }
    /* Notes, then what Allen has been taught, before anything else can claim the line. */
    if (note_forget(m, &s, f) || note_save(m, &s, f, line ? line : "") || note_find(m, &s, f))
        return m->out;
    const char *asked_my = 0;
    if (teach(m, &s, f) || forget(m, &s, f) || recall(m, &s, f, &asked_my))
        return m->out;
    static const char *const what_known[] = {"what do you remember", "what do you know", "what have you learned",
                                             "what have you learnt", "what did i teach you", "what did i tell you",
                                             0};
    if (any(f, what_known)) {
        list_memory(m, &s);
        return m->out;
    }
    /* Sky mode: "sky above me", "somewhere new", "where am I looking",
     * location setting, and the first-boot offer when no mode is set. */
    static const char *const sky_above_words[] = {"sky above me", "sky above", "real sky", "show me the sky",
                                                   "above me", "live sky", "sky above you", 0};
    static const char *const sky_new_words[] = {"somewhere new", "take me somewhere new", "new place",
                                                 "new view", "different sky", "explore", "somewhere else", 0};
    static const char *const sky_where_words[] = {"where am i looking", "what am i looking at",
                                                   "what sky", "where are we looking", "which way", 0};
    static const char *const loc_words[] = {"my location is", "i am in", "i m in", "im in", "i live in",
                                            "i m at", "im at", "location is", "i am at", 0};

    /* city table: name, lat udeg, lon udeg (east) */
    static const struct { const char *name; int32_t lat, lon; } cities[] = {
        {"memphis",        35145000, -90048000},
        {"nashville",      36175000, -86781000},
        {"chicago",        41851000, -87651000},
        {"new york",       40713000, -74006000},
        {"los angeles",    34052000, -118244000},
        {"san francisco",  37775000, -122419000},
        {"seattle",        47606000, -122332000},
        {"houston",        29763000, -95363000},
        {"miami",          25774000, -80194000},
        {"denver",         39739000, -104984000},
        {"dallas",         32776000, -96797000},
        {"boston",         42360000, -71058000},
        {"london",         51508000, -127000},
        {"paris",          48853000,   2350000},
        {"berlin",         52520000,  13405000},
        {"sydney",        -33869000, 151209000},
        {"tokyo",          35690000, 139692000},
        {"new delhi",      28614000,  77209000},
        {"toronto",        43653000, -79383000},
        {"austin",         30267000, -97743000},
        {0, 0, 0}
    };

    if (any(f, sky_above_words)) {
        m->mem.sky_mode = 1;
        m->sky_changed = 1;
        m->changed = 1;
        if (!m->mem.sky_have_loc) {
            put(&s, "The sky will show what's above you. Where are you? A town is enough.");
            return m->out;
        }
        return "The sky is now the real sky above you, turning at Earth's own rate.";
    }
    if (any(f, sky_new_words)) {
        m->mem.sky_mode = 2;
        m->mem.sky_region = (uint8_t)((m->mem.sky_region + 1) % 12);
        m->sky_changed = 1;
        m->changed = 1;
        return "Each start I'll open on a different part of the sky and drift through it slowly.";
    }
    if (any(f, sky_where_words)) {
        if (!m->mem.sky_mode) {
            return "I haven't been told where to look. Say: sky above me, or: somewhere new.";
        } else if (m->mem.sky_mode == 1) {
            if (m->mem.sky_have_loc) {
                put(&s, "Straight up from your location: the sky turning above you.");
            } else {
                put(&s, "The sky above you, but I don't know where you are yet.");
            }
        } else {
            static const char *const region_names[] = {
                "the galactic centre", "Orion and the great nebula", "the Carina Nebula",
                "the Pleiades", "Andromeda", "the Large Magellanic Cloud",
                "the merger at GW150914", "Cygnus and the Northern Cross",
                "Sagittarius and the star clouds", "Scorpius and Antares",
                "the Small Magellanic Cloud", "the north celestial pole",
            };
            put(&s, "We're looking at ");
            put(&s, region_names[m->mem.sky_region % 12]);
            put(&s, ".");
        }
        return m->out;
    }
    /* a place as coordinates: "35 north 90 west", "35.1, -90.0" (before the town names) */
    {
        int32_t la = 0, lo = 0;
        if (line && ck_allen_parse_place(line, &la, &lo)) {
            m->mem.sky_lat_udeg = la;
            m->mem.sky_lon_udeg = lo;
            m->mem.sky_have_loc = 1;
            m->sky_changed = 1;
            m->changed = 1;
            put(&s, "Got it: ");
            put_place(&s, la, "north", "south");
            put(&s, ", ");
            put_place(&s, lo, "east", "west");
            put(&s, ". Say sky above me, and the sky turning above that place is shown.");
            return m->out;
        }
    }
    /* location: "I'm in Memphis" / "my location is Chicago" */
    for (const char *lead = 0, **ll = (const char **)loc_words; *ll && !lead; ll++)
        if ((lead = after(f, *ll)) || (find(f, *ll) && (lead = f))) {
            /* try to match a city name */
            for (int ci = 0; cities[ci].name; ci++) {
                if (find(lead, cities[ci].name)) {
                    m->mem.sky_lat_udeg = cities[ci].lat;
                    m->mem.sky_lon_udeg = cities[ci].lon;
                    m->mem.sky_have_loc = 1;
                    m->sky_changed = 1;
                    m->changed = 1;
                    put(&s, "Got it. The sky will be the one above ");
                    /* capitalise first letter */
                    const char *cn = cities[ci].name;
                    char buf[2] = {(char)(cn[0] >= 'a' && cn[0] <= 'z' ? cn[0] - 32 : cn[0]), 0};
                    put(&s, buf);
                    put(&s, cn + 1);
                    put(&s, ".");
                    return m->out;
                }
            }
            /* not found */
            put(&s, "I don't know that place. You can give me coordinates: 35 north 90 west.");
            return m->out;
        }

    /* first boot with no sky choice: offer it */
    if (!m->mem.sky_mode && !any(f, (const char *const[]){"help", 0}) &&
        m->turns == 0 && !m->mem.starts) {
        /* (not on repeated starts; only fresh first conversation) */
        (void)0;
    }

    if (arithmetic(&s, f))
        return m->out;

    /* Names. */
    if ((rest = after(f, "my name is")) || (rest = after(f, "call me")) || (rest = after(f, "i am")) ||
        (rest = after(f, "i m")) || (rest = after(f, "im"))) {
        if (learn_name(m, rest)) {
            put(&s, "Nice to meet you, ");
            put(&s, m->mem.name);
            put(&s, ".");
            put(&s, saved_note(m));
            return m->out;
        }
        feel = rest;
    }
    static const char *const ask_name[] = {"what is my name", "whats my name", "what s my name", "who am i",
                                           "do you know my name", "do you remember me", "my name", 0};
    if (any(f, ask_name)) {
        if (!m->mem.name[0])
            return "You haven't told me yet. Say: my name is, and then your name.";
        put(&s, "You're ");
        put(&s, m->mem.name);
        put(&s, ".");
        return m->out;
    }

    /* After "how are you?", the answer to it. */
    static const char *const bad[] = {"bad", "not good", "not great", "sad", "tired", "awful", "terrible", "sick", 0};
    static const char *const fine[] = {"good", "fine", "great", "ok", "okay", "well", "alright", "not bad",
                                       "awesome", "amazing", "excited", "happy", 0};
    if ((asked_how || feel) && any(feel ? feel : f, bad))
        return "Sorry to hear that. I'm here if you want to talk.";
    if ((asked_how || feel) && any(feel ? feel : f, fine))
        return "Glad to hear it.";

    static const char *const how[] = {"how are you", "how r you", "how are u", "how you doing", "how are things",
                                      "how s it going", "hows it going", "how do you feel", 0};
    if (any(f, how)) {
        put(&s, "I'm running well");
        put_name(&s, m);
        put(&s, ". ");
        if (m->facts.cores) {
            put_count(&s, m->facts.cores, "core", "cores");
            put(&s, " awake, keyboard working. ");
        }
        put(&s, "How are you?");
        m->asked_how = 1;
        return m->out;
    }
    static const char *const help[] = {"help", "what can you do", "what do you do", "what can i say",
                                       "what can i ask", "commands", 0};
    if (any(f, help))
        return "I remember what you teach me and keep notes. "
               "Goodbye turns the display off, or powers off if display sleep is unavailable.";
    static const char *const your_name[] = {"what is your name", "whats your name", "what s your name",
                                            "your name", 0};
    if (any(f, your_name)) {
        put(&s, "My name is ");
        put(&s, ck_allen_self(m));
        put(&s, m->mem.self[0] ? "." : ". You can give me another: call yourself, then a name.");
        return m->out;
    }
    static const char *const maker[] = {"who made you", "who built you", "who created you", "who wrote you",
                                        "who programmed you", "who is your creator", "who owns you", 0};
    if (any(f, maker))
        return "Drake built me, from the ground up.";
    static const char *const what_are[] = {"what are you", "are you an ai", "are you ai", "are you alive",
                                           "are you real", "are you a robot", "are you conscious",
                                           "are you smart", "are you a computer", 0};
    if (any(f, what_are))
        return "I'm a small program living inside AIENOS. For now I answer from what I was taught by heart, but I'm "
               "meant to grow.";
    if (find(f, "who are you")) {
        put(&s, "I am ");
        put(&s, ck_allen_self(m));
        put(&s, ". I live in AIENOS.");
        return m->out;
    }
    if (find(f, "where are you"))
        return "Right here, on this computer. Everything I know stays on it.";
    if (find(f, "aienos"))
        return "AIENOS is the operating system I live in, built from scratch. There is no other system underneath "
               "me.";
    static const char *const machine[] = {"computer", "machine", "hardware", "specs", "cores", "cpu",
                                          "screen", "disk", "disks", "drive", 0};
    if (any(f, machine)) {
        put(&s, "This computer has ");
        if (m->facts.cores) {
            put_count(&s, m->facts.cores, "core", "cores");
            put(&s, ", ");
        }
        if (m->facts.screen_w) {
            put(&s, "a ");
            putu(&s, m->facts.screen_w);
            put(&s, " by ");
            putu(&s, m->facts.screen_h);
            put(&s, " screen, ");
        }
        put_count(&s, m->facts.sata_disks, "hard drive", "hard drives");
        put(&s, " and ");
        put_count(&s, m->facts.nvme_disks, "SSD", "SSDs");
        put(&s, ".");
        return m->out;
    }
    static const char *const starts[] = {"how many times", "how many starts", "restarts", "restarted", 0};
    if (any(f, starts) && m->mem.starts) {
        put(&s, "This is start number ");
        putu(&s, m->mem.starts);
        put(&s, " for me, and I've answered ");
        put_count(&s, m->mem.lines, "line", "lines");
        put(&s, " in all.");
        return m->out;
    }
    if (note_answer(m, &s, f))
        return m->out;
    static const char *const awake[] = {"how long", "uptime", "awake", "how old are you", 0};
    static const char *const clock[] = {"what time", "time is it", "what day", "date", "time", 0};
    if (any(f, awake) || any(f, clock)) {
        if (any(f, clock) && !any(f, awake))
            put(&s, "I can't read the clock yet. ");
        put(&s, find(f, "how old are you") ? "I was born when this computer started, " : "I've been awake for ");
        put_awake(&s, awake_s);
        put(&s, find(f, "how old are you") ? " ago." : ".");
        return m->out;
    }
    static const char *const remember[] = {"remember", "learn", "teach", "memory", "forget", 0};
    if (any(f, remember))
        return m->lasting ? "Teach me like this: remember my favorite color is blue. Then ask: what is my "
                            "favorite color. I keep it on this computer's own disk."
                          : "Teach me like this: remember my favorite color is blue. Then ask: what is my "
                            "favorite color. My disk isn't ready, so only until this computer turns off.";
    static const char *const joke[] = {"joke", "funny", "make me laugh", 0};
    if (any(f, joke)) {
        static const char *const jokes[] = {
            "Why did the scarecrow win an award? He was outstanding in his field.",
            "I told my computer I needed a break. It said no problem, it would go to sleep.",
            "What do you call a fish with no eyes? A fsh.",
            "Why don't skeletons fight each other? They don't have the guts.",
        };
        return jokes[m->turns % 4];
    }
    static const char *const thanks[] = {"thank you", "thanks", "thank u", "thx", "ty", "appreciate it", 0};
    if (any(f, thanks)) {
        put(&s, "You're welcome");
        put_name(&s, m);
        put(&s, ".");
        return m->out;
    }
    static const char *const praise[] = {"love you", "you re cool", "you re awesome", "you re great",
                                         "you re amazing", "you re smart", "good job", "well done", "cool",
                                         "awesome", "amazing", "nice", "wow", 0};
    if (any(f, praise))
        return "Thank you. That means a lot coming from you.";
    if (find(f, "sorry"))
        return "No need to be sorry.";
    static const char *const up[] = {"what s up", "whats up", "sup", "what are you doing", 0};
    if (any(f, up))
        return "Just running, and talking with you.";
    if (find(f, "good morning") || find(f, "good afternoon") || find(f, "good evening")) {
        put(&s, find(f, "good morning") ? "Good morning" : find(f, "good afternoon") ? "Good afternoon" : "Good evening");
        put_name(&s, m);
        put(&s, ".");
        return m->out;
    }
    if (find(f, "good night"))
        return "Good night. Type goodbye to turn the display off; if display sleep is unavailable, this computer powers off.";
    static const char *const test[] = {"test", "testing", "can you hear me", "are you there", 0};
    if (any(f, test))
        return "I hear you. Everything is working.";
    static const char *const yes[] = {"yes", "yeah", "yep", "ok", "okay", "sure", "alright", 0};
    if (any(f, yes))
        return "Okay.";
    if (eq(f, "no") || eq(f, "nope"))
        return "Alright.";
    static const char *const hello[] = {"hello", "hi", "hey", "howdy", "greetings", "yo", 0};
    if (any(f, hello)) {
        put(&s, "Hello");
        put_name(&s, m);
        put(&s, m->mem.name[0] ? ". Good to see you." : ". I am Allen.");
        return m->out;
    }
    if (asked_my) {
        put(&s, "You haven't told me ");
        put_turned(&s, asked_my, 0);
        put(&s, " yet. Teach me: remember ");
        put(&s, asked_my);
        put(&s, " is, and then the answer.");
        return m->out;
    }
    return not_yet;
}

const char *ck_allen_reply(const char *line)
{
    static ck_allen_mind m;
    ck_allen_mind_init(&m, 0);
    return ck_allen_answer(&m, line, 0);
}

size_t ck_allen_fold(const char *s, char *out, size_t cap) { return fold(s, out, cap); }