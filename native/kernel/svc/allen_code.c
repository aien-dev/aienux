/* allen_code.c -- Code from the Spark, the kernel side; see allen_code.h. */
#include "allen_code.h"

static int digit(char c) { return c >= '0' && c <= '9'; }

/* a decimal number that fits 32 bits (at most 10 digits), then p past it */
static int num(const char **p, uint32_t *v)
{
    const char *s = *p;
    uint64_t x = 0;
    unsigned k = 0;
    while (digit(*s) && k < 11)
        x = x * 10u + (uint64_t)(*s++ - '0'), k++;
    if (!k || k > 10 || x > 0xffffffffu)
        return 0;
    *v = (uint32_t)x, *p = s;
    return 1;
}

static int hex8(const char **p, uint32_t *v)
{
    const char *s = *p;
    uint32_t x = 0;
    for (unsigned k = 0; k < 8; k++, s++) {
        char c = *s;
        unsigned d = digit(c) ? (unsigned)(c - '0') : c >= 'a' && c <= 'f' ? (unsigned)(c - 'a' + 10) : 16u;
        if (d > 15)
            return 0;
        x = x << 4 | d;
    }
    *v = x, *p = s;
    return 1;
}

uint32_t ck_code_crc32(const void *p, size_t n)
{
    const uint8_t *b = p;
    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < n; i++) {
        c ^= b[i];
        for (int k = 0; k < 8; k++)
            c = c & 1u ? (c >> 1) ^ 0xedb88320u : c >> 1;
    }
    return ~c;
}

void ck_code_begin(ck_code_rx *rx, uint32_t seq)
{
    rx->seq = seq;
    rx->n = rx->bytes = rx->crc = rx->got = rx->refused = 0;
    rx->lang[0] = 0;
    for (unsigned i = 0; i < CK_CODE_PIECES; i++)
        rx->have[i] = 0;
    rx->text[0] = 0;
}

static int refuse(ck_code_rx *rx)
{
    rx->refused++;
    return -1;
}

int ck_code_take(ck_code_rx *rx, const char *line)
{
    if (!line || line[0] != 'C' || line[1] != 'O' || line[2] != 'D' || line[3] != 'E' || line[4] != ' ')
        return 0;
    const char *p = line + 5;
    uint32_t seq, i, n, bytes, crc;
    if (!num(&p, &seq) || *p++ != ' ' || !num(&p, &i) || *p++ != '/' || !num(&p, &n) || *p++ != ' ' ||
        !num(&p, &bytes) || *p++ != ' ' || !hex8(&p, &crc) || *p++ != ' ')
        return refuse(rx);
    if (seq != rx->seq || !bytes || bytes > CK_CODE_MAX || n != (bytes + CK_CODE_PIECE - 1) / CK_CODE_PIECE ||
        i < 1 || i > n)
        return refuse(rx);
    if (rx->n && (n != rx->n || bytes != rx->bytes || crc != rx->crc))
        return refuse(rx); /* the head must agree with the pieces before it */
    /* the language: one word of [a-z0-9+#._-], or "-" for none */
    char lang[CK_CODE_LANG];
    unsigned ln = 0;
    while (*p && *p != ' ') {
        char c = *p++;
        int ok = (c >= 'a' && c <= 'z') || digit(c) || c == '+' || c == '#' || c == '.' || c == '_' || c == '-';
        if (!ok || ln + 1 >= sizeof lang)
            return refuse(rx);
        lang[ln++] = c;
    }
    lang[ln] = 0;
    if (!ln || *p++ != ' ')
        return refuse(rx);
    if (ln == 1 && lang[0] == '-')
        lang[0] = 0;
    /* the piece, unescaped into place */
    uint32_t want = i < n ? CK_CODE_PIECE : bytes - (n - 1) * CK_CODE_PIECE;
    char piece[CK_CODE_PIECE];
    uint32_t k = 0;
    while (*p) {
        char c = *p++;
        if (c == '\\') {
            c = *p++;
            if (c == 'n')
                c = '\n';
            else if (c != '\\')
                return refuse(rx);
        } else if ((unsigned char)c < 32 || (unsigned char)c > 126) {
            return refuse(rx);
        }
        if (k >= want)
            return refuse(rx);
        piece[k++] = c;
    }
    if (k != want)
        return refuse(rx);
    if (!rx->n) {
        rx->n = n, rx->bytes = bytes, rx->crc = crc;
        for (unsigned j = 0; j <= ln; j++)
            rx->lang[j] = lang[j];
    }
    char *at = rx->text + (i - 1) * CK_CODE_PIECE;
    for (uint32_t j = 0; j < k; j++)
        at[j] = piece[j];
    if (!rx->have[i - 1]) {
        rx->have[i - 1] = 1;
        rx->got++;
    }
    return 1;
}

uint32_t ck_code_missing(const ck_code_rx *rx)
{
    if (!rx->n)
        return 1;
    for (uint32_t i = 0; i < rx->n; i++)
        if (!rx->have[i])
            return i + 1;
    return 0;
}

int ck_code_state(ck_code_rx *rx)
{
    if (!rx->n)
        return CK_CODE_NONE;
    if (rx->got < rx->n)
        return CK_CODE_PART;
    rx->text[rx->bytes] = 0;
    if (ck_code_crc32(rx->text, rx->bytes) != rx->crc)
        return CK_CODE_BAD;
    for (uint32_t i = 0; i < rx->bytes; i++)
        if (!rx->text[i]) /* a NUL can never be sent (the pieces are printable), but never trust the sum alone */
            return CK_CODE_BAD;
    return ck_code_lines(rx->text, rx->bytes) <= CK_CODE_LINES ? CK_CODE_WHOLE : CK_CODE_BAD;
}

uint32_t ck_code_lines(const char *text, uint32_t bytes)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < bytes; i++)
        n += text[i] == '\n';
    return n + (bytes && text[bytes - 1] != '\n');
}

uint32_t ck_code_rows(const char *text, uint32_t bytes, uint32_t cols, uint32_t *starts, uint16_t *lens,
                      uint16_t *line_of, uint32_t max)
{
    uint32_t rows = 0, line = 1, i = 0;
    if (!cols)
        return 0;
    while (i < bytes && rows < max) {
        uint32_t e = i;
        while (e < bytes && text[e] != '\n')
            e++;
        /* line [i, e) in rows of at most cols */
        uint32_t s = i;
        int first = 1;
        do {
            uint32_t take = e - s;
            if (take > cols) {
                take = cols;
                for (uint32_t j = cols; j > cols / 2; j--)
                    if (text[s + j - 1] == ' ') {
                        take = j;
                        break;
                    }
            }
            starts[rows] = s;
            lens[rows] = (uint16_t)take;
            line_of[rows] = first ? (uint16_t)(line < 65535u ? line : 65535u) : 0;
            rows++;
            first = 0;
            s += take;
        } while (s < e && rows < max);
        i = e + 1;
        line++;
    }
    return rows;
}

uint32_t ck_code_scroll(uint32_t top, int32_t delta, uint32_t rows, uint32_t page)
{
    int64_t t = (int64_t)top + delta, last = rows > page ? (int64_t)(rows - page) : 0;
    return (uint32_t)(t < 0 ? 0 : t > last ? last : t);
}

void ck_code_wait_begin(ck_code_wait *w, uint64_t now_ms)
{
    w->asked_ms = w->heard_ms = now_ms;
    w->slow_said = 0;
}

void ck_code_wait_heard(ck_code_wait *w, uint64_t now_ms) { w->heard_ms = now_ms; }

int ck_code_wait_tick(ck_code_wait *w, uint64_t now_ms)
{
    if (now_ms - w->heard_ms >= CK_CODE_GONE_MS)
        return CK_CODE_WAIT_GONE;
    if (!w->slow_said && now_ms - w->asked_ms >= CK_CODE_SLOW_MS) {
        w->slow_said = 1;
        return CK_CODE_WAIT_SLOW;
    }
    return CK_CODE_WAIT_NOTHING;
}
