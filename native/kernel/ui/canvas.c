/* canvas.c -- Allen's canvas, the light side; see canvas.h and docs/ALLEN_CANVAS.md. */
#include "canvas.h"

#define S(v) ((int32_t)(((int64_t)(v) * (int64_t)c->k + 128) / 256))

/* The palette (docs/ALLEN_PRESENCE_RESEARCH.md, ui/allen_ui.c): nothing else is drawn. */
#define C_NIGHT CK_RGB(6, 8, 18)
#define C_TEXT CK_RGB(236, 240, 252)
#define C_HINT CK_RGB(150, 160, 196)
#define C_GLOW CK_RGB(150, 170, 255)
#define C_MIST CK_RGB(200, 210, 255)
#define C_INK CK_RGB(64, 86, 196)
#define C_WARM CK_RGB(255, 212, 150)
#define C_WARM2 CK_RGB(255, 222, 180)

#define OPEN_MS 450u  /* a bloom: the spark's flight before the thing opens */
#define SPARK_MS 650u
#define COME_MS 700u  /* each line eases in over this */
#define ROW_MS 70u    /* and the next line starts this much later */
#define GO_MS 700u    /* leaving, and the sky's change of tone */
#define BETWEEN_MS 260u /* one thing after another */

/* The large digits: one pool, rasterized whole at start so drawing never writes to it. */
static uint8_t big_pool[384u << 10];
static const char big_set[] = "0123456789.,:%$+-/x\"";

static int is_big(char ch)
{
    for (const char *p = big_set; *p; p++)
        if (*p == ch)
            return 1;
    return 0;
}

static unsigned len_of(const char *s)
{
    unsigned n = 0;
    while (s[n])
        n++;
    return n;
}

static uint32_t role_rgb(unsigned role)
{
    static const uint32_t rgb[4] = {C_TEXT, C_MIST, C_HINT, C_WARM2};
    return rgb[role & 3];
}

static ck_af_face *face_of(const ck_allen_canvas *c, unsigned f)
{
    switch (f) {
    case 0:
        return c->voice;
    case 1:
        return c->body;
    case 2:
        return c->small;
    default:
        return (ck_af_face *)&c->big; /* drawing reads only: every glyph it may meet was made at start */
    }
}

static int32_t cap_of(const ck_af_face *f) { return (int32_t)(f->s256 * 20u / 256u); }

int ck_allen_canvas_init(ck_allen_canvas *c, int32_t w, int32_t h, uint32_t k, int32_t cx, int32_t cy,
                         ck_af_face *voice, ck_af_face *body, ck_af_face *small)
{
    for (size_t i = 0; i < sizeof *c; i++)
        ((char *)c)[i] = 0;
    c->w = w, c->h = h, c->k = k, c->cx = cx, c->cy = cy;
    c->voice = voice, c->body = body, c->small = small;
    unsigned cap = 60u * k / 256u;
    for (;;) {
        ck_af_face_init(&c->big, cap, 700, big_pool, sizeof big_pool);
        int ok = 1;
        for (const char *p = big_set; *p && ok; p++)
            ok = ck_af_glyph_get(&c->big, *p) != 0;
        if (ok)
            return 0;
        if (cap < 20)
            return -1;
        cap = cap * 9 / 10;
    }
}

/* ---- laying a thing out ---- */

static int put(ck_canvas_item *it, const char *s, unsigned n)
{
    if ((unsigned)it->used + n + 1 > CK_CANVAS_TEXT)
        return -1;
    int at = it->used;
    for (unsigned i = 0; i < n; i++)
        it->text[it->used++] = s[i];
    it->text[it->used++] = 0;
    return at;
}

static void row(ck_canvas_item *it, int off, unsigned len, int32_t dx, int32_t dy, unsigned face, unsigned role,
                unsigned dot)
{
    if (off < 0 || !len || it->nrows >= CK_CANVAS_ROWS)
        return;
    it->row[it->nrows++] = (ck_canvas_row){(uint16_t)off, (uint16_t)len, dx, dy, (uint8_t)face, (uint8_t)role,
                                           (uint8_t)dot};
}

/* Field text at off (len letters) wrapped to width with face f, at most max lines, from top; each line's
 * start x is indent, or centred in width when centre is set. The next line's top is returned. A text that
 * does not fit ends with "...". */
static int32_t wrapped(const ck_allen_canvas *c, ck_canvas_item *it, int off, unsigned face, unsigned role,
                       int32_t indent, int32_t width, int32_t top, int32_t lh, unsigned max, int centre, unsigned dot)
{
    if (off < 0)
        return top;
    ck_af_face *f = face_of(c, face);
    const char *s = it->text + off;
    unsigned n = len_of(s), i = 0, lines = 0;
    int32_t cap = cap_of(f);
    while (i < n && lines < max && it->nrows < CK_CANVAS_ROWS) {
        while (i < n && s[i] == ' ')
            i++;
        if (i >= n)
            break;
        unsigned start = i, last_space = 0, j = i;
        int32_t wpx = 0;
        while (j < n) {
            int32_t aw = (int32_t)((ck_af_advance(f, s[j]) + 255) / 256);
            if (wpx + aw > width && j > start)
                break;
            if (s[j] == ' ')
                last_space = j;
            wpx += aw;
            j++;
        }
        if (j < n && last_space > start)
            j = last_space;
        unsigned ln = j - start;
        int lo = off + (int)start;
        if (j < n && (lines + 1 == max || it->nrows + 1 == CK_CANVAS_ROWS)) { /* the last line there is room for */
            char tmp[CK_CANVAS_TEXT];
            unsigned m = ln;
            while (m && (int32_t)ck_af_measure(f, s + start, m) + (int32_t)ck_af_measure(f, "...", 3) > width)
                m--;
            while (m && s[start + m - 1] == ' ')
                m--;
            for (unsigned q = 0; q < m; q++)
                tmp[q] = s[start + q];
            tmp[m] = '.', tmp[m + 1] = '.', tmp[m + 2] = '.';
            lo = put(it, tmp, m + 3);
            ln = m + 3;
            j = n;
        }
        int32_t dx = indent;
        if (centre && lo >= 0)
            dx = indent + (width - (int32_t)ck_af_measure(f, it->text + lo, ln)) / 2;
        row(it, lo, ln, dx, top + cap, face, role, dot && lines == 0);
        top += lh;
        lines++;
        i = j;
    }
    return top;
}

static int field(ck_canvas_item *it, unsigned i) { return i < it->nf ? (int)it->f[i] : -1; }

/* Rows and shapes for a thing of width W; its height. */
static int32_t lay_out(const ck_allen_canvas *c, ck_canvas_item *it, int32_t W)
{
    int32_t y = 0, ind = S(26);
    unsigned lead = it->warm ? 3u : 0u;
    it->nrows = 0, it->rr = 0, it->used = it->base; /* what an earlier lay-out added goes */
    switch (it->kind) {
    case CK_THING_NOTE: {
        int title = it->nf >= 2 ? field(it, 0) : -1, body = it->nf >= 2 ? field(it, 1) : field(it, 0);
        if (title >= 0)
            y = wrapped(c, it, title, 0, lead, ind, W - ind, y, S(40), 2, 0, 0) + S(6);
        y = wrapped(c, it, body, 1, 1, ind, W - ind, y, S(33), 6, 0, 0);
        break;
    }
    case CK_THING_LIST: {
        unsigned first = 0;
        if (it->nf >= 2) {
            y = wrapped(c, it, field(it, 0), 1, it->warm ? 3u : 2u, ind, W - ind, y, S(32), 1, 0, 0) + S(6);
            first = 1;
        }
        for (unsigned i = first; i < it->nf; i++)
            y = wrapped(c, it, field(it, i), 1, 0, ind + S(24), W - ind - S(24), y, S(37), 2, 0, 1);
        break;
    }
    case CK_THING_NUMBER: {
        const char *v = it->text + it->f[0];
        unsigned n = len_of(v), b = 0;
        while (b < n && (is_big(v[b]) || (v[b] == ' ' && b + 1 < n && is_big(v[b + 1]) && v[b + 1] != '"')))
            b++;
        while (b && v[b - 1] == ' ')
            b--;
        unsigned u = b;
        while (u < n && v[u] == ' ')
            u++;
        int32_t bw = b ? (int32_t)ck_af_measure(&c->big, v, b) : 0;
        int32_t uw = u < n ? (int32_t)ck_af_measure(c->voice, v + u, n - u) : 0;
        int32_t gap = b && u < n ? S(10) : 0, x0 = (W - bw - gap - uw) / 2, bc = cap_of(&c->big);
        if (b)
            row(it, it->f[0], b, x0, bc, 3, lead, 0);
        if (u < n)
            row(it, it->f[0] + (int)u, n - u, x0 + bw + gap, b ? bc : cap_of(c->voice), 0, b ? 2u : lead, 0);
        y = (b ? bc : cap_of(c->voice)) + S(26);
        if (it->nf >= 2)
            y = wrapped(c, it, field(it, 1), 1, 2, 0, W, y, S(31), 2, 1, 0);
        it->rx = W / 2, it->ry = bc / 2; /* where the digits' glow sits */
        break;
    }
    case CK_THING_QUOTE: {
        y = wrapped(c, it, field(it, 0), 0, 0, ind, W - ind, 0, S(40), 5, 0, 0);
        if (it->nf >= 2)
            y = wrapped(c, it, field(it, 1), 1, 2, ind + S(30), W - ind - S(30), y + S(6), S(32), 1, 0, 0);
        break;
    }
    case CK_THING_PERSON: {
        it->rx = S(42), it->ry = S(46), it->rr = S(38);
        const char *name = it->text + it->f[0];
        char ini = name[0] >= 'a' && name[0] <= 'z' ? (char)(name[0] - 32) : name[0];
        int io = put(it, &ini, 1);
        row(it, io, 1, it->rx - (int32_t)ck_af_measure(c->voice, &ini, 1) / 2, it->ry + cap_of(c->voice) / 2, 0, 3, 0);
        int32_t nx = S(104), top = S(4);
        if (it->way) {
            const char *wd = it->way == CK_WAY_TO ? "a message for" : "a message from";
            top = wrapped(c, it, put(it, wd, len_of(wd)), 2, 2, nx, W - nx, top, S(28), 1, 0, 0);
        }
        top = wrapped(c, it, field(it, 0), 0, lead, nx, W - nx, top, S(40), 1, 0, 0) + S(4);
        y = wrapped(c, it, field(it, 1), 1, 1, nx, W - nx, top, S(33), 4, 0, 0);
        y = ck_max(y, it->ry + it->rr + S(10));
        break;
    }
    case CK_THING_TIMER: {
        it->rx = W / 2, it->ry = S(66), it->rr = S(62);
        const char *when = it->text + it->f[0];
        it->dur = ck_scene_duration_ms(when);
        y = it->ry + it->rr + S(22);
        int label = it->nf >= 2 ? field(it, 1) : -1;
        if (!it->dur && (int32_t)ck_af_measure(c->voice, when, len_of(when)) <= it->rr * 17 / 10) {
            int32_t ww = (int32_t)ck_af_measure(c->voice, when, len_of(when));
            row(it, it->f[0], len_of(when), it->rx - ww / 2, it->ry + cap_of(c->voice) / 2, 0, lead, 0);
        } else if (!it->dur) { /* too long for the ring: under it, and the label after */
            y = wrapped(c, it, it->f[0], 0, lead, 0, W, y, S(38), 2, 1, 0);
        }
        if (label >= 0)
            y = wrapped(c, it, label, 1, 0, 0, W, y, S(33), 2, 1, 0);
        break;
    }
    default: { /* a picture or a video, still on its way */
        it->rx = 0, it->ry = 0, it->rr = W * 9 / 16;
        y = wrapped(c, it, field(it, 0), 1, 0, S(6), W - S(6), it->rr + S(22), S(33), 2, 0, 0);
        if (it->nf >= 2)
            y = wrapped(c, it, field(it, 1), 2, 2, S(6), W - S(6), y, S(28), 1, 0, 0);
        break;
    }
    }
    return y;
}

/* ---- arriving, staying, leaving ---- */

static uint32_t opens(const ck_canvas_item *it) { return it->born + (it->arrive == CK_ARRIVE_BLOOM ? OPEN_MS : 0u); }
static uint32_t come_ms(const ck_canvas_item *it) { return it->arrive == CK_ARRIVE_RISE ? 950u : COME_MS; }

/* how far line j has come in at t, 0..256, eased */
static uint32_t come(const ck_canvas_item *it, unsigned j, uint32_t t)
{
    uint32_t o = opens(it) + j * ROW_MS;
    return ck_ease_out(ck_prog(t, o, o + come_ms(it)));
}

static uint32_t stays(const ck_canvas_item *it, uint32_t t)
{
    uint32_t keep = it->gone ? 256u - ck_smooth(ck_prog(t, it->gone, it->gone + GO_MS)) : 256u;
    return it->quiet ? keep * 176u >> 8 : keep;
}

static int32_t lift(const ck_allen_canvas *c, const ck_canvas_item *it, uint32_t e)
{
    if (it->arrive == CK_ARRIVE_FADE)
        return 0;
    return (int32_t)((int64_t)S(it->arrive == CK_ARRIVE_RISE ? 24 : 10) * (256 - e) >> 8);
}

static uint32_t arrived_by(const ck_canvas_item *it) { return opens(it) + (it->nrows + 1u) * ROW_MS + come_ms(it) + 40u; }

static int timer_runs(const ck_canvas_item *it, uint32_t t)
{
    return it->kind == CK_THING_TIMER && it->dur && t < opens(it) + it->dur + 10000u;
}

/* where the spark is at t (16ths of a pixel), k steps back along its trail */
static void spark_at(const ck_allen_canvas *c, const ck_canvas_item *it, int32_t p, int32_t *sx, int32_t *sy)
{
    uint32_t e = ck_smooth(ck_clamp(p, 0, 256));
    *sx = c->hx + (int32_t)((int64_t)(it->ax - c->hx) * e >> 8);
    *sy = c->hy + (int32_t)((int64_t)(it->ay - c->hy) * e >> 8) - (int32_t)((int64_t)S(70) * 16 * (e * (256 - e) >> 6) >> 8);
}

/* the part of the screen the spark lights at t: its head and trail with the halo round them */
static ck_rect spark_box(const ck_allen_canvas *c, const ck_canvas_item *it, uint32_t t)
{
    if (t < it->born || t >= it->born + SPARK_MS + 40)
        return (ck_rect){0, 0, 0, 0};
    int32_t p = ck_prog(t, it->born, it->born + SPARK_MS), x0, y0, x1, y1, m = S(36) + 2;
    spark_at(c, it, p, &x0, &y0);
    spark_at(c, it, p - 5 * 14, &x1, &y1);
    ck_rect a = {ck_min(x0, x1) / 16 - m, ck_min(y0, y1) / 16 - m, 0, 0};
    a.w = ck_max(x0, x1) / 16 + m - a.x, a.h = ck_max(y0, y1) / 16 + m - a.y;
    return a;
}

static ck_rect ring_box(const ck_allen_canvas *c, const ck_canvas_item *it)
{
    int32_t m = it->rr + S(14);
    return (ck_rect){it->x + it->rx - m, it->y + it->ry - m, 2 * m, 2 * m};
}

static ck_rect field_box(const ck_canvas_item *it) { return (ck_rect){it->x, it->y, it->w, it->rr}; }

unsigned ck_allen_canvas_count(const ck_allen_canvas *c)
{
    unsigned n = 0;
    for (unsigned i = 0; i < CK_CANVAS_SLOTS; i++)
        n += c->item[i].live && !c->item[i].gone;
    return n;
}

void ck_allen_canvas_clear(ck_allen_canvas *c, uint32_t t)
{
    for (unsigned i = 0; i < CK_CANVAS_SLOTS; i++)
        if (c->item[i].live && !c->item[i].gone)
            c->item[i].gone = t ? t : 1;
}

/* where things go: two columns beside Allen and one place above him */
enum { COL_LEFT, COL_RIGHT, COL_ABOVE };

void ck_allen_canvas_show(ck_allen_canvas *c, const ck_scene *s, uint32_t t)
{
    ck_allen_canvas_clear(c, t);
    uint8_t from = c->tone_w >= 128 ? c->tone_to : c->tone_from;
    c->tone_from = from, c->tone_to = s ? s->tone : 0, c->tone_t = t ? t : 1, c->tone_w = 0;
    c->water = s ? s->water : 0;
    if (!s)
        return;
    int32_t colw = S(440), abovew = S(600), top_min = c->cy - S(362), bot_max = c->cy + S(210);
    int32_t col_h[3] = {0, 0, 0};
    unsigned col_n[3] = {0, 0, 0}, mine[CK_SCENE_THINGS] = {0}, col_of[CK_SCENE_THINGS] = {0}, n = 0;
    for (unsigned i = 0; i < s->nthings && i < CK_SCENE_THINGS; i++) {
        const ck_scene_thing *th = &s->thing[i];
        /* a slot: a free one, else the one furthest gone */
        unsigned slot = CK_CANVAS_SLOTS;
        for (unsigned j = 0; j < CK_CANVAS_SLOTS && slot == CK_CANVAS_SLOTS; j++)
            if (!c->item[j].live)
                slot = j;
        for (unsigned j = 0; j < CK_CANVAS_SLOTS && slot == CK_CANVAS_SLOTS; j++) {
            int taken = 0;
            for (unsigned q = 0; q < n; q++)
                taken |= mine[q] == j;
            if (!taken && (slot == CK_CANVAS_SLOTS || c->item[j].gone < c->item[slot].gone))
                slot = j;
        }
        if (slot == CK_CANVAS_SLOTS)
            break;
        ck_canvas_item *it = &c->item[slot];
        for (size_t b = 0; b < sizeof *it; b++)
            ((char *)it)[b] = 0;
        it->kind = th->kind, it->warm = th->warm, it->quiet = th->quiet, it->arrive = th->arrive, it->way = th->way;
        for (unsigned f = 0; f < th->nf && f < CK_SCENE_FIELDS; f++) {
            const char *v = ck_scene_field(s, th, f);
            int at = put(it, v, len_of(v));
            if (at < 0)
                break;
            it->f[it->nf++] = (uint16_t)at;
        }
        if (!it->nf)
            continue;
        it->base = it->used;
        /* its place: asked for if there is room, else wherever there is */
        int32_t hw = lay_out(c, it, colw), ha = 0;
        int ok[3];
        ok[COL_LEFT] = col_n[COL_LEFT] < 2 && col_h[COL_LEFT] + (col_n[COL_LEFT] ? S(56) : 0) + hw <= bot_max - top_min;
        ok[COL_RIGHT] = col_n[COL_RIGHT] < 2 && col_h[COL_RIGHT] + (col_n[COL_RIGHT] ? S(56) : 0) + hw <= bot_max - top_min;
        ok[COL_ABOVE] = 0;
        if (!col_n[COL_ABOVE] && (th->place == CK_PLACE_ABOVE || (!ok[COL_LEFT] && !ok[COL_RIGHT]))) {
            ha = lay_out(c, it, abovew);
            ok[COL_ABOVE] = ha <= S(206);
        }
        int col = -1;
        if (th->place == CK_PLACE_ABOVE && ok[COL_ABOVE])
            col = COL_ABOVE;
        else if (th->place == CK_PLACE_LEFT && ok[COL_LEFT])
            col = COL_LEFT;
        else if (th->place == CK_PLACE_RIGHT && ok[COL_RIGHT])
            col = COL_RIGHT;
        else if (ok[COL_RIGHT] && (!ok[COL_LEFT] || col_h[COL_RIGHT] <= col_h[COL_LEFT]))
            col = COL_RIGHT;
        else if (ok[COL_LEFT])
            col = COL_LEFT;
        else if (ok[COL_ABOVE])
            col = COL_ABOVE;
        if (col < 0) {
            c->dropped++;
            continue;
        }
        if (col != COL_ABOVE && ha) /* laid out for above, going to a column after all */
            lay_out(c, it, colw);
        else if (col == COL_ABOVE && !ha)
            ha = lay_out(c, it, abovew);
        it->w = col == COL_ABOVE ? abovew : colw;
        it->h = col == COL_ABOVE ? ha : hw;
        it->y = col_h[col] + (col_n[col] ? S(56) : 0); /* the offset within its column, for now */
        col_h[col] = it->y + it->h;
        col_n[col]++;
        it->live = 1;
        it->born = t + 120u + n * BETWEEN_MS;
        mine[n] = slot, col_of[n] = (unsigned)col, n++;
        c->shown++;
    }
    /* the columns stand centred on Allen's height; the place above, near the top of the sky */
    for (unsigned q = 0; q < n; q++) {
        ck_canvas_item *it = &c->item[mine[q]];
        unsigned col = col_of[q];
        if (col == COL_ABOVE) {
            it->x = c->cx - it->w / 2;
            it->y = c->cy - S(366) + (S(206) - it->h) / 2;
        } else {
            int32_t top = ck_clamp(c->cy - col_h[col] / 2, top_min, bot_max - col_h[col]);
            it->x = col == COL_LEFT ? c->cx - S(770) : c->cx + S(330);
            it->y += top;
        }
        it->ax = (it->x + (it->rr && it->kind != CK_THING_PICTURE && it->kind != CK_THING_VIDEO ? it->rx : S(2))) * 16;
        it->ay = (it->y + (it->rr ? (it->kind == CK_THING_PICTURE || it->kind == CK_THING_VIDEO ? it->rr / 2 : it->ry)
                                  : it->h / 2)) * 16;
        if (it->kind == CK_THING_PICTURE || it->kind == CK_THING_VIDEO)
            it->ax = (it->x + it->w / 2) * 16;
        it->box = (ck_rect){it->x - S(124), it->y - S(132), it->w + S(248), it->h + S(256)};
    }
}

void ck_allen_canvas_at(ck_allen_canvas *c, uint32_t t, int32_t hx, int32_t hy, ck_flow *flow, void (*mark)(ck_rect))
{
    c->t = t, c->hx = hx, c->hy = hy;
    if (c->tone_t) {
        c->tone_w = ck_smooth(ck_prog(t, c->tone_t, c->tone_t + GO_MS));
        if (t <= c->tone_t + GO_MS + 40 && c->tone_from != c->tone_to)
            mark((ck_rect){0, 0, c->w, c->h});
    }
    if (flow && c->water == CK_WATER_LIVELY && t - c->stir_t >= 1400u) {
        /* lively water: a gentle current pushed round Allen now and then */
        c->stir_t = t;
        uint32_t ang = ck_hash((int32_t)(t / 1400u), 7, 11) & 0xffffu;
        int32_t r = S(330), px = c->cx + (int32_t)((int64_t)r * ck_cos(ang) >> 14),
                py = c->cy + (int32_t)((int64_t)r * ck_sin(ang) >> 14);
        int32_t vx = (int32_t)((int64_t)-90 * ck_sin(ang) >> 14), vy = (int32_t)((int64_t)90 * ck_cos(ang) >> 14);
        ck_flow_touch(flow, px, py, vx, vy, 1500, S(70));
    }
    for (unsigned i = 0; i < CK_CANVAS_SLOTS; i++) {
        ck_canvas_item *it = &c->item[i];
        if (!it->live)
            continue;
        if (!it->gone && t > it->born + CK_CANVAS_HOLD_MS && !timer_runs(it, t))
            it->gone = t;
        if (it->gone && t >= it->gone + GO_MS) {
            mark(it->box);
            it->live = 0;
            continue;
        }
        if ((t + 40 >= it->born && t <= opens(it) + come_ms(it) + 40) || it->gone)
            mark(it->box); /* its shade and shapes are still easing in, or it is leaving: all of it */
        else if (t + 40 >= it->born && t <= arrived_by(it))
            for (unsigned j = 1; j < it->nrows; j++) { /* the shade has settled: only the lines still coming */
                uint32_t o = opens(it) + j * ROW_MS;
                if (t + 40 < o || t > o + come_ms(it) + 40)
                    continue;
                const ck_canvas_row *rw = &it->row[j];
                int32_t cap = cap_of(face_of(c, rw->face)), up = cap + S(26) + lift(c, it, 0);
                mark((ck_rect){it->x - S(6), it->y + rw->dy - up, it->w + S(12), up + cap / 2 + S(12)});
            }
        if (it->arrive == CK_ARRIVE_BLOOM && t + 40 >= it->born && t <= it->born + SPARK_MS + 80) {
            ck_rect sb = spark_box(c, it, t); /* only where the spark is and was: the sky between is left alone */
            mark(sb);
            mark(it->spark_was);
            it->spark_was = sb;
        }
        if (flow && it->arrive == CK_ARRIVE_BLOOM && !it->landed && t >= it->born + SPARK_MS) {
            it->landed = 1; /* the spark lands on the water */
            ck_flow_touch(flow, it->ax / 16, it->ay / 16, 0, -50, 9000, S(36));
        }
        uint32_t step = it->kind == CK_THING_TIMER ? 250u : it->rr && it->kind != CK_THING_PERSON ? 66u : 0u;
        if (step) {
            uint32_t tq = t - t % step;
            if (tq != it->tq) {
                it->tq = tq;
                mark(it->kind == CK_THING_TIMER ? ring_box(c, it) : field_box(it));
            }
        }
    }
}

/* ---- drawing ---- */

/* the sky's tone at a row: added colour and its weight, and how much of the sky is kept */
static void tone_row(unsigned tone, int32_t y, int32_t h, uint32_t *rgb, uint32_t *a, uint32_t *keep)
{
    *rgb = 0, *a = 0, *keep = 256;
    if (tone == CK_TONE_DAWN) { /* a warm light low on the horizon */
        uint32_t s = ck_smooth(ck_prog((uint32_t)y, (uint32_t)(h * 40 / 100), (uint32_t)h));
        *rgb = C_WARM, *a = 40u * (s * s >> 8) >> 8;
    } else if (tone == CK_TONE_CLEAR) { /* a cool lift high in the sky */
        *rgb = C_MIST, *a = 22u * (256u - ck_smooth(ck_prog((uint32_t)y, 0, (uint32_t)(h * 62 / 100)))) >> 8;
    } else if (tone == CK_TONE_DEEP) { /* the night deeper and quieter */
        *keep = 200;
    }
}

static void draw_tone(const ck_allen_canvas *c, ck_canvas *cv, const ck_rect *r)
{
    if (c->tone_from == CK_TONE_NIGHT && c->tone_to == CK_TONE_NIGHT)
        return;
    uint32_t w = c->tone_t ? c->tone_w : 256;
    for (int32_t y = r->y; y < r->y + r->h; y++) {
        uint32_t c0, a0, k0, c1, a1, k1;
        tone_row(c->tone_from, y, c->h, &c0, &a0, &k0);
        tone_row(c->tone_to, y, c->h, &c1, &a1, &k1);
        a0 = a0 * (256 - w) >> 8, a1 = a1 * w >> 8;
        uint32_t keep = (k0 * (256 - w) + k1 * w) >> 8;
        if (!a0 && !a1 && keep >= 256)
            continue;
        uint32_t *px = cv->px + (size_t)y * (size_t)cv->w;
        for (int32_t x = r->x; x < r->x + r->w; x++) {
            uint32_t v = keep < 256 ? ck_scale(px[x], keep) : px[x];
            if (a0)
                v = ck_add(v, c0, a0);
            if (a1)
                v = ck_add(v, c1, a1);
            px[x] = v;
        }
    }
}

/* The shade words rest in: an oval over the rectangle a, full across most of it and easing to nothing at its
 * rim, so it never shows a straight edge even over the bright band of the sky. */
static void shade(ck_canvas *cv, const ck_rect *r, ck_rect a, uint32_t amt)
{
    ck_rect d = a, all = {0, 0, cv->w, cv->h};
    if (!amt || a.w < 4 || a.h < 4 || !ck_clip(&d, r) || !ck_clip(&d, &all))
        return;
    int64_t hx = a.w / 2, hy = a.h / 2, cx = a.x + hx, cy = a.y + hy;
    int64_t ix = ((int64_t)1 << 32) / (hx * hx), iy = ((int64_t)1 << 32) / (hy * hy);
    const int64_t inner = 30000, inv = (256 << 16) / (65536 - inner); /* the rim eases over the last part */
    for (int32_t y = d.y; y < d.y + d.h; y++) {
        int64_t dy = y - cy, qy = dy * dy * iy >> 16;
        if (qy >= 65536)
            continue;
        /* only the span of this row inside the oval */
        int64_t lim = (int64_t)ck_isqrt((uint64_t)(((65536 - qy) << 16) / ix)) + 1;
        int32_t x0 = (int32_t)ck_max(d.x, (int32_t)(cx - lim)), x1 = (int32_t)ck_min(d.x + d.w, (int32_t)(cx + lim + 1));
        uint32_t *px = cv->px + (size_t)y * (size_t)cv->w;
        for (int32_t x = x0; x < x1; x++) {
            int64_t dx = x - cx, q = qy + (dx * dx * ix >> 16); /* 65536 at the rim */
            if (q >= 65536)
                continue;
            if (q <= inner) {
                px[x] = ck_mix(px[x], C_NIGHT, amt);
                continue;
            }
            uint32_t k = (256 - ck_smooth((int32_t)((q - inner) * inv >> 16))) * amt >> 8;
            if (k)
                px[x] = ck_mix(px[x], C_NIGHT, k);
        }
    }
}

/* A soft oval of light filling the rectangle a (no edge: it fades to nothing at the rim, (1 - d^2)^2). */
static void soft_oval(ck_canvas *cv, const ck_rect *r, ck_rect a, uint32_t color, uint32_t amt)
{
    ck_rect d = a, all = {0, 0, cv->w, cv->h};
    if (!amt || a.w < 4 || a.h < 4 || !ck_clip(&d, r) || !ck_clip(&d, &all))
        return;
    int64_t hx = a.w / 2, hy = a.h / 2, cx = a.x + hx, cy = a.y + hy;
    int64_t ix = ((int64_t)1 << 32) / (hx * hx), iy = ((int64_t)1 << 32) / (hy * hy); /* 1/half^2, 2^32 */
    for (int32_t y = d.y; y < d.y + d.h; y++) {
        int64_t dy = y - cy, qy = dy * dy * iy >> 16;
        if (qy >= 65536)
            continue;
        uint32_t *px = cv->px + (size_t)y * (size_t)cv->w;
        for (int32_t x = d.x; x < d.x + d.w; x++) {
            int64_t dx = x - cx, q = qy + (dx * dx * ix >> 16);
            if (q >= 65536)
                continue;
            uint32_t f = (uint32_t)(65536 - q) >> 8; /* 0..256 */
            uint32_t k = (f * f >> 8) * amt >> 8;
            if (k)
                px[x] = ck_add(px[x], color, k);
        }
    }
}

/* A ring of light: centre (cx, cy) px, radius rad px, half its width hw (16ths of a px); the part from the
 * top clockwise to `upto` (65536ths of a turn) at a_on, the rest at a_off. */
static void ring(ck_canvas *cv, const ck_rect *r, int32_t cx, int32_t cy, int32_t rad, int32_t hw, uint32_t color,
                 uint32_t on_color, uint32_t a_on, uint32_t a_off, uint32_t upto)
{
    ck_rect d = {cx - rad - 2, cy - rad - 2, 2 * rad + 5, 2 * rad + 5}, all = {0, 0, cv->w, cv->h};
    if (!ck_clip(&d, r) || !ck_clip(&d, &all))
        return;
    int64_t R = (int64_t)rad * 16, lo = R - hw - 16, hi = R + hw + 16;
    lo = lo < 0 ? 0 : lo;
    for (int32_t y = d.y; y < d.y + d.h; y++) {
        int64_t dy = (int64_t)(y - cy) * 16;
        uint32_t *px = cv->px + (size_t)y * (size_t)cv->w;
        for (int32_t x = d.x; x < d.x + d.w; x++) {
            int64_t dx = (int64_t)(x - cx) * 16, d2 = dx * dx + dy * dy;
            if (d2 < lo * lo || d2 > hi * hi)
                continue;
            int64_t dist = (int64_t)ck_isqrt((uint64_t)d2), e = dist > R ? dist - R : R - dist;
            int64_t cov = hw + 8 - e;
            if (cov <= 0)
                continue;
            uint32_t cv16 = cov >= 16 ? 256u : (uint32_t)cov * 16u;
            int lit = upto >= 65536u || ck_atan2((int32_t)dx, (int32_t)-dy) < upto;
            uint32_t a = (lit ? a_on : a_off) * cv16 >> 8;
            if (a)
                px[x] = ck_add(px[x], lit ? on_color : color, a);
        }
    }
}

static unsigned put_2(char *s, uint32_t v)
{
    s[0] = (char)('0' + v / 10 % 10), s[1] = (char)('0' + v % 10);
    return 2;
}

/* a running timer's time left at tq, as "m:ss" or "h:mm:ss" */
static unsigned clock_text(const ck_canvas_item *it, char *s, uint32_t *left)
{
    uint32_t start = opens(it), now = it->tq > start ? it->tq - start : 0;
    *left = now >= it->dur ? 0 : it->dur - now;
    uint32_t sec = (*left + 999) / 1000, n = 0;
    if (!sec) {
        s[0] = 'n', s[1] = 'o', s[2] = 'w';
        return 3;
    }
    if (sec >= 3600) {
        uint32_t hh = sec / 3600;
        if (hh >= 10)
            s[n++] = (char)('0' + hh / 10);
        s[n++] = (char)('0' + hh % 10);
        s[n++] = ':';
        n += put_2(s + n, sec / 60 % 60);
    } else {
        uint32_t mm = sec / 60;
        if (mm >= 10)
            s[n++] = (char)('0' + mm / 10);
        s[n++] = (char)('0' + mm % 10);
    }
    s[n++] = ':';
    n += put_2(s + n, sec % 60);
    return n;
}

static void text(const ck_allen_canvas *c, ck_canvas *cv, const ck_rect *r, unsigned face, int32_t x, int32_t y,
                 const char *s, unsigned n, uint32_t color, uint32_t a)
{
    ck_af_face *f = face_of(c, face);
    uint64_t pen = (uint64_t)(x < 0 ? 0 : x) * 256;
    for (unsigned i = 0; i < n; i++) {
        if (face == 3 && !is_big(s[i])) { /* never a glyph the large face has not made */
            pen += ck_af_advance(f, s[i]);
            continue;
        }
        pen = ck_letter(cv, r, f, pen, y, s[i], color, a, 0);
    }
}

static void draw_item(const ck_allen_canvas *c, const ck_canvas_item *it, ck_canvas *cv, const ck_rect *r, uint32_t t)
{
    if (!ck_meets(it->box, r) || t < opens(it))
        return;
    uint32_t keep = stays(it, t), e0 = come(it, 0, t), D = e0 * keep >> 8;
    if (!D)
        return;
    int32_t x = it->x, y = it->y - lift(c, it, e0);
    uint32_t edge = it->warm ? C_WARM2 : C_GLOW;
    int pict = it->kind == CK_THING_PICTURE || it->kind == CK_THING_VIDEO;
    /* the shade its words rest in: no edge, so the sky is never cut by a frame */
    shade(cv, r, (ck_rect){x - S(110), y - S(80), it->w + S(220), it->h + S(160)}, 118 * D >> 8);
    switch (it->kind) {
    case CK_THING_NOTE:
    case CK_THING_LIST:
    case CK_THING_QUOTE: { /* a thread of light down its side, brightest a third of the way down */
        int32_t tx = (x + S(2)) * 16 + 8, y0 = (y - S(6)) * 16, y1 = (y + it->h / 3) * 16, y2 = (y + it->h + S(6)) * 16;
        uint32_t a = 190 * D >> 8;
        if (it->kind == CK_THING_QUOTE) { /* a quotation: warm, and a short stroke before who said it */
            edge = C_WARM2;
            if (it->nrows > 1) {
                const ck_canvas_row *w = &it->row[it->nrows - 1];
                int32_t wy = (y + w->dy - cap_of(c->body) / 3) * 16;
                ck_thread(cv, r, (x + S(26)) * 16, wy, (x + S(48)) * 16, wy, (int32_t)c->k / 16,
                          C_HINT, 150 * D >> 8, 150 * D >> 8);
            }
        }
        ck_thread(cv, r, tx, y0, tx, y1, (int32_t)c->k / 16, edge, 0, a);
        ck_thread(cv, r, tx, y1, tx, y2, (int32_t)c->k / 16, edge, a, 0);
        ck_glow(cv, r, tx, y1, S(5) * 16, edge, 120 * D >> 8);
        break;
    }
    case CK_THING_NUMBER:
        ck_glow(cv, r, (x + it->rx) * 16, (y + it->ry) * 16, S(150) * 16, it->warm ? C_WARM : C_GLOW, 30 * D >> 8);
        break;
    case CK_THING_PERSON: {
        int32_t px = x + it->rx, py = y + it->ry;
        ck_glow(cv, r, px * 16, py * 16, it->rr * 16 * 3 / 2, it->way == CK_WAY_FROM ? C_WARM : C_GLOW, 34 * D >> 8);
        ring(cv, r, px, py, it->rr, S(1) * 16 / 2 + 6, C_GLOW, it->warm ? C_WARM2 : C_MIST, 170 * D >> 8, 0, 65536u);
        break;
    }
    case CK_THING_TIMER: {
        int32_t px = x + it->rx, py = y + it->ry;
        uint32_t left = 0, upto = 65536u;
        char s[16];
        unsigned n = 0;
        if (it->dur) {
            n = clock_text(it, s, &left);
            upto = (uint32_t)((uint64_t)left * 65536u / it->dur);
        }
        ck_glow(cv, r, px * 16, py * 16, it->rr * 16 * 5 / 4, C_INK, 46 * D >> 8);
        ring(cv, r, px, py, it->rr, S(1) * 16 / 2 + 6, C_GLOW, it->warm ? C_WARM2 : C_MIST, 210 * D >> 8,
             (it->dur ? 52u : 120u) * D >> 8, upto);
        if (it->dur && left) { /* the moving end of the light */
            int32_t hx = px * 16 + (int32_t)((int64_t)it->rr * 16 * ck_sin(upto) >> 14);
            int32_t hy = py * 16 - (int32_t)((int64_t)it->rr * 16 * ck_cos(upto) >> 14);
            ck_glow(cv, r, hx, hy, S(7) * 16, C_WARM, 230 * D >> 8);
        }
        if (n) {
            int32_t ww = (int32_t)ck_af_measure(c->voice, s, n);
            text(c, cv, r, 0, px - ww / 2, py + cap_of(c->voice) / 2, s, n, left ? C_TEXT : C_WARM2, 235 * D >> 8);
        }
        break;
    }
    default:
        break;
    }
    if (pict) { /* the place where the picture will be: light gathering, no frame */
        ck_rect fbox = {x, y, it->w, it->rr};
        /* a soft field of deep blue light where it will be, its edges dissolving into the sky, and one
         * brighter light drifting slowly across it, as if the picture were gathering */
        soft_oval(cv, r, fbox, C_INK, 92 * D >> 8);
        int32_t mx = x + it->w / 2, my = y + it->rr / 2;
        uint32_t ang = (uint32_t)((uint64_t)it->tq * 65536u / 19000u);
        int32_t gx = mx + (int32_t)((int64_t)(it->w / 5) * ck_sin(ang) >> 14);
        ck_glow(cv, r, gx * 16, my * 16, it->rr * 16 * 11 / 20, C_GLOW, 30 * D >> 8);
        if (it->kind == CK_THING_VIDEO) { /* a still play mark, drawn in light */
            int32_t s3 = S(15);
            ring(cv, r, mx, my, S(40), S(1) * 16 / 2 + 6, C_GLOW, C_MIST, 150 * D >> 8, 0, 65536u);
            int32_t ax = (mx - s3 * 2 / 3) * 16, bx = (mx + s3) * 16, ty = (my - s3) * 16, by = (my + s3) * 16;
            uint32_t a = 210 * D >> 8;
            ck_thread(cv, r, ax, ty, ax, by, (int32_t)c->k / 16, C_MIST, a, a);
            ck_thread(cv, r, ax, ty, bx, my * 16, (int32_t)c->k / 16, C_MIST, a, a);
            ck_thread(cv, r, ax, by, bx, my * 16, (int32_t)c->k / 16, C_MIST, a, a);
        }
        static const char wait[] = "not here yet";
        int32_t ww = (int32_t)ck_af_measure(c->small, wait, sizeof wait - 1);
        text(c, cv, r, 2, mx - ww / 2, y + it->rr - S(22), wait, sizeof wait - 1, C_HINT, 120 * D >> 8);
    }
    /* the lines of type, one after another */
    for (unsigned j = 0; j < it->nrows; j++) {
        const ck_canvas_row *rw = &it->row[j];
        uint32_t e = come(it, j, t), a = e * keep >> 8;
        if (!a)
            continue;
        int32_t ry = it->y - lift(c, it, e) + rw->dy, rx = x + rw->dx;
        if (rw->dot)
            ck_glow(cv, r, (x + S(26) + S(7)) * 16, (ry - cap_of(face_of(c, rw->face)) / 2) * 16, S(4) * 16,
                    it->warm ? C_WARM : C_GLOW, 210 * a >> 8);
        uint32_t aa = rw->face == 3 && it->kind == CK_THING_QUOTE ? a * 120 >> 8 : a;
        text(c, cv, r, rw->face, rx, ry, it->text + rw->off, rw->len, role_rgb(rw->role), aa);
    }
}

static void draw_spark(const ck_allen_canvas *c, const ck_canvas_item *it, ck_canvas *cv, const ck_rect *r, uint32_t t)
{
    if (it->arrive != CK_ARRIVE_BLOOM || t < it->born || t >= it->born + SPARK_MS + 40)
        return;
    int32_t p = ck_prog(t, it->born, it->born + SPARK_MS);
    /* a warm point with a soft halo and a fading trail, on a gentle arc up and over to its place */
    uint32_t fade = 256 - ck_smooth(ck_prog(t, it->born + SPARK_MS - 120, it->born + SPARK_MS + 40));
    for (int k = 0; k < 6; k++) {
        int32_t pk = p - k * 14, sx, sy;
        if (pk < 0)
            break;
        spark_at(c, it, pk, &sx, &sy);
        uint32_t a = (256u - (uint32_t)k * 40u) * fade >> 8;
        if (!k)
            ck_glow(cv, r, sx, sy, S(34) * 16, C_GLOW, 70 * a >> 8);
        ck_glow(cv, r, sx, sy, S(9 - k) * 16, C_WARM2, 240 * a >> 8);
    }
}

void ck_allen_canvas_draw(const ck_allen_canvas *c, ck_canvas *cv, const ck_rect *r)
{
    draw_tone(c, cv, r);
    for (unsigned i = 0; i < CK_CANVAS_SLOTS; i++)
        if (c->item[i].live)
            draw_item(c, &c->item[i], cv, r, c->t);
    for (unsigned i = 0; i < CK_CANVAS_SLOTS; i++)
        if (c->item[i].live)
            draw_spark(c, &c->item[i], cv, r, c->t);
}
