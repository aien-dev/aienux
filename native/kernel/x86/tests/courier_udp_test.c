/* courier_udp_test.c -- host test of Allen's Ethernet courier (x86/courier_udp_x86.c)
 * against the real relay (crates/allen-courier --udp).
 *
 * The courier is the kernel's own code, built with -DCK_COURIER_HOST. Its frame
 * device is a UDP socket: a frame the courier sends is unwrapped (Ethernet, IP,
 * UDP, by the same net library the kernel uses) and its datagram sent to the
 * relay's port on this machine; a datagram the relay sends back is wrapped into
 * an Ethernet frame the courier reads. Optionally every Nth frame is lost in each
 * direction (a seeded pseudo-random 1 in DROP_EVERY), to prove the resend and the
 * "GOT" rules.
 *
 * usage: courier_udp_test RELAY_PORT DROP_EVERY JOB_FILE [MAX_SECONDS]
 * Prints plain lines, then "COURIER_UDP: PASS ..." or "COURIER_UDP: FAIL <why>". */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "../../../net/aienos_net.h"
#include "../courier_udp.h"

typedef struct {
    int fd;
    struct sockaddr_in relay;
    unsigned drop_every;
    unsigned sent, got, lost_out, lost_in;
    uint32_t rng; /* xorshift: the same losses every run */
    uint8_t mac[6];
} dev;

static uint64_t ms_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}

/* 1 about once in `every` calls (0: never). A strict every-Nth rule can line up with the
 * resend cycle and starve one datagram forever, which no real wire does. */
static int lose(dev *d)
{
    if (!d->drop_every)
        return 0;
    d->rng ^= d->rng << 13, d->rng ^= d->rng >> 17, d->rng ^= d->rng << 5;
    return d->rng % d->drop_every == 0;
}

static int d_send(void *ctx, const uint8_t *frame, size_t n)
{
    dev *d = ctx;
    net_eth_frame e;
    net_ipv4_header ih;
    net_udp_header uh;
    const uint8_t *ip, *data;
    size_t ipn, dn;
    if (net_eth_parse(frame, n, &e) != NET_OK || e.ethertype != NET_ETHERTYPE_IPV4)
        return -1;
    if (net_ipv4_parse(e.payload, e.payload_len, &ih, &ip, &ipn) != NET_OK || ih.protocol != NET_IPPROTO_UDP)
        return -1;
    if (net_udp_parse(ip, ipn, ih.source, ih.destination, &uh, &data, &dn) != NET_OK)
        return -1;
    if (uh.destination_port != CK_COURIER_PORT || uh.source_port != CK_COURIER_PORT)
        return -1;
    if (e.destination.b[0] != 0xff || ih.destination.b[0] != 255 || ih.source.b[0] != 0)
        return -1; /* the lab shape: broadcast out, source address 0.0.0.0 */
    d->sent++;
    if (lose(d)) {
        d->lost_out++;
        return 0;
    }
    return sendto(d->fd, data, dn, 0, (struct sockaddr *)&d->relay, sizeof d->relay) == (ssize_t)dn ? 0 : -1;
}

static size_t d_recv(void *ctx, uint8_t *frame, size_t cap)
{
    dev *d = ctx;
    static uint8_t dg[2048], udp[2100], ipb[2200];
    ssize_t n = recvfrom(d->fd, dg, sizeof dg, 0, 0, 0);
    if (n <= 0)
        return 0;
    d->got++;
    if (lose(d)) {
        d->lost_in++;
        return 0;
    }
    const net_ipv4 src = {{192, 168, 1, 108}}, dst = {{255, 255, 255, 255}};
    net_udp_header uh = {CK_COURIER_PORT, CK_COURIER_PORT};
    size_t uw = 0, iw = 0, ew = 0;
    if (net_udp_build(&uh, dg, (size_t)n, src, dst, udp, sizeof udp, &uw) != NET_OK)
        return 0;
    net_ipv4_header ih = {0};
    ih.ttl = 64;
    ih.protocol = NET_IPPROTO_UDP;
    ih.source = src;
    ih.destination = dst;
    if (net_ipv4_build(&ih, udp, uw, ipb, sizeof ipb, &iw) != NET_OK)
        return 0;
    net_eth_frame e;
    memset(e.destination.b, 0xff, 6);
    memset(e.source.b, 0x11, 6);
    e.ethertype = NET_ETHERTYPE_IPV4;
    e.payload = ipb;
    e.payload_len = iw;
    if (net_eth_build(&e, frame, cap, &ew) != NET_OK)
        return 0;
    return ew;
}

static int d_link(void *ctx)
{
    (void)ctx;
    return 1;
}
static uint64_t d_now(void *ctx)
{
    (void)ctx;
    return ms_now();
}

static int fail(const char *why)
{
    printf("COURIER_UDP: FAIL %s\n", why);
    return 1;
}

int main(int argc, char **argv)
{
    if (argc < 4)
        return fail("usage: courier_udp_test RELAY_PORT DROP_EVERY JOB_FILE [MAX_SECONDS]");
    dev d;
    memset(&d, 0, sizeof d);
    d.fd = socket(AF_INET, SOCK_DGRAM, 0);
    d.relay.sin_family = AF_INET;
    d.relay.sin_port = htons((uint16_t)atoi(argv[1]));
    inet_pton(AF_INET, "127.0.0.1", &d.relay.sin_addr);
    d.drop_every = (unsigned)atoi(argv[2]);
    d.rng = 0x2545f491u;
    unsigned max_s = argc > 4 ? (unsigned)atoi(argv[4]) : 60;
    fcntl(d.fd, F_SETFL, O_NONBLOCK);
    static char job[CK_COURIER_JOB_MAX];
    FILE *f = fopen(argv[3], "r");
    if (!f)
        return fail("no job file");
    size_t jn = fread(job, 1, sizeof job - 1, f);
    fclose(f);
    while (jn && (job[jn - 1] == '\n' || job[jn - 1] == '\r'))
        jn--;
    job[jn] = 0;

    const ck_netio io = {&d, d_send, d_recv, d_link, d_now};
    const uint8_t mac[6] = {0x00, 0x1f, 0xf3, 0x12, 0x34, 0x56}, none[4] = {0, 0, 0, 0}, peer[4] = {255, 255, 255, 255};
    const ck_courier *c = ck_courier_udp_init(&io, mac, none, peer, CK_COURIER_PORT, 1000);
    if (!c)
        return fail("no courier");

    uint64_t t0 = ms_now();
    char line[CK_COURIER_LINE_MAX];
    while (!c->ready()) {
        c->take(line, sizeof line); /* pumps */
        if (ms_now() - t0 > 10000)
            return fail("no HI from the relay within 10 s");
        usleep(2000);
    }
    uint64_t t_ready = ms_now() - t0;
    printf("courier test: the relay said hello after %llu ms\n", (unsigned long long)t_ready);

    if (c->send(job, jn))
        return fail("the courier would not take the job");
    uint64_t t1 = ms_now(), t_first = 0, t_whole = 0;
    unsigned parts = 0, expect = 1, waits = 0;
    char said[1024];
    said[0] = 0;
    for (;;) {
        if (ms_now() - t1 > max_s * 1000u)
            return fail("the answer did not come whole in time");
        if (!c->take(line, sizeof line)) {
            usleep(2000);
            continue;
        }
        unsigned seq = 0, i = 0, n = 0;
        if (sscanf(line, "WAIT %u", &seq) == 1) {
            waits++;
            continue;
        }
        int off = 0;
        if (sscanf(line, "SAY %u %u/%u %n", &seq, &i, &n, &off) < 3 || off == 0)
            return fail("a line that is neither SAY nor WAIT");
        if (i != expect)
            return fail("answer parts out of order or repeated");
        expect++;
        parts++;
        if (!t_first)
            t_first = ms_now() - t1;
        if (strlen(said) + strlen(line + off) + 2 < sizeof said) {
            if (said[0])
                strcat(said, " ");
            strcat(said, line + off);
        }
        printf("courier test: part %u/%u: %s\n", i, n, line + off);
        if (n && i == n) {
            t_whole = ms_now() - t1;
            break;
        }
    }
    unsigned in, out, lines, resends, dropped;
    ck_courier_udp_stats(&in, &out, &lines, &resends, &dropped);
    printf("courier test: first part after %llu ms, whole after %llu ms, %u parts, %u waits; datagrams in %u out %u, "
           "lines taken %u, job resends %u, dropped %u; lost out %u in %u\n",
           (unsigned long long)t_first, (unsigned long long)t_whole, parts, waits, in, out, lines, resends, dropped,
           d.lost_out, d.lost_in);
    if (!said[0] || strstr(said, "lost my thread"))
        return fail("the answer had no words (the model was not reached)");
    if (d.drop_every && !d.lost_out && !d.lost_in)
        return fail("loss was asked for and none happened");
    printf("COURIER_UDP: PASS (%u parts, first %llu ms, whole %llu ms, drop every %u)\n", parts,
           (unsigned long long)t_first, (unsigned long long)t_whole, d.drop_every);
    return 0;
}
