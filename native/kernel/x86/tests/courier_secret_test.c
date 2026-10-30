/* courier_secret_test.c -- the Ethernet courier must never carry or keep the wizard's API key.
 *
 * The real courier (x86/courier_udp_x86.c) is compiled into this file with -DCK_COURIER_HOST,
 * so the test reads its static job buffer. The clock, the link and the frames are mocks: every
 * frame the courier sends is unwrapped by the same net library the kernel uses and recorded;
 * the frames it reads are built here. Checks:
 *   1. a "KEYTEST" line (empty, with a space, or a whole key test) is refused before anything is
 *      sent, and leaves an earlier job's bytes, counters and resend timer untouched;
 *   2. a normal job is sent, and sent again every 2 s until it is answered;
 *   3. an ACK or a first LINE with the job's own number clears every byte of the job buffer and
 *      ends the resends; a wrong number, or another boot's run id, does not clear it;
 *   4. a new init clears a job still held.
 * Prints "COURIER_SECRET: PASS" or "COURIER_SECRET: FAIL <why>". */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include COURIER_X86_C

typedef struct {
    uint64_t now;
    unsigned sent, jobs, hellos, hits; /* frames out; JOB frames; HELLO frames; frames holding `needle` */
    const char *needle;
    char in[8][1400]; /* datagrams waiting to be read, as wire text */
    size_t in_n[8];
    unsigned in_cnt, in_pos;
} t_dev;

static t_dev dev;

static int t_send(void *ctx, const uint8_t *fr, size_t n)
{
    t_dev *d = ctx;
    net_eth_frame e;
    net_ipv4_header ih;
    net_udp_header uh;
    const uint8_t *ipp, *data;
    size_t ipn, dn;
    if (net_eth_parse(fr, n, &e) != NET_OK || net_ipv4_parse(e.payload, e.payload_len, &ih, &ipp, &ipn) != NET_OK ||
        net_udp_parse(ipp, ipn, ih.source, ih.destination, &uh, &data, &dn) != NET_OK)
        return -1;
    d->sent++;
    if (dn > 48 && !memcmp(data + 38, " kind=JOB ", 10))
        d->jobs++;
    if (dn > 48 && !memcmp(data + 38, " kind=HELLO ", 12))
        d->hellos++;
    if (d->needle && memmem(data, dn, d->needle, strlen(d->needle)))
        d->hits++;
    return 0;
}

static size_t t_recv(void *ctx, uint8_t *fr, size_t cap)
{
    t_dev *d = ctx;
    static uint8_t udp[1500], ipb[1600];
    if (d->in_pos >= d->in_cnt)
        return 0;
    size_t k = d->in_pos++, uw = 0, iw = 0, ew = 0;
    const net_ipv4 src = {{192, 168, 1, 108}}, dst = {{255, 255, 255, 255}};
    net_udp_header uh = {CK_COURIER_PORT, CK_COURIER_PORT};
    net_ipv4_header ih = {0};
    ih.ttl = 64;
    ih.protocol = NET_IPPROTO_UDP;
    ih.source = src;
    ih.destination = dst;
    if (net_udp_build(&uh, (const uint8_t *)d->in[k], d->in_n[k], src, dst, udp, sizeof udp, &uw) != NET_OK ||
        net_ipv4_build(&ih, udp, uw, ipb, sizeof ipb, &iw) != NET_OK)
        return 0;
    net_eth_frame e;
    memset(e.destination.b, 0xff, 6);
    memset(e.source.b, 0x11, 6);
    e.ethertype = NET_ETHERTYPE_IPV4;
    e.payload = ipb;
    e.payload_len = iw;
    return net_eth_build(&e, fr, cap, &ew) == NET_OK ? ew : 0;
}

static int t_link(void *ctx)
{
    (void)ctx;
    return 1;
}
static uint64_t t_now(void *ctx) { return ((t_dev *)ctx)->now; }

static const ck_netio io_ = {&dev, t_send, t_recv, t_link, t_now};
static const uint8_t t_mac[6] = {0x00, 0x1f, 0xf3, 0x12, 0x34, 0x56}, t_none[4] = {0, 0, 0, 0},
                     t_peer[4] = {255, 255, 255, 255};
static const ck_courier *c;

static unsigned failures;
#define EXPECT(cond, ...)                                                                                          \
    do {                                                                                                           \
        if (!(cond)) {                                                                                             \
            failures++;                                                                                            \
            printf("COURIER_SECRET: FAIL ");                                                                       \
            printf(__VA_ARGS__);                                                                                   \
            printf("\n");                                                                                          \
        }                                                                                                          \
    } while (0)

static void fresh(void)
{
    memset(&dev, 0, sizeof dev);
    dev.now = 1000;
    c = ck_courier_udp_init(&io_, t_mac, t_none, t_peer, CK_COURIER_PORT, 1000);
}

/* A datagram from the far side for this boot's run id (or run_override), read at the next pump. */
static void inject(const char *kind, uint32_t seq, unsigned part, const char *body, uint64_t run_override)
{
    char *p = dev.in[dev.in_cnt];
    int n = snprintf(p, sizeof dev.in[0], "ALLEN-COURIER v=1 run=%016llx kind=%s seq=%u part=%u/1 bytes=%zu\n%s",
                     (unsigned long long)(run_override ? run_override : run), kind, (unsigned)seq, part, strlen(body), body);
    dev.in_n[dev.in_cnt++] = (size_t)n;
}

static void pump_for(uint64_t ms)
{
    dev.now += ms;
    char line[CK_COURIER_LINE_MAX];
    c->take(line, sizeof line);
}

static int all_zero(void)
{
    for (unsigned i = 0; i < sizeof job; i++)
        if (job[i])
            return 0;
    return 1;
}

#define SECRET "sk-live-0123456789abcdef"

static void keytest_refused(void)
{
    static const char *const forms[] = {"KEYTEST", "KEYTEST ", "KEYTEST 1 openai https://api.example " SECRET,
                                        "KEYTEST 7 anthropic - " SECRET "\nmore", "KEYTESTS x",
                                        "KEYTEST\t1 " SECRET, "KEYTEST\n1 " SECRET};
    /* with an earlier job held, and with nothing held */
    for (int held = 0; held < 2; held++) {
        for (unsigned f = 0; f < sizeof forms / sizeof *forms; f++) {
            fresh();
            if (held)
                EXPECT(c->send("chat hello allen", 16) == 0, "the normal job was not taken");
            static char snap[sizeof job];
            memcpy(snap, job, sizeof job);
            unsigned jn = job_n, jp = job_parts, so = st_out, sent = dev.sent;
            uint32_t js = job_seq;
            int jo = job_out, ja = job_acked;
            dev.needle = SECRET;
            int rc = c->send(forms[f], (unsigned)strlen(forms[f]));
            EXPECT(rc != 0, "KEYTEST form %u (%s) was taken (rc %d)", f, held ? "job held" : "no job", rc);
            EXPECT(dev.sent == sent && st_out == so, "KEYTEST form %u: a frame went out", f);
            EXPECT(dev.hits == 0, "KEYTEST form %u: the key was on the wire", f);
            EXPECT(!memcmp(snap, job, sizeof job), "KEYTEST form %u: the job buffer changed (%s)", f,
                   held ? "job held" : "no job");
            EXPECT(jn == job_n && jp == job_parts && js == job_seq && jo == job_out && ja == job_acked,
                   "KEYTEST form %u: job state changed", f);
            pump_for(5000);
            EXPECT(dev.hits == 0, "KEYTEST form %u: the key went out at a later resend", f);
            EXPECT(memmem(job, sizeof job, SECRET, sizeof SECRET - 1) == 0, "KEYTEST form %u: the key is in the buffer", f);
        }
    }
    /* Unrelated protocol lines keep their existing behavior. */
    fresh();
    EXPECT(c->send("STATUS x", 8) == 0 && c->send("keytest 1", 9) == 0, "an ordinary line starting like it was refused");
}

static void resend_and_clear(void)
{
    /* resends before the answer */
    fresh();
    EXPECT(c->send("chat hello allen " SECRET, 17 + sizeof SECRET - 1) == 0, "job not taken");
    unsigned j0 = dev.jobs, r0 = st_resends;
    EXPECT(j0 == 1, "the job went out %u times at send, want 1", j0);
    pump_for(1000);
    EXPECT(dev.jobs == j0, "resent after 1 s");
    pump_for(1200);
    EXPECT(dev.jobs == j0 + 1 && st_resends == r0 + 1, "no resend after 2 s (jobs %u)", dev.jobs);
    EXPECT(job_n && !all_zero(), "the job is not held before the answer");

    /* a wrong number does not clear it: ACK, LINE, and another boot's run id */
    uint32_t seq = job_seq;
    inject("ACK", seq + 5, 0, "", 0);
    inject("LINE", seq + 5, 1, "SAY no", 0);
    inject("ACK", seq, 0, "", run + 1);
    pump_for(10);
    EXPECT(job_n && !all_zero() && job_out, "a wrong-number or foreign-run ACK/LINE cleared the job");
    pump_for(2100);
    EXPECT(dev.jobs == j0 + 2, "the resends stopped for a wrong-number answer (jobs %u)", dev.jobs);

    /* the matching ACK clears every byte and stops the resends */
    dev.in_cnt = dev.in_pos = 0;
    inject("ACK", seq, 0, "", 0);
    pump_for(10);
    EXPECT(all_zero() && job_n == 0 && job_parts == 0 && job_out == 0, "the matching ACK left bytes in the job buffer");
    unsigned jobs = dev.jobs;
    pump_for(10000);
    EXPECT(dev.jobs == jobs, "the job was sent again after its ACK");
    EXPECT(c->send("chat next", 9) == 0 && job_seq == seq + 1 && dev.jobs == jobs + 1, "no new job after the ACK");

    /* the first matching LINE does the same, and the line is still delivered */
    fresh();
    EXPECT(c->send("chat " SECRET, 5 + sizeof SECRET - 1) == 0, "job not taken");
    seq = job_seq;
    dev.in_cnt = dev.in_pos = 0;
    inject("LINE", seq, 1, "SAY hello", 0);
    char line[CK_COURIER_LINE_MAX];
    int got = c->take(line, sizeof line);
    EXPECT(all_zero() && job_n == 0 && job_out == 0, "the first matching LINE left bytes in the job buffer");
    EXPECT(got == 1 && !strcmp(line, "SAY hello"), "the answer line was lost (%d '%s')", got, line);
    jobs = dev.jobs;
    pump_for(10000);
    EXPECT(dev.jobs == jobs, "the job was sent again after its LINE");

    /* a LINE for another job number does not clear */
    fresh();
    EXPECT(c->send("chat " SECRET, 5 + sizeof SECRET - 1) == 0, "job not taken");
    dev.in_cnt = dev.in_pos = 0;
    inject("LINE", job_seq + 3, 1, "SAY other", 0);
    pump_for(10);
    EXPECT(job_n && job_out && !all_zero(), "a LINE for another job cleared the buffer");
}

static void reinit_clears(void)
{
    fresh();
    EXPECT(c->send("chat " SECRET, 5 + sizeof SECRET - 1) == 0, "job not taken");
    EXPECT(!all_zero(), "setup: job not held");
    c = ck_courier_udp_init(&io_, t_mac, t_none, t_peer, CK_COURIER_PORT, 1000);
    EXPECT(all_zero() && job_n == 0 && job_parts == 0 && job_out == 0 && job_acked == 0 && job_seq == 0,
           "init left the previous job in the buffer");
    EXPECT(memmem(job, sizeof job, SECRET, sizeof SECRET - 1) == 0, "the key survived init");
}

int main(void)
{
    keytest_refused();
    resend_and_clear();
    reinit_clears();
    if (failures)
        return 1;
    printf("COURIER_SECRET: PASS\n");
    return 0;
}
