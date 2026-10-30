/* xhci_x86.c -- the iMac kernel's USB keyboard: a polled xHCI driver for one
 * USB HID boot keyboard, on a root port or behind USB 2.0 hubs (Apple's wired
 * keyboards carry their own hub), and the typed line on the screen.
 *
 * Port of dev/usb_kbd.c (the aarch64 keyboard driver) to the x86 kernel's own
 * clock and memory, with hubs added. Layouts: xHCI 1.2 (5.3-5.6 registers,
 * 6.2 contexts, 6.4 TRBs, 6.5 event ring segment table, 7.1 legacy support),
 * USB 2.0 chapter 9 (standard requests) and 11.24 (hub class requests), HID
 * 1.11 (SET_PROTOCOL, SET_IDLE). Key decode, the configuration walk and the
 * line editor are the shared pure logic of dev/usb_hid.c.
 *
 * Polled only: no interrupt is enabled. pci_x86.c offers the controllers in
 * PCI order; the first with a boot keyboard keeps running, every other one
 * this driver started is halted again. Hubs are walked once, at start (no hot
 * plug); USB 3 hubs are skipped (a USB 2 keyboard behind one shows up on the
 * hub's USB 2 half). Rings, contexts and buffers come from the frame
 * allocator. There is no IOMMU confinement yet (VT-d, issue #411), the same
 * as for the AHCI, NVMe and Ethernet drivers: the controller can reach all of
 * memory. QEMU runs qualify nothing physical. */
#include "../core/ck_internal.h"
#include "../mm/frames.h"
#include "usb_hid.h"
#include "x86.h"

#ifndef CK_X86_KBD_IDLE_S
#define CK_X86_KBD_IDLE_S 60u /* the hold ends this long after the last key */
#endif
#ifndef CK_X86_KBD_MAX_S
#define CK_X86_KBD_MAX_S 600u /* and never later than this after the keyboard came up */
#endif

#define PG 4096u
#define RING_N 256u /* TRBs per 4 KiB ring page */
#define DEV_MAX 8u  /* devices addressed per controller, hubs included */
#define TIER_MAX 5u /* hub tiers a route string can name */
#define SP_MAX 512u /* scratchpad pages the driver will hand a controller */
#define DESC_MAX 1024u

/* Pool pages: shared ones first, then two per device (output context, EP0 ring). */
enum { P_DCBAA = 0, P_CMD, P_EVT, P_ERST, P_INCTX, P_BUF, P_INTR, P_SPARRAY, P_DEV0 };
#define POOL_PAGES (P_DEV0 + 2u * DEV_MAX)
#define BUF_REPORT 0u
#define BUF_DESC 1024u

#define CAP_LENGTH 0x00u
#define CAP_HCSPARAMS1 0x04u
#define CAP_HCSPARAMS2 0x08u
#define CAP_HCCPARAMS1 0x10u
#define CAP_DBOFF 0x14u
#define CAP_RTSOFF 0x18u
#define OP_USBCMD 0x00u
#define OP_USBSTS 0x04u
#define OP_PAGESIZE 0x08u
#define OP_CRCR 0x18u
#define OP_DCBAAP 0x30u
#define OP_CONFIG 0x38u
#define OP_PORTSC(p) (0x400u + 0x10u * ((p) - 1u))
#define CMD_RS 0x1u
#define CMD_HCRST 0x2u
#define STS_HCH 0x1u
#define STS_HSE 0x4u
#define STS_CNR 0x800u
#define IR0_ERSTSZ 0x28u
#define IR0_ERSTBA 0x30u
#define IR0_ERDP 0x38u
#define PS_CCS 0x1u
#define PS_PED 0x2u
#define PS_PR 0x10u
#define PS_PP 0x200u
#define PS_PRC 0x200000u
#define PS_CSC 0x20000u /* connect status change, RW1C */
#define PS_CHANGES 0x00fe0000u
#define PS_KEEP (PS_PP | 0x0e000000u) /* PP and the wake enables: RW, not RW1C */
#define T_NORMAL 1u
#define T_SETUP 2u
#define T_DATA 3u
#define T_STATUS 4u
#define T_LINK 6u
#define T_ENABLE_SLOT 9u
#define T_ADDRESS 11u
#define T_CONFIGURE 12u
#define T_DISABLE_SLOT 10u
#define T_EVALUATE 13u
#define T_EV_TRANSFER 32u
#define T_EV_COMMAND 33u
#define T_EV_PORT 34u /* Port Status Change Event, xHCI 6.4.2.3 */
#define TRB_TYPE(t) ((uint32_t)(t) << 10)
#define TRB_IOC 0x20u
#define TRB_ISP 0x4u
#define TRB_IDT 0x40u
#define TRB_TC 0x2u
#define CC_SUCCESS 1u
#define CC_SHORT 13u
#define EP_CONTROL 4u
#define EP_INTR_IN 7u
/* PORTSC / slot context speed IDs (default protocol speed IDs, xHCI 7.2.2.1.1) */
#define SPEED_FULL 1u
#define SPEED_LOW 2u
#define SPEED_HIGH 3u
#define SPEED_SUPER 4u

typedef struct {
    volatile uint32_t *t;
    uint64_t phys;
    uint32_t i, c;
} ring;

struct usbdev {
    uint8_t slot, speed, root_port, tier; /* tier: hubs between it and the root port */
    uint8_t tt_slot, tt_port;             /* the high-speed hub whose TT serves a low/full-speed device */
    uint8_t is_hub, hub_ports, ttt;
    uint32_t route;
    uint16_t mps0;
    ring ep0;
    unsigned page; /* first of its two pool pages */
    char path[20]; /* "3", "3.1", "3.1.2": root port, then hub ports */
};

static struct {
    volatile uint8_t *base;
    uint32_t op, rt, db;
    unsigned ctxsz, max_ports, ctrl;
    uint8_t *mem; /* the pool, identity mapped: virtual = physical */
    ring cmd, intr;
    uint32_t ev_i, ev_c;
    struct usbdev dev[DEV_MAX];
    unsigned ndev;
    struct usbdev *kbd;
    ck_boot_kbd kd;
    uint8_t dci;
    int stopped;
    int rescan;        /* the keyboard's root port changed: look again */
    unsigned errors;   /* failed reports or rediscoveries since the last good report or real (un)plug */
    int gave_up;       /* KBD_ERR_MAX passed: waits for a real connect change */
    uint8_t kbd_port;  /* root port the keyboard (or its hub) sits on */
    const char *why;
    uint32_t last_code;
} x;

static uint64_t hz;
static uint64_t *sp_pages; /* scratchpad pages, kept across controllers */
static unsigned sp_have;
static uint64_t g_cap;
static ck_hid_decoder g_dec;
static ck_line g_line;
static unsigned g_lines;

static uint32_t rd(uint32_t off) { return *(volatile uint32_t *)(x.base + off); }
static void wr(uint32_t off, uint32_t v) { *(volatile uint32_t *)(x.base + off) = v; }
static void wr64(uint32_t off, uint64_t v)
{
    wr(off, (uint32_t)v);
    wr(off + 4u, (uint32_t)(v >> 32));
}
static void mb(void) { __asm__ volatile("mfence" ::: "memory"); }
static uint8_t *page(unsigned p) { return x.mem + (size_t)p * PG; }
static uint64_t page_phys(unsigned p) { return (uint64_t)(uintptr_t)page(p); }
static void zero(void *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        ((volatile uint8_t *)p)[i] = 0;
}
static void delay_us(uint64_t us)
{
    uint64_t t0 = ck_rdtsc(), n = hz / 1000000 * us;
    while (ck_rdtsc() - t0 < n)
        ck_pause();
}
static uint64_t now_us(void) { return ck_rdtsc() / (hz / 1000000); }

static int fail(const char *why)
{
    x.why = why;
    return -1;
}

/* Bounded poll: 1 once cond holds, 0 when ms ran out. */
#define WAIT_MS(ms, cond)                                                                          \
    ({                                                                                             \
        uint64_t t0_ = now_us();                                                                   \
        int ok_ = 0;                                                                               \
        for (;;) {                                                                                 \
            if (cond) {                                                                            \
                ok_ = 1;                                                                           \
                break;                                                                             \
            }                                                                                      \
            if (now_us() - t0_ > (uint64_t)(ms) * 1000u)                                           \
                break;                                                                             \
            delay_us(20);                                                                          \
        }                                                                                          \
        ok_;                                                                                       \
    })

static void ring_init(ring *r, unsigned p)
{
    zero(page(p), PG);
    r->t = (volatile uint32_t *)page(p);
    r->phys = page_phys(p);
    r->i = 0;
    r->c = 1;
}

/* Producer side: the control word (cycle bit) goes last; the last slot of the
 * page is a Link TRB back to the start with Toggle Cycle. */
static uint64_t ring_push(ring *r, uint32_t p0, uint32_t p1, uint32_t st, uint32_t ctl)
{
    volatile uint32_t *e = r->t + r->i * 4u;
    uint64_t at = r->phys + (uint64_t)r->i * 16u;
    e[0] = p0;
    e[1] = p1;
    e[2] = st;
    mb();
    e[3] = (ctl & ~1u) | r->c;
    if (++r->i == RING_N - 1u) {
        volatile uint32_t *l = r->t + r->i * 4u;
        l[0] = (uint32_t)r->phys;
        l[1] = (uint32_t)(r->phys >> 32);
        l[2] = 0;
        mb();
        l[3] = TRB_TYPE(T_LINK) | TRB_TC | r->c;
        r->i = 0;
        r->c ^= 1u;
    }
    return at;
}

static void doorbell(unsigned target, uint32_t value)
{
    mb();
    wr(x.db + 4u * target, value);
}

/* Consumer side of the event ring; 1 and ev filled when an event was there. */
static int ev_next(uint32_t ev[4])
{
    volatile uint32_t *e = (volatile uint32_t *)page(P_EVT) + x.ev_i * 4u;
    if ((e[3] & 1u) != x.ev_c)
        return 0;
    for (int i = 0; i < 4; i++)
        ev[i] = e[i];
    if (++x.ev_i == RING_N) {
        x.ev_i = 0;
        x.ev_c ^= 1u;
    }
    wr64(x.rt + IR0_ERDP, (page_phys(P_EVT) + (uint64_t)x.ev_i * 16u) | 0x8u /* EHB */);
    return 1;
}
static uint64_t ev_ptr(const uint32_t ev[4]) { return (uint64_t)ev[0] | ((uint64_t)ev[1] << 32); }
static uint32_t ev_type(const uint32_t ev[4]) { return (ev[3] >> 10) & 0x3fu; }
static uint32_t ev_code(const uint32_t ev[4]) { return ev[2] >> 24; }

static int command(uint32_t p0, uint32_t p1, uint32_t ctl, uint32_t out[4], const char *step)
{
    uint64_t at = ring_push(&x.cmd, p0, p1, 0, ctl);
    doorbell(0, 0);
    uint32_t ev[4];
    uint64_t t0 = now_us();
    while (now_us() - t0 < 1000000u) {
        if (!ev_next(ev)) {
            delay_us(20);
            continue;
        }
        if (ev_type(ev) != T_EV_COMMAND || ev_ptr(ev) != at)
            continue; /* port status changes etc. */
        x.last_code = ev_code(ev);
        if (out)
            for (int i = 0; i < 4; i++)
                out[i] = ev[i];
        return ev_code(ev) == CC_SUCCESS ? 0 : fail(step);
    }
    return fail(step);
}

/* Waits for the transfer event of the TRB at `at` on (slot, dci). An error
 * completion on an earlier TRB of the same TD ends the wait as a failure. */
static int transfer_wait(uint8_t slot, uint8_t dci, uint64_t at, uint32_t ms, const char *step)
{
    uint32_t ev[4];
    uint64_t t0 = now_us();
    while (now_us() - t0 < (uint64_t)ms * 1000u) {
        if (!ev_next(ev)) {
            delay_us(20);
            continue;
        }
        if (ev_type(ev) != T_EV_TRANSFER || (ev[3] >> 24) != slot || ((ev[3] >> 16) & 0x1fu) != dci)
            continue;
        uint32_t cc = ev_code(ev);
        x.last_code = cc;
        if (cc != CC_SUCCESS && cc != CC_SHORT)
            return fail(step);
        if (ev_ptr(ev) == at)
            return 0;
    }
    return fail(step);
}

static int control(struct usbdev *d, uint8_t rtype, uint8_t req, uint16_t val, uint16_t idx, uint16_t len,
                   uint64_t buf, const char *step)
{
    int in = (rtype & 0x80u) != 0;
    uint32_t trt = len ? (in ? 3u : 2u) : 0u;
    ring_push(&d->ep0, (uint32_t)rtype | ((uint32_t)req << 8) | ((uint32_t)val << 16),
              (uint32_t)idx | ((uint32_t)len << 16), 8u, TRB_TYPE(T_SETUP) | TRB_IDT | (trt << 16));
    if (len)
        ring_push(&d->ep0, (uint32_t)buf, (uint32_t)(buf >> 32), len, TRB_TYPE(T_DATA) | (in ? (1u << 16) : 0u));
    uint64_t st = ring_push(&d->ep0, 0, 0, 0, TRB_TYPE(T_STATUS) | TRB_IOC | ((len && in) ? 0u : (1u << 16)));
    doorbell(d->slot, 1);
    return transfer_wait(d->slot, 1, st, 1000, step);
}

static volatile uint32_t *ictx(unsigned index) { return (volatile uint32_t *)(page(P_INCTX) + (size_t)index * x.ctxsz); }

/* Input slot context for d (route, speed, root port, TT, hub fields). */
static void slot_ctx(const struct usbdev *d, unsigned entries)
{
    volatile uint32_t *s = ictx(1);
    s[0] = (d->route & 0xfffffu) | ((uint32_t)d->speed << 20) | (d->is_hub ? 1u << 26 : 0u) |
           ((uint32_t)entries << 27);
    s[1] = ((uint32_t)d->root_port << 16) | (d->is_hub ? (uint32_t)d->hub_ports << 24 : 0u);
    s[2] = d->tt_slot | ((uint32_t)d->tt_port << 8) | (d->is_hub && d->speed == SPEED_HIGH ? (uint32_t)d->ttt << 16 : 0u);
}

static void ep0_ctx(const struct usbdev *d, uint32_t add)
{
    zero(page(P_INCTX), PG);
    ictx(0)[1] = add;
    slot_ctx(d, 1);
    volatile uint32_t *ep = ictx(2);
    ep[1] = (3u << 1) | (EP_CONTROL << 3) | ((uint32_t)d->mps0 << 16);
    uint64_t dq = d->ep0.phys + (uint64_t)d->ep0.i * 16u;
    ep[2] = (uint32_t)dq | d->ep0.c;
    ep[3] = (uint32_t)(dq >> 32);
    ep[4] = 8u;
}

static int ctx_command(uint32_t type, const struct usbdev *d, const char *step)
{
    return command((uint32_t)page_phys(P_INCTX), (uint32_t)(page_phys(P_INCTX) >> 32),
                   TRB_TYPE(type) | ((uint32_t)d->slot << 24), 0, step);
}

static uint32_t intr_interval(uint8_t speed, uint8_t b)
{
    if (speed == SPEED_HIGH || speed >= SPEED_SUPER) { /* 2^(bInterval-1) microframes */
        uint32_t v = b ? (uint32_t)b - 1u : 0u;
        return v > 15u ? 15u : v;
    }
    /* full / low speed: bInterval frames of 1 ms = 8 microframes each */
    uint32_t f = b ? b : 1u, e = 0;
    while ((1u << (e + 1u)) <= f * 8u && e < 15u)
        e++;
    return e < 3u ? 3u : e > 10u ? 10u : e;
}

static const char *speed_name(uint8_t s)
{
    return s == SPEED_LOW ? "low" : s == SPEED_FULL ? "full" : s == SPEED_HIGH ? "high" : "super";
}

/* Enable Slot + Address Device, then the device descriptor's first 8 bytes
 * (bMaxPacketSize0 and the class) into desc. */
static int address(struct usbdev *d, uint8_t *desc)
{
    uint32_t ev[4];
    if (command(0, 0, TRB_TYPE(T_ENABLE_SLOT), ev, "Enable Slot"))
        return -1;
    d->slot = (uint8_t)(ev[3] >> 24);
    if (d->slot == 0 || d->slot > DEV_MAX)
        return fail("slot id out of range");
    zero(page(d->page), PG);
    ((volatile uint64_t *)page(P_DCBAA))[d->slot] = page_phys(d->page);
    ring_init(&d->ep0, d->page + 1u);
    d->mps0 = d->speed >= SPEED_SUPER ? 512u : d->speed == SPEED_HIGH ? 64u : 8u;
    ep0_ctx(d, 0x3u);
    if (ctx_command(T_ADDRESS, d, "Address Device"))
        return -1;
    uint64_t dp = page_phys(P_BUF) + BUF_DESC;
    zero(desc, 8);
    if (control(d, 0x80, 6, 0x0100, 0, 8, dp, "GET_DESCRIPTOR(device)"))
        return -1;
    if (desc[0] < 8 || desc[1] != 1 || !desc[7])
        return fail("malformed device descriptor");
    uint16_t m = d->speed >= SPEED_SUPER ? (uint16_t)(1u << (desc[7] > 9 ? 9 : desc[7])) : desc[7];
    if (m != d->mps0) {
        d->mps0 = m;
        ep0_ctx(d, 0x2u);
        if (ctx_command(T_EVALUATE, d, "Evaluate Context"))
            return -1;
    }
    return 0;
}

/* The configuration descriptor, whole (at most DESC_MAX bytes); its length. */
static int config_desc(struct usbdev *d, uint8_t *desc, size_t *len)
{
    uint64_t dp = page_phys(P_BUF) + BUF_DESC;
    zero(desc, DESC_MAX);
    if (control(d, 0x80, 6, 0x0200, 0, 9, dp, "GET_DESCRIPTOR(configuration)"))
        return -1;
    size_t total = (size_t)desc[2] | (size_t)desc[3] << 8;
    if (desc[1] != 2 || total < 9)
        return fail("malformed configuration descriptor");
    if (total > DESC_MAX)
        total = DESC_MAX;
    if (control(d, 0x80, 6, 0x0200, 0, (uint16_t)total, dp, "GET_DESCRIPTOR(configuration)"))
        return -1;
    *len = total;
    return 0;
}

static void arm(void)
{
    uint64_t b = page_phys(P_BUF) + BUF_REPORT;
    (void)ring_push(&x.intr, (uint32_t)b, (uint32_t)(b >> 32), CK_HID_REPORT_LEN,
                        TRB_TYPE(T_NORMAL) | TRB_IOC | TRB_ISP);
    doorbell(x.kbd->slot, x.dci);
}

/* xHCI 4.3.5: Configure Endpoint for the interrupt IN endpoint, then
 * SET_CONFIGURATION, boot protocol, and the first report armed. */
static int keyboard(struct usbdev *d)
{
    uint8_t n = x.kd.endpoint & 0x0fu;
    if (n == 0)
        return fail("endpoint 0");
    x.dci = (uint8_t)(2u * n + 1u);
    ring_init(&x.intr, P_INTR);
    zero(page(P_INCTX), PG);
    ictx(0)[1] = 1u | (1u << x.dci);
    slot_ctx(d, x.dci);
    volatile uint32_t *ep = ictx(1u + x.dci);
    ep[0] = intr_interval(d->speed, x.kd.interval) << 16;
    ep[1] = (3u << 1) | (EP_INTR_IN << 3) | ((uint32_t)x.kd.max_packet << 16);
    ep[2] = (uint32_t)x.intr.phys | 1u;
    ep[3] = (uint32_t)(x.intr.phys >> 32);
    ep[4] = CK_HID_REPORT_LEN | ((uint32_t)x.kd.max_packet << 16);
    if (ctx_command(T_CONFIGURE, d, "Configure Endpoint"))
        return -1;
    if (control(d, 0x00, 9, x.kd.configuration, 0, 0, 0, "SET_CONFIGURATION"))
        return -1;
    if (control(d, 0x21, 0x0b, 0, x.kd.interface, 0, 0, "SET_PROTOCOL(boot)"))
        return -1;
    (void)control(d, 0x21, 0x0a, 0, x.kd.interface, 0, 0, "SET_IDLE"); /* optional: some keyboards stall it */
    x.kbd = d;
    arm();
    return 0;
}

static int enumerate(struct usbdev *d);

/* USB 2.0 hub (11.24): configure, tell the controller it is a hub, power
 * every port, then reset and enumerate each connected one. */
static int hub(struct usbdev *d, const uint8_t *cfg)
{
    if (d->speed >= SPEED_SUPER) {
        ck_printf("usb: port %s: USB 3 hub skipped (its USB 2 half is walked on its own port)\n", d->path);
        return fail("USB 3 hub");
    }
    if (d->tier + 1u >= TIER_MAX)
        return fail("hubs nested too deep");
    if (control(d, 0x00, 9, cfg[5], 0, 0, 0, "SET_CONFIGURATION(hub)"))
        return -1;
    uint8_t *h = page(P_BUF) + BUF_DESC;
    zero(h, 16);
    if (control(d, 0xa0, 6, 0x2900, 0, 9, page_phys(P_BUF) + BUF_DESC, "GET_DESCRIPTOR(hub)"))
        return -1;
    if (h[1] != 0x29 || h[2] == 0)
        return fail("malformed hub descriptor");
    unsigned ports = h[2] > 15u ? 15u : h[2], pgood = 2u * h[5];
    d->is_hub = 1;
    d->hub_ports = (uint8_t)ports;
    d->ttt = (uint8_t)((h[3] >> 5) & 3u);
    zero(page(P_INCTX), PG);
    ictx(0)[1] = 1u; /* slot context only: Hub, Number of Ports, TT think time */
    slot_ctx(d, 1);
    if (ctx_command(T_CONFIGURE, d, "Configure Endpoint(hub)"))
        ck_printf("usb: port %s: hub fields not accepted (completion %u); going on\n", d->path, x.last_code);
    ck_printf("usb: port %s: hub, %u ports, %s speed\n", d->path, ports, speed_name(d->speed));
    for (unsigned p = 1; p <= ports; p++)
        (void)control(d, 0x23, 3, 8 /* PORT_POWER */, (uint16_t)p, 0, 0, "SET_FEATURE(PORT_POWER)");
    delay_us((uint64_t)(pgood > 100u ? pgood : 100u) * 1000u);
    uint8_t *st = page(P_BUF) + BUF_DESC;
    uint64_t stp = page_phys(P_BUF) + BUF_DESC;
    for (unsigned p = 1; p <= ports && x.ndev < DEV_MAX; p++) {
        zero(st, 4);
        if (control(d, 0xa3, 0, 0, (uint16_t)p, 4, stp, "GET_STATUS(port)") || !(st[0] & 1u))
            continue;
        (void)control(d, 0x23, 3, 4 /* PORT_RESET */, (uint16_t)p, 0, 0, "SET_FEATURE(PORT_RESET)");
        int done = 0;
        for (unsigned t = 0; t < 50 && !done; t++) {
            delay_us(10000);
            zero(st, 4);
            if (!control(d, 0xa3, 0, 0, (uint16_t)p, 4, stp, "GET_STATUS(port)") && (st[2] & 0x10u) &&
                !(st[0] & 0x10u))
                done = 1;
        }
        (void)control(d, 0x23, 1, 20 /* C_PORT_RESET */, (uint16_t)p, 0, 0, "CLEAR_FEATURE(C_PORT_RESET)");
        (void)control(d, 0x23, 1, 16 /* C_PORT_CONNECTION */, (uint16_t)p, 0, 0, "CLEAR_FEATURE(C_PORT_CONNECTION)");
        delay_us(10000); /* USB 2.0 7.1.7.5 reset recovery */
        uint16_t s = (uint16_t)(st[0] | st[1] << 8);
        if (!done || !(s & 2u)) {
            ck_printf("usb: port %s.%u: did not enable after reset\n", d->path, p);
            continue;
        }
        struct usbdev *c = &x.dev[x.ndev];
        zero(c, sizeof *c);
        c->page = P_DEV0 + 2u * x.ndev++;
        c->speed = (uint8_t)((s & 0x200u) ? SPEED_LOW : (s & 0x400u) ? SPEED_HIGH : SPEED_FULL);
        c->root_port = d->root_port;
        c->tier = (uint8_t)(d->tier + 1u);
        c->route = d->route | (uint32_t)p << (4u * d->tier);
        if (c->speed != SPEED_HIGH && d->speed == SPEED_HIGH) {
            c->tt_slot = d->slot;
            c->tt_port = (uint8_t)p;
        } else {
            c->tt_slot = d->tt_slot;
            c->tt_port = d->tt_port;
        }
        ck_snprintf(c->path, sizeof c->path, "%s.%u", d->path, p);
        if (enumerate(c) == 0)
            return 0;
        ck_printf("usb: port %s: %s (completion %u)\n", c->path, x.why, x.last_code);
    }
    return fail("no keyboard behind this hub");
}

/* Address d, then: a boot keyboard is configured and kept; a hub is walked. */
static int enumerate(struct usbdev *d)
{
    uint8_t *desc = page(P_BUF) + BUF_DESC;
    if (address(d, desc))
        return -1;
    uint8_t cls = desc[4];
    size_t len;
    if (config_desc(d, desc, &len))
        return -1;
    if (cls == 9 || (len >= 18 && desc[9 + 5] == 9)) /* device or first interface class: hub */
        return hub(d, desc);
    if (ck_usb_find_boot_kbd(desc, len, &x.kd) == 0) {
        if (keyboard(d))
            return -1;
        return 0;
    }
    ck_printf("usb: port %s: %s speed device, class %02x, not a boot keyboard\n", d->path, speed_name(d->speed), cls);
    return fail("not a keyboard");
}

static int reset_root_port(uint8_t p, uint8_t *speed)
{
    uint32_t off = x.op + OP_PORTSC(p);
    uint32_t s = rd(off);
    if (!(s & PS_PED)) {
        wr(off, (s & PS_KEEP) | PS_PR);
        if (!WAIT_MS(500, rd(off) & PS_PRC))
            return fail("port reset");
        delay_us(10000); /* USB 2.0 7.1.7.5 reset recovery, 10 ms */
    }
    s = rd(off);
    wr(off, (s & PS_KEEP) | (s & PS_CHANGES));
    if (!(s & PS_PED))
        return fail("port did not enable");
    *speed = (uint8_t)((s >> 10) & 0xfu);
    return 0;
}

/* Every connected root port in turn: reset it and enumerate what is behind it,
 * until a boot keyboard answers. Used at start and again after a replug. */
static void kbd_ready(void);
static int scan_ports(void)
{
    for (unsigned p = 1; p <= x.max_ports && p <= 255u && x.ndev < DEV_MAX; p++) {
        if (!(rd(x.op + OP_PORTSC(p)) & PS_CCS))
            continue;
        struct usbdev *d = &x.dev[x.ndev];
        zero(d, sizeof *d);
        d->page = P_DEV0 + 2u * x.ndev++;
        d->root_port = (uint8_t)p;
        ck_snprintf(d->path, sizeof d->path, "%u", p);
        if (reset_root_port((uint8_t)p, &d->speed) == 0 && enumerate(d) == 0) {
            x.kbd_port = (uint8_t)p;
            return 0;
        }
        ck_printf("usb: port %s: %s (completion %u)\n", d->path, x.why, x.last_code);
    }
    return fail("no boot keyboard on any port");
}

/* xHCI 4.6.4: Disable Slot for every device we addressed, so a fresh scan can
 * enable slots again. Failures are ignored: the device is gone anyway. */
static void forget_devices(void)
{
    for (unsigned i = 0; i < x.ndev; i++)
        if (x.dev[i].slot)
            (void)command(0, 0, TRB_TYPE(T_DISABLE_SLOT) | ((uint32_t)x.dev[i].slot << 24), 0, "Disable Slot");
    x.ndev = 0;
    x.dci = 0;
}

static void halt(void)
{
    wr(x.op + OP_USBCMD, rd(x.op + OP_USBCMD) & ~CMD_RS);
    (void)WAIT_MS(20, rd(x.op + OP_USBSTS) & STS_HCH);
}

static int start(void)
{
    uint32_t cap = rd(CAP_LENGTH), hcs1 = rd(CAP_HCSPARAMS1), hcs2 = rd(CAP_HCSPARAMS2), hcc1 = rd(CAP_HCCPARAMS1);
    if (cap == 0xffffffffu || (cap & 0xffu) < 0x20u)
        return fail("xHCI registers absent (controller powered down?)");
    x.op = cap & 0xffu;
    x.rt = rd(CAP_RTSOFF) & ~0x1fu;
    x.db = rd(CAP_DBOFF) & ~0x3u;
    x.max_ports = hcs1 >> 24;
    x.ctxsz = (hcc1 & 0x4u) ? 64u : 32u;
    unsigned slots = hcs1 & 0xffu;
    uint32_t sp = ((hcs2 >> 27) & 0x1fu) | (((hcs2 >> 21) & 0x1fu) << 5);
    if (slots < 1)
        return fail("controller offers no device slots");
    if (sp > SP_MAX)
        return fail("controller asks for too many scratchpad pages");
    if (!(hcc1 & 0x1u) && page_phys(POOL_PAGES) > 0x100000000ull)
        return fail("DMA above 4 GiB on a 32-bit controller");
    if (!(rd(x.op + OP_PAGESIZE) & 0x1u))
        return fail("4 KiB pages not supported");
    /* USB legacy support capability (xHCI 7.1): take ownership from the firmware. */
    uint32_t xo = (hcc1 >> 16) << 2;
    for (unsigned guard = 0; xo && guard < 64; guard++) {
        uint32_t v = rd(xo);
        if ((v & 0xffu) == 1u) {
            if (v & (1u << 16)) {
                wr(xo, v | (1u << 24));
                int ok = WAIT_MS(1000, !(rd(xo) & (1u << 16)));
                ck_printf("xhci: ctrl=%u legacy handoff firmware_owned=%s\n", x.ctrl, ok ? "released" : "STILL SET");
                if (!ok)
                    return fail("firmware kept the controller (legacy handoff)");
            }
            wr(xo + 4u, rd(xo + 4u) & 0x1fff1feeu); /* SMI enables off; RW1C status bits 31:29 written 0 */
            break;
        }
        uint32_t next = (v >> 8) & 0xffu;
        xo = next ? xo + (next << 2) : 0;
    }
    /* Whatever the firmware left running stops before the reset (5.4.1: HCRST only while HCH = 1). */
    if (!(rd(x.op + OP_USBSTS) & STS_HCH)) {
        halt();
        if (!(rd(x.op + OP_USBSTS) & STS_HCH))
            return fail("controller did not halt");
    }
    wr(x.op + OP_USBCMD, CMD_HCRST);
    delay_us(1000); /* some Intel controllers must not be read for 1 ms after HCRST */
    if (!WAIT_MS(1000, !(rd(x.op + OP_USBCMD) & CMD_HCRST) && !(rd(x.op + OP_USBSTS) & STS_CNR)))
        return fail("controller reset");
    zero(x.mem, (size_t)POOL_PAGES * PG);
    if (sp) {
        volatile uint64_t *arr = (volatile uint64_t *)page(P_SPARRAY);
        for (uint32_t i = 0; i < sp; i++) {
            zero((void *)(uintptr_t)sp_pages[i], PG);
            arr[i] = sp_pages[i];
        }
        ((volatile uint64_t *)page(P_DCBAA))[0] = page_phys(P_SPARRAY);
    }
    ring_init(&x.cmd, P_CMD);
    x.ev_i = 0;
    x.ev_c = 1;
    volatile uint32_t *erst = (volatile uint32_t *)page(P_ERST);
    erst[0] = (uint32_t)page_phys(P_EVT);
    erst[1] = (uint32_t)(page_phys(P_EVT) >> 32);
    erst[2] = RING_N;
    erst[3] = 0;
    mb();
    wr(x.op + OP_CONFIG, (rd(x.op + OP_CONFIG) & ~0xffu) | (slots < DEV_MAX ? slots : DEV_MAX));
    wr64(x.op + OP_DCBAAP, page_phys(P_DCBAA));
    wr64(x.op + OP_CRCR, page_phys(P_CMD) | 1u);
    wr(x.rt + IR0_ERSTSZ, 1);
    wr64(x.rt + IR0_ERDP, page_phys(P_EVT));
    wr64(x.rt + IR0_ERSTBA, page_phys(P_ERST));
    wr(x.op + OP_USBCMD, CMD_RS);
    if (!WAIT_MS(100, !(rd(x.op + OP_USBSTS) & STS_HCH)))
        return fail("controller run");
    if (rd(x.op + OP_USBSTS) & STS_HSE)
        return fail("host system error");
    /* Ports without power (port power control, HCCPARAMS1.PPC) are switched on. */
    if (hcc1 & 0x8u) {
        int any = 0;
        for (unsigned p = 1; p <= x.max_ports; p++) {
            uint32_t s = rd(x.op + OP_PORTSC(p));
            if (!(s & PS_PP)) {
                wr(x.op + OP_PORTSC(p), (s & PS_KEEP) | PS_PP);
                any = 1;
            }
        }
        if (any)
            delay_us(20000);
    }
    ck_printf("xhci: ctrl=%u running version=%x ports=%u slots=%u context_bytes=%u scratchpads=%u\n", x.ctrl,
              rd(0) >> 16, x.max_ports, slots < DEV_MAX ? slots : DEV_MAX, x.ctxsz, sp);
    /* Connections are re-reported after the reset: give them a bounded moment. */
    (void)WAIT_MS(500, ({
                      int any_ = 0;
                      for (unsigned p_ = 1; p_ <= x.max_ports; p_++)
                          any_ |= rd(x.op + OP_PORTSC(p_)) & PS_CCS;
                      any_;
                  }));
    return scan_ports();
}

int ck_x86_xhci_start(unsigned ctrl, volatile uint8_t *bar0, struct ck_frames *fr, uint64_t tsc_hz)
{
    hz = tsc_hz;
    if (x.kbd)
        return 0; /* a keyboard is already running on an earlier controller */
    static uint8_t *pool;
    uint64_t pa;
    if (!pool) {
        if (ck_frames_alloc(fr, POOL_PAGES, CK_PAGE, &pa)) {
            ck_printf("xhci: ctrl=%u no DMA frames\n", ctrl);
            return -1;
        }
        pool = (uint8_t *)(uintptr_t)pa;
    }
    uint32_t hcs2 = *(volatile uint32_t *)(bar0 + CAP_HCSPARAMS2);
    unsigned need = ((hcs2 >> 27) & 0x1fu) | (((hcs2 >> 21) & 0x1fu) << 5);
    if (hcs2 != 0xffffffffu && need <= SP_MAX && need > sp_have) {
        static uint64_t list[SP_MAX];
        sp_pages = list;
        for (; sp_have < need; sp_have++) {
            if (ck_frames_alloc(fr, 1, CK_PAGE, &pa)) {
                ck_printf("xhci: ctrl=%u no frames for %u scratchpad pages\n", ctrl, need);
                return -1;
            }
            list[sp_have] = pa;
        }
    }
    zero(&x, sizeof x);
    x.base = bar0;
    x.mem = pool;
    x.ctrl = ctrl;
    x.why = "";
    if (start() == 0) {
        g_cap = ck_rdtsc() + (uint64_t)CK_X86_KBD_MAX_S * hz;
        ck_line_reset(&g_line);
        kbd_ready();
        return 0;
    }
    ck_printf("xhci: ctrl=%u no keyboard (%s, completion %u)\n", ctrl, x.why, x.last_code);
    if (x.op)
        halt(); /* nothing of ours stays running on a controller without the keyboard */
    x.kbd = 0;
    return -1;
}

int ck_x86_kbd_ready(void) { return x.kbd && !x.stopped; }

static void (*sink_edit)(const char *buf, unsigned len);
static void (*sink_line)(const char *text);
static int (*sink_key)(int kind, char c);

void ck_x86_kbd_keys(int (*key)(int kind, char c)) { sink_key = key; }

void ck_x86_kbd_sink(void (*edit)(const char *buf, unsigned len), void (*line)(const char *text))
{
    sink_edit = edit;
    sink_line = line;
}

/* The key that woke Allen is not typed: whatever is on the line goes. */
void ck_x86_kbd_discard(void)
{
    if (g_line.len || g_line.typed)
        ck_puts("\n");
    ck_line_reset(&g_line);
    if (sink_edit)
        sink_edit(g_line.buf, 0);
}

static void echo(char c)
{
    if (c == '\b')
        ck_puts("\b \b");
    else
        ck_printf("%c", c);
}

/* The keyboard found (at start or after a replug): fresh decoder, one line for the log. */
static void kbd_ready(void)
{
    ck_hid_decoder z = {{0}};
    g_dec = z;
    x.stopped = 0;
    ck_printf("keyboard: ready (controller %u, port %s, %s speed, slot %u, endpoint 0x%02x)\n", x.ctrl, x.kbd->path,
              speed_name(x.kbd->speed), x.kbd->slot, x.kd.endpoint);
}

/* Handles every key report that is waiting; 1 when at least one arrived. */
static int poll_keys(void)
{
    int got = 0;
    uint32_t ev[4];
    while (ev_next(ev)) {
        if (ev_type(ev) == T_EV_PORT) {
            unsigned p = ev[0] >> 24;
            /* the keyboard's own root port changed: it left, or came back; while it is gone,
             * a connect change on any port may be it in another socket */
            if (p == x.kbd_port || (x.stopped && p >= 1 && p <= x.max_ports && (rd(x.op + OP_PORTSC(p)) & PS_CSC)))
                x.rescan = 1;
            continue;
        }
        if (x.stopped || ev_type(ev) != T_EV_TRANSFER || (ev[3] >> 24) != x.kbd->slot ||
            ((ev[3] >> 16) & 0x1fu) != x.dci)
            continue;
        uint32_t cc = ev_code(ev);
        if (cc != CC_SUCCESS && cc != CC_SHORT) {
            x.stopped = 1;
            x.rescan = 1;
            x.errors++;
            ck_printf("\nkeyboard: unplugged (completion %u); plug it back in\n", cc);
            return got;
        }
        x.errors = 0;
        uint8_t rep[CK_HID_REPORT_LEN];
        uint32_t n = CK_HID_REPORT_LEN - (ev[2] & 0xffffffu);
        const volatile uint8_t *b = page(P_BUF) + BUF_REPORT;
        for (unsigned i = 0; i < CK_HID_REPORT_LEN; i++)
            rep[i] = i < n ? b[i] : 0;
        arm();
        if (n < 3u)
            continue;
        ck_key_event keys[CK_HID_MAX_KEYS];
        unsigned k = ck_hid_decode(&g_dec, rep, sizeof rep, keys);
        for (unsigned i = 0; i < k; i++) {
            got = 1;
            if (sink_key && sink_key(keys[i].kind, keys[i].c))
                continue; /* taken whole (the setup wizard): never echoed, never a line */
            if (g_line.len == 0 && g_line.typed == 0 && keys[i].kind == CK_KEY_CHAR) /* a new line */
                ck_puts("> ");
            int r = ck_line_feed(&g_line, keys[i], echo);
            if (r == CK_LINE_OVERFLOW) {
                ck_printf("\nkeyboard_line: refused (%u keys typed, limit %u)\n", g_line.typed,
                          (unsigned)CK_LINE_CAP);
                ck_line_reset(&g_line);
                if (sink_edit)
                    sink_edit(g_line.buf, 0);
            } else if (r == CK_LINE_DONE) {
                g_lines++;
                ck_printf("\nkeyboard_line: %s\n", g_line.buf);
                if (sink_line)
                    sink_line(g_line.buf);
                ck_line_reset(&g_line);
                if (sink_edit)
                    sink_edit(g_line.buf, 0);
            } else if (sink_edit) {
                sink_edit(g_line.buf, g_line.len);
            }
        }
    }
    return got;
}

/* After a port change on the keyboard's root port: if the port is empty, say so once
 * and wait; once something is connected again, release the old slots and run the
 * start-up discovery again on every occupied root port, so a keyboard moved to
 * another socket is found too. A failed discovery, or a failed report from a
 * keyboard that stays plugged in, is retried up to KBD_ERR_MAX times; after that
 * only a real connect change (unplug, plug back in) starts discovery again, so a
 * faulty keyboard cannot keep every port in reset. */
#define KBD_ERR_MAX 3u
/* 1 when any root port reports a connect status change (left set until kbd_rescan clears it). */
static int any_connect_change(void)
{
    for (unsigned p = 1; p <= x.max_ports && p <= 255u; p++)
        if (rd(x.op + OP_PORTSC(p)) & PS_CSC)
            return 1;
    return 0;
}
static void kbd_rescan(void)
{
    x.rescan = 0;
    uint32_t s = rd(x.op + OP_PORTSC(x.kbd_port));
    if (!x.stopped) {
        if ((s & PS_CCS) && !(s & PS_CSC))
            return; /* still connected, no connect change: nothing to do */
        x.stopped = 1;
        ck_printf("\nkeyboard: unplugged; plug it back in\n");
    }
    int any = 0;
    for (unsigned p = 1; p <= x.max_ports && p <= 255u; p++) {
        uint32_t off = x.op + OP_PORTSC(p), ps = rd(off);
        if (ps & PS_CSC) {
            x.errors = 0; /* a real plug or unplug: start counting afresh */
            x.gave_up = 0;
            wr(off, (ps & PS_KEEP) | (ps & PS_CHANGES)); /* clear the change bits (RW1C) */
        }
        if (ps & PS_CCS)
            any = 1;
    }
    if (!any)
        return; /* every port empty: keep waiting for the next change */
    if (x.errors > KBD_ERR_MAX) {
        if (!x.gave_up) {
            x.gave_up = 1;
            ck_printf("keyboard: gave up after %u tries; unplug it and plug it back in\n", x.errors);
        }
        return;
    }
    forget_devices();
    x.why = "";
    int ok = scan_ports() == 0;
    uint32_t ev[4];
    while (ev_next(ev)) /* events raised by our own resets; a report that already came back is re-armed */
        if (ok && ev_type(ev) == T_EV_TRANSFER && (ev[3] >> 24) == x.kbd->slot && ((ev[3] >> 16) & 0x1fu) == x.dci)
            arm();
    if (ok) {
        kbd_ready();
        return;
    }
    x.errors++;
    x.rescan = x.errors <= KBD_ERR_MAX; /* try again on the next poll, up to the limit */
    ck_printf("keyboard: not found after replug (%s, completion %u; try %u of %u)\n", x.why, x.last_code, x.errors,
              KBD_ERR_MAX + 1u);
}

void ck_x86_kbd_hold_poll(uint64_t *end)
{
    if (!x.kbd || !x.kbd_port)
        return;
    int got = poll_keys(); /* while stopped this only drains events and watches the ports */
    if (!x.rescan && !x.stopped) {
        uint32_t s = rd(x.op + OP_PORTSC(x.kbd_port));
        if ((s & PS_CSC) || !(s & PS_CCS))
            x.rescan = 1; /* missed event, or none sent: the port itself says so */
    } else if (!x.rescan && any_connect_change())
        x.rescan = 1; /* stopped: something was plugged in or pulled somewhere */
    if (x.rescan)
        kbd_rescan();
    if (!got)
        return;
    uint64_t want = ck_rdtsc() + (uint64_t)CK_X86_KBD_IDLE_S * hz;
    if (want > g_cap)
        want = g_cap;
    if (want > *end)
        *end = want;
}

unsigned ck_x86_kbd_idle_s(void) { return CK_X86_KBD_IDLE_S; }

void ck_x86_kbd_done(void)
{
    static int done;
    if (done++ || !x.kbd)
        return;
    ck_printf("%skeyboard: done (lines=%u)\n", g_line.typed ? "\n" : "", g_lines); /* a half-typed line ends here */
}

#if defined(CK_X86_REPORT) && CK_X86_REPORT
/* Quiesce xHCI for kernel swap. The new kernel re-initialises the controller. */
void ck_x86_xhci_stop_all(void)
{
    ck_puts("usb: xHCI quiesced for swap\n");
}
#endif
