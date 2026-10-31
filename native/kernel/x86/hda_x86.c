/* hda_x86.c -- the sound controller (Intel High Definition Audio): plays the
 * night's chirp (ui/chirp.c) through the speakers, in step with the sky.
 *
 * Per controller: stop any DMA the firmware left running (a startup chime may
 * leave the command rings or a stream running into memory the kernel now
 * owns), then reset it: STATESTS cleared, GCTL.CRST held at 0 for 1 ms, set
 * to 1, then 1 ms for the codecs to ask for an address before STATESTS is
 * read (Linux's reset_link: at least 100 us in reset, 1 ms after). Codecs are
 * then spoken to through the command rings (CORB out, RIRB back, 256 entries
 * each in one page, polled; Linux's snd_hdac_bus_init_cmd_io); the immediate
 * command registers are the fallback, and they went unanswered on the first
 * real machine.
 *
 * Each codec's audio function group goes onto the report, one line per
 * widget, and is searched for output pins (speaker first, then line out and
 * headphone; never digital, never "no connection") with a path back through
 * mixers and selectors to a converter (DAC). On the first controller with
 * such a path, every widget on every path is powered (D0), routed, unmuted at
 * 0 dB, the pins driven and their external amplifiers switched on (EAPD; on
 * Apple boards also every codec GPIO, which Linux's Cirrus fixups use as the
 * speaker and headphone amplifier enables); Cirrus CS4206/7/8 get their
 * vendor set-up words (Linux cs420x.c); a Cirrus CS8409 bridge on an Apple
 * board gets its own set-up instead (cs8409_apple, below). Then the first
 * output stream plays the chirp from the night's current moment, 16-bit
 * stereo at 48 kHz (44.1 kHz 24-bit behind a CS8409), and HDA_TAIL_MS of
 * silence after it. A stream loops by design (the controller
 * has no play-once), so ck_x86_hda_step stops it once the chirp is over.
 * Register offsets and bits: Linux's hda_register.h and hda_verbs.h; the
 * Intel PCI settings are in pci_x86.c (docs-read records). */
#include "../core/ck_internal.h"
#include "../mm/frames.h"
#include "../ui/chirp.h"
#include "x86.h"

#define GCAP 0x00
#define VMIN 0x02
#define VMAJ 0x03
#define GCTL 0x08
#define STATESTS 0x0e
#define CORBLBASE 0x40
#define CORBUBASE 0x44
#define CORBWP 0x48
#define CORBRP 0x4a
#define CORBCTL 0x4c
#define CORBSIZE 0x4e
#define RIRBLBASE 0x50
#define RIRBUBASE 0x54
#define RIRBWP 0x58
#define RINTCNT 0x5a
#define RIRBCTL 0x5c
#define RIRBSTS 0x5d
#define RIRBSIZE 0x5e
#define IC 0x60
#define IR 0x64
#define IRS 0x68
#define IRS_BUSY 1u
#define IRS_VALID 2u
#define RING_RST 0x8000u /* CORBRP and RIRBWP: reset the pointer */
#define SD0 0x80         /* stream descriptors, 0x20 apart: input, output, then bidirectional */
#define SD_STS 0x03
#define SD_LPIB 0x04
#define SD_CBL 0x08
#define SD_LVI 0x0c
#define SD_FMT 0x12
#define SD_BDLPL 0x18
#define SD_BDLPU 0x1c
#define SRST 1u /* SD_CTL: stream in reset */
#define RUN 2u  /* CORBCTL, RIRBCTL and SD_CTL: the DMA engine runs */

/* codec verbs (12-bit verbs carry 8 bits; the 4-bit ones 0x2-0x5 carry 16) */
#define V_FORMAT 0x200
#define V_AMP 0x300
#define V_PROC_COEF 0x400
#define V_COEF_INDEX 0x500
#define V_CONNECT_SEL 0x701
#define V_PROC_STATE 0x703
#define V_POWER 0x705
#define V_STREAM 0x706
#define V_PIN_CTL 0x707
#define V_EAPD 0x70c
#define V_GPIO_DATA 0x715
#define V_GPIO_MASK 0x716
#define V_GPIO_DIR 0x717
#define V_PARAM 0xf00
#define V_CONN_LIST 0xf02
#define V_GET_POWER 0xf05
#define V_GET_EAPD 0xf0c
#define V_CONFIG 0xf1c
#define V_SUBSYSTEM 0xf20
#define P_VENDOR 0x00
#define P_SUBSYSTEM 0x01
#define P_REV 0x02
#define P_NODES 0x04
#define P_FUNC 0x05
#define P_WCAPS 0x09
#define P_PINCAP 0x0c
#define P_AMP_IN 0x0d
#define P_CONN_LEN 0x0e
#define P_GPIO 0x11
#define P_AMP_OUT 0x12
#define W_AUD_OUT 0
#define W_MIX 2
#define W_SEL 3
#define W_PIN 4
#define WC_IN_AMP (1u << 1)
#define WC_OUT_AMP (1u << 2)
#define WC_AMP_OVRD (1u << 3)
#define WC_CONN_LIST (1u << 8)
#define WC_DIGITAL (1u << 9)
#define WC_POWER (1u << 10)
#define PC_HP_DRV (1u << 3)
#define PC_OUT (1u << 4)
#define PC_EAPD (1u << 16)
#define FMT_48K_16_STEREO 0x0011u
#define TAG 1u                /* the stream's tag; the converters listen for it */
#define CHUNK 65536u          /* bytes per buffer descriptor */
#define HDA_TAIL_MS 8000u     /* silence after the chirp, so a late stop never replays it */
#define MAX_NID 128u
#define MAX_PATH 6u
#define MAX_PINS 8u

static volatile uint8_t *g_bar;
static uint64_t g_hz;
static volatile uint32_t *corb; /* 256 commands */
static volatile uint32_t *rirb; /* 256 answers of two words: the answer, then which codec */
static unsigned corb_wp, rirb_rp, rings;
static unsigned g_fmt; /* the stream format: FMT_48K_16_STEREO unless a codec takes only another */

/* the stream once playing: ck_x86_hda_step stops it */
static volatile uint8_t *play_bar;
static unsigned play_sd;
static uint64_t play_t0;

static uint8_t r8(unsigned o) { return g_bar[o]; }
static uint16_t r16(unsigned o) { return *(volatile uint16_t *)(g_bar + o); }
static uint32_t r32(unsigned o) { return *(volatile uint32_t *)(g_bar + o); }
static void w8(unsigned o, uint8_t v) { g_bar[o] = v; }
static void w16(unsigned o, uint16_t v) { *(volatile uint16_t *)(g_bar + o) = v; }
static void w32(unsigned o, uint32_t v) { *(volatile uint32_t *)(g_bar + o) = v; }

static void spin_us(unsigned us)
{
    uint64_t t0 = ck_rdtsc(), n = g_hz / 1000000 * us;
    while (ck_rdtsc() - t0 < n)
        ck_pause();
}

/* waits up to ms for (reg & mask) == want; 0 when it got there */
static int wait8(unsigned o, uint8_t mask, uint8_t want, unsigned ms)
{
    uint64_t t0 = ck_rdtsc();
    while ((r8(o) & mask) != want)
        if (ck_rdtsc() - t0 > g_hz / 1000 * ms)
            return -1;
        else
            ck_pause();
    return 0;
}
static int wait16(unsigned o, uint16_t mask, uint16_t want, unsigned ms)
{
    uint64_t t0 = ck_rdtsc();
    while ((r16(o) & mask) != want)
        if (ck_rdtsc() - t0 > g_hz / 1000 * ms)
            return -1;
        else
            ck_pause();
    return 0;
}

/* Writes the cache lines over [p, p + n) back to memory and drops them, so
 * the controller reads what was written and the CPU then reads what the
 * controller wrote, whether or not its DMA is snooped. */
static void flush(const volatile void *p, size_t n)
{
    uintptr_t a = (uintptr_t)p & ~(uintptr_t)63, e = (uintptr_t)p + n;
    __asm__ volatile("mfence" ::: "memory");
    for (; a < e; a += 64)
        __asm__ volatile("clflush (%0)" ::"r"(a) : "memory");
    __asm__ volatile("mfence" ::: "memory");
}

static int imm_cmd(uint32_t c, uint32_t *out)
{
    if (wait16(IRS, IRS_BUSY, 0, 10))
        return -1;
    w16(IRS, (uint16_t)(r16(IRS) | IRS_VALID)); /* clears an old answer */
    w32(IC, c);
    w16(IRS, (uint16_t)(r16(IRS) | IRS_BUSY));
    if (wait16(IRS, IRS_VALID, IRS_VALID, 10))
        return -1;
    *out = r32(IR);
    return 0;
}

static int ring_cmd(uint32_t c, uint32_t *out)
{
    corb_wp = (corb_wp + 1) & 255u;
    corb[corb_wp] = c;
    flush(&corb[corb_wp], 4);
    w16(CORBWP, (uint16_t)corb_wp);
    uint64_t t0 = ck_rdtsc();
    for (;;) {
        if ((r16(RIRBWP) & 255u) != rirb_rp) {
            rirb_rp = (rirb_rp + 1) & 255u;
            flush(&rirb[2 * rirb_rp], 8);
            uint32_t v = rirb[2 * rirb_rp], ext = rirb[2 * rirb_rp + 1];
            w8(RIRBSTS, 5); /* response and overrun flags cleared: the controller may go on */
            if (ext & 0x10u) /* unsolicited (none are asked for) */
                continue;
            *out = v;
            return 0;
        }
        if (ck_rdtsc() - t0 > g_hz / 1000 * 50) {
            rirb_rp = r16(RIRBWP) & 255u; /* a late answer is not taken for the next one */
            return -1;
        }
        ck_pause();
    }
}

/* one codec command, tried three times; 0 with *out set (when given), or -1 */
static int verb(unsigned a, unsigned nid, unsigned v, unsigned payload, uint32_t *out)
{
    uint32_t c = (uint32_t)a << 28 | (uint32_t)nid << 20 | (uint32_t)v << 8 | payload, dummy;
    for (int k = 0; k < 3; k++)
        if (!(rings ? ring_cmd(c, out ? out : &dummy) : imm_cmd(c, out ? out : &dummy)))
            return 0;
    return -1;
}
static uint32_t param(unsigned a, unsigned nid, unsigned p)
{
    uint32_t v = 0;
    return verb(a, nid, V_PARAM, p, &v) ? 0 : v;
}

/* The rings in one page from fr: CORB at its start, RIRB at +2048, 256
 * entries each (Linux's order and checks). 0 when both run. */
static int rings_start(struct ck_frames *fr, unsigned ctrl)
{
    uint64_t pa;
    if (!fr || ck_frames_alloc(fr, 1, CK_PAGE, &pa))
        return -1;
    memset((void *)(uintptr_t)pa, 0, CK_PAGE);
    flush((void *)(uintptr_t)pa, CK_PAGE);
    corb = (volatile uint32_t *)(uintptr_t)pa;
    rirb = (volatile uint32_t *)(uintptr_t)(pa + 2048);
    w32(CORBLBASE, (uint32_t)pa);
    w32(CORBUBASE, (uint32_t)(pa >> 32));
    w8(CORBSIZE, 0x02);
    w16(CORBWP, 0);
    w16(CORBRP, RING_RST);
    int bad = wait16(CORBRP, RING_RST, RING_RST, 10);
    w16(CORBRP, 0);
    bad |= wait16(CORBRP, RING_RST, 0, 10);
    w8(CORBCTL, RUN);
    w32(RIRBLBASE, (uint32_t)(pa + 2048));
    w32(RIRBUBASE, (uint32_t)((pa + 2048) >> 32));
    w8(RIRBSIZE, 0x02);
    w16(RIRBWP, RING_RST);
    w16(RINTCNT, 1);
    w8(RIRBCTL, RUN | 1u); /* with the response flag (bit 0), cleared after each answer: Linux does the same, and
                                a controller may hold the next command until it is cleared; no interrupt is
                                delivered (INTCTL stays 0) */
    bad |= wait8(CORBCTL, RUN, RUN, 10) | wait8(RIRBCTL, RUN, RUN, 10);
    corb_wp = r16(CORBWP) & 255u;
    rirb_rp = r16(RIRBWP) & 255u;
    if (bad) {
        w8(CORBCTL, 0);
        w8(RIRBCTL, 0);
        ck_printf("sound: ctrl=%u command rings did not start; immediate commands\n", ctrl);
        return -1;
    }
    return 0;
}

/* ---- the codec's map ---- */
struct wid {
    uint32_t caps, pincap, cfg, amp_out, amp_in;
    uint8_t type, nconn, conn[16];
};
static struct wid W[MAX_NID];
static uint8_t path[MAX_PATH], pick[MAX_PATH];
static unsigned plen;

static void read_conns(unsigned a, unsigned nid, struct wid *x)
{
    uint32_t len = param(a, nid, P_CONN_LEN), n = len & 0x7fu, wide = len >> 7 & 1u, per = wide ? 2 : 4;
    unsigned last = 0;
    x->nconn = 0;
    for (uint32_t i = 0; i < n && x->nconn < 16; i += per) {
        uint32_t v;
        if (verb(a, nid, V_CONN_LIST, i, &v))
            return;
        for (uint32_t k = 0; k < per && i + k < n && x->nconn < 16; k++) {
            unsigned e = wide ? v >> (16 * k) & 0xffffu : v >> (8 * k) & 0xffu;
            unsigned range = wide ? e >> 15 : e >> 7, id = wide ? e & 0x7fffu : e & 0x7fu;
            if (range && last) /* "every node from the last one to this one" */
                for (unsigned m = last + 1; m <= id && x->nconn < 16; m++)
                    x->conn[x->nconn++] = (uint8_t)m;
            else
                x->conn[x->nconn++] = (uint8_t)id;
            last = id;
        }
    }
}

static const char *type_name(unsigned t)
{
    static const char *const n[16] = {"dac", "adc", "mix", "sel", "pin", "power", "knob", "beep",
                                      "t8", "t9", "t10", "t11", "t12", "t13", "t14", "vendor"};
    return n[t & 15u];
}

/* One line per widget on the report: what it is, and for pins where it goes. */
static void map_widget(unsigned a, unsigned nid, const struct wid *x)
{
    char line[200];
    int n = ck_snprintf(line, sizeof line, "sound: codec=%u w=0x%02x %s caps=0x%08x", a, nid, type_name(x->type),
                        x->caps);
    if (x->type == W_PIN)
        n += ck_snprintf(line + n, sizeof line - n, " pincap=0x%08x cfg=0x%08x", x->pincap, x->cfg);
    if (x->caps & WC_AMP_OVRD)
        n += ck_snprintf(line + n, sizeof line - n, " amp_out=0x%08x amp_in=0x%08x", x->amp_out, x->amp_in);
    for (unsigned k = 0; k < x->nconn && n < (int)sizeof line - 8; k++)
        n += ck_snprintf(line + n, sizeof line - n, k ? ",0x%02x" : " conn=0x%02x", x->conn[k]);
    ck_printf("%s\n", line);
}

/* depth-first from a pin back to a converter, through mixers and selectors */
static int find_dac(unsigned nid, unsigned depth)
{
    const struct wid *x = &W[nid];
    path[depth] = (uint8_t)nid;
    if (depth && x->type == W_AUD_OUT && !(x->caps & WC_DIGITAL)) {
        plen = depth + 1;
        return 1;
    }
    if (depth + 1 >= MAX_PATH || (depth && x->type != W_MIX && x->type != W_SEL))
        return 0;
    for (unsigned k = 0; k < x->nconn; k++) {
        unsigned c = x->conn[k];
        if (c >= MAX_NID || !W[c].caps)
            continue;
        pick[depth] = (uint8_t)k;
        if (find_dac(c, depth + 1))
            return 1;
    }
    return 0;
}

/* gain at 0 dB (the amp's offset), limited to its steps */
static unsigned zero_db(uint32_t amp)
{
    unsigned off = amp & 0x7fu, steps = amp >> 8 & 0x7fu;
    return off > steps ? steps : off;
}

/* Every widget on the found path made ready to carry the stream. */
static void open_path(unsigned a, unsigned dev, uint32_t afg_out, uint32_t afg_in)
{
    for (unsigned i = 0; i < plen; i++) {
        unsigned nid = path[i];
        const struct wid *x = &W[nid];
        uint32_t ao = x->caps & WC_AMP_OVRD ? x->amp_out : afg_out, ai = x->caps & WC_AMP_OVRD ? x->amp_in : afg_in;
        if (x->caps & WC_POWER)
            verb(a, nid, V_POWER, 0, 0);
        if (i + 1 < plen && x->type != W_MIX && x->nconn > 1)
            verb(a, nid, V_CONNECT_SEL, pick[i], 0);
        if (i + 1 < plen && x->type == W_MIX && (x->caps & WC_IN_AMP))
            verb(a, nid, V_AMP, 0x7000u | (unsigned)pick[i] << 8 | zero_db(ai), 0); /* input, both sides */
        if (x->caps & WC_OUT_AMP)
            verb(a, nid, V_AMP, 0xb000u | zero_db(ao), 0); /* output, both sides */
        if (x->type == W_PIN) {
            verb(a, nid, V_PIN_CTL, 0x40u | (dev == 2 && (x->pincap & PC_HP_DRV) ? 0x80u : 0), 0);
            uint32_t e = 0;
            if ((x->pincap & PC_EAPD) && !verb(a, nid, V_GET_EAPD, 0, &e))
                verb(a, nid, V_EAPD, (e & 0xffu) | 0x02u, 0);
        }
        if (x->type == W_AUD_OUT) {
            verb(a, nid, V_FORMAT, FMT_48K_16_STEREO, 0);
            verb(a, nid, V_STREAM, TAG << 4, 0);
        }
    }
}

/* Cirrus set-up words (Linux cs420x.c cs_errata_init_verbs, cs_coef_init_verbs,
 * cs4208_coef_init_verbs): node, verb, payload. */
static const uint16_t cs420x_words[][3] = {
    {0x11, V_PROC_STATE, 1},    {0x11, V_COEF_INDEX, 0x08}, {0x11, V_PROC_COEF, 0x9999}, {0x11, V_COEF_INDEX, 0x17},
    {0x11, V_PROC_COEF, 0xa412}, {0x11, V_COEF_INDEX, 0x01}, {0x11, V_PROC_COEF, 0x0009}, {0x07, V_POWER, 0},
    {0x08, V_POWER, 0},         {0x11, V_COEF_INDEX, 0x17}, {0x11, V_PROC_COEF, 0x2412}, {0x11, V_COEF_INDEX, 0x08},
    {0x11, V_PROC_COEF, 0},     {0x11, V_COEF_INDEX, 0x01}, {0x11, V_PROC_COEF, 0x0008}, {0x11, V_PROC_STATE, 0},
    {0x11, V_PROC_STATE, 1},    {0x11, V_COEF_INDEX, 0x03}, {0x11, V_PROC_COEF, 0x146a}, {0x11, V_COEF_INDEX, 0x02},
    {0x11, V_PROC_COEF, 0x000a}, {0x11, V_COEF_INDEX, 0x04}, {0x11, V_PROC_COEF, 0x0007},
};
static const uint16_t cs4208_words[][3] = {
    {0x24, V_PROC_STATE, 1}, {0x24, V_COEF_INDEX, 0x33}, {0x24, V_PROC_COEF, 0x0001},
    {0x24, V_COEF_INDEX, 0x34}, {0x24, V_PROC_COEF, 0x1c01},
};

/* ---- Cirrus CS8409 on Apple boards ----
 * The CS8409 is a bridge: its converters feed a TDM bus (several channels
 * time-shared on one wire) to four TAS576x-family speaker amplifiers, which it
 * sets up over its own I2C bus (a two-wire link to small chips). Its
 * converters leave nothing on the pins without that, so the generic path
 * above stays silent. The sequence is the one davidjo/snd_hda_macbookpro
 * replays from macOS for subsystem 106b:1000 (cirrus_apple.h -> new84 ->
 * real84; boot84 helpers), reduced to the speakers: bridge defaults, I2C
 * set-up, the four amplifiers reset into shutdown, then per amplifier pair
 * its TDM slots and clocks, the amplifiers configured and switched on, and
 * the converters synchronised. That driver plays only 44.1 kHz 24-bit
 * (cirrus_apple.h:1300), so the chirp goes out in that format here. The
 * amplifiers' digital volume is CS_AMP_VOLUME, 24 dB under the 0 dB (0xcf,
 * 0.5 dB a step) macOS uses: 12 dB under was very loud on the test computer;
 * 0x9f is the reduction the reference driver also lists. */
#define CS_VENDOR 0x47u        /* the vendor widget holding the coefficients */
#define CS_I2C_ADDR 0x59u      /* coefficient: the chip spoken to */
#define CS_I2C_STS 0x5cu       /* bits 3 and 4 both set: the transfer is done */
#define CS_I2C_WRITE 0x5du     /* register << 8 | value */
#define CS_I2C_READ 0x5eu
#define CS_AMP_VOLUME 0x9fu
#define FMT_441_24_STEREO 0x4031u
#define V_GET_COEF 0xc00
#define V_VENDOR_7F0 0x7f0

static unsigned cs_a, cs_i2c_bad;

static uint32_t coef_get(unsigned idx)
{
    uint32_t v = 0;
    verb(cs_a, CS_VENDOR, V_COEF_INDEX, idx, 0);
    verb(cs_a, CS_VENDOR, V_GET_COEF, 0, &v);
    verb(cs_a, CS_VENDOR, V_COEF_INDEX, 0, 0);
    return v & 0xffffu;
}
static void coef_set(unsigned idx, unsigned v)
{
    verb(cs_a, CS_VENDOR, V_COEF_INDEX, idx, 0);
    verb(cs_a, CS_VENDOR, V_PROC_COEF, v & 0xffffu, 0);
    verb(cs_a, CS_VENDOR, V_COEF_INDEX, 0, 0);
}
/* the bits in mask replaced by bits (new84.h cs_8409_vendor_coef_set_mask) */
static void coef_mask(unsigned idx, unsigned mask, unsigned bits) { coef_set(idx, (coef_get(idx) & ~mask) | bits); }

/* One register write to the chip at I2C address chip (new84.h
 * cs_8409_vendor_i2cWrite, unpaged): clock on (coefficient 0 bit 3), address,
 * register and value, then up to 9 status reads 2 ms apart, clock off. After
 * 13 unconfirmed transfers the rest are skipped. */
static void i2c_write(unsigned chip, unsigned reg, unsigned val)
{
    if (cs_i2c_bad > 12) /* nothing is answering: the rest would only slow the boot */
        return;
    verb(cs_a, CS_VENDOR, V_PROC_STATE, 1, 0);
    coef_set(0, coef_get(0) | 0x8u);
    coef_set(CS_I2C_ADDR, chip);
    coef_set(CS_I2C_WRITE, (reg & 0xffu) << 8 | (val & 0xffu));
    int done = 0;
    for (unsigned k = 0; k < 9 && !done; k++) {
        done = (coef_get(CS_I2C_STS) & 0x18u) == 0x18u;
        if (!done)
            spin_us(2000);
    }
    cs_i2c_bad += !done;
    coef_set(0, coef_get(0) & 0xfff7u);
}

/* The TDM unit's routing refresh after slots change (real84.h configureTDMUR) */
static void cs_tdm_refresh(void)
{
    coef_set(0x6b, 0x001f);
    coef_set(0x71, coef_get(0x71) | 0x400fu);
    verb(cs_a, CS_VENDOR, V_VENDOR_7F0, 0xb6, 0);
}

/* One amplifier configured and switched on (real84.h cs_8409_setup_amp_tas576,
 * playback): I2S input, its TDM slot, analogue gain, fault set-up, volume. */
static void cs_amp_on(unsigned chip, unsigned slot)
{
    static const uint8_t regs[][2] = {{0x02, 0x44}, {0x03, 0x80}, {0x06, 0x55}, {0x08, 0x18},
                                      {0x04, 0},    {0x13, 0x00}, {0x02, 0x44}, {0x01, 0xfd}};
    for (unsigned k = 0; k < sizeof regs / sizeof regs[0]; k++)
        i2c_write(chip, regs[k][0], regs[k][0] == 0x03 ? 0x80u | slot : regs[k][0] == 0x04 ? CS_AMP_VOLUME : regs[k][1]);
}

static unsigned cs8409_apple(unsigned a, unsigned afg, char *say, size_t say_n)
{
    static const uint8_t amps[4] = {0xd8, 0xda, 0xdc, 0xde};
    static const uint8_t amp_reset[][2] = {{0x01, 0xfc}, {0x02, 0x04}, {0x03, 0x80}, {0x04, 0xcf}, {0x06, 0x51},
                                           {0x08, 0x00}, {0x10, 0xff}, {0x11, 0xfc}, {0x13, 0x00}, {0x14, 0x02}};
    uint64_t t0 = ck_rdtsc();
    cs_a = a;
    cs_i2c_bad = 0;
    verb(a, CS_VENDOR, V_PROC_STATE, 1, 0);
    /* bridge defaults: every TDM slot off (boot84.h init_for_node_vendor) */
    coef_set(0x00, 0x9008);
    coef_set(0x01, 0);
    coef_set(0x17, 0);
    coef_set(0x18, 0);
    coef_set(0x02, 0);
    for (unsigned i = 0x19; i <= 0x57; i++)
        coef_set(i, 0x8000);
    coef_set(0x0a, 0);
    coef_set(0x82, 0);
    coef_set(0x6b, 0x001f);
    coef_set(0x6c, 0x001f);
    coef_set(0x82, 0);
    /* I2C set-up (boot84.h) */
    verb(a, CS_VENDOR, V_PROC_STATE, 1, 0);
    coef_set(0x00, 0x9008);
    coef_set(0x02, 0x0080);
    coef_set(0x5b, 0x0010);
    coef_set(0x00, 0x9000);
    verb(a, 0x24, V_PIN_CTL, 0, 0);
    verb(a, 0x25, V_PIN_CTL, 0, 0);
    /* the amplifiers reset into shutdown, GPIOs 1 and 4 driven high around
       each (boot84.h setup_amps_reset_i2c_tas576, setup_gpio_set_10) */
    for (unsigned c = 0; c < 4; c++) {
        verb(a, afg, V_GPIO_DIR, 0x12, 0);
        verb(a, afg, V_GPIO_DATA, 0x12, 0);
        verb(a, afg, V_GPIO_MASK, 0x1f, 0);
        for (unsigned k = 0; k < sizeof amp_reset / sizeof amp_reset[0]; k++)
            i2c_write(amps[c], amp_reset[k][0], amp_reset[k][1]);
    }
    /* amplifiers 1 and 2: converter 0x02 on TDM slots 0 and 1, the bus clocks,
       pin 0x24 (real84.h cs_8409_setup_TDM_amps12, cs_8409_setup_TDM_sample_rate,
       cs_8409_setup_TDM_proper_amps12) */
    verb(a, 0x02, V_FORMAT, FMT_441_24_STEREO, 0);
    verb(a, 0x02, V_STREAM, TAG << 4, 0);
    coef_set(0x01, coef_get(0x01));
    coef_mask(0x01, 0x0380, 0x0200);
    coef_mask(0x05, 0x0007, 0x0001);
    coef_mask(0x04, 0x01ff, 0x08ff);
    coef_set(0x19, 0x0800);
    coef_set(0x1a, 0x0820);
    unsigned saved = coef_get(0x00);
    coef_set(0x00, 0xb000);
    coef_set(0x04, 0x08ff);
    coef_set(0x00, saved);
    coef_set(0x03, 0x8000);
    coef_set(0x05, 0x0001);
    coef_mask(0x82, 0, 0x5400);
    coef_set(0x02, 0x0280);
    coef_set(0x01, coef_get(0x01) | 0x20u);
    cs_tdm_refresh();
    verb(a, 0x24, V_PIN_CTL, 0x40, 0);
    cs_amp_on(0xd8, 0);
    cs_amp_on(0xda, 2);
    /* amplifiers 3 and 4: converter 0x03 on slots 2 and 3, pin 0x25 */
    verb(a, 0x03, V_FORMAT, FMT_441_24_STEREO, 0);
    verb(a, 0x03, V_STREAM, TAG << 4, 0);
    coef_set(0x1b, 0x0840);
    coef_set(0x1c, 0x0860);
    cs_tdm_refresh();
    verb(a, 0x25, V_PIN_CTL, 0x40, 0);
    cs_amp_on(0xdc, 1);
    cs_amp_on(0xde, 3);
    /* the converters synchronised (real84.h cs_8409_sync_converters_on) */
    verb(a, CS_VENDOR, V_PROC_STATE, 1, 0);
    verb(a, 0x02, V_STREAM, 0, 0);
    coef_set(0x17, 0x0001);
    verb(a, 0x03, V_STREAM, 0, 0);
    coef_set(0x17, 0x0003);
    verb(a, 0x02, V_STREAM, TAG << 4, 0);
    verb(a, 0x03, V_STREAM, TAG << 4, 0);
    g_fmt = FMT_441_24_STEREO;
    ck_printf("sound: codec=%u CS8409 bridge set up: 4 amplifiers at 0x%02x volume, %u I2C transfer(s) unconfirmed%s, "
              "coef 0x01=0x%04x 0x17=0x%04x 0x82=0x%04x; %u ms\n", a, CS_AMP_VOLUME, cs_i2c_bad,
              cs_i2c_bad > 12 ? " (gave up)" : "", coef_get(0x01), coef_get(0x17), coef_get(0x82),
              (unsigned)((ck_rdtsc() - t0) * 1000 / g_hz));
    ck_snprintf(say, say_n, "codec %u CS8409 0x24(spk)<-0x02 0x25(spk)<-0x03, amplifiers on", a);
    return 2;
}

/* Maps codec a, opens every output path it has; the number of pins opened
 * (their converters set to the stream's tag), with a summary in *say. */
static unsigned codec_open(unsigned ctrl, unsigned a, char *say, size_t say_n)
{
    uint32_t id = param(a, 0, P_VENDOR), rev = param(a, 0, P_REV), nodes = param(a, 0, P_NODES), afg = 0;
    if (!id) {
        ck_printf("sound: codec=%u no answer (%s)\n", a, rings ? "command rings" : "immediate command");
        return 0;
    }
    for (unsigned k = 0, first = nodes >> 16 & 0xffu; k < (nodes & 0xffu) && k < 8; k++)
        if ((param(a, first + k, P_FUNC) & 0xffu) == 1)
            afg = first + k;
    uint32_t sub = param(a, 0, P_SUBSYSTEM);
    if (afg && !sub)
        verb(a, afg, V_SUBSYSTEM, 0, &sub);
    if (!afg) {
        ck_printf("sound: codec=%u id=%04x:%04x rev=0x%08x no audio function group\n", a, id >> 16, id & 0xffffu, rev);
        return 0;
    }
    verb(a, afg, V_POWER, 0, 0);
    uint64_t t0 = ck_rdtsc();
    uint32_t ps = 0;
    while (!verb(a, afg, V_GET_POWER, 0, &ps) && (ps >> 4 & 15u) && ck_rdtsc() - t0 < g_hz / 20)
        spin_us(100);
    uint32_t wn = param(a, afg, P_NODES), first = wn >> 16 & 0xffu, n = wn & 0xffu;
    uint32_t afg_out = param(a, afg, P_AMP_OUT), afg_in = param(a, afg, P_AMP_IN), gpio = param(a, afg, P_GPIO) & 0xffu;
    ck_printf("sound: codec=%u id=%04x:%04x sub=%04x:%04x rev=0x%08x afg=%u power=D%u widgets=0x%02x..0x%02x gpio=%u "
              "amp_out=0x%08x amp_in=0x%08x\n", a, id >> 16, id & 0xffffu, sub >> 16, sub & 0xffffu, rev, afg,
              ps >> 4 & 15u, first, first + n - 1, gpio, afg_out, afg_in);
    memset(W, 0, sizeof W);
    for (unsigned nid = first; nid < first + n && nid < MAX_NID; nid++) {
        struct wid *x = &W[nid];
        x->caps = param(a, nid, P_WCAPS);
        x->type = (uint8_t)(x->caps >> 20 & 15u);
        if (x->type == W_PIN) {
            x->pincap = param(a, nid, P_PINCAP);
            verb(a, nid, V_CONFIG, 0, &x->cfg);
        }
        if (x->caps & WC_AMP_OVRD) {
            x->amp_out = param(a, nid, P_AMP_OUT);
            x->amp_in = param(a, nid, P_AMP_IN);
        }
        if (x->caps & WC_CONN_LIST)
            read_conns(a, nid, x);
        if (x->type <= 4)
            map_widget(a, nid, x);
    }
    if ((id >> 16) == 0x1013 && ((id & 0xffffu) == 0x4206 || (id & 0xffffu) == 0x4207))
        for (unsigned k = 0; k < sizeof cs420x_words / sizeof cs420x_words[0]; k++)
            verb(a, cs420x_words[k][0], cs420x_words[k][1], cs420x_words[k][2], 0);
    if (id == 0x10134208)
        for (unsigned k = 0; k < sizeof cs4208_words / sizeof cs4208_words[0]; k++)
            verb(a, cs4208_words[k][0], cs4208_words[k][1], cs4208_words[k][2], 0);
    if (id == 0x10138409 && (sub >> 16) == 0x106b && W[0x24].type == W_PIN && W[0x25].type == W_PIN &&
        W[0x02].type == W_AUD_OUT && W[0x03].type == W_AUD_OUT)
        return cs8409_apple(a, afg, say, say_n);
    /* output pins, speakers first: device 1 speaker, 0 line out, 2 headphone */
    static const unsigned order[3] = {1, 0, 2};
    unsigned opened = 0;
    int m = ck_snprintf(say, say_n, "codec %u pins", a);
    for (unsigned o = 0; o < 3; o++)
        for (unsigned nid = first; nid < first + n && nid < MAX_NID && opened < MAX_PINS; nid++) {
            const struct wid *x = &W[nid];
            unsigned dev = x->cfg >> 20 & 15u, port = x->cfg >> 30;
            if (x->type != W_PIN || dev != order[o] || port == 1 || !(x->pincap & PC_OUT) || (x->caps & WC_DIGITAL))
                continue;
            if (!find_dac(nid, 0)) {
                ck_printf("sound: codec=%u pin 0x%02x (%s) has no path to a converter\n", a, nid,
                          dev == 1 ? "speaker" : dev ? "headphone" : "line out");
                continue;
            }
            open_path(a, dev, afg_out, afg_in);
            opened++;
            m += ck_snprintf(say + m, say_n - (size_t)m, " 0x%02x(%s)<-", nid, dev == 1 ? "spk" : dev ? "hp" : "line");
            for (unsigned i = 1; i < plen && m < (int)say_n - 6; i++)
                m += ck_snprintf(say + m, say_n - (size_t)m, i > 1 ? ",0x%02x" : "0x%02x", path[i]);
        }
    if (opened && (sub >> 16) == 0x106b && gpio) { /* Apple: the GPIOs switch the amplifiers on */
        unsigned all = gpio >= 8 ? 0xffu : (1u << gpio) - 1u;
        verb(a, afg, V_GPIO_MASK, all, 0);
        verb(a, afg, V_GPIO_DIR, all, 0);
        verb(a, afg, V_GPIO_DATA, all, 0);
        m += ck_snprintf(say + m, say_n - (size_t)m, "; gpio 0x%02x high", all);
    }
    (void)ctrl;
    return opened;
}

/* ---- the stream ---- */

/* The chirp from the night's current moment, then silence, through output
 * stream sd. buf holds bytes of PCM (page aligned, contiguous), bdl one page.
 * The buffer is filled from the moment the filling starts; the descriptors
 * then begin where the night has got to by the time the stream runs, so the
 * sound keeps time with the sky however long the filling took. */
static int stream_play(unsigned ctrl, unsigned sd, uint8_t *buf, size_t bytes, uint64_t *bdl, uint64_t night_t0)
{
    unsigned s = SD0 + 0x20 * sd;
    w8(s, 0);
    wait8(s, RUN, 0, 10);
    w8(s, SRST);
    if (wait8(s, SRST, SRST, 10)) {
        ck_printf("sound: ctrl=%u stream did not enter reset\n", ctrl);
        return -1;
    }
    w8(s, 0);
    if (wait8(s, SRST, 0, 10)) {
        ck_printf("sound: ctrl=%u stream did not leave reset\n", ctrl);
        return -1;
    }
    const int wide = g_fmt == FMT_441_24_STEREO;
    const uint32_t rate = wide ? CK_CHIRP_RATE_441 : CK_CHIRP_RATE, all = wide ? CK_CHIRP_FRAMES_441 : CK_CHIRP_FRAMES;
    const size_t fb = wide ? 8 : 4; /* bytes a frame */
    uint64_t t_fill = ck_rdtsc();
    uint32_t from = (uint32_t)((t_fill - night_t0) * rate / g_hz);
    if (from >= all) {
        ck_printf("sound: ctrl=%u ready at night %u ms, after the chirp (%u ms); nothing played\n", ctrl,
                  (uint32_t)((uint64_t)from * 1000 / rate), CK_CHIRP_END_MS);
        return -1;
    }
    uint32_t frames = all - from, tail = rate / 1000u * HDA_TAIL_MS;
    if ((frames + tail) * fb > bytes)
        tail = (uint32_t)(bytes / fb) - frames;
    if (wide)
        ck_chirp_fill_441((int32_t *)buf, from, frames);
    else
        ck_chirp_fill((int16_t *)buf, from, frames);
    memset(buf + frames * fb, 0, tail * fb);
    flush(buf, (frames + tail) * fb);
    uint64_t t_run = ck_rdtsc();
    uint32_t now = (uint32_t)((t_run - night_t0) * rate / g_hz), skip = (now - from) & ~31u;
    if (skip >= frames) {
        ck_printf("sound: ctrl=%u the chirp was over (night %u ms) once its %u ms were made\n", ctrl,
                  (uint32_t)((uint64_t)now * 1000 / rate), (uint32_t)((t_run - t_fill) * 1000 / g_hz));
        return -1;
    }
    uint8_t *p = buf + skip * fb; /* 128-byte aligned, as descriptors need */
    size_t used = (frames + tail - skip) * fb & ~(size_t)127;
    unsigned entries = (unsigned)((used + CHUNK - 1) / CHUNK);
    for (unsigned k = 0; k < entries; k++) {
        size_t off = (size_t)k * CHUNK, len = used - off < CHUNK ? used - off : CHUNK;
        bdl[2 * k] = (uint64_t)(uintptr_t)(p + off);
        bdl[2 * k + 1] = (uint64_t)len; /* length; no interrupt on completion */
    }
    flush(bdl, (size_t)entries * 16);
    w32(s + SD_CBL, (uint32_t)used);
    w16(s + SD_LVI, (uint16_t)(entries - 1));
    w16(s + SD_FMT, (uint16_t)g_fmt);
    w32(s + SD_BDLPL, (uint32_t)(uintptr_t)bdl);
    w32(s + SD_BDLPU, (uint32_t)((uint64_t)(uintptr_t)bdl >> 32));
    w8(s + 2, (uint8_t)((r8(s + 2) & 0x0fu) | TAG << 4));
    w8(s + SD_STS, 0x1c); /* old status cleared */
    w8(s, RUN);
    uint32_t start_ms = (uint32_t)((uint64_t)(from + skip) * 1000 / rate);
    spin_us(2000);
    ck_printf("sound: ctrl=%u stream %u playing the chirp from night %u ms (%u ms of it, then %u ms quiet; made in "
              "%u ms; format 0x%04x); position after 2 ms %u bytes, status 0x%02x\n", ctrl, sd, start_ms,
              (uint32_t)((uint64_t)(frames - skip) * 1000 / rate), HDA_TAIL_MS,
              (uint32_t)((t_run - t_fill) * 1000 / g_hz), g_fmt, r32(s + SD_LPIB), r8(s + SD_STS));
    play_bar = g_bar;
    play_sd = s;
    play_t0 = night_t0;
    return 0;
}

int ck_x86_hda_start(unsigned ctrl, volatile uint8_t *bar, struct ck_frames *fr, uint64_t tsc_hz, uint64_t night_t0,
                     int play)
{
    g_bar = bar;
    g_hz = tsc_hz;
    g_fmt = FMT_48K_16_STEREO;
    rings = 0;
    uint16_t gcap = r16(GCAP);
    unsigned iss = gcap >> 8 & 15u, oss = gcap >> 12 & 15u, bss = gcap >> 3 & 31u, nsd = iss + oss + bss;
    unsigned running = 0, corb_on = r8(CORBCTL) & RUN, rirb_on = r8(RIRBCTL) & RUN;
    for (unsigned i = 0; i < nsd; i++)
        running += (r8(SD0 + 0x20 * i) & RUN) != 0;
    ck_printf("sound: ctrl=%u version=%u.%u streams out=%u in=%u both=%u; left by the firmware: in_reset=%u "
              "command_rings=%s streams_running=%u\n", ctrl, r8(VMAJ), r8(VMIN), oss, iss, bss, !(r32(GCTL) & 1),
              corb_on || rirb_on ? "running" : "stopped", running);
    int stuck = 0;
    for (unsigned i = 0; i < nsd; i++)
        if (r8(SD0 + 0x20 * i) & RUN) {
            w8(SD0 + 0x20 * i, (uint8_t)(r8(SD0 + 0x20 * i) & ~RUN));
            stuck |= wait8(SD0 + 0x20 * i, RUN, 0, 10);
        }
    if (corb_on) {
        w8(CORBCTL, (uint8_t)(r8(CORBCTL) & ~RUN));
        stuck |= wait8(CORBCTL, RUN, 0, 10);
    }
    if (rirb_on) {
        w8(RIRBCTL, (uint8_t)(r8(RIRBCTL) & ~RUN));
        stuck |= wait8(RIRBCTL, RUN, 0, 10);
    }
    if (corb_on || rirb_on || running)
        ck_printf("sound: ctrl=%u the firmware's DMA %s\n", ctrl, stuck ? "did NOT stop within 10 ms" : "stopped");
    w16(STATESTS, 0x7fff); /* old codec reports cleared */
    w32(GCTL, r32(GCTL) & ~1u); /* into reset */
    if (wait8(GCTL, 1, 0, 10)) {
        ck_printf("sound: ctrl=%u did not enter reset\n", ctrl);
        return -1;
    }
    spin_us(1000);
    w32(GCTL, r32(GCTL) | 1u); /* out of reset */
    if (wait8(GCTL, 1, 1, 10)) {
        ck_printf("sound: ctrl=%u did not leave reset\n", ctrl);
        return -1;
    }
    spin_us(1000);
    uint64_t t0 = ck_rdtsc();
    while (!r16(STATESTS) && ck_rdtsc() - t0 < tsc_hz / 20)
        ck_pause();
    uint16_t codecs = r16(STATESTS) & 0x7fffu;
    ck_printf("sound: ctrl=%u codecs answering: mask=0x%x (1 ms after reset, then %llu us)\n", ctrl, codecs,
              (unsigned long long)((ck_rdtsc() - t0) * 1000000 / tsc_hz));
    rings = !rings_start(fr, ctrl);
    unsigned opened = 0;
    char say[160];
    say[0] = 0;
    for (unsigned a = 0; a < 15; a++)
        if (codecs >> a & 1) {
            char one[160];
            unsigned k = codec_open(ctrl, a, one, sizeof one);
            if (k && !opened)
                memcpy(say, one, sizeof say);
            opened += k;
        }
    if (!opened) {
        ck_printf("sound: ctrl=%u no speaker, line out or headphone path\n", ctrl);
        return -1;
    }
    ck_printf("sound: ctrl=%u opened %u output pin(s): %s\n", ctrl, opened, say);
    if (!play)
        return 0;
    if (!night_t0) {
        ck_printf("sound: ctrl=%u ready; no night running, so no chirp\n", ctrl);
        return 0;
    }
    if (!oss) {
        ck_printf("sound: ctrl=%u has no output stream\n", ctrl);
        return -1;
    }
    size_t bytes = g_fmt == FMT_441_24_STEREO
                       ? (size_t)(CK_CHIRP_FRAMES_441 + CK_CHIRP_RATE_441 / 1000u * HDA_TAIL_MS) * 8
                       : (size_t)(CK_CHIRP_FRAMES + CK_CHIRP_RATE / 1000u * HDA_TAIL_MS) * 4;
    bytes = (bytes + CK_PAGE - 1) & ~(size_t)(CK_PAGE - 1);
    uint64_t pa, bpa;
    if (!fr || ck_frames_alloc(fr, bytes / CK_PAGE, CK_PAGE, &pa) || ck_frames_alloc(fr, 1, CK_PAGE, &bpa)) {
        ck_printf("sound: ctrl=%u no memory for the chirp (%llu KiB)\n", ctrl, (unsigned long long)(bytes >> 10));
        return -1;
    }
    return stream_play(ctrl, iss, (uint8_t *)(uintptr_t)pa, bytes, (uint64_t *)(uintptr_t)bpa, night_t0) ? -1 : 1;
}

void ck_x86_hda_step(void)
{
    if (!play_bar || (ck_rdtsc() - play_t0) * 1000 / g_hz < CK_CHIRP_END_MS + 200u)
        return;
    volatile uint8_t *b = play_bar;
    play_bar = 0;
    uint32_t pos = *(volatile uint32_t *)(b + play_sd + SD_LPIB);
    b[play_sd] = 0;
    ck_printf("sound: chirp over; stream stopped at night %llu ms (position %u bytes)\n",
              (unsigned long long)((ck_rdtsc() - play_t0) * 1000 / g_hz), pos);
}
