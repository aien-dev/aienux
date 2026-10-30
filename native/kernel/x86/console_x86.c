/* console_x86.c -- ck_puts/ck_printf for AIENOS-KERNEL-0.
 *
 * Three sinks. COM1 (I/O port 0x3f8) only when a 16550 answers there (the
 * scratch register keeps a written value): QEMU has one, the 2017 iMac has
 * none, so nothing is sent there on the iMac. The GOP framebuffer
 * (core/fbcon.c) once the kernel mapped it and called ck_x86_console_fb_on.
 * And a RAM copy of the first CK_X86_LOG_BYTES (64 KiB, console_log.h) written, replayed onto the
 * screen when it comes on, so the screen also shows the lines from before
 * ExitBootServices (UEFI ConOut is not used: on a Mac it is invisible unless
 * the firmware is switched to text mode); report_x86.c also sends that copy
 * over the network. '\n' goes to COM1 as CR LF. A fault
 * while drawing turns the screen off for good. While the startup picture
 * plays (ck_x86_console_fb_hold) the screen is known but the text stays off
 * it; ck_x86_console_fb_show brings the text back (a panic always does). */
#include "../core/ck_internal.h"
#include "../core/fbcon.h"
#include "x86.h"
#include "console_log.h"

#define COM1 0x3f8

static int com1_on;
static struct ck_fbcon fb;
static int fb_on, fb_busy;
static int fb_held; /* the screen is known but the text stays off it (the startup picture) */
static void (*held_tick)(void);
static int tick_busy;
static struct ck_x86_log klog;

int ck_x86_com1_probe(void)
{
    ck_outb(COM1 + 7, 0x5a);
    if (ck_inb(COM1 + 7) != 0x5a)
        return com1_on = 0;
    ck_outb(COM1 + 7, 0xa5);
    com1_on = ck_inb(COM1 + 7) == 0xa5;
    return com1_on;
}

static void com1_putc(char c)
{
    /* LSR bit 5: transmit holding register empty. Bounded: never hangs. */
    for (unsigned i = 0; i < 100000 && !(ck_inb(COM1 + 5) & 0x20); i++)
        ck_pause();
    ck_outb(COM1, (uint8_t)c);
}

static void fb_write(const char *s, size_t n)
{
    if (!fb_on)
        return;
    if (fb_busy) {
        fb_on = 0; /* re-entered from a fault inside the drawing: screen off */
        return;
    }
    fb_busy = 1;
    ck_fbcon_write(&fb, s, n);
    fb_busy = 0;
}

static void write_n(const char *s, size_t n)
{
    ck_x86_log_append(&klog, s, n);
    fb_write(s, n);
    if (fb_held && held_tick && !tick_busy) {
        tick_busy = 1; /* a fault while drawing prints, and must not draw again */
        held_tick();
        tick_busy = 0;
    }
    if (!com1_on)
        return;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\n')
            com1_putc('\r');
        com1_putc(s[i]);
    }
}

/* The boot sequence owns the screen while it plays (x86/boot_x86.c): the
 * console stops drawing on it, then starts again on a cleared screen. */
static int fb_paused;
void ck_x86_console_fb_pause(void)
{
    if (fb_on)
        fb_paused = 1, fb_on = 0;
}
void ck_x86_console_fb_resume(void)
{
    if (!fb_paused)
        return;
    fb_paused = 0;
    ck_fbcon_clear(&fb);
    fb_on = 1;
}

int ck_x86_console_fb_on(volatile void *base, uint32_t width, uint32_t height, uint32_t pitch,
                         uint32_t format, uint32_t *scale, uint32_t *cols, uint32_t *rows)
{
    if (held_tick) {
        if (ck_fbcon_attach(&fb, base, width, height, pitch, format) != 0)
            return -1;
        fb_held = 1;
    } else {
        if (ck_fbcon_setup(&fb, base, width, height, pitch, format) != 0)
            return -1;
        fb_on = 1;
        fb_write(klog.buf, klog.len);
    }
    *scale = fb.scale;
    *cols = fb.cols;
    *rows = fb.rows;
    return 0;
}

int ck_x86_console_fb_get(volatile uint32_t **px, uint32_t *width, uint32_t *height, uint32_t *pitch,
                          uint32_t *format)
{
    if (!(fb_on || fb_held) || fb_busy)
        return -1;
    *px = fb.px;
    *width = fb.width;
    *height = fb.height;
    *pitch = fb.pitch;
    *format = fb.format;
    return 0;
}

void ck_x86_console_fb_off(void)
{
    fb_on = fb_held = 0; /* text keeps going to the RAM copy and COM1 */
    held_tick = 0;
}

void ck_x86_console_fb_hold(void (*tick)(void)) { held_tick = tick; }

void ck_x86_console_fb_show(void)
{
    if (!fb_held || fb_busy)
        return;
    fb_held = 0;
    held_tick = 0;
    fb_on = 1;
    fb_busy = 1;
    ck_fbcon_clear(&fb);
    fb_busy = 0;
    fb_write(klog.buf, klog.len);
}

const char *ck_x86_console_log(size_t *len)
{
    *len = klog.len;
    return klog.buf;
}

size_t ck_x86_console_log_dropped(void)
{
    return klog.dropped;
}

void ck_puts(const char *s)
{
    write_n(s, strlen(s));
}

struct pbuf {
    char b[128];
    size_t n;
};

static void pbuf_emit(void *ctx, char c)
{
    struct pbuf *p = ctx;
    p->b[p->n++] = c;
    if (p->n == sizeof p->b) {
        write_n(p->b, p->n);
        p->n = 0;
    }
}

void ck_vprintf(const char *fmt, va_list ap)
{
    struct pbuf p;
    p.n = 0;
    ck_vformat(pbuf_emit, &p, fmt, ap);
    if (p.n)
        write_n(p.b, p.n);
}

void ck_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    ck_vprintf(fmt, ap);
    va_end(ap);
}
