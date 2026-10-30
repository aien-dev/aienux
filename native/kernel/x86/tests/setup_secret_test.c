/* setup_secret_test.c -- the setup wizard must never send its API key to the courier, and a key
 * test must fail locally and erase the key.
 *
 * The real wizard (svc/setup_wizard.c) and the real iMac adapter (x86/setup_x86.c) are compiled
 * into this file. The courier is a mock that counts every line it is asked to send and keeps
 * a copy; the screen and the disk record are stubs. The wizard is put at its key-testing step
 * with a key held (as the real key typing leaves it), then the adapter's step function runs the
 * way the kernel's loop calls it. Built for x86-64 (the adapter reads rdtsc).
 * Prints "SETUP_SECRET: PASS" or "SETUP_SECRET: FAIL <why>". */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include SETUP_X86_C

/* ---- stubs for what the adapter calls outside the wizard ---- */
int ck_snprintf(char *buf, size_t n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}
void ck_printf(const char *fmt, ...) { (void)fmt; }
void ck_puts(const char *s) { (void)s; }
int ck_setup_save(ck_setup_store *st, const char *t, unsigned n)
{
    (void)st, (void)t, (void)n;
    return 0;
}
int ck_setup_load(ck_setup_store *st, const disk_dev *p, char *t, unsigned cap, unsigned *n)
{
    (void)st, (void)p, (void)t, (void)cap, (void)n;
    return 1;
}
void ck_allen_ui_input(const char *buf, unsigned len, uint64_t now) { (void)buf, (void)len, (void)now; }
void ck_allen_ui_said(const char *line, const char *reply, uint64_t now) { (void)line, (void)reply, (void)now; }
void ck_allen_ui_panel(const ck_allen_ui_panel_view *p, uint64_t now) { (void)p, (void)now; }
const disk_dev *ck_x86_memory_part(void) { return 0; }
int ck_allen_ui_formed(uint64_t now)
{
    (void)now;
    return 1;
}
int ck_x86_fwcfg_file(uint64_t rsdp, const char *name, char *buf, unsigned cap, unsigned *n)
{
    (void)rsdp, (void)name, (void)buf, (void)cap, (void)n;
    return -1;
}
int ck_x86_pci_fn(unsigned i, uint16_t *v, uint16_t *d, uint32_t *c)
{
    (void)i, (void)v, (void)d, (void)c;
    return -1;
}
const disk_dev *ck_x86_ahci_dev(unsigned i, const char **model)
{
    (void)i, (void)model;
    return 0;
}

/* ---- the courier mock ---- */
static unsigned n_sent;
static char sent_copy[8][512];
static int m_ready(void) { return 1; }
static int m_send(const char *line, unsigned n)
{
    if (n_sent < 8) {
        unsigned k = n < sizeof sent_copy[0] - 1 ? n : (unsigned)sizeof sent_copy[0] - 1;
        memcpy(sent_copy[n_sent], line, k);
        sent_copy[n_sent][k] = 0;
    }
    n_sent++;
    return 0; /* the wire took it */
}
static int m_take(char *out, unsigned cap)
{
    (void)out, (void)cap;
    return 0;
}
static const ck_courier mock_wire = {m_ready, m_send, m_take};

static unsigned failures;
#define EXPECT(cond, ...)                                                                                          \
    do {                                                                                                           \
        if (!(cond)) {                                                                                             \
            failures++;                                                                                            \
            printf("SETUP_SECRET: FAIL ");                                                                         \
            printf(__VA_ARGS__);                                                                                   \
            printf("\n");                                                                                          \
        }                                                                                                          \
    } while (0)

#define SECRET "sk-live-0123456789abcdef"

static int key_bytes_left(const sw_wizard *w)
{
    for (unsigned i = 0; i < sizeof w->key; i++)
        if (w->key[i])
            return 1;
    return 0;
}

/* The wizard just after the user pressed Enter on a typed key: testing, a request wanted. */
static void at_key_test(int service, const char *url)
{
    static si_inv inv;
    memset(&inv, 0, sizeof inv);
    memset(&wiz, 0, sizeof wiz);
    sw_start(&wiz, &inv, 1, 0, 0, 0);
    wiz.step = SW_TESTING;
    wiz.test_out = 1;
    wiz.service = service;
    strcpy(wiz.llm_url, url);
    strcpy(wiz.key, SECRET);
    wiz.nkey = (unsigned)strlen(SECRET);
    n_sent = 0;
    memset(sent_copy, 0, sizeof sent_copy);
    wire = &mock_wire;
    on = 1;
    pending = 0;
    t0 = ck_rdtsc();
    hz = 1000000000u;
}

static void run_service(int service, const char *url, const char *name)
{
    at_key_test(service, url);
    EXPECT(wiz.nkey == strlen(SECRET) && key_bytes_left(&wiz), "%s: setup did not hold the key", name);
    ck_x86_setup_step();
    EXPECT(n_sent == 0, "%s: the adapter sent %u line(s) to the courier (first: '%.12s...')", name, n_sent,
           sent_copy[0]);
    for (unsigned i = 0; i < n_sent && i < 8; i++)
        EXPECT(!strstr(sent_copy[i], SECRET) && strncmp(sent_copy[i], "KEYTEST", 7), "%s: a key test line went out", name);
    EXPECT(wiz.nkey == 0 && !key_bytes_left(&wiz), "%s: the key is still held after the local refusal (nkey %u)", name, wiz.nkey);
    EXPECT(sw_step_of(&wiz) == SW_TEST_FAILED, "%s: step is %s, want TEST_FAILED", name, sw_step_name(sw_step_of(&wiz)));
    EXPECT(wiz.test_out == 0, "%s: a test is still marked outstanding", name);
    const sw_view *v = sw_view_of(&wiz);
    EXPECT(!strstr(v->say, SECRET) && !strstr(v->typed, SECRET), "%s: the screen shows the key", name);
    EXPECT(wiz.why[0] != 0, "%s: no reason is shown for the refusal", name);
    /* the key is not in the wizard struct at all */
    EXPECT(memmem(&wiz, sizeof wiz, SECRET, sizeof SECRET - 1) == 0, "%s: the key is somewhere in the wizard state", name);
    /* a second pass does nothing more */
    ck_x86_setup_step();
    EXPECT(n_sent == 0, "%s: a later pass sent a line", name);
}

static void late_replies(void)
{
    /* an old KEYOK / KEYBAD from a host helper must not change the wizard or leak: it is taken and dropped */
    at_key_test(SW_LLM_OPENAI, "http://127.0.0.1:18091");
    wiz.test_out = 2; /* a test counted as outstanding */
    static sw_wizard before;
    memcpy(&before, &wiz, sizeof wiz);
    EXPECT(ck_x86_setup_line("KEYOK 1 some-model") == 1, "a KEYOK line was passed on as an ordinary answer");
    EXPECT(ck_x86_setup_line("KEYBAD 1 nope") == 1, "a KEYBAD line was passed on as an ordinary answer");
    EXPECT(!memcmp(&before, &wiz, sizeof wiz), "an old key-test reply changed the wizard");
    EXPECT(ck_x86_setup_line("SAY hello") == 0, "an ordinary answer line was swallowed");
}

int main(void)
{
    run_service(SW_LLM_OPENAI, "http://127.0.0.1:18091", "openai-style");
    run_service(SW_LLM_CLAUDE, "https://api.anthropic.com", "claude");
    run_service(SW_LLM_OTHER, "http://192.168.1.50:8000", "another service");
    late_replies();
    if (failures)
        return 1;
    printf("SETUP_SECRET: PASS\n");
    return 0;
}
