/* allen_scene.c -- Allen's canvas, the words side; see allen_scene.h and docs/ALLEN_CANVAS.md. */
#include "allen_scene.h"

#define LINE_MAX_BYTES CK_SCENE_BYTES
#define SAY_ROOM 159u /* Allen's words: allen_ui.c TEXT_MAX - 1 */

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int alpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static int digit(int c) { return c >= '0' && c <= '9'; }

/* a == the word w (any case), n letters of a */
static int is(const char *a, unsigned n, const char *w)
{
    unsigned i = 0;
    for (; i < n && w[i]; i++)
        if (lower(a[i]) != w[i])
            return 0;
    return i == n && !w[i];
}

static int any_of(const char *a, unsigned n, const char *const *list)
{
    for (; *list; list++)
        if (is(a, n, *list))
            return 1;
    return 0;
}

const char *ck_scene_kind_name(unsigned kind)
{
    static const char *const names[] = {"none", "note", "list", "number", "quote", "person", "timer", "picture", "video"};
    return kind < sizeof names / sizeof names[0] ? names[kind] : "none";
}

/* the kind a first word names: exact (1) or a near word the model reached for (2, counted as tidied) */
static unsigned kind_of(const char *w, unsigned n, int *near)
{
    *near = 0;
    for (unsigned k = CK_THING_NOTE; k <= CK_THING_VIDEO; k++)
        if (is(w, n, ck_scene_kind_name(k)))
            return k;
    static const struct {
        const char *w;
        unsigned k;
    } nears[] = {{"card", CK_THING_NOTE},     {"text", CK_THING_NOTE},     {"items", CK_THING_LIST},
                 {"bignumber", CK_THING_NUMBER}, {"image", CK_THING_PICTURE}, {"photo", CK_THING_PICTURE},
                 {"clip", CK_THING_VIDEO},    {"movie", CK_THING_VIDEO},   {"countdown", CK_THING_TIMER},
                 {"reminder", CK_THING_TIMER}, {"message", CK_THING_PERSON}, {"friend", CK_THING_PERSON}};
    for (unsigned i = 0; i < sizeof nears / sizeof nears[0]; i++)
        if (is(w, n, nears[i].w)) {
            *near = 1;
            return nears[i].k;
        }
    return 0;
}

/* how much room each field of a kind has (bytes, without the NUL) */
static unsigned room(unsigned kind, unsigned i)
{
    switch (kind) {
    case CK_THING_NOTE:
        return i == 0 ? 48 : 240;
    case CK_THING_LIST:
        return i == 0 ? 48 : 64;
    case CK_THING_NUMBER:
        return i == 0 ? 16 : 64;
    case CK_THING_QUOTE:
        return i == 0 ? 220 : 48;
    case CK_THING_PERSON:
        return i == 0 ? 32 : 200;
    case CK_THING_TIMER:
        return i == 0 ? 24 : 64;
    default: /* picture, video: what it shows, where it comes from */
        return i == 0 ? 120 : 64;
    }
}

/* the fewest fields a kind needs to show anything */
static unsigned needs(unsigned kind) { return kind == CK_THING_PERSON ? 2u : 1u; }

struct parse {
    ck_scene *s;
    ck_scene_stats st;
};

/* Copies n bytes of a cleaned field into the scene's text, cut to max at a
 * word (with "..." when cut) so a field never ends mid-word. Returns its
 * offset, or -1 when the text is full. */
static int keep(struct parse *p, const char *a, unsigned n, unsigned max)
{
    while (n && a[0] == ' ')
        a++, n--;
    while (n && a[n - 1] == ' ')
        n--;
    int cut = 0;
    if (n > max) {
        cut = 1;
        unsigned m = max > 3 ? max - 3 : max, sp = m;
        while (sp > m / 2 && a[sp] != ' ')
            sp--;
        n = sp > m / 2 ? sp : m;
        while (n && a[n - 1] == ' ')
            n--;
    }
    ck_scene *s = p->s;
    unsigned need = n + (cut ? 3u : 0u) + 1u;
    if ((unsigned)s->used + need > CK_SCENE_TEXT)
        return -1;
    int at = s->used;
    for (unsigned i = 0; i < n; i++)
        s->text[s->used++] = a[i];
    if (cut) {
        s->text[s->used++] = '.', s->text[s->used++] = '.', s->text[s->used++] = '.';
        p->st.cut++;
    }
    s->text[s->used++] = 0;
    return at;
}

/* A say: one or two sentences, cut at the last sentence end that fits, else at a word. */
static void say(struct parse *p, const char *a, unsigned n)
{
    while (n && (a[0] == ' ' || a[0] == ':'))
        a++, n--;
    while (n && a[n - 1] == ' ')
        n--;
    if (n > SAY_ROOM) {
        unsigned end = 0;
        for (unsigned i = SAY_ROOM / 3; i < SAY_ROOM; i++)
            if ((a[i] == '.' || a[i] == '!' || a[i] == '?') && (i + 1 == n || a[i + 1] == ' '))
                end = i + 1;
        if (end) {
            p->st.cut++;
            n = end;
        }
    }
    int at = keep(p, a, n, SAY_ROOM);
    if (at >= 0) {
        p->s->say = (uint16_t)at;
        p->s->has_say = 1;
    }
}

static void mood(struct parse *p, const char *a, unsigned n)
{
    static const char *const night[] = {"night", "calm", "still-night", 0};
    static const char *const dawn[] = {"dawn", "warm", "morning", "sunrise", "golden", 0};
    static const char *const deep[] = {"deep", "quiet", "dark", "soft", 0};
    static const char *const clear[] = {"clear", "bright", "day", "light", 0};
    for (unsigned i = 0; i < n;) {
        while (i < n && !alpha(a[i]) && a[i] != '-')
            i++;
        unsigned w = i;
        while (i < n && (alpha(a[i]) || a[i] == '-'))
            i++;
        unsigned wn = i - w;
        if (!wn)
            continue;
        const char *x = a + w;
        if (any_of(x, wn, night))
            p->s->tone = CK_TONE_NIGHT;
        else if (any_of(x, wn, dawn))
            p->s->tone = CK_TONE_DAWN;
        else if (any_of(x, wn, deep))
            p->s->tone = CK_TONE_DEEP;
        else if (any_of(x, wn, clear))
            p->s->tone = CK_TONE_CLEAR;
        else if (is(x, wn, "still"))
            p->s->water = CK_WATER_STILL;
        else if (is(x, wn, "slow"))
            p->s->water = CK_WATER_SLOW;
        else if (is(x, wn, "lively"))
            p->s->water = CK_WATER_LIVELY;
        else if (!is(x, wn, "water") && !is(x, wn, "sky") && !is(x, wn, "tone"))
            p->st.unknown++;
    }
}

/* one word before the colon: where, how much, how it comes, which way */
static int modifier(ck_scene_thing *t, const char *w, unsigned n)
{
    if (is(w, n, "left"))
        t->place = CK_PLACE_LEFT;
    else if (is(w, n, "right"))
        t->place = CK_PLACE_RIGHT;
    else if (is(w, n, "above") || is(w, n, "top"))
        t->place = CK_PLACE_ABOVE;
    else if (is(w, n, "warm"))
        t->warm = 1, t->quiet = 0;
    else if (is(w, n, "quiet"))
        t->quiet = 1, t->warm = 0;
    else if (is(w, n, "bloom"))
        t->arrive = CK_ARRIVE_BLOOM;
    else if (is(w, n, "rise"))
        t->arrive = CK_ARRIVE_RISE;
    else if (is(w, n, "fade"))
        t->arrive = CK_ARRIVE_FADE;
    else if (t->kind == CK_THING_PERSON && is(w, n, "to"))
        t->way = CK_WAY_TO;
    else if (t->kind == CK_THING_PERSON && is(w, n, "from"))
        t->way = CK_WAY_FROM;
    else
        return 0;
    return 1;
}

static void thing(struct parse *p, unsigned kind, const char *a, unsigned n)
{
    ck_scene *s = p->s;
    if (s->nthings >= CK_SCENE_THINGS) {
        p->st.over++;
        return;
    }
    ck_scene_thing t = {0};
    t.kind = (uint8_t)kind;
    /* the head: the words before the first colon, when they are only words */
    unsigned colon = n;
    for (unsigned i = 0; i < n; i++)
        if (a[i] == ':') {
            colon = i;
            break;
        }
    int head = colon < n && colon <= 40;
    for (unsigned i = 0; head && i < colon; i++)
        if (!alpha(a[i]) && a[i] != ' ' && a[i] != '-')
            head = 0;
    unsigned i = 0;
    if (head) {
        while (i < colon) {
            while (i < colon && a[i] == ' ')
                i++;
            unsigned w = i;
            while (i < colon && a[i] != ' ')
                i++;
            if (i > w && !modifier(&t, a + w, i - w))
                p->st.unknown++;
        }
        i = colon + 1;
    } else { /* no colon after the words: take the known words at the front */
        for (;;) {
            while (i < n && a[i] == ' ')
                i++;
            unsigned w = i;
            while (i < n && alpha(a[i]))
                i++;
            if (i == w || (i < n && a[i] != ' ' && a[i] != ':') || !modifier(&t, a + w, i - w)) {
                i = w;
                break;
            }
        }
        if (i < n && a[i] == ':')
            i++;
    }
    /* the fields */
    unsigned nf = 0, start = i;
    for (unsigned j = i; j <= n; j++) {
        if (j < n && a[j] != '|')
            continue;
        unsigned fs = start, fe = j;
        while (fs < fe && a[fs] == ' ')
            fs++;
        start = j + 1;
        if (fs == fe) /* an empty field (two bars together, a bar at the end) */
            continue;
        if (nf >= CK_SCENE_FIELDS) {
            p->st.cut++;
            break;
        }
        int at = keep(p, a + fs, fe - fs, room(kind, nf));
        if (at < 0) {
            p->st.cut++;
            break;
        }
        t.f[nf++] = (uint16_t)at;
    }
    t.nf = (uint8_t)nf;
    if (nf < needs(kind)) {
        p->st.rejected++;
        return;
    }
    if (kind == CK_THING_NUMBER) { /* a number shows a number */
        const char *v = s->text + t.f[0];
        int d = 0;
        for (; *v; v++)
            d |= digit(*v);
        if (!d) {
            p->st.rejected++;
            return;
        }
    }
    s->thing[s->nthings++] = t;
    p->st.kept++;
}

int ck_scene_parse(const char *in, size_t len, ck_scene *s, ck_scene_stats *st)
{
    struct parse p = {s, {0}};
    if (!s)
        return 0;
    for (unsigned i = 0; i < sizeof *s; i++)
        ((char *)s)[i] = 0;
    if (!in)
        len = 0;
    size_t n = 0;
    while (n < len && n < CK_SCENE_BYTES && in[n])
        n++;
    if (n == CK_SCENE_BYTES && n < len && in[n])
        p.st.truncated = 1;
    char ln[LINE_MAX_BYTES + 1];
    char plain[SAY_ROOM + 1]; /* the first line nobody understood: the say if nothing else is */
    unsigned plain_n = 0;
    int thinking = 0, had_mood = 0;
    for (size_t i = 0; i < n;) {
        size_t e = i;
        while (e < n && in[e] != '\n')
            e++;
        int whole = e < n || !p.st.truncated; /* a line the byte limit cut through is dropped */
        /* clean: printable ASCII, tabs as spaces, runs of spaces folded, ends trimmed */
        unsigned m = 0, dropped = 0;
        for (size_t j = i; j < e; j++) {
            unsigned char c = (unsigned char)in[j];
            if (c == '\t')
                c = ' ';
            if (c < 32 || c > 126) {
                dropped += c != '\r';
                continue;
            }
            if (c == ' ' && (m == 0 || ln[m - 1] == ' '))
                continue;
            ln[m++] = (char)c;
        }
        while (m && ln[m - 1] == ' ')
            m--;
        ln[m] = 0;
        i = e + 1;
        if (!whole) {
            p.st.over++;
            break;
        }
        /* the model's private thinking, if it shows any, is not a scene */
        if (m >= 7 && is(ln, 7, "<think>"))
            thinking = 1;
        if (thinking) {
            for (unsigned j = 0; j + 8 <= m; j++)
                if (is(ln + j, 8, "</think>"))
                    thinking = 0;
            continue;
        }
        if (!m)
            continue;
        if (m >= 3 && ln[0] == '`' && ln[1] == '`' && ln[2] == '`') { /* a code fence round the scene */
            p.st.tidied++;
            continue;
        }
        if (++p.st.lines > CK_SCENE_LINES) {
            p.st.over++;
            continue;
        }
        p.st.cut += dropped ? 1u : 0u;
        /* a list mark, a number or bold marks before the word: taken off, counted */
        unsigned a = 0;
        int tidy = 0;
        for (;;) {
            if (a < m && (ln[a] == '-' || ln[a] == '*' || ln[a] == '#' || ln[a] == '>' || ln[a] == '"') &&
                (a + 1 == m || ln[a + 1] == ' ' || ln[a + 1] == '*' || ln[a + 1] == '#' || alpha(ln[a + 1]))) {
                a++, tidy = 1;
            } else if (a < m && digit(ln[a])) {
                unsigned b = a;
                while (b < m && digit(ln[b]))
                    b++;
                if (b < m && (ln[b] == '.' || ln[b] == ')') && b + 1 < m && ln[b + 1] == ' ')
                    a = b + 1, tidy = 1;
                else
                    break;
            } else
                break;
            while (a < m && ln[a] == ' ')
                a++;
        }
        unsigned w = a;
        while (w < m && alpha(ln[w]))
            w++;
        unsigned wn = w - a;
        unsigned r = w; /* the rest of the line after the first word, past bold marks and a quote */
        while (r < m && (ln[r] == '*' || ln[r] == '"'))
            r++, tidy = 1;
        int near = 0;
        unsigned kind = wn ? kind_of(ln + a, wn, &near) : 0;
        int understood = 1;
        if (wn && is(ln + a, wn, "say") && (r == m || ln[r] == ' ' || ln[r] == ':')) {
            if (p.s->has_say)
                p.st.over++;
            else {
                say(&p, ln + r, m - r);
                p.st.kept++;
            }
        } else if (wn && is(ln + a, wn, "mood") && (r == m || ln[r] == ' ' || ln[r] == ':')) {
            if (had_mood)
                p.st.over++;
            else {
                mood(&p, ln + r, m - r);
                had_mood = 1;
                p.st.kept++;
            }
        } else if (kind && (r == m || ln[r] == ' ' || ln[r] == ':')) {
            unsigned before = p.st.rejected;
            thing(&p, kind, ln + r, m - r);
            if (p.st.rejected != before)
                understood = 0;
            tidy |= near;
        } else {
            p.st.rejected++;
            understood = 0;
        }
        if (understood && tidy)
            p.st.tidied++;
        if (!understood && !plain_n) {
            unsigned k = m - a < SAY_ROOM ? m - a : SAY_ROOM;
            for (unsigned j = 0; j < k; j++)
                plain[j] = ln[a + j];
            plain_n = k;
        }
    }
    if (!p.s->has_say && !p.s->nthings && !had_mood && plain_n)
        say(&p, plain, plain_n); /* plain words: Allen still says them */
    if (st)
        *st = p.st;
    return p.s->has_say || p.s->nthings || had_mood;
}

const char *ck_scene_field(const ck_scene *s, const ck_scene_thing *t, unsigned i)
{
    if (!s || !t || i >= t->nf || i >= CK_SCENE_FIELDS || t->f[i] >= CK_SCENE_TEXT)
        return "";
    return s->text + t->f[i];
}

const char *ck_scene_say(const ck_scene *s) { return s && s->has_say && s->say < CK_SCENE_TEXT ? s->text + s->say : ""; }

int ck_scene_clean(const ck_scene_stats *st, const ck_scene *s)
{
    return st && s && !st->rejected && !st->tidied && !st->over && !st->truncated && s->has_say;
}

uint32_t ck_scene_duration_ms(const char *w)
{
    if (!w)
        return 0;
    uint64_t total = 0;
    int parts = 0;
    const char *p = w;
    for (;;) {
        while (*p == ' ' || *p == ',')
            p++;
        if (!*p)
            break;
        const char *word = p;
        while (alpha(*p))
            p++;
        if (p > word) { /* "in 25 minutes", "for 10 min", "and" */
            unsigned n = (unsigned)(p - word);
            if (is(word, n, "in") || is(word, n, "for") || is(word, n, "and"))
                continue;
            return 0;
        }
        if (!digit(*p))
            return 0;
        uint64_t v = 0, tenths = 0;
        while (digit(*p) && v < 100000)
            v = v * 10 + (uint64_t)(*p++ - '0');
        if (digit(*p))
            return 0;
        if (*p == '.' && digit(p[1])) {
            tenths = (uint64_t)(p[1] - '0');
            p += 2;
            while (digit(*p))
                p++;
        }
        while (*p == ' ')
            p++;
        const char *u = p;
        while (alpha(*p))
            p++;
        unsigned un = (unsigned)(p - u);
        uint64_t ms;
        static const char *const hours[] = {"h", "hr", "hrs", "hour", "hours", 0};
        static const char *const mins[] = {"m", "min", "mins", "minute", "minutes", 0};
        static const char *const secs[] = {"s", "sec", "secs", "second", "seconds", 0};
        if (any_of(u, un, hours))
            ms = 3600000;
        else if (any_of(u, un, mins))
            ms = 60000;
        else if (any_of(u, un, secs))
            ms = 1000;
        else
            return 0;
        total += v * ms + tenths * ms / 10;
        parts++;
        if (total > 86400000ull)
            return 0;
    }
    return parts ? (uint32_t)total : 0;
}

size_t ck_scene_unwire(char *b, size_t n)
{
    size_t o = 0;
    for (size_t i = 0; i < n && b[i]; i++) {
        if (b[i] == '\\' && i + 1 < n && b[i + 1] == 'n')
            b[o++] = '\n', i++;
        else if (b[i] == '\\' && i + 1 < n && b[i + 1] == '\\')
            b[o++] = '\\', i++;
        else
            b[o++] = b[i];
    }
    if (o < n)
        b[o] = 0;
    return o;
}
