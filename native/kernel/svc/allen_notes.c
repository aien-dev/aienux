/* allen_notes.c -- Allen's notes; see allen_notes.h. */
#include "allen_notes.h"

#include <stddef.h>

/* Small words that say nothing about what a note is about. */
static const char *const small_words[] = {
    "the", "an", "and", "or", "but", "is", "are", "was", "were", "be", "been", "am", "me", "my", "mine", "you",
    "your", "yours", "we", "our", "it", "its", "this", "that", "these", "those", "of", "in", "on", "at", "to",
    "for", "from", "with", "about", "as", "by", "what", "whats", "where", "when", "who", "which", "why", "how",
    "do", "does", "did", "have", "has", "had", "can", "could", "would", "should", "will", "there", "here", "then",
    "than", "so", "not", "no", "yes", "just", "also", "please", "show", "tell", "give", "find", "search", "look",
    "up", "get", "got", "any", "anything", "something", "thing", "things", "note", "notes", "saved", "save",
    "remember", "know", "all", "some", "again", "more", "ok", "okay", "hey", "hi", "hello", "allen", "want",
    "need", "let", "lets", "remind", "pull", "down", "write", "jot", "keep", 0};

static int same_word(const char *a, size_t n, const char *w)
{
    size_t i = 0;
    while (i < n && w[i] && a[i] == w[i])
        i++;
    return i == n && !w[i];
}

static int is_small(const char *w, size_t n)
{
    for (const char *const *p = small_words; *p; p++)
        if (same_word(w, n, *p))
            return 1;
    return 0;
}

static uint32_t hash(const char *w, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++)
        h = (h ^ (uint8_t)w[i]) * 16777619u;
    return h ? h : 1;
}

unsigned ck_allen_key_words(const char *text, uint32_t *word, unsigned max)
{
    unsigned k = 0;
    const char *s = text;
    while (*s && k < max) {
        char w[32];
        size_t n = 0;
        int digits = 1;
        /* one word: a run of letters and digits, lowercased; anything else
         * (apostrophes too: "what's" is "what" "s") ends it */
        while (*s && !((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9')))
            s++;
        while ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9')) {
            char c = *s++;
            if (c >= 'A' && c <= 'Z')
                c = (char)(c - 'A' + 'a');
            if (c < '0' || c > '9')
                digits = 0;
            if (n < sizeof w)
                w[n++] = c;
        }
        if (!n || n == sizeof w)
            continue;
        if (!digits && (n < 3 || is_small(w, n)))
            continue;
        if (!digits && n > 3 && w[n - 1] == 's' && w[n - 2] != 's') /* "codes" is "code" */
            n--;
        uint32_t h = hash(w, n);
        unsigned dup = 0;
        for (unsigned i = 0; i < k; i++)
            dup |= word[i] == h;
        if (!dup)
            word[k++] = h;
    }
    return k;
}

void ck_allen_note_index(ck_allen_note *n)
{
    n->nword = (uint8_t)ck_allen_key_words(n->text, n->word, CK_ALLEN_NOTE_WORDS);
}

int ck_allen_note_set(ck_allen_note *n, const char *text, uint32_t start)
{
    while (*text == ' ')
        text++;
    size_t len = 0;
    while (text[len]) {
        if ((uint8_t)text[len] < 0x20 || (uint8_t)text[len] > 0x7e)
            return -1;
        len++;
    }
    while (len && text[len - 1] == ' ')
        len--;
    if (!len || len >= CK_ALLEN_NOTE_MAX)
        return -1;
    char *d = (char *)n;
    for (size_t i = 0; i < sizeof *n; i++)
        d[i] = 0;
    for (size_t i = 0; i < len; i++)
        n->text[i] = text[i];
    n->len = (uint16_t)len;
    n->start = start;
    ck_allen_note_index(n);
    return 0;
}

unsigned ck_allen_note_links(const ck_allen_note *a, const ck_allen_note *b)
{
    unsigned k = 0;
    for (unsigned i = 0; i < a->nword; i++)
        for (unsigned j = 0; j < b->nword; j++)
            k += a->word[i] == b->word[j];
    return k;
}

unsigned ck_allen_note_degree(const ck_allen_notes *ns, uint32_t i)
{
    unsigned d = 0;
    for (uint32_t j = 0; j < ns->n; j++)
        d += j != i && !ns->note[j].gone && ck_allen_note_links(&ns->note[i], &ns->note[j]) > 0;
    return d;
}

unsigned ck_allen_notes_live(const ck_allen_notes *ns)
{
    unsigned k = 0;
    for (uint32_t i = 0; i < ns->n; i++)
        k += !ns->note[i].gone;
    return k;
}

int ck_allen_notes_best(const ck_allen_notes *ns, const char *question, int strict, unsigned *also)
{
    uint32_t q[CK_ALLEN_NOTE_WORDS];
    unsigned nq = ck_allen_key_words(question, q, CK_ALLEN_NOTE_WORDS), best_score = 0, count = 0;
    int best = -1;
    if (also)
        *also = 0;
    if (!ns || !nq)
        return -1;
    for (uint32_t i = 0; i < ns->n; i++) {
        const ck_allen_note *n = &ns->note[i];
        if (n->gone)
            continue;
        unsigned score = 0;
        for (unsigned a = 0; a < nq; a++)
            for (unsigned b = 0; b < n->nword; b++)
                if (q[a] == n->word[b]) {
                    score++;
                    break;
                }
        if (!score || (strict ? score < nq : score * 2 < nq))
            continue;
        count++;
        if (score >= best_score) { /* >=: the newest wins a tie */
            best_score = score;
            best = (int)i;
        }
    }
    if (also && count)
        *also = count - 1;
    return best;
}
