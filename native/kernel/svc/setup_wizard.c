/* setup_wizard.c -- Allen's first-run setup; see setup_wizard.h. */
#include "setup_wizard.h"

#define HELLO_MS 6000u    /* hello stays about as long as it takes to read */
#define BACK_MS 4000u     /* "welcome back" after a restart */
#define SCAN_LINE_MS 700u /* the window eases one found line in every this often */
#define SCAN_REST_MS 2500u
#define AUTO_PICK_MS 10000u /* a single model is used if nothing is chosen */
#define TEST_MS 30000u
#define READY_MS 15000u

static const char *const step_names[SW_STEPS] = {
    "hello", "intent", "other", "other-check", "scan", "consent", "each", "games", "ai",
    "llm", "service", "url", "key", "testing", "test-failed", "ready", "done",
};

const char *sw_step_name(int step) { return step >= 0 && step < SW_STEPS ? step_names[step] : "?"; }

static void zero(void *p, unsigned n)
{
    volatile uint8_t *b = p; /* volatile: the key is wiped even when nothing reads it after */
    while (n--)
        *b++ = 0;
}

static int same(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

static int starts(const char *s, const char *w)
{
    while (*w)
        if (*s++ != *w++)
            return 0;
    return 1;
}

/* ---- the log ---- */

static void event(sw_wizard *w, const char *a, const char *b)
{
    char *d = w->ev[(w->ev_head + w->ev_n) % SW_EVENTS];
    unsigned at = si_copy(d, a, SW_LINE);
    if (b)
        si_cat(d, at, b, SW_LINE);
    if (w->ev_n < SW_EVENTS)
        w->ev_n++;
    else
        w->ev_head = (w->ev_head + 1) % SW_EVENTS;
}

int sw_event(sw_wizard *w, char *out, unsigned cap)
{
    if (!w->ev_n)
        return 0;
    si_copy(out, w->ev[w->ev_head], cap);
    w->ev_head = (w->ev_head + 1) % SW_EVENTS;
    w->ev_n--;
    return 1;
}

/* ---- the view ---- */

static void say(sw_wizard *w, const char *s)
{
    if (!same(w->v.say, s)) {
        si_copy(w->v.say, s, SW_SAY);
        w->v.said_gen++;
    }
}

static char *line(sw_wizard *w)
{
    if (w->v.nline >= SW_LINES)
        return 0;
    char *d = w->v.line[w->v.nline++];
    d[0] = 0;
    return d;
}

static void add_line(sw_wizard *w, const char *s)
{
    char *d = line(w);
    if (d)
        si_copy(d, s, SW_LINE);
}

static void choice(sw_wizard *w, const char *s)
{
    if (w->v.nchoice < SW_CHOICES)
        si_copy(w->v.choice[w->v.nchoice++], s, SW_CHOICE);
}

static unsigned gb(char *d, unsigned at, uint32_t mib, unsigned cap)
{
    at = si_num(d, at, (mib + 512) / 1024, cap);
    return si_cat(d, at, " GB", cap);
}

/* ---- what Allen would set up ---- */

static const si_display *main_screen(const sw_wizard *w) { return w->inv->ndisplay ? &w->inv->display[0] : 0; }

static int screen_can_go_faster(const sw_wizard *w)
{
    const si_display *s = main_screen(w);
    return s && s->hz && s->max_hz > s->hz;
}

static unsigned item_text(const sw_wizard *w, unsigned it, char *d, unsigned cap)
{
    unsigned at = 0;
    int g = si_best_gpu(w->inv);
    switch (it) {
    case SW_I_DRIVERS:
        return si_copy(d, "Use my own drivers for the parts I can drive", cap);
    case SW_I_SCREEN:
        at = si_copy(d, "Set your screen to ", cap);
        at = si_num(d, at, main_screen(w)->max_hz, cap);
        return si_cat(d, at, " times a second", cap);
    case SW_I_CORES:
        return si_copy(d, "Keep the fast processor cores free for games", cap);
    case SW_I_AI_GPU:
        if (g < 0)
            return si_copy(d, "Run the AI on the processor", cap);
        at = si_copy(d, "Run the AI on your ", cap);
        return si_cat(d, at, w->inv->gpu[g].name, cap);
    case SW_I_BUILD:
        return si_copy(d, "Build drivers for any part I can't drive yet", cap);
    }
    d[0] = 0;
    return 0;
}

static const char *const item_id[SW_I_COUNT] = {"drivers-own", "screen-refresh", "cpu-games", "ai-gpu",
                                                "drivers-build"};

static void plan_items(sw_wizard *w)
{
    w->nitem = 0;
    w->item[w->nitem++] = SW_I_DRIVERS;
    if ((w->want & SW_WANT_GAMES) && screen_can_go_faster(w))
        w->item[w->nitem++] = SW_I_SCREEN;
    if (w->want & SW_WANT_GAMES)
        w->item[w->nitem++] = SW_I_CORES;
    if (w->want & SW_WANT_AI)
        w->item[w->nitem++] = SW_I_AI_GPU;
    w->item[w->nitem++] = SW_I_BUILD;
}

/* ---- the AI models found ---- */

static int is_27b(const char *n) { return starts(n, "qwen3.8-27b"); }

static void cand_from_model(sw_cand *c, const si_model *m)
{
    zero(c, sizeof *c);
    c->kind = SW_LLM_LOCAL;
    c->src = m->src;
    si_copy(c->name, m->name, sizeof c->name);
    if (m->running)
        si_copy(c->url, m->where, sizeof c->url);
    if (m->src == SI_SRC_PAIR) {
        si_copy(c->label, "Allen's own model pair", sizeof c->label);
        si_copy(c->hint, "27B + 30B, working together, on this computer", sizeof c->hint);
        return;
    }
    si_copy(c->label, is_27b(m->name) ? "The 27B model" : m->name, sizeof c->label);
    si_copy(c->hint,
            m->running                    ? "Already running on this computer"
            : m->src == SI_SRC_OLLAMA     ? "Installed with Ollama on this computer"
            : m->src == SI_SRC_LMSTUDIO   ? "Installed with LM Studio on this computer"
                                          : "A model file on this computer",
            sizeof c->hint);
}

/* On this computer first (the pair, then what is running, then what is
 * installed), then other AienOS computers nearby. */
static void find_models(sw_wizard *w)
{
    const si_inv *v = w->inv;
    w->ncand = 0;
    for (int pass = 0; pass < 3; pass++)
        for (unsigned i = 0; i < v->nmodel && w->ncand < SW_CANDS; i++) {
            const si_model *m = &v->model[i];
            int p = m->src == SI_SRC_PAIR ? 0 : m->running ? 1 : 2;
            if (p == pass)
                cand_from_model(&w->cand[w->ncand++], m);
        }
    for (unsigned i = 0; i < v->npeer && w->ncand < SW_CANDS; i++) {
        const si_peer *q = &v->peer[i];
        if (q->kind != SI_PEER_AIENOS || !q->model[0] || !q->url[0])
            continue;
        sw_cand *c = &w->cand[w->ncand++];
        zero(c, sizeof *c);
        c->kind = SW_LLM_PEER;
        c->src = SI_SRC_PEER;
        si_copy(c->name, q->model, sizeof c->name);
        si_copy(c->url, q->url, sizeof c->url);
        si_copy(c->label, is_27b(q->model) ? "The 27B model" : q->model, sizeof c->label);
        unsigned at = si_copy(c->hint, "On ", sizeof c->hint);
        at = si_cat(c->hint, at, q->name[0] ? q->name : "another AienOS computer", sizeof c->hint);
        si_cat(c->hint, at, ", nearby", sizeof c->hint);
    }
}

/* the memory a model can use: the best graphics card's, else main memory */
static uint32_t ai_mib(const sw_wizard *w)
{
    int g = si_best_gpu(w->inv);
    return g >= 0 ? w->inv->gpu[g].vram_mib : 0;
}

static int fetch_big(const sw_wizard *w)
{
    uint32_t m = ai_mib(w);
    return m >= SW_MIB_30B || (!m && w->inv->ram_mib >= SW_MIB_30B * 2);
}

/* ---- steps ---- */

static void render(sw_wizard *w);

static void go(sw_wizard *w, int step, uint32_t now)
{
    if (step == SW_CONSENT)
        plan_items(w);
    w->step = step;
    w->step_t = now;
    w->v.lit = 0;
    w->listing = step == SW_LLM ? w->listing : 0;
    event(w, "step ", sw_step_name(step));
    render(w);
}

/* after the consent: the paths for what they want, then the AI, then ready */
static void next_path(sw_wizard *w, int from, uint32_t now)
{
    if (from < SW_GAMES && (w->want & SW_WANT_GAMES))
        go(w, SW_GAMES, now);
    else if (from < SW_AI && (w->want & SW_WANT_AI))
        go(w, SW_AI, now);
    else
        go(w, SW_LLM, now);
}

static void games_lines(sw_wizard *w)
{
    const si_display *s = main_screen(w);
    char *d = line(w);
    unsigned at;
    if (s && s->hz && s->max_hz > s->hz) {
        at = si_copy(d, "Screen: ", SW_LINE);
        at = si_num(d, at, s->hz, SW_LINE);
        at = si_cat(d, at, " times a second now, it can do ", SW_LINE);
        at = si_num(d, at, s->max_hz, SW_LINE);
        si_cat(d, at, ". My graphics driver will set it.", SW_LINE);
    } else if (s && s->hz) {
        at = si_copy(d, "Screen: ", SW_LINE);
        at = si_num(d, at, s->hz, SW_LINE);
        si_cat(d, at, " times a second, the most it can do.", SW_LINE);
    } else if (s) {
        at = si_copy(d, "Screen: ", SW_LINE);
        at = si_num(d, at, s->w, SW_LINE);
        at = si_cat(d, at, " by ", SW_LINE);
        at = si_num(d, at, s->h, SW_LINE);
        si_cat(d, at, "; how often it refreshes is not known yet.", SW_LINE);
    } else
        si_copy(d, "Screen: I can't read your screen's modes yet.", SW_LINE);
    int g = si_best_gpu(w->inv);
    d = line(w);
    if (g < 0)
        si_copy(d, "Graphics: none I can use for games here.", SW_LINE);
    else {
        at = si_copy(d, "Graphics: ", SW_LINE);
        at = si_cat(d, at, w->inv->gpu[g].name, SW_LINE);
        if (w->inv->gpu[g].driver[0]) {
            at = si_cat(d, at, ", driven by ", SW_LINE);
            si_cat(d, at, w->inv->gpu[g].driver, SW_LINE);
        } else
            si_cat(d, at, ", no driver for it yet.", SW_LINE);
    }
    d = line(w);
    const si_cpu *c = &w->inv->cpu;
    if (c->fast && c->efficient) {
        at = si_copy(d, "Processor: the ", SW_LINE);
        at = si_num(d, at, c->fast, SW_LINE);
        at = si_cat(d, at, " fast cores stay free for the game, the rest on the ", SW_LINE);
        at = si_num(d, at, c->efficient, SW_LINE);
        si_cat(d, at, " efficient ones.", SW_LINE);
    } else if (c->fast) {
        at = si_copy(d, "Processor: I'll keep its ", SW_LINE);
        at = si_num(d, at, c->fast, SW_LINE);
        si_cat(d, at, " cores free for the game.", SW_LINE);
    } else
        si_copy(d, "Processor: I'll keep it free for the game.", SW_LINE);
    add_line(w, "Frames a second: I'll measure them the first time a game runs. Nothing to do now.");
}

static void fit_line(sw_wizard *w, const char *what, uint32_t mib, uint32_t mem, int shared)
{
    char *d = line(w);
    if (!d)
        return;
    unsigned at = si_copy(d, what, SW_LINE);
    at = si_cat(d, at, " needs ", SW_LINE);
    at = gb(d, at, mib, SW_LINE);
    at = si_cat(d, at, " for its weights: ", SW_LINE);
    uint32_t ram = w->inv->ram_mib;
    if (mem && mib <= mem)
        si_cat(d, at, shared || !mem ? "it fits." : "it fits on the graphics card.", SW_LINE);
    else if (mem && !shared && mib <= mem + ram)
        si_cat(d, at, "it fits if I share main memory, slower.", SW_LINE);
    else if (!mem && ram && mib <= ram)
        si_cat(d, at, "it fits in main memory, on the processor.", SW_LINE);
    else
        si_cat(d, at, "too big for this computer.", SW_LINE);
}

static void ai_lines(sw_wizard *w)
{
    int g = si_best_gpu(w->inv);
    uint32_t mem = ai_mib(w);
    int shared = g >= 0 && w->inv->gpu[g].kind == SI_GPU_UNIFIED;
    char *d = line(w);
    unsigned at;
    if (g < 0 || !mem) {
        at = si_copy(d, g < 0 ? "No graphics card I can use for AI here" : "Your graphics memory is not known yet",
                     SW_LINE);
        si_cat(d, at, "; the processor will run it.", SW_LINE);
    } else {
        at = si_copy(d, w->inv->gpu[g].name, SW_LINE);
        at = si_cat(d, at, shared ? " shares " : " has ", SW_LINE);
        at = gb(d, at, mem, SW_LINE);
        si_cat(d, at, shared ? " of memory with the processor." : " of its own memory.", SW_LINE);
    }
    fit_line(w, "The 27B model", SW_MIB_27B, mem, shared);
    fit_line(w, "The 30B model", SW_MIB_30B, mem, shared);
    fit_line(w, "A small 4B model", SW_MIB_4B, mem, shared);
    add_line(w, "The working memory on top of the weights is not measured yet.");
}

static void llm_view(sw_wizard *w)
{
    char s[SW_SAY];
    unsigned at = si_copy(s, (w->want & SW_WANT_AI) ? "" : "I build drivers with an AI. ", SW_SAY);
    int connect_ok = w->courier || w->listing != 2; /* offered; without a wire, picking it says why and it goes */
    if (w->ncand == 1 && !w->listing) {
        const sw_cand *c = &w->cand[0];
        at = si_cat(s, at, "I found ", SW_SAY);
        at = si_cat(s, at, c->label, SW_SAY);
        at = si_cat(s, at, c->kind == SW_LLM_PEER ? " nearby. I'll use it." : " on this computer. I'll use it.", SW_SAY);
        say(w, s);
        add_line(w, c->hint);
        choice(w, "Use it");
        choice(w, "Choose another");
        si_copy(w->v.aside, "I'll use it in 10 seconds unless you choose", sizeof w->v.aside);
        si_copy(w->v.hint, w->v.lit == 0 ? c->hint : "See everything I found, or connect your own", SW_LINE);
        return;
    }
    if (w->ncand) {
        if (w->ncand == 1)
            at = si_cat(s, at, "Here's what I found.", SW_SAY);
        else {
            at = si_cat(s, at, "I found ", SW_SAY);
            at = si_num(s, at, w->ncand, SW_SAY);
            at = si_cat(s, at, " AI models. The first one fits best.", SW_SAY);
        }
        if (w->listing == 2)
            at = si_cat(s, at, " I can't keep a key safely here yet. Set it up on the host instead.", SW_SAY);
        say(w, s);
        unsigned shown = w->ncand < SW_CHOICES - 1 ? w->ncand : SW_CHOICES - 1;
        for (unsigned i = 0; i < shown; i++)
            choice(w, w->cand[i].label);
        if (connect_ok)
            choice(w, "Connect my own");
        if ((unsigned)w->v.lit < shown)
            si_copy(w->v.hint, w->cand[w->v.lit].hint, SW_LINE);
        else
            si_copy(w->v.hint, "Claude, ChatGPT, or another service", SW_LINE);
    } else {
        at = si_cat(s, at, "I didn't find an AI model on this computer or nearby.", SW_SAY);
        if (w->listing == 2)
            at = si_cat(s, at, " I can't keep a key safely here yet. Set it up on the host instead.", SW_SAY);
        say(w, s);
        choice(w, "Get one for me");
        if (connect_ok)
            choice(w, "Connect my own");
        if (w->v.lit == 0)
            si_copy(w->v.hint, fetch_big(w) ? "The 30B model, a 29 GB download" : "The small 4B model, an 8 GB download",
                    SW_LINE);
        else
            si_copy(w->v.hint, "Claude, ChatGPT, or another service", SW_LINE);
    }
    si_copy(w->v.aside, "Not now: Escape", sizeof w->v.aside);
}

static const char *service_name(int s) { return s == SW_LLM_CLAUDE ? "Claude" : s == SW_LLM_OPENAI ? "ChatGPT" : "that service"; }

static void ready_lines(sw_wizard *w)
{
    char *d;
    unsigned at;
    if ((w->consent & 1u << SW_I_SCREEN) && main_screen(w)) {
        d = line(w);
        at = si_copy(d, "Games will use your screen at ", SW_LINE);
        at = si_num(d, at, main_screen(w)->max_hz, SW_LINE);
        si_cat(d, at, " times a second once my graphics driver is in.", SW_LINE);
    }
    d = line(w);
    switch (w->llm_kind) {
    case SW_LLM_LOCAL:
    case SW_LLM_PEER:
        at = si_copy(d, "Your AI is ", SW_LINE);
        at = si_cat(d, at, w->llm_name, SW_LINE);
        si_cat(d, at, w->llm_kind == SW_LLM_PEER ? ", on a computer nearby." : ", on this computer.", SW_LINE);
        break;
    case SW_LLM_CLAUDE:
    case SW_LLM_OPENAI:
    case SW_LLM_OTHER:
        at = si_copy(d, "Your AI is ", SW_LINE);
        at = si_cat(d, at, w->llm_kind == SW_LLM_OTHER ? "your own service" : service_name(w->llm_kind), SW_LINE);
        si_cat(d, at, ", through your key. It works.", SW_LINE);
        break;
    case SW_LLM_FETCH:
        at = si_copy(d, "I'll get ", SW_LINE);
        at = si_cat(d, at, w->llm_name, SW_LINE);
        si_cat(d, at, " when this computer can download it.", SW_LINE);
        break;
    default:
        si_copy(d, "No AI yet. Tell me when you'd like one.", SW_LINE);
    }
    if (w->consent & 1u << SW_I_BUILD)
        add_line(w, "Next I'll check every part and build drivers for the ones I can't drive yet.");
    else if (w->consent_asked < 0 || !w->consent)
        add_line(w, "Nothing was changed. Say set up again any time.");
    add_line(w, "Ask me anything, or tell me what you'd like to do.");
}

static void render(sw_wizard *w)
{
    sw_view *v = &w->v;
    char buf[SW_SAY];
    unsigned at;
    v->nline = 0;
    v->nchoice = 0;
    v->hint[0] = v->aside[0] = v->prompt[0] = 0;
    v->ntyped = 0;
    v->typed[0] = 0;
    switch (w->step) {
    case SW_HELLO:
        say(w, w->resumed ? "Welcome back. Let's carry on where we stopped."
                          : "Hello. I'm Allen. I'm going to look after this computer with you.");
        break;
    case SW_INTENT:
        say(w, "What would you like this computer to do best?");
        choice(w, "Play games smoothly");
        choice(w, "Run an AI model here");
        choice(w, "Something else");
        si_copy(v->hint,
                v->lit == 0   ? "Faster frames, a steady picture, the most from your screen and graphics."
                : v->lit == 1 ? "Use the graphics and memory in this computer to run an AI that stays on it."
                              : "Tell me in your own words.",
                SW_LINE);
        si_copy(v->aside, "Not now: Escape", sizeof v->aside);
        break;
    case SW_OTHER:
        say(w, "Tell me what you'd like to do.");
        si_copy(v->prompt, "In your own words", sizeof v->prompt);
        v->ntyped = si_copy(v->typed, w->text, SW_TEXT);
        break;
    case SW_OTHER_CHECK:
        if ((w->want & (SW_WANT_GAMES | SW_WANT_AI)) == (SW_WANT_GAMES | SW_WANT_AI))
            say(w, "You want smoother games and an AI here. I'll set up both.");
        else if (w->want & SW_WANT_GAMES)
            say(w, "You want smoother games. I'll focus on your screen, graphics and processor.");
        else if (w->want & SW_WANT_AI)
            say(w, "You want to run an AI here. I'll set up the graphics and memory for it.");
        else {
            at = si_copy(buf, "You'd like this: ", SW_SAY);
            char wish[72];
            si_copy(wish, w->wish, sizeof wish);
            at = si_cat(buf, at, wish, SW_SAY);
            si_cat(buf, at, ". I'll keep everything else simple.", SW_SAY);
            say(w, buf);
        }
        choice(w, "That's right");
        choice(w, "Not quite");
        break;
    case SW_SCAN: {
        say(w, (w->want & ~SW_WANT_ASKED) || w->resumed ? "Getting to know this computer."
                                                        : "That's fine. I'll set up the basics first. Getting to know "
                                                          "this computer.");
        char found[SW_LINES][96];
        unsigned n = si_summary(w->inv, found, SW_LINES);
        for (unsigned i = 0; i < n; i++)
            add_line(w, found[i]);
        if (n <= 1 && !w->inv->ndisplay && !w->inv->cpu.fast)
            add_line(w, "I couldn't find much yet. I'll keep looking while you use it.");
        break;
    }
    case SW_CONSENT:
        say(w, "Here's what I'd like to set up:");
        for (unsigned i = 0; i < w->nitem; i++) {
            char *d = line(w);
            if (d)
                item_text(w, w->item[i], d, SW_LINE);
        }
        add_line(w, "Nothing on your other drives is changed. You can undo any of this.");
        choice(w, "Set it up");
        choice(w, "Show me each one");
        choice(w, "Not now");
        break;
    case SW_EACH: {
        say(w, "Shall I do this?");
        char *d = line(w);
        item_text(w, w->item[w->each], d, SW_LINE);
        choice(w, "Yes");
        choice(w, "Skip");
        si_copy(v->aside, "Skip the rest: Escape", sizeof v->aside);
        break;
    }
    case SW_GAMES:
        say(w, "Making games smoother.");
        games_lines(w);
        choice(w, "Carry on");
        break;
    case SW_AI:
        say(w, "Finding the best way to run an AI here.");
        ai_lines(w);
        choice(w, "Carry on");
        break;
    case SW_LLM:
        llm_view(w);
        break;
    case SW_SERVICE:
        say(w, "Which service?");
        choice(w, "Claude");
        choice(w, "ChatGPT");
        choice(w, "Another service");
        si_copy(v->aside, "Back: Escape", sizeof v->aside);
        break;
    case SW_URL:
        say(w, "What's your service's web address? It starts with https:// and its settings page shows it.");
        si_copy(v->prompt, "Its web address", sizeof v->prompt);
        v->ntyped = si_copy(v->typed, w->text, SW_TEXT);
        if (w->why[0])
            add_line(w, w->why);
        break;
    case SW_KEY:
        at = si_copy(buf, "Type your key for ", SW_SAY);
        at = si_cat(buf, at, service_name(w->service), SW_SAY);
        at = si_cat(buf, at, w->service == SW_LLM_OTHER ? ", or press Enter if it needs none." : ".", SW_SAY);
        si_cat(buf, at, " I test it once and never show it again; it is not written to this disk.", SW_SAY);
        say(w, buf);
        at = si_copy(v->prompt, "Key for ", sizeof v->prompt);
        si_cat(v->prompt, at, service_name(w->service), sizeof v->prompt);
        for (unsigned i = 0; i < w->nkey && i + 1 < SW_TEXT; i++)
            v->typed[i] = '*';
        v->ntyped = w->nkey < SW_TEXT - 1 ? w->nkey : SW_TEXT - 1;
        v->typed[v->ntyped] = 0;
        if (w->why[0])
            add_line(w, w->why);
        break;
    case SW_TESTING:
        say(w, "Testing it with one short request.");
        si_copy(v->aside, "Stop: Escape", sizeof v->aside);
        break;
    case SW_TEST_FAILED:
        at = si_copy(buf, "That didn't work. ", SW_SAY);
        if (w->why[0]) {
            at = si_cat(buf, at, "The service said: ", SW_SAY);
            at = si_cat(buf, at, w->why, SW_SAY);
            at = si_cat(buf, at, ". ", SW_SAY);
        }
        si_cat(buf, at, "Try again?", SW_SAY);
        say(w, buf);
        choice(w, "Try again");
        choice(w, "Choose another");
        break;
    case SW_READY:
        say(w, "Allen is ready.");
        ready_lines(w);
        choice(w, "Start");
        break;
    default:
        say(w, "");
        break;
    }
    if (v->nchoice && (unsigned)v->lit >= v->nchoice)
        v->lit = 0;
    v->gen++;
}

/* ---- picking ---- */

static void classify(sw_wizard *w)
{
    char low[SW_TEXT + 2];
    unsigned n = 0;
    low[n++] = ' ';
    for (unsigned i = 0; w->wish[i] && n < SW_TEXT; i++) {
        char c = w->wish[i];
        low[n++] = c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c >= 'a' && c <= 'z' ? c : c >= '0' && c <= '9' ? c : ' ';
    }
    low[n++] = ' ';
    low[n] = 0;
    static const char *const games[] = {"game", "gaming", "fps", "frame", "play", "steam", "esport", 0};
    static const char *const ai[] = {" ai ", "model", "llm", " chat", "gpt", "claude", "assistant", "agent", 0};
    unsigned want = 0;
    for (unsigned i = 0; i < n; i++) {
        for (unsigned k = 0; games[k]; k++)
            if (starts(low + i, games[k]))
                want |= SW_WANT_GAMES;
        for (unsigned k = 0; ai[k]; k++)
            if (starts(low + i, ai[k]))
                want |= SW_WANT_AI;
    }
    w->want = SW_WANT_ASKED | (want ? want : SW_WANT_OTHER);
}

static void intent_event(sw_wizard *w)
{
    unsigned x = w->want;
    event(w, "intent ",
          (x & SW_WANT_GAMES) && (x & SW_WANT_AI) ? "games+ai"
          : x & SW_WANT_GAMES                     ? "games"
          : x & SW_WANT_AI                        ? "ai"
          : x & SW_WANT_OTHER                     ? "other"
                                                  : "not now");
}

static void pick_llm(sw_wizard *w, unsigned kind, const char *name, const char *url, uint32_t now)
{
    w->llm_kind = (uint8_t)kind;
    si_copy(w->llm_name, name, sizeof w->llm_name);
    si_copy(w->llm_url, url, sizeof w->llm_url);
    event(w,
          kind == SW_LLM_LOCAL   ? "llm picked local "
          : kind == SW_LLM_PEER  ? "llm picked peer "
          : kind == SW_LLM_FETCH ? "llm picked fetch "
                                 : "llm picked service ",
          name);
    go(w, SW_READY, now);
}

static void connect_own(sw_wizard *w, uint32_t now)
{
    if (!w->courier) { /* nowhere safe for a key: say so, offer the rest */
        w->listing = 2;
        event(w, "connect refused: no wire to keep a key", 0);
        render(w);
        return;
    }
    go(w, SW_SERVICE, now);
}

static void choose(sw_wizard *w, unsigned i, uint32_t now)
{
    switch (w->step) {
    case SW_INTENT:
        if (i == 2) {
            w->ntext = 0;
            w->text[0] = 0;
            go(w, SW_OTHER, now);
            return;
        }
        w->want = SW_WANT_ASKED | (i == 0 ? SW_WANT_GAMES : SW_WANT_AI);
        intent_event(w);
        go(w, SW_SCAN, now);
        return;
    case SW_OTHER_CHECK:
        if (i == 0) {
            intent_event(w);
            go(w, SW_SCAN, now);
        } else {
            w->ntext = 0;
            w->text[0] = 0;
            go(w, SW_OTHER, now);
        }
        return;
    case SW_CONSENT:
        if (i == 0) {
            for (unsigned k = 0; k < w->nitem; k++)
                w->consent |= 1u << w->item[k];
            w->consent_asked = 1;
            event(w, "consent all", 0);
            next_path(w, SW_CONSENT, now);
        } else if (i == 1) {
            w->each = 0;
            event(w, "consent each", 0);
            go(w, SW_EACH, now);
        } else {
            w->consent_asked = -1;
            event(w, "consent not now", 0);
            next_path(w, SW_CONSENT, now);
        }
        return;
    case SW_EACH: {
        unsigned it = w->item[w->each];
        if (i == 0)
            w->consent |= 1u << it;
        event(w, i == 0 ? "consent yes " : "consent skip ", item_id[it]);
        if (++w->each >= w->nitem) {
            w->consent_asked = 1;
            next_path(w, SW_CONSENT, now);
        } else {
            w->v.lit = 0;
            render(w);
        }
        return;
    }
    case SW_GAMES:
    case SW_AI:
        next_path(w, w->step, now);
        return;
    case SW_LLM:
        if (w->ncand == 1 && !w->listing) {
            if (i == 0)
                pick_llm(w, w->cand[0].kind, w->cand[0].name, w->cand[0].url, now);
            else {
                w->listing = 1;
                w->v.lit = 0;
                render(w);
            }
            return;
        }
        if (w->ncand) {
            unsigned shown = w->ncand < SW_CHOICES - 1 ? w->ncand : SW_CHOICES - 1;
            if (i < shown)
                pick_llm(w, w->cand[i].kind, w->cand[i].name, w->cand[i].url, now);
            else
                connect_own(w, now);
            return;
        }
        if (i == 0)
            pick_llm(w, SW_LLM_FETCH, fetch_big(w) ? "the 30B model" : "the small 4B model", "", now);
        else
            connect_own(w, now);
        return;
    case SW_SERVICE:
        w->service = i == 0 ? SW_LLM_CLAUDE : i == 1 ? SW_LLM_OPENAI : SW_LLM_OTHER;
        w->why[0] = 0;
        w->ntext = 0;
        w->text[0] = 0;
        sw_forget_key(w);
        if (w->service == SW_LLM_OTHER)
            go(w, SW_URL, now);
        else {
            si_copy(w->llm_url, w->service == SW_LLM_CLAUDE ? "https://api.anthropic.com" : "https://api.openai.com",
                    sizeof w->llm_url);
            go(w, SW_KEY, now);
        }
        return;
    case SW_TEST_FAILED:
        w->why[0] = 0;
        if (i == 0)
            go(w, SW_KEY, now);
        else {
            w->listing = 1;
            go(w, SW_LLM, now);
        }
        return;
    case SW_READY:
        event(w, "done", 0);
        go(w, SW_DONE, now);
        return;
    }
}

/* Escape: "Not now" on a question, "Back" on the steps inside one */
static void not_now(sw_wizard *w, uint32_t now)
{
    switch (w->step) {
    case SW_HELLO:
    case SW_SCAN:
        go(w, w->step == SW_HELLO ? SW_INTENT : SW_CONSENT, now);
        return;
    case SW_INTENT:
        w->want = SW_WANT_ASKED;
        intent_event(w);
        go(w, SW_SCAN, now);
        return;
    case SW_OTHER:
    case SW_OTHER_CHECK:
        go(w, SW_INTENT, now);
        return;
    case SW_CONSENT:
        choose(w, 2, now);
        return;
    case SW_EACH:
        while (w->each < w->nitem)
            event(w, "consent skip ", item_id[w->item[w->each++]]);
        w->consent_asked = 1;
        next_path(w, SW_CONSENT, now);
        return;
    case SW_GAMES:
    case SW_AI:
        next_path(w, w->step, now);
        return;
    case SW_LLM:
        w->llm_kind = SW_LLM_NONE;
        event(w, "llm skipped", 0);
        go(w, SW_READY, now);
        return;
    case SW_SERVICE:
        go(w, SW_LLM, now);
        return;
    case SW_URL:
    case SW_KEY:
    case SW_TESTING:
    case SW_TEST_FAILED:
        sw_forget_key(w);
        w->test_out = 0;
        w->why[0] = 0;
        go(w, SW_SERVICE, now);
        return;
    case SW_READY:
        choose(w, 0, now);
        return;
    }
}

static int typing(int step) { return step == SW_OTHER || step == SW_URL || step == SW_KEY; }

static void submit(sw_wizard *w, uint32_t now)
{
    switch (w->step) {
    case SW_OTHER:
        if (!w->ntext)
            return;
        si_copy(w->wish, w->text, sizeof w->wish);
        classify(w);
        go(w, SW_OTHER_CHECK, now);
        return;
    case SW_URL:
        if (!starts(w->text, "http://") && !starts(w->text, "https://")) {
            si_copy(w->why, "That doesn't look like a web address. It starts with http:// or https://.", SW_LINE);
            render(w);
            return;
        }
        si_copy(w->llm_url, w->text, sizeof w->llm_url);
        w->why[0] = 0;
        go(w, SW_KEY, now);
        return;
    case SW_KEY:
        if (!w->nkey && w->service != SW_LLM_OTHER) {
            si_copy(w->why, "I need the key to connect. Type it, or press Escape to go back.", SW_LINE);
            render(w);
            return;
        }
        w->why[0] = 0;
        w->test_out = 1;
        event(w, "key test wanted ", w->service == SW_LLM_CLAUDE ? "claude" : w->service == SW_LLM_OPENAI ? "openai" : "other");
        go(w, SW_TESTING, now);
        return;
    }
}

int sw_key(sw_wizard *w, int key, char c, uint32_t now)
{
    unsigned gen = w->v.gen;
    if (w->step == SW_DONE)
        return 0;
    if (typing(w->step)) {
        char *buf = w->step == SW_KEY ? w->key : w->text;
        unsigned *n = w->step == SW_KEY ? &w->nkey : &w->ntext;
        if (key == SW_K_CHAR && c >= ' ' && c < 127 && *n + 1 < SW_TEXT) {
            buf[(*n)++] = c;
            buf[*n] = 0;
            render(w);
        } else if (key == SW_K_BACK && *n) {
            buf[--(*n)] = 0;
            render(w);
        } else if (key == SW_K_ENTER)
            submit(w, now);
        else if (key == SW_K_ESC)
            not_now(w, now);
        return w->v.gen != gen;
    }
    unsigned nc = w->v.nchoice;
    switch (key) {
    case SW_K_LEFT:
    case SW_K_UP:
        if (nc) {
            w->v.lit = (w->v.lit + (int)nc - 1) % (int)nc;
            render(w);
        }
        break;
    case SW_K_RIGHT:
    case SW_K_DOWN:
    case SW_K_TAB:
        if (nc) {
            w->v.lit = (w->v.lit + 1) % (int)nc;
            render(w);
        }
        break;
    case SW_K_ENTER:
        if (nc)
            choose(w, (unsigned)w->v.lit, now);
        else if (w->step == SW_HELLO || w->step == SW_SCAN)
            not_now(w, now); /* Enter moves on from a screen that moves on by itself */
        break;
    case SW_K_ESC:
        not_now(w, now);
        break;
    case SW_K_CHAR:
        if (c >= '1' && c <= '9' && (unsigned)(c - '1') < nc) /* a choice by its number */
            choose(w, (unsigned)(c - '1'), now);
        else if (w->step == SW_INTENT && c > ' ' && c < 127) { /* typing on the question: their own words */
            w->text[0] = c;
            w->text[1] = 0;
            w->ntext = 1;
            go(w, SW_OTHER, now);
            render(w);
        }
        break;
    }
    return w->v.gen != gen;
}

int sw_tick(sw_wizard *w, uint32_t now)
{
    unsigned gen = w->v.gen;
    uint32_t in = now - w->step_t;
    switch (w->step) {
    case SW_HELLO:
        if (in >= (w->resumed ? BACK_MS : HELLO_MS)) {
            if (w->resumed) {
                int to = w->resumed;
                w->resumed = 0;
                go(w, to, now);
            } else
                go(w, SW_INTENT, now);
        }
        break;
    case SW_SCAN:
        if (in >= w->v.nline * SCAN_LINE_MS + SCAN_REST_MS) {
            go(w, SW_CONSENT, now);
        }
        break;
    case SW_LLM:
        if (w->ncand == 1 && !w->listing && in >= AUTO_PICK_MS) {
            event(w, "llm auto-picked after 10 s", 0);
            pick_llm(w, w->cand[0].kind, w->cand[0].name, w->cand[0].url, now);
        }
        break;
    case SW_TESTING:
        if (w->test_out == 2 && now - w->test_t >= TEST_MS)
            sw_test_done(w, 0, "no answer came back", now);
        break;
    case SW_READY:
        if (in >= READY_MS)
            choose(w, 0, now);
        break;
    }
    return w->v.gen != gen;
}

int sw_test_wanted(sw_wizard *w, const char **kind, const char **base_url, const char **model, const char **key)
{
    if (w->test_out != 1 || w->step != SW_TESTING)
        return 0;
    *kind = w->service == SW_LLM_CLAUDE ? "anthropic" : "openai";
    *base_url = w->llm_url;
    *model = "auto";
    *key = w->key;
    w->test_out = 2;
    w->test_t = w->step_t;
    return 1;
}

void sw_test_done(sw_wizard *w, int ok, const char *why, uint32_t now)
{
    if (w->step != SW_TESTING || w->test_out != 2)
        return;
    w->test_out = 0;
    sw_forget_key(w);
    if (ok) {
        event(w, "key test ok", 0);
        pick_llm(w, (unsigned)w->service,
                 w->service == SW_LLM_CLAUDE ? "Claude" : w->service == SW_LLM_OPENAI ? "ChatGPT" : "your own service",
                 w->llm_url, now);
        return;
    }
    event(w, "key test failed", 0);
    si_copy(w->why, why ? why : "", SW_LINE);
    go(w, SW_TEST_FAILED, now);
}

void sw_forget_key(sw_wizard *w)
{
    zero(w->key, sizeof w->key);
    w->nkey = 0;
}

const sw_view *sw_view_of(const sw_wizard *w) { return &w->v; }
int sw_step_of(const sw_wizard *w) { return w->step; }

/* ---- records ---- */

static unsigned kv_num(char *d, unsigned at, const char *k, uint32_t v, unsigned cap)
{
    at = si_cat(d, at, k, cap);
    at = si_cat(d, at, " ", cap);
    at = si_num(d, at, v, cap);
    return si_cat(d, at, "\n", cap);
}

static unsigned kv_str(char *d, unsigned at, const char *k, const char *v, unsigned cap)
{
    at = si_cat(d, at, k, cap);
    at = si_cat(d, at, " ", cap);
    at = si_cat(d, at, v[0] ? v : "-", cap);
    return si_cat(d, at, "\n", cap);
}

/* fits: the whole record, or 0 */
static unsigned whole(char *out, unsigned at, unsigned cap)
{
    if (at + 1 >= cap) {
        out[0] = 0;
        return 0;
    }
    return at;
}

unsigned sw_save(const sw_wizard *w, char *out, unsigned cap)
{
    if (!cap)
        return 0;
    unsigned at = si_copy(out, "aienos-setup v1\n", cap);
    at = kv_num(out, at, "step", (uint32_t)w->step, cap);
    at = kv_num(out, at, "want", w->want, cap);
    at = kv_num(out, at, "consent", w->consent, cap);
    at = kv_num(out, at, "asked", (uint32_t)(w->consent_asked + 1), cap);
    at = kv_num(out, at, "llm", w->llm_kind, cap);
    at = kv_str(out, at, "name", w->llm_name, cap);
    at = kv_str(out, at, "url", w->llm_url, cap);
    return whole(out, at, cap);
}

unsigned sw_llm_conf(const sw_wizard *w, char *out, unsigned cap)
{
    if (!cap)
        return 0;
    out[0] = 0;
    unsigned at = 0;
    switch (w->llm_kind) {
    case SW_LLM_LOCAL:
    case SW_LLM_PEER:
        if (!w->llm_url[0])
            return 0; /* a file nothing serves yet: nothing to call */
        at = kv_str(out, at, "kind", "openai", cap);
        at = kv_str(out, at, "base_url", w->llm_url, cap);
        at = kv_str(out, at, "model", w->llm_name, cap);
        at = kv_str(out, at, "key_env", "none", cap);
        break;
    case SW_LLM_CLAUDE:
        at = kv_str(out, at, "kind", "anthropic", cap);
        at = kv_str(out, at, "base_url", w->llm_url, cap);
        at = kv_str(out, at, "model", "auto", cap);
        at = kv_str(out, at, "key_env", "ANTHROPIC_API_KEY", cap);
        break;
    case SW_LLM_OPENAI:
    case SW_LLM_OTHER:
        at = kv_str(out, at, "kind", "openai", cap);
        at = kv_str(out, at, "base_url", w->llm_url, cap);
        at = kv_str(out, at, "model", "auto", cap);
        at = kv_str(out, at, "key_env", w->llm_kind == SW_LLM_OPENAI ? "OPENAI_API_KEY" : "AIENOS_LLM_KEY", cap);
        break;
    default:
        return 0;
    }
    return whole(out, at, cap);
}

unsigned sw_consent(const sw_wizard *w, char *out, unsigned cap)
{
    if (!cap)
        return 0;
    unsigned at = si_copy(out, "aienos-consent v1\n", cap);
    for (unsigned i = 0; i < w->nitem; i++) {
        at = si_cat(out, at, item_id[w->item[i]], cap);
        at = si_cat(out, at, w->consent & 1u << w->item[i] ? " yes\n" : " no\n", cap);
    }
    return whole(out, at, cap);
}

/* the value after "key " on its own line of a record, copied to v (cap bytes); 0 if absent */
static int field(const char *s, unsigned n, const char *key, char *v, unsigned cap)
{
    unsigned kl = si_len(key);
    for (unsigned i = 0; i < n;) {
        unsigned e = i;
        while (e < n && s[e] != '\n')
            e++;
        if (e - i > kl && s[i + kl] == ' ') {
            unsigned k = 0;
            while (k < kl && s[i + k] == key[k])
                k++;
            if (k == kl) {
                unsigned m = 0;
                for (unsigned j = i + kl + 1; j < e && m + 1 < cap; j++)
                    v[m++] = s[j];
                v[m] = 0;
                if (m == 1 && v[0] == '-')
                    v[0] = 0;
                return 1;
            }
        }
        i = e + 1;
    }
    return 0;
}

static int field_num(const char *s, unsigned n, const char *key, uint32_t *out)
{
    char v[12];
    if (!field(s, n, key, v, sizeof v) || !v[0])
        return 0;
    uint32_t x = 0;
    for (unsigned i = 0; v[i]; i++) {
        if (v[i] < '0' || v[i] > '9' || x > 100000000u)
            return 0;
        x = x * 10 + (uint32_t)(v[i] - '0');
    }
    *out = x;
    return 1;
}

static int saved_ok(const char *s, unsigned n) { return s && n >= 16 && starts(s, "aienos-setup v1\n"); }

int sw_saved_done(const char *saved, unsigned n)
{
    uint32_t step;
    return saved_ok(saved, n) && field_num(saved, n, "step", &step) && step == SW_DONE;
}

/* where a restart carries on: a step inside a question goes back to the question */
static int resume_at(uint32_t step)
{
    switch (step) {
    case SW_OTHER:
    case SW_OTHER_CHECK:
        return SW_INTENT;
    case SW_EACH:
        return SW_CONSENT;
    case SW_SERVICE:
    case SW_URL:
    case SW_KEY:
    case SW_TESTING:
    case SW_TEST_FAILED:
        return SW_LLM;
    default:
        return step < SW_STEPS ? (int)step : SW_HELLO;
    }
}

void sw_start(sw_wizard *w, const si_inv *inv, int courier, const char *saved, unsigned n, uint32_t now)
{
    zero(w, sizeof *w);
    w->inv = inv;
    w->courier = courier;
    w->step_t = now;
    find_models(w);
    uint32_t step = 0, want = 0, consent = 0, asked = 1, kind = 0;
    if (saved_ok(saved, n) && field_num(saved, n, "step", &step) && step > SW_HELLO && step < SW_DONE) {
        field_num(saved, n, "want", &want);
        field_num(saved, n, "consent", &consent);
        field_num(saved, n, "asked", &asked);
        field_num(saved, n, "llm", &kind);
        w->want = want & 15u;
        w->consent = consent & ((1u << SW_I_COUNT) - 1);
        w->consent_asked = asked <= 2 ? (int)asked - 1 : 0;
        if (kind <= SW_LLM_FETCH)
            w->llm_kind = (uint8_t)kind;
        field(saved, n, "name", w->llm_name, sizeof w->llm_name);
        field(saved, n, "url", w->llm_url, sizeof w->llm_url);
        plan_items(w);
        w->resumed = resume_at(step);
        event(w, "resumed at ", sw_step_name(w->resumed));
    }
    w->step = SW_HELLO;
    event(w, "step ", "hello");
    render(w);
}

unsigned sw_record(const sw_wizard *w, char *out, unsigned cap)
{
    unsigned n = sw_save(w, out, cap);
    unsigned m = n ? sw_llm_conf(w, out + n, cap - n) : 0; /* 0 too: no AI picked yet */
    unsigned c = n ? sw_consent(w, out + n + m, cap - n - m) : 0;
    if (!n || !c) {
        if (cap)
            out[0] = 0;
        return 0;
    }
    return n + m + c;
}
