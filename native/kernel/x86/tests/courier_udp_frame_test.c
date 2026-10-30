/* Actual Ethernet test: no UDP unwrapping/rewrapping between the kernel
 * courier and allen-courier. Run only inside ck_courier_udp_frame_test.sh's
 * disposable network namespace. The captured relay LINE also drives refusal
 * and duplicate controls through the courier's real receive filter. */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "../../../net/aienos_net.h"
#include "../courier_udp.h"

#define FRAME_MAX 1600u
static struct {
    int fd, hold, drop_got;
    struct sockaddr_ll wire;
    uint8_t mac[6], first[FRAME_MAX], inject[FRAME_MAX];
    size_t first_n, inject_n;
    unsigned got, lines;
    const char *out;
} dev;

static void require(int ok, const char *why)
{
    if (!ok) {
        fprintf(stderr, "COURIER_FRAMES: FAIL %s (errno=%d)\n", why, errno);
        exit(1);
    }
}
static uint64_t now(void *ctx)
{
    (void)ctx;
    struct timespec t;
    require(clock_gettime(CLOCK_MONOTONIC, &t) == 0, "clock");
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}
static int link_up(void *ctx) { (void)ctx; return 1; }
static void save(const char *name, const uint8_t *f, size_t n)
{
    char path[4096];
    require(snprintf(path, sizeof path, "%s/%s.frame", dev.out, name) < (int)sizeof path, "frame path");
    FILE *file = fopen(path, "wb");
    require(file != NULL, "open frame output");
    require(fwrite(f, 1, n, file) == n, "write frame output");
    require(fclose(file) == 0, "close frame output");
}
static int decode(const uint8_t *f, size_t n, net_eth_frame *e, net_ipv4_header *ih,
                  net_udp_header *uh, const uint8_t **data, size_t *dn)
{
    const uint8_t *ip;
    size_t ipn;
    return net_eth_parse(f, n, e) == NET_OK && e->ethertype == NET_ETHERTYPE_IPV4 &&
           net_ipv4_parse(e->payload, e->payload_len, ih, &ip, &ipn) == NET_OK &&
           ih->protocol == NET_IPPROTO_UDP &&
           net_udp_parse(ip, ipn, ih->source, ih->destination, uh, data, dn) == NET_OK &&
           uh->source_port == CK_COURIER_PORT && uh->destination_port == CK_COURIER_PORT;
}
static int kind(const uint8_t *d, size_t n, const char *name)
{
    char text[2048], key[32];
    if (n >= sizeof text) return 0;
    memcpy(text, d, n); text[n] = 0;
    snprintf(key, sizeof key, " kind=%s ", name);
    return strstr(text, key) != NULL;
}
static int transmit(void *ctx, const uint8_t *f, size_t n)
{
    (void)ctx;
    net_eth_frame e;
    net_ipv4_header ih;
    net_udp_header uh;
    const uint8_t *data;
    size_t dn;
    const uint8_t zero[4] = {0}, limited[4] = {255,255,255,255}, broadcast[6] = {255,255,255,255,255,255};
    require(decode(f, n, &e, &ih, &uh, &data, &dn), "emitted frame/checksum");
    require(!memcmp(ih.source.b, zero, 4) && !memcmp(ih.destination.b, limited, 4) &&
            !memcmp(e.destination.b, broadcast, 6), "outgoing zero-IP limited broadcast");
    if (kind(data, dn, "HELLO")) save("hello", f, n);
    if (kind(data, dn, "JOB")) save("job", f, n);
    if (kind(data, dn, "GOT")) {
        dev.got++;
        save("got", f, n);
        if (dev.drop_got) return 0;
    }
    return sendto(dev.fd, f, n, 0, (struct sockaddr *)&dev.wire, sizeof dev.wire) == (ssize_t)n ? 0 : -1;
}
static size_t receive(void *ctx, uint8_t *f, size_t cap)
{
    (void)ctx;
    if (dev.inject_n) {
        size_t n = dev.inject_n;
        require(n <= cap, "injected frame bound");
        memcpy(f, dev.inject, n); dev.inject_n = 0;
        return n;
    }
    if (dev.hold == 2) return 0;
    for (;;) {
        struct sockaddr_ll from;
        socklen_t flen = sizeof from;
        ssize_t n = recvfrom(dev.fd, f, cap, MSG_DONTWAIT, (struct sockaddr *)&from, &flen);
        if (n < 0) { require(errno == EAGAIN || errno == EWOULDBLOCK, "raw receive"); return 0; }
        if (from.sll_pkttype == PACKET_OUTGOING) continue;
        net_eth_frame e;
        net_ipv4_header ih;
        net_udp_header uh;
        const uint8_t *data;
        size_t dn;
        if (!decode(f, (size_t)n, &e, &ih, &uh, &data, &dn)) continue;
        const uint8_t spark[4] = {192,168,1,108}, subnet[4] = {192,168,1,255};
        require(!memcmp(ih.source.b, spark, 4) && !memcmp(ih.destination.b, subnet, 4), "real relay reply addresses");
        for (unsigned i = 0; i < 6; i++) require(e.destination.b[i] == 255, "real relay broadcast MAC");
        if (kind(data, dn, "HI")) save("hi", f, (size_t)n);
        if (kind(data, dn, "ACK")) save("ack", f, (size_t)n);
        if (kind(data, dn, "LINE")) {
            dev.lines++;
            char name[32];
            snprintf(name, sizeof name, "line-%u", dev.lines);
            save(name, f, (size_t)n);
            if (dev.hold == 1) {
                memcpy(dev.first, f, (size_t)n); dev.first_n = (size_t)n;
                save("line", f, (size_t)n);
                dev.hold = 2;
                return 0;
            }
        }
        return (size_t)n;
    }
}
static void inject(const uint8_t *f, size_t n)
{
    require(n <= sizeof dev.inject && !dev.inject_n, "single injected frame");
    memcpy(dev.inject, f, n); dev.inject_n = n;
}
static void refusals(const ck_courier *c)
{
    uint8_t bad[FRAME_MAX];
    char line[CK_COURIER_LINE_MAX];
    unsigned before = dev.got;
    memcpy(bad, dev.first, dev.first_n);
    bad[0] = 0x02; /* neither ours nor broadcast */
    inject(bad, dev.first_n);
    require(!c->take(line, sizeof line) && dev.got == before, "not-ours MAC must not GOT");
    memcpy(bad, dev.first, dev.first_n);
    bad[dev.first_n - 1] ^= 1; /* checksum no longer matches */
    inject(bad, dev.first_n);
    require(!c->take(line, sizeof line) && dev.got == before, "bad UDP checksum must not GOT");
    inject(dev.first, 30);
    require(!c->take(line, sizeof line) && dev.got == before, "truncated IP must not GOT");
    /* Change only run, then produce a valid UDP checksum: this is a protocol
     * refusal, not accidentally another checksum refusal. */
    memcpy(bad, dev.first, dev.first_n);
    net_eth_frame e;
    net_ipv4_header ih;
    net_udp_header uh;
    const uint8_t *data;
    size_t dn, written;
    require(decode(bad, dev.first_n, &e, &ih, &uh, &data, &dn), "control decode");
    size_t off = (size_t)(data - bad);
    size_t run_at = off + strlen("ALLEN-COURIER v=1 run=");
    bad[run_at] = bad[run_at] == '0' ? '1' : '0';
    require(net_udp_build(&uh, bad + off, dn, ih.source, ih.destination, bad + off - 8,
                          sizeof bad - off + 8, &written) == NET_OK, "control checksum");
    require(decode(bad, dev.first_n, &e, &ih, &uh, &data, &dn), "wrong-run valid checksums");
    inject(bad, dev.first_n);
    require(!c->take(line, sizeof line) && dev.got == before, "wrong run must not GOT");
    inject(dev.first, dev.first_n);
    require(c->take(line, sizeof line) && dev.got == before + 1, "real LINE must GOT immediately");
    printf("real LINE body: %s\n", line);
    inject(dev.first, dev.first_n);
    require(!c->take(line, sizeof line) && dev.got == before + 2, "duplicate LINE must GOT without duplicate body");
    puts("COURIER_FRAME_FILTER: PASS wrong-run/not-ours/bad-checksum/truncated/valid/duplicate");
}
int main(int argc, char **argv)
{
    require(argc == 3 || argc == 4, "usage: courier_udp_frame_test IFACE OUTPUT_DIR [drop-got]");
    dev.out = argv[2]; dev.hold = 1; dev.drop_got = argc == 4;
    dev.fd = socket(AF_PACKET, SOCK_RAW, htons(0x0800));
    require(dev.fd >= 0, "raw socket (requires isolated CAP_NET_RAW)");
    dev.wire.sll_family = AF_PACKET;
    dev.wire.sll_protocol = htons(0x0800);
    dev.wire.sll_ifindex = (int)if_nametoindex(argv[1]);
    require(dev.wire.sll_ifindex != 0 && !bind(dev.fd, (struct sockaddr *)&dev.wire, sizeof dev.wire), "bind raw interface");
    struct ifreq req = {0};
    require(strlen(argv[1]) < sizeof req.ifr_name, "interface name");
    strcpy(req.ifr_name, argv[1]);
    require(!ioctl(dev.fd, SIOCGIFHWADDR, &req), "interface MAC");
    memcpy(dev.mac, req.ifr_hwaddr.sa_data, 6);
    const ck_netio io = {NULL, transmit, receive, link_up, now};
    const uint8_t zero[4] = {0}, limited[4] = {255,255,255,255};
    const ck_courier *c = ck_courier_udp_init(&io, dev.mac, zero, limited, CK_COURIER_PORT, 1000);
    uint64_t start = now(NULL);
    char line[CK_COURIER_LINE_MAX];
    while (!c->ready()) {
        c->take(line, sizeof line);
        require(now(NULL) - start < 5000, "no real HI for source-zero HELLO");
        usleep(1000);
    }
    const char job[] = "JOB 1 chat Answer as Allen.\thello allen, how was your day\tself=Allen\tname=Drake\tstarts=20\tnote asked: hi / said: Hello";
    require(!c->send(job, sizeof job - 1), "send real job");
    while (!dev.first_n) {
        c->take(line, sizeof line);
        require(now(NULL) - start < 5000, "no real LINE for job");
        usleep(1000);
    }
    refusals(c);
    dev.hold = 0;
    int whole = 0;
    while (!whole) {
        if (c->take(line, sizeof line)) {
            unsigned seq, part, total;
            printf("real LINE body: %s\n", line);
            if (sscanf(line, "SAY %u %u/%u", &seq, &part, &total) == 3 && total && part == total) whole = 1;
        }
        require(now(NULL) - start < 5000, "answer did not finish");
        usleep(1000);
    }
    unsigned before = dev.lines;
    uint64_t quiet = now(NULL);
    while (now(NULL) - quiet < 700) {
        c->take(line, sizeof line);
        usleep(1000);
    }
    require(dev.lines == before, "relay retransmitted LINE after GOT (ACK path failed)");
    printf("COURIER_FRAMES: PASS zero-IP HELLO/JOB/GOT limited broadcast, real subnet HI/ACK/LINE, %u LINE frames %u GOT frames\n", dev.lines, dev.got);
    close(dev.fd);
    return 0;
}
