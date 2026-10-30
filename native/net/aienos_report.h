/* aienos_report.h -- a machine's boot report as UDP broadcast datagrams, so
 * the owner does not have to photograph the screen. Same wire format as the
 * iMac report (native/kernel/x86/report_x86.c), with the first word as a
 * parameter: each datagram goes from 0.0.0.0:PORT to 255.255.255.255:PORT in
 * an Ethernet broadcast frame (TTL 1, nothing to configure or answer) and
 * starts with one header line
 *   "<TAG> v=1 run=<16 hex> part=<i>/<n> bytes=<total>\n"
 * followed by bytes [i*CHUNK, i*CHUNK + CHUNK) of the report.
 * Pure functions: no device, no clock, no heap. */
#ifndef AIENOS_NET_REPORT_H
#define AIENOS_NET_REPORT_H
#include <stddef.h>
#include <stdint.h>

#define NET_REPORT_PORT 47031u   /* UDP, source and destination (the iMac's port) */
#define NET_REPORT_CHUNK 1024u   /* report bytes per datagram */
#define NET_REPORT_MAX 65536u    /* largest report */
#define NET_REPORT_TAG_SPARK "AIENUX-SPARK-REPORT"
#define NET_REPORT_TAG_MAX 32u

typedef struct {
    uint64_t run;
    unsigned part, parts;
    size_t total;            /* report bytes in all parts */
    size_t offset;           /* where this part's data goes */
    const uint8_t *data;     /* view into the frame */
    size_t len;
} net_report_part;

/* Datagrams needed for a report of len bytes; 0 when len is 0 or too big. */
unsigned net_report_parts(size_t len);
/* Ethernet frame (no FCS) carrying part `part` of the report, broadcast from
 * `mac`. Bytes written, 0 on a bad argument or a buffer too small. */
size_t net_report_frame(const char *tag, const uint8_t mac[6], uint64_t run, const char *log, size_t len,
                        unsigned part, uint8_t *out, size_t cap);
/* Parse one received Ethernet frame. 0 and *out filled when it is a part of
 * a `tag` report whose header, ports, checksums and lengths all agree;
 * negative otherwise. */
int net_report_parse(const char *tag, const uint8_t *frame, size_t n, net_report_part *out);
#endif
