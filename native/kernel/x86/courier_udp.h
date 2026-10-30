/* courier_udp.h -- Allen's courier over Ethernet (x86/courier_udp_x86.c):
 * the same lines as the serial courier (svc/courier.h), carried to the far-off
 * worker on the Spark in UDP datagrams, the way the test computer's netboot
 * loader and lab report already talk to it: Ethernet broadcast frames, source
 * address 0.0.0.0 (this kernel has no address of its own and asks for none),
 * destination 255.255.255.255, one port both ways. No ARP, no DHCP, no
 * floating point. Lab image only (make imac-kernel CK_X86_REPORT=1): the
 * default image has no way to send or receive a frame.
 *
 * Every datagram is a text head and a body:
 *   "ALLEN-COURIER v=1 run=<16 hex> kind=<K> seq=<n> part=<i>/<n> bytes=<total>\n<body>"
 * run names this boot (the kernel's clock at start mixed with its MAC); the
 * far side echoes it and the kernel ignores any other. Kinds:
 *   HELLO  kernel -> far side, every 2 s until HI, then every 10 s (a keepalive)
 *   HI     far side -> kernel; ready() is 1 while one came within 30 s
 *   JOB    kernel -> far side, the job line cut into parts of 1000 bytes,
 *          the whole set sent again every 2 s until ACK or the first LINE
 *   ACK    far side -> kernel: the job arrived whole
 *   LINE   far side -> kernel: one answer line ("SAY ..." or "WAIT ...") per
 *          datagram, part = its number within the job's answer (from 1)
 *   GOT    kernel -> far side for every LINE taken: the far side sends a line
 *          again until it hears GOT, so no part is lost; a line out of order
 *          is dropped and comes again in its turn
 * Lines taken through take() are the LINE bodies, in order, never twice.
 * The frame device is handed in (ck_netio), so the same code runs on the
 * host against a real relay (x86/tests/courier_udp_test.c): the kernel binds
 * it to the Broadcom chip (tg3_x86.c) and its clock. */
#ifndef AIENOS_COURIER_UDP_H
#define AIENOS_COURIER_UDP_H
#include <stddef.h>
#include <stdint.h>
#include "../svc/courier.h"

#define CK_COURIER_PORT 47033u      /* both ends (the relay and the scripts read this line) */
#define CK_COURIER_PART 1000u       /* body bytes per JOB datagram */
#define CK_COURIER_JOB_MAX 4096u    /* the longest job line */
#define CK_COURIER_LINE_MAX 320u    /* the longest answer line taken */

typedef struct ck_netio {
    void *ctx;
    int (*send)(void *ctx, const uint8_t *frame, size_t n); /* a whole Ethernet frame: 0, or -1 */
    size_t (*recv)(void *ctx, uint8_t *frame, size_t cap);  /* the next frame waiting, bytes; 0: none */
    int (*link)(void *ctx);                                  /* 1 while frames can go */
    uint64_t (*now)(void *ctx);                              /* a clock counting hz per second */
} ck_netio;

/* Sets the courier up on io (kept by reference) with this machine's MAC,
 * its source address (0.0.0.0 when none) and the far side's (broadcast by
 * default), and calls HELLO. Returns the courier; never NULL. One instance. */
const ck_courier *ck_courier_udp_init(const ck_netio *io, const uint8_t mac[6], const uint8_t ip[4],
                                      const uint8_t peer_ip[4], uint16_t port, uint64_t hz);
/* Counters for the lab report (no words): datagrams in, out, lines taken,
 * job resends, dropped frames. */
void ck_courier_udp_stats(unsigned *in, unsigned *out, unsigned *lines, unsigned *resends, unsigned *dropped);
/* The run id of this instance (0 before init). */
uint64_t ck_courier_udp_run(void);
#endif
