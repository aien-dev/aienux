/* aienos_report.c -- see aienos_report.h. Built on the M6-A stack
 * (aienos_net.c) for every layer below the header line. */
#include "aienos_report.h"

#include <string.h>

#include "aienos_net.h"

static const net_ipv4 ANY = {{0, 0, 0, 0}}, BCAST = {{255, 255, 255, 255}};

static size_t tag_len(const char *tag)
{
    size_t n = 0;
    while (tag && tag[n] && n <= NET_REPORT_TAG_MAX)
        n++;
    return tag && n && n <= NET_REPORT_TAG_MAX ? n : 0;
}

static void hex16(char *out, uint64_t v)
{
    for (int i = 15; i >= 0; i--, v >>= 4)
        out[i] = "0123456789abcdef"[v & 15];
}

static size_t dec(char *out, unsigned long v)
{
    char t[20];
    size_t n = 0, k = 0;
    do
        t[n++] = (char)('0' + v % 10);
    while ((v /= 10) && n < sizeof t);
    while (n)
        out[k++] = t[--n];
    return k;
}

unsigned net_report_parts(size_t len)
{
    if (!len || len > NET_REPORT_MAX)
        return 0;
    return (unsigned)((len + NET_REPORT_CHUNK - 1) / NET_REPORT_CHUNK);
}

size_t net_report_frame(const char *tag, const uint8_t mac[6], uint64_t run, const char *log, size_t len,
                        unsigned part, uint8_t *out, size_t cap)
{
    uint8_t data[NET_REPORT_TAG_MAX + 96 + NET_REPORT_CHUNK], udp[8 + sizeof data], ip[20 + sizeof udp];
    unsigned parts = net_report_parts(len);
    size_t t = tag_len(tag);
    if (!t || !mac || !log || !out || part >= parts)
        return 0;
    size_t off = (size_t)part * NET_REPORT_CHUNK;
    size_t n = len - off < NET_REPORT_CHUNK ? len - off : NET_REPORT_CHUNK;
    size_t h = 0;
    memcpy(data, tag, t);
    h += t;
    memcpy(data + h, " v=1 run=", 9);
    h += 9;
    hex16((char *)data + h, run);
    h += 16;
    memcpy(data + h, " part=", 6);
    h += 6;
    h += dec((char *)data + h, part);
    data[h++] = '/';
    h += dec((char *)data + h, parts);
    memcpy(data + h, " bytes=", 7);
    h += 7;
    h += dec((char *)data + h, (unsigned long)len);
    data[h++] = '\n';
    memcpy(data + h, log + off, n);

    net_udp_header uh = {NET_REPORT_PORT, NET_REPORT_PORT};
    size_t uw = 0, iw = 0, ew = 0;
    if (net_udp_build(&uh, data, h + n, ANY, BCAST, udp, sizeof udp, &uw) != NET_OK)
        return 0;
    net_ipv4_header ih;
    ih.dscp_ecn = 0;
    ih.identification = (uint16_t)part;
    ih.ttl = 1; /* this network only */
    ih.protocol = NET_IPPROTO_UDP;
    ih.source = ANY;
    ih.destination = BCAST;
    if (net_ipv4_build(&ih, udp, uw, ip, sizeof ip, &iw) != NET_OK)
        return 0;
    net_eth_frame e;
    memset(e.destination.b, 0xff, 6);
    memcpy(e.source.b, mac, 6);
    e.ethertype = NET_ETHERTYPE_IPV4;
    e.payload = ip;
    e.payload_len = iw;
    if (net_eth_build(&e, out, cap, &ew) != NET_OK)
        return 0;
    return ew;
}

/* Reads a decimal number ending at `stop`; -1 on no digits or overflow. */
static long num(const uint8_t *p, size_t n, size_t *i, char stop)
{
    unsigned long v = 0;
    size_t s = *i;
    while (*i < n && p[*i] >= '0' && p[*i] <= '9' && *i - s < 9)
        v = v * 10 + (unsigned long)(p[(*i)++] - '0');
    if (*i == s || *i >= n || p[*i] != stop)
        return -1;
    (*i)++;
    return (long)v;
}

static int lit(const uint8_t *p, size_t n, size_t *i, const char *s, size_t sn)
{
    if (n - *i < sn || memcmp(p + *i, s, sn) != 0)
        return 0;
    *i += sn;
    return 1;
}

int net_report_parse(const char *tag, const uint8_t *frame, size_t n, net_report_part *out)
{
    size_t t = tag_len(tag);
    if (!t || !frame || !out)
        return -1;
    net_eth_frame e;
    if (net_eth_parse(frame, n, &e) != NET_OK || e.ethertype != NET_ETHERTYPE_IPV4)
        return -2;
    net_ipv4_header ih;
    const uint8_t *ipp = 0, *up = 0;
    size_t ipn = 0, un = 0;
    if (net_ipv4_parse(e.payload, e.payload_len, &ih, &ipp, &ipn) != NET_OK || ih.protocol != NET_IPPROTO_UDP)
        return -3;
    net_udp_header uh;
    if (net_udp_parse(ipp, ipn, ih.source, ih.destination, &uh, &up, &un) != NET_OK ||
        uh.destination_port != NET_REPORT_PORT)
        return -4;
    size_t i = 0;
    if (!lit(up, un, &i, tag, t) || !lit(up, un, &i, " v=1 run=", 9) || un - i < 16)
        return -5;
    uint64_t run = 0;
    for (int k = 0; k < 16; k++) {
        uint8_t c = up[i++];
        unsigned v = c >= '0' && c <= '9' ? (unsigned)(c - '0') : c >= 'a' && c <= 'f' ? (unsigned)(c - 'a') + 10u : 16u;
        if (v > 15)
            return -5;
        run = run << 4 | v;
    }
    long part, parts, total;
    if (!lit(up, un, &i, " part=", 6) || (part = num(up, un, &i, '/')) < 0 || (parts = num(up, un, &i, ' ')) < 0 ||
        !lit(up, un, &i, "bytes=", 6) || (total = num(up, un, &i, '\n')) < 0)
        return -6;
    if (net_report_parts((size_t)total) != (unsigned long)parts || part >= parts)
        return -7;
    size_t off = (size_t)part * NET_REPORT_CHUNK;
    size_t want = (size_t)total - off < NET_REPORT_CHUNK ? (size_t)total - off : NET_REPORT_CHUNK;
    if (un - i != want)
        return -8;
    out->run = run;
    out->part = (unsigned)part;
    out->parts = (unsigned)parts;
    out->total = (size_t)total;
    out->offset = off;
    out->data = up + i;
    out->len = want;
    return 0;
}
