/* setup_wizard.h -- Allen's first-run setup, as a conversation in his window.
 *
 * The screens follow docs/FIRST_RUN_UX.md (L266, draft 1), screens 3 to 9:
 *
 *   hello -> the one question (games / an AI model / something else) ->
 *   getting to know this computer (what setup_inv.h found, line by line) ->
 *   the one consent (Set it up / Show me each one / Not now) ->
 *   the intent path (games: screen, graphics, processor, what cannot be
 *   measured yet; AI: the graphics memory and which models fit, from their
 *   measured sizes on disk) -> which AI (found here, found nearby, or
 *   connect my own: Claude, ChatGPT, another service; the key is tested with
 *   one request) -> "Allen is ready".
 *
 * Every question can be skipped (Escape is "Not now"), and every step is
 * saved (sw_save) so a restart carries on from the step it stopped at.
 *
 * Pure: no screen, no disk, no wire. The caller feeds keys and the clock, and
 * takes back what to show (sw_view), lines for the log (sw_event), a test
 * request for a service key (sw_test_wanted / sw_test_done), and at the end
 * the records the driver builder reads (sw_llm_conf, sw_consent; L268).
 * The key the person types lives only in this struct; sw_forget_key wipes it
 * once the test is done, and it is never written into any record.
 *
 * Freestanding: no libc, no heap. Host tested in svc/tests/setup_wizard_test.c. */
#ifndef AIENOS_SETUP_WIZARD_H
#define AIENOS_SETUP_WIZARD_H
#include <stdint.h>
#include "setup_inv.h"

#define SW_SAY 160u     /* Allen's words for a screen */
#define SW_LINE 96u
#define SW_LINES 8u     /* panel lines under his words */
#define SW_CHOICE 32u
#define SW_CHOICES 5u
#define SW_TEXT 200u    /* what the person types (a key, an address, a sentence) */
#define SW_ITEMS 5u     /* things in the consent list */
#define SW_CANDS 6u     /* AI models to choose from */
#define SW_EVENTS 8u

enum sw_key {
    SW_K_UP = 1,
    SW_K_DOWN,
    SW_K_LEFT,
    SW_K_RIGHT,
    SW_K_TAB,
    SW_K_ENTER,
    SW_K_ESC,
    SW_K_BACK,
    SW_K_CHAR, /* c: a printable character */
};

enum sw_step {
    SW_HELLO = 0,
    SW_INTENT,
    SW_OTHER,       /* typing: what they want, in their words */
    SW_OTHER_CHECK, /* That's right / Not quite */
    SW_SCAN,
    SW_CONSENT,
    SW_EACH,        /* the consent list, one item at a time */
    SW_GAMES,
    SW_AI,
    SW_LLM,
    SW_SERVICE,     /* Claude / ChatGPT / Another service */
    SW_URL,         /* typing: another service's address */
    SW_KEY,         /* typing: the key, shown as dots */
    SW_TESTING,     /* the one test request is out */
    SW_TEST_FAILED,
    SW_READY,
    SW_DONE,
    SW_STEPS
};

/* what they want the computer for (bits) */
enum { SW_WANT_GAMES = 1, SW_WANT_AI = 2, SW_WANT_OTHER = 4, SW_WANT_ASKED = 8 /* the question was answered or skipped */ };

/* things that change the machine, each asked once (the consent list) */
enum sw_item { SW_I_DRIVERS = 0, SW_I_SCREEN, SW_I_CORES, SW_I_AI_GPU, SW_I_BUILD, SW_I_COUNT };

/* where the chosen AI is */
enum sw_llm_kind {
    SW_LLM_NONE = 0,
    SW_LLM_LOCAL,  /* a model or server on this computer (or its host) */
    SW_LLM_PEER,   /* another AienOS computer nearby */
    SW_LLM_CLAUDE,
    SW_LLM_OPENAI,
    SW_LLM_OTHER,  /* any OpenAI-compatible service */
    SW_LLM_FETCH,  /* "get one for me": to be downloaded */
};

typedef struct {
    char name[SI_NAME];    /* what it is called where it was found */
    char label[SW_CHOICE]; /* its choice on screen */
    char hint[SW_LINE];    /* one plain line about it */
    char url[SI_URL];      /* where it answers ("" for a file not served yet) */
    uint8_t kind;          /* SW_LLM_LOCAL or SW_LLM_PEER */
    uint8_t src;           /* enum si_model_src */
} sw_cand;

/* what the window shows now */
typedef struct {
    char say[SW_SAY];               /* Allen's words */
    char line[SW_LINES][SW_LINE];   /* the panel, eased in one after another */
    unsigned nline;
    char choice[SW_CHOICES][SW_CHOICE];
    unsigned nchoice;
    int lit;                        /* the lit choice */
    char hint[SW_LINE];             /* the lit choice's line, under the row */
    char aside[64];                 /* small, under everything ("Not now: Escape") */
    char prompt[64];                /* typing: the bar's hint; "" = choices */
    char typed[SW_TEXT];            /* what is typed, as shown (dots for a key) */
    unsigned ntyped;
    unsigned gen;                   /* +1 on every change */
    unsigned said_gen;              /* +1 when say changes */
} sw_view;

typedef struct {
    sw_view v;
    const si_inv *inv;
    int step;
    uint32_t step_t;   /* when this step began, ms */
    unsigned want;     /* SW_WANT_* */
    char wish[SW_TEXT];/* their own words, from "Something else" */
    /* the consent list */
    uint8_t item[SW_ITEMS]; /* enum sw_item, in the order shown */
    unsigned nitem, each;   /* each: the item being asked in SW_EACH */
    unsigned consent;       /* bit per sw_item: yes */
    int consent_asked;      /* 0 not yet, 1 answered, -1 Not now */
    /* the AI */
    sw_cand cand[SW_CANDS];
    unsigned ncand;
    int listing;            /* SW_LLM: the full list (after "Choose another") */
    uint8_t llm_kind;
    char llm_name[SI_NAME];
    char llm_url[SI_URL];
    int service;            /* SW_LLM_CLAUDE / OPENAI / OTHER being connected */
    char key[SW_TEXT];      /* the key: here only, wiped by sw_forget_key */
    unsigned nkey;
    char text[SW_TEXT];     /* the line being typed (not the key) */
    unsigned ntext;
    int courier;            /* a wire to the host can carry the key test */
    int test_out;           /* a test request is wanted (1) or out (2) */
    uint32_t test_t;
    char why[SW_LINE];      /* what the service said when the test failed */
    int resumed;
    /* the log */
    char ev[SW_EVENTS][SW_LINE];
    unsigned ev_head, ev_n;
} sw_wizard;

/* Starts at hello (or at a saved step: saved = sw_save's text, n bytes;
 * NULL for a first start). courier: 1 when a wire to the host is there to
 * test a key (without one, Allen says he cannot keep a key safely yet). */
void sw_start(sw_wizard *w, const si_inv *inv, int courier, const char *saved, unsigned n, uint32_t now_ms);
/* A key. Returns 1 when the view changed. */
int sw_key(sw_wizard *w, int key, char c, uint32_t now_ms);
/* The clock: steps that move on by themselves (hello, the scan, the 10 s
 * auto-pick of a single model, a test that never answers). 1 when the view changed. */
int sw_tick(sw_wizard *w, uint32_t now_ms);
/* The test request for a service key: 1 when one is wanted and fills kind
 * ("anthropic" or "openai"), base_url, model and the key, then marks it out.
 * 0 when none is wanted. */
int sw_test_wanted(sw_wizard *w, const char **kind, const char **base_url, const char **model, const char **key);
/* Its answer: ok, or what went wrong (plain words, may be NULL). */
void sw_test_done(sw_wizard *w, int ok, const char *why, uint32_t now_ms);
/* Wipes the key (after the test, or when leaving the key screens). */
void sw_forget_key(sw_wizard *w);
const sw_view *sw_view_of(const sw_wizard *w);
int sw_step_of(const sw_wizard *w);
const char *sw_step_name(int step);
/* The next line for the log (no words the person typed): 1 and out filled, or 0. */
int sw_event(sw_wizard *w, char *out, unsigned cap);

/* Records (text, one item per line; each returns its length, 0 if cap is short):
 *   sw_save:    "aienos-setup v1" then step, want, consent, asked, llm kind/name/url
 *               (enough to carry on after a restart; never the key, never the wish)
 *   sw_llm_conf: what L268 reads: kind, base_url, model, key_env (never the key)
 *   sw_consent:  "aienos-consent v1" then one "<item> yes|no" per thing asked */
unsigned sw_save(const sw_wizard *w, char *out, unsigned cap);
unsigned sw_llm_conf(const sw_wizard *w, char *out, unsigned cap);
unsigned sw_consent(const sw_wizard *w, char *out, unsigned cap);
/* The three in one text, as kept on disk (svc/setup_store.h): sw_save, then
 * sw_llm_conf (absent until an AI is picked), then sw_consent. 0 if cap is short. */
unsigned sw_record(const sw_wizard *w, char *out, unsigned cap);
/* 1 when a saved record says setup ran to the end. */
int sw_saved_done(const char *saved, unsigned n);

/* The model sizes Allen knows, measured on disk (du -sm on the Spark,
 * 2026-10-20): weights only; the working memory on top is not measured. */
#define SW_MIB_27B 29460u   /* qwen3.8-27b-fp8 */
#define SW_MIB_30B 29751u   /* qwen3-30b-a3b-instruct-2507-fp8 */
#define SW_MIB_4B 7688u     /* qwen3-4b-instruct-2507 */
#endif
