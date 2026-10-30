/* courier_x86.c -- Allen's courier on the second serial port (COM2, I/O
 * 0x2f8), a 16550 like the console's COM1. Polled from the hold loop, no
 * interrupt: LSR bit 0 (Data Ready) says a byte waits in RBR, LSR bit 5
 * (THRE) that the next may be written (PC16550D datasheet, Table II and
 * section 8.5; recorded with docs-read). 115200 8N1, FIFOs on.
 *
 * The wire speaks lines. "HELLO <who>" is a call: whoever hears one answers
 * "HI <who>" and is ready; "HI" is the answer and is never answered, so the
 * exchange always ends. This side calls once when the port is set up; the far
 * side calls until it hears either. It does not matter who was up first, nor
 * that setting up the port clears the FIFOs and so eats whatever the far side
 * sent before (in QEMU the far side is a relay on the host joined to -serial
 * unix:...; on hardware with no second port, or nobody on it, Allen stays on
 * his own). Lines longer than the buffer are dropped whole rather than cut. */
#include "x86.h"
#include "../svc/courier.h"

#define COM2 0x2f8
#define LINE_MAX 1024u

static int on, hello;
static char in[LINE_MAX];
static unsigned in_n;
static int in_drop;        /* the current line overflowed: drop it to its newline */
static char line[LINE_MAX]; /* a whole line waiting for take() */
static unsigned line_n;
static int line_ready;

static int probe(void)
{
    ck_outb(COM2 + 7, 0x5a);
    if (ck_inb(COM2 + 7) != 0x5a)
        return 0;
    ck_outb(COM2 + 7, 0xa5);
    return ck_inb(COM2 + 7) == 0xa5;
}

static void putc_(char c)
{
    for (unsigned i = 0; i < 100000 && !(ck_inb(COM2 + 5) & 0x20); i++)
        ck_pause();
    ck_outb(COM2, (uint8_t)c);
}

static int send(const char *s, unsigned n)
{
    if (!on)
        return -1;
    for (unsigned i = 0; i < n; i++)
        putc_(s[i] == '\n' ? ' ' : s[i]);
    putc_('\n');
    return 0;
}

static int ready(void) { return on && hello; }

/* Reads what waits in the FIFO; a complete line is kept for take(). Reading
 * stops while a line waits, so the next one stays in the port (the far side
 * waits for room) rather than being dropped behind it. */
static void pump(void)
{
    while (!line_ready && (ck_inb(COM2 + 5) & 0x01)) {
        char c = (char)ck_inb(COM2);
        if (c == '\r')
            continue;
        if (c != '\n') {
            if (in_n + 1 < LINE_MAX)
                in[in_n++] = c;
            else
                in_drop = 1;
            continue;
        }
        in[in_n] = 0;
        if (!in_drop && in_n) {
            if (in[0] == 'H' && in[1] == 'E' && in[2] == 'L' && in[3] == 'L' && in[4] == 'O' &&
                (in[5] == 0 || in[5] == ' ')) {
                hello = 1;
                send("HI allen", 8);
            } else if (in[0] == 'H' && in[1] == 'I' && (in[2] == 0 || in[2] == ' ')) {
                hello = 1;
            } else if (!line_ready) {
                for (unsigned i = 0; i <= in_n; i++)
                    line[i] = in[i];
                line_n = in_n;
                line_ready = 1;
            }
        }
        in_n = 0;
        in_drop = 0;
    }
}

static int take(char *out, unsigned cap)
{
    if (!on)
        return 0;
    pump();
    if (!line_ready || !out || !cap)
        return 0;
    unsigned n = line_n < cap - 1 ? line_n : cap - 1;
    for (unsigned i = 0; i < n; i++)
        out[i] = line[i];
    out[n] = 0;
    line_ready = 0;
    return 1;
}

static const ck_courier courier = {ready, send, take};

const ck_courier *ck_x86_courier_init(void)
{
    if (!probe())
        return 0;
    ck_outb(COM2 + 1, 0x00); /* no interrupts: polled */
    ck_outb(COM2 + 3, 0x80); /* DLAB on: divisor */
    ck_outb(COM2 + 0, 0x01); /* 115200 */
    ck_outb(COM2 + 1, 0x00);
    ck_outb(COM2 + 3, 0x03); /* 8N1, DLAB off */
    ck_outb(COM2 + 2, 0xc7); /* FIFOs on and cleared, 14-byte trigger */
    ck_outb(COM2 + 4, 0x0b); /* DTR, RTS, OUT2 */
    on = 1;
    send("HELLO allen", 11);
    return &courier;
}

int ck_x86_courier_hello(void)
{
    if (!on)
        return 0;
    pump();
    return hello;
}
