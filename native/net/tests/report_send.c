/* report_send.c -- host tool for scripts/ck_spark_report_test.sh: builds the
 * Spark boot report frames with native/net/aienos_report.c exactly as the
 * kernel does (net_report_frame), checks each with net_report_parse, and
 * sends each frame's UDP payload to 127.0.0.1:PORT, last part first and
 * every part twice, so the listener must reorder and ignore repeats.
 *   report_send PORT INFILE */
#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../aienos_net.h"
#include "../aienos_report.h"

int main(int argc, char **argv)
{
    static char log[NET_REPORT_MAX + 1];
    if (argc != 3)
        return 2;
    FILE *f = fopen(argv[2], "rb");
    size_t len = f ? fread(log, 1, sizeof log, f) : 0;
    if (f)
        fclose(f);
    unsigned parts = net_report_parts(len);
    if (!parts) {
        fprintf(stderr, "report_send: bad input length %zu\n", len);
        return 1;
    }
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in to;
    memset(&to, 0, sizeof to);
    to.sin_family = AF_INET;
    to.sin_port = htons((uint16_t)atoi(argv[1]));
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    static const uint8_t mac[6] = {0x48, 0x11, 0x22, 0x33, 0x44, 0x55};
    for (int rep = 0; rep < 2; rep++)
        for (unsigned k = 0; k < parts; k++) {
            unsigned p = parts - 1 - k;
            uint8_t fr[1600];
            size_t n = net_report_frame(NET_REPORT_TAG_SPARK, mac, 0xfedcba9876543210ull, log, len, p, fr, sizeof fr);
            net_report_part rp;
            net_eth_frame e;
            net_ipv4_header ih;
            net_udp_header uh;
            const uint8_t *ipp, *up;
            size_t ipn, un;
            if (!n || net_report_parse(NET_REPORT_TAG_SPARK, fr, n, &rp) || rp.part != p ||
                net_eth_parse(fr, n, &e) || net_ipv4_parse(e.payload, e.payload_len, &ih, &ipp, &ipn) ||
                net_udp_parse(ipp, ipn, ih.source, ih.destination, &uh, &up, &un)) {
                fprintf(stderr, "report_send: part %u does not build or parse\n", p);
                return 1;
            }
            if (sendto(s, up, un, 0, (struct sockaddr *)&to, sizeof to) != (ssize_t)un) {
                perror("sendto");
                return 1;
            }
        }
    close(s);
    printf("sent %u parts twice, %zu bytes\n", parts, len);
    return 0;
}
