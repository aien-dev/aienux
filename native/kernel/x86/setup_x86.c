/* setup_x86.c -- Allen's setup wizard (svc/setup_wizard.h) in his window on
 * the x86 kernel:
 *
 *   1. what this computer is: in the launcher image (CK_X86_HOSTDESC=1) the
 *      host description a launcher hands over as a QEMU fw_cfg file
 *      (svc/setup_inv.h), then in every image what this kernel finds itself
 *      (display controllers on PCI, the screen mode the firmware set, the
 *      processor's name and the cores that started, the RAM in the memory
 *      map, the SATA disks);
 *   2. the saved record on Allen's disk section (svc/setup_store.h): a
 *      finished setup does not start again; an unfinished one carries on at
 *      its step. Saved after every step;
 *   3. keys come here before the line editor while the wizard is up, so
 *      nothing typed into it is echoed or logged (the key above all);
 *   4. API keys are never submitted to the unencrypted courier. A requested
 *      key test fails locally and erases the key; configure credentials on
 *      the host instead. The discovered local model needs no key here.
 *
 * The console (COM1) gets "wizard: <event>" lines only (sw_event: steps and
 * choices, never what was typed). */
#include "../core/ck_internal.h"
#include "../svc/courier.h"
#include "usb_hid.h"
#include "../svc/setup_inv.h"
#include "../svc/setup_store.h"
#include "../svc/setup_wizard.h"
#include "../ui/setup_view.h"
#include "x86.h"

static si_inv inv;
static sw_wizard wiz;
static ck_setup_store store;
static const disk_dev *part;
static const ck_courier *wire;
static uint64_t hz, t0;
static int on;
static int pending;       /* open, waiting for Allen to form before the first word */
static unsigned saved_n;  /* the saved record to carry on from (0: a first start) */
static unsigned said = ~0u, gen = ~0u;
static char text[CK_SETUP_TEXT + 1];
#if CK_X86_HOSTDESC
static char host[4096];
#endif

static uint32_t now_ms(void) { return (uint32_t)((ck_rdtsc() - t0) * 1000u / hz); }

/* The processor's own name (CPUID 0x80000002..4), leading spaces dropped. */
static void cpu_name(char out[49])
{
    uint32_t r[4];
    out[0] = 0;
    ck_cpuid(0x80000000u, 0, r);
    if (r[0] < 0x80000004u)
        return;
    for (uint32_t l = 0; l < 3; l++) {
        ck_cpuid(0x80000002u + l, 0, r);
        memcpy(out + 16 * l, r, 16);
    }
    out[48] = 0;
    unsigned s = 0;
    while (out[s] == ' ')
        s++;
    for (unsigned i = 0; s && i + s < 49; i++)
        out[i] = out[i + s];
}

static void gather(const struct ck_x86_setup_facts *f)
{
    si_init(&inv);
#if CK_X86_HOSTDESC
    unsigned n = 0;
    ck_puts("wizard: launcher image: the setup wizard reads the host description (QEMU fw_cfg, read-only)\n");
    int rc = ck_x86_fwcfg_file(f->rsdp, "opt/aienos/host", host, sizeof host, &n);
    if (rc >= 0) {
        unsigned items = si_parse(&inv, host, n);
        ck_printf("wizard: host description %u bytes%s, %u items, %u not understood\n", n, rc ? " (cut)" : "", items,
                  inv.skipped);
    } else {
        ck_puts("wizard: no host description; only what this computer says about itself\n");
    }
#else
    ck_puts("wizard: only what this computer says about itself\n");
#endif
    for (unsigned i = 0;; i++) {
        uint16_t vendor, device;
        uint32_t cls;
        if (ck_x86_pci_fn(i, &vendor, &device, &cls))
            break;
        inv.npci++;
        if ((cls >> 16) == 0x03)
            si_add_gpu(&inv, vendor, device, cls);
    }
    if (!inv.ndisplay && f->screen_w && f->screen_h)
        si_add_display(&inv, (uint16_t)f->screen_w, (uint16_t)f->screen_h);
    char cpu[49];
    cpu_name(cpu);
    if (f->cores)
        si_add_cpus(&inv, f->cores, cpu[0] ? cpu : "this processor");
    si_add_ram(&inv, f->ram_bytes);
    for (unsigned i = 0;; i++) {
        const char *model = 0;
        const disk_dev *d = ck_x86_ahci_dev(i, &model);
        if (!d)
            break;
        si_add_disk(&inv, model && model[0] ? model : "a disk", d->block_count * d->block_size, SI_DISK_UNKNOWN);
    }
    ck_printf("wizard: found %u graphics, %u screen%s, %u PCI functions, %u disk%s, %u model%s, %u nearby\n", inv.ngpu,
              inv.ndisplay, inv.ndisplay == 1 ? "" : "s", inv.npci, inv.ndisk, inv.ndisk == 1 ? "" : "s", inv.nmodel,
              inv.nmodel == 1 ? "" : "s", inv.npeer);
}

static void events(void)
{
    char ev[SW_LINE];
    while (sw_event(&wiz, ev, sizeof ev))
        ck_printf("wizard: %s\n", ev);
}

/* The step, then llm.conf and the consent record, in one copy (setup_store.h). */
static void save(void)
{
    if (!part)
        return;
    unsigned n = sw_record(&wiz, text, CK_SETUP_TEXT + 1);
    if (!n) {
        ck_puts("wizard: the record did not fit; not saved\n");
        return;
    }
    int rc = ck_setup_save(&store, text, n);
    if (rc) {
        ck_printf("wizard: saving the step FAILED (rc=%d); setup carries on without it\n", rc);
        part = 0;
        return;
    }
    ck_printf("wizard: saved at %s (generation %llu, %u bytes)\n", sw_step_name(sw_step_of(&wiz)),
              (unsigned long long)store.gen, n);
}

static void show(void)
{
    const sw_view *v = sw_view_of(&wiz);
    int done = sw_step_of(&wiz) == SW_DONE;
    if (v->gen == gen && !done)
        return;
    gen = v->gen;
    ck_setup_show(v, &said, done, ck_rdtsc());
    if (done) {
        on = 0;
        ck_puts("wizard: finished; Allen's window is his own again\n");
    }
}

/* The first screen once Allen can speak: its timers (hello moves on after 6 s)
 * count from his first word, not from the window opening. */
static void begin(void)
{
    if (!pending || !ck_allen_ui_formed(ck_rdtsc()))
        return;
    pending = 0;
    t0 = ck_rdtsc();
    sw_start(&wiz, &inv, wire != 0, saved_n ? text : 0, saved_n, now_ms());
    said = gen = ~0u;
    events();
    show();
}

int ck_x86_setup_open(const struct ck_x86_setup_facts *f, const struct ck_courier *c, uint64_t tsc_hz, int again)
{
    unsigned n = 0;
    int rc = 1;
    hz = tsc_hz;
    t0 = ck_rdtsc();
    wire = c;
    part = ck_x86_memory_part();
    if (part) {
        rc = ck_setup_load(&store, part, text, sizeof text, &n);
        if (rc < 0) {
            ck_printf("wizard: the saved record could not be read (rc=%d); setup starts fresh and is not kept\n", rc);
            part = 0;
        }
    } else {
        ck_puts("wizard: no room on Allen's disk section this start; setup is not kept\n");
    }
    if (!part && !again) { /* it could not remember being finished: it would come back every start */
        ck_puts("wizard: not shown, nowhere to keep it this start (\"set up again\" opens it)\n");
        return 0;
    }
    /* By itself only at the first start, or to resume a setup saved part way
     * (a power cut); a later start with nothing saved leaves Allen's window to him. */
    if (rc == 1 && !again && f->starts > 1) {
        ck_printf("wizard: not shown, start %u and nothing half done (\"set up again\" opens it)\n",
                  (unsigned)f->starts);
        return 0;
    }
    if (rc == 0 && !again && sw_saved_done(text, n)) {
        ck_printf("wizard: setup finished on an earlier start (generation %llu); not shown\n",
                  (unsigned long long)store.gen);
        return 0;
    }
    gather(f);
    saved_n = rc == 0 && !again ? n : 0;
    on = pending = 1; /* keys before Allen has formed are taken and dropped */
    begin();
    return 1;
}

int ck_x86_setup_on(void) { return on; }

int ck_x86_setup_key(int kind, char c)
{
    static const uint8_t map[] = {
        [CK_KEY_CHAR] = SW_K_CHAR, [CK_KEY_ENTER] = SW_K_ENTER, [CK_KEY_BACKSPACE] = SW_K_BACK,
        [CK_KEY_ESCAPE] = SW_K_ESC, [CK_KEY_UP] = SW_K_UP,       [CK_KEY_DOWN] = SW_K_DOWN,
        [CK_KEY_LEFT] = SW_K_LEFT,  [CK_KEY_RIGHT] = SW_K_RIGHT, [CK_KEY_TAB] = SW_K_TAB,
    };
    if (!on)
        return 0;
    if (pending)
        return 1;
    if (kind <= 0 || (unsigned)kind >= sizeof map || !map[kind])
        return 1; /* anything else is taken and dropped while the wizard is up */
    int step = sw_step_of(&wiz);
    sw_key(&wiz, map[kind], c, now_ms());
    events();
    if (sw_step_of(&wiz) != step)
        save();
    show();
    return 1;
}

/* Old courier key-test replies cannot change setup: no key test is sent. */
int ck_x86_setup_line(const char *line)
{
    size_t n = strlen(line);
    return (n >= 6 && !memcmp(line, "KEYOK ", 6)) || (n >= 7 && !memcmp(line, "KEYBAD ", 7));
}

void ck_x86_setup_step(void)
{
    if (!on)
        return;
    if (pending) {
        begin();
        return;
    }
    int step = sw_step_of(&wiz);
    sw_tick(&wiz, now_ms());
    const char *kind, *url, *model, *key;
    if (sw_test_wanted(&wiz, &kind, &url, &model, &key)) {
        sw_test_done(&wiz, 0, "Set the service key on the host, then choose its local model here.", now_ms());
    }
    events();
    if (sw_step_of(&wiz) != step)
        save();
    show();
}
