/* courier.h -- A courier carries lines between Allen and a far-off worker
 * (a language model on another computer), one line each way, newline ended.
 * Allen never sees the wire: he hands the courier a job line
 * (svc/allen_intent.h) and takes answers back when they come. Which wire is
 * the machine's business: the test computer's x86 kernel offers a courier on
 * its second serial port (x86/courier_x86.c) when one is there; Ethernet
 * comes once the network card may receive (aienos#411). Without a courier
 * Allen answers from this computer alone, as before. */
#ifndef AIENOS_COURIER_H
#define AIENOS_COURIER_H

typedef struct ck_courier {
    /* 1 once the far side has called, or answered our call, on this wire. */
    int (*ready)(void);
    /* Sends the line (n bytes, no newline) and a newline: 0, or -1 if it could not be sent. */
    int (*send)(const char *line, unsigned n);
    /* A whole line from the far side, newline removed, NUL-terminated, at
     * most cap - 1 bytes: 1, or 0 when none has arrived yet. */
    int (*take)(char *out, unsigned cap);
} ck_courier;

#endif
