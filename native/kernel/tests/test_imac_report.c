/* test_imac_report.c -- host test for the iMac kernel's lab-only network
 * report (x86/report_x86.c, built with -DCK_X86_REPORT_HOST): the allowlist
 * filter and the frames.
 *
 *   test_imac_report                    unit checks on the filter and frames
 *   test_imac_report send PORT LOGFILE  also writes a sample report to
 *       LOGFILE and sends every part's UDP payload to 127.0.0.1:PORT (last
 *       part first, then all again), so scripts/ck_imac_report_test.sh can
 *       check that ck_imac_report_listen rebuilds LOGFILE byte for byte.
 *   test_imac_report sendfull PORT LOGFILE  the same for a full 64-part,
 *       sealed report (run id 9), the most the console copy can hold.
 *   test_imac_report legacy PORT        sends one datagram in the retired
 *       v=1 raw-console format, which the listener must refuse. */
#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE /* memmem */
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "aienos_net.h"
#include "ck_test.h"
#include "console_log.h"
#include "report_x86.h"

static const uint8_t mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
/* 56 lines of 61 bytes, each an allowlisted "pci: " line: 4 parts. */
static char log_text[56 * 61];

/* A console log mixing allowlisted lines with lines that must never leave
 * the machine: keys off the allowlist (chat, keys, tokens, the memory
 * layout), near-miss keys, control and non-ASCII bytes inside an allowed
 * key, an over-long line, and an unterminated tail. */
static const char allowed_part[] =
    "AIENOS-KERNEL-0\n"
    "aienos_commit: 0123abcd\n"
    "dmar: inventory-only register_access=none; DMA confinement NOT QUALIFIED\n"
    "pci: ahci 00:1f.2 id=8086:2922 class=0106\n"
    "net_tg3: link up 1000 Mb/s full duplex\n"
    "summary: cores=4/4 timer=ok sata_disks=1 nvme_disks=1 gpt_valid=1 writes=none net=up\n"
    "screen: Allen answered (11 characters in, 15 out)\n"
    "allen_memory: saved generation 2 to copy B (1 fact) in 9 ms\n"
    "ahci_part: disk=0 entry=1 type=38daac89-5ead-4b40-8b1e-3687a7418061 first_lba=2048 last_lba=2099199 mib=1024 name=\"AIENOS Allen memory\" (AIENOS)\n"
    "kernel: alive\n"
    "panic: exception: vector 14 #PF error=0x2\n"
    "AIENOS-KERNEL-0 DONE\n";

static size_t build_mixed(char *out, size_t cap)
{
    static const char *const lines[] = {
        "AIENOS-KERNEL-0\n",
        "allen: secret-chat-SENTINEL\n",
        "aienos_commit: 0123abcd\n",
        "dmar: inventory-only register_access=none; DMA confinement NOT QUALIFIED\n",
        "key: SENTINEL-KEY\n",
        "memory: frame_ranges=3 free_bytes=1 heap=0x1000+64 SENTINEL-MEM\n",
        "pci: ahci 00:1f.2 id=8086:2922 class=0106\n",
        "token: SENTINEL-TOKEN\n",
        "credential: SENTINEL-CRED\n",
        "chat: user said SENTINEL-CHAT\n",
        "nett: SENTINEL-NEAR\n",
        "net SENTINEL-NOCOLON\n",
        " pci: SENTINEL-LEADING-SPACE\n",
        "PCI: SENTINEL-CASE\n",
        "net: ok \x1b[2J SENTINEL-ESC\n",
        "summary: \tSENTINEL-TAB\n",
        "report: caf\xc3\xa9 SENTINEL-UTF8\n",
        "power: \r SENTINEL-CR\n",
        "net_tg3: link up 1000 Mb/s full duplex\n",
        "summary: cores=4/4 timer=ok sata_disks=1 nvme_disks=1 gpt_valid=1 writes=none net=up\n",
        "keyboard_line: Hello SENTINEL-TYPED\n",
        "screen: Allen answered (11 characters in, 15 out)\n",
        "allen: Your favorite color is SENTINEL-FACT.\n",
        "allen_memory: saved generation 2 to copy B (1 fact) in 9 ms\n",
        "allen_memoryx: SENTINEL-NEAR2\n",
        "ahci_part: disk=0 entry=1 type=38daac89-5ead-4b40-8b1e-3687a7418061 first_lba=2048 last_lba=2099199 mib=1024 name=\"AIENOS Allen memory\" (AIENOS)\n",
        "\n",
        "kernel: alive\n",
        "panic: exception: vector 14 #PF error=0x2\n",
        "AIENOS-KERNEL-0 DONE\n",
    };
    size_t w = 0;
    for (size_t i = 0; i < sizeof lines / sizeof lines[0]; i++) {
        size_t n = strlen(lines[i]);
        if (w + n > cap)
            return 0;
        memcpy(out + w, lines[i], n);
        w += n;
        if (i == 3) { /* an over-long but otherwise allowed line */
            const char *p = "stack: ";
            memcpy(out + w, p, 7);
            w += 7;
            for (unsigned k = 0; k < CK_X86_REPORT_LINE_MAX; k++)
                out[w++] = 'x';
            memcpy(out + w, "SENTINEL-LONG\n", 14);
            w += 14;
        }
    }
    static const char tail[] = "screen: SENTINEL-TAIL"; /* no newline */
    memcpy(out + w, tail, sizeof tail - 1);
    return w + sizeof tail - 1;
}

static void filter_checks(void)
{
    static char raw[4096], out[4096];
    size_t rl = build_mixed(raw, sizeof raw);
    CHECK(rl > 0);
    CHECK(memmem(raw, rl, "SENTINEL", 8) != 0);
    size_t n = ck_x86_report_filter(raw, rl, out, sizeof out);
    /* The allowed lines survive byte for byte, in order; nothing else does. */
    CHECK(n == sizeof allowed_part - 1 && memcmp(out, allowed_part, n) == 0);
    CHECK(memmem(out, n, "SENTINEL", 8) == 0);
    for (size_t i = 0; i < n; i++)
        CHECK(out[i] == '\n' || (out[i] >= 0x20 && out[i] <= 0x7e));
    /* Nor does any sentinel reach a frame built from the filtered copy. */
    uint8_t f[1600];
    for (unsigned p = 0; p < ck_x86_report_parts(n); p++) {
        size_t fl = ck_x86_report_frame(mac, 1, out, n, p, f, sizeof f);
        CHECK(fl > 0 && memmem(f, fl, "SENTINEL", 8) == 0);
    }
    /* A line at the length cap is kept, one byte more is dropped. */
    char edge[2 * CK_X86_REPORT_LINE_MAX + 8], eo[sizeof edge];
    size_t e = 0;
    memcpy(edge, "cpu: ", 5);
    for (e = 5; e < CK_X86_REPORT_LINE_MAX; e++)
        edge[e] = 'a';
    edge[e++] = '\n';
    size_t keep = e;
    memcpy(edge + e, "cpu: ", 5);
    for (size_t k = 5; k <= CK_X86_REPORT_LINE_MAX; k++)
        edge[e + k] = 'b';
    e += CK_X86_REPORT_LINE_MAX + 1;
    edge[e++] = '\n';
    CHECK(ck_x86_report_filter(edge, e, eo, sizeof eo) == keep && memcmp(eo, edge, keep) == 0);
    /* Only whole lines are written: a line that does not fit is dropped. */
    size_t first = strlen("AIENOS-KERNEL-0\n");
    CHECK(ck_x86_report_filter(raw, rl, out, first + 5) == first && memcmp(out, raw, first) == 0);
    CHECK(ck_x86_report_filter(raw, rl, out, first - 1) == 14 && memcmp(out, "kernel: alive\n", 14) == 0);
    CHECK(ck_x86_report_filter(0, rl, out, sizeof out) == 0);
    CHECK(ck_x86_report_filter(raw, rl, 0, sizeof out) == 0);
    CHECK(ck_x86_report_filter(raw, 0, out, sizeof out) == 0);
}
/* Unwraps one frame; returns the UDP payload view or 0. */
/* The late-diagnostics boundary, through the console's own append
 * (x86/console_log.h): the boot lines stay, an allowlisted public line
 * written after the old 16 KiB cap still reaches the report, conversation
 * lines written before and after it never do, and a copy that fills says so. */
static const uint8_t *unwrap(const uint8_t *f, size_t n, size_t *plen);
static char full_report[CK_X86_REPORT_MAX]; /* the sealed worst case, for the listener round trip */
static size_t full_len;
static struct ck_x86_log klog;
static size_t written;

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...)
{
    char line[300];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    ck_x86_log_append(&klog, line, (size_t)n);
    written += (size_t)n;
}

static void late_checks(void)
{
    static char out[CK_X86_REPORT_MAX], raw_cut[CK_X86_LOG_BYTES], rebuilt[CK_X86_REPORT_MAX];
    uint8_t f[1600];
    const size_t old_cap = 16384;

    CHECK(CK_X86_LOG_BYTES == CK_X86_REPORT_MAX);
    CHECK(CK_X86_REPORT_MAX / CK_X86_REPORT_CHUNK == 64); /* the listener's MAX_PARTS */

    /* The boot, then conversation until the log is past the old cap. */
    say("AIENOS-KERNEL-0\naienos_commit: 0123abcd\n");
    for (unsigned i = 0; klog.len < old_cap + 2048 && klog.dropped == 0; i++) {
        say("pci: fn 00:%02x.0 id=8086:%04x class=0600\n", i & 31, i);
        say("keyboard_line: SENTINEL-CHAT-%u private words typed to Allen\n", i);
        say("allen: SENTINEL-REPLY-%u private words Allen said back\n", i);
    }
    size_t before = klog.len;
    say("local_model: starting public BOS experiment tokens=32 cpu_limit_s=120\n");
    say("keyboard_line: SENTINEL-AFTER-STARTING\n");
    say("local_model: LATE-COMPLETION public fixed prompt, 32 tokens in 41 s\n");
    say("allen: SENTINEL-AFTER-COMPLETION\n");
    say("screen: unterminated SENTINEL-TAIL");
    CHECK(before > old_cap && klog.dropped == 0 && klog.len == written);

    size_t n = ck_x86_report_filter(klog.buf, klog.len, out, sizeof out);
    CHECK(n > 0 && n <= CK_X86_REPORT_MAX);
    CHECK(memcmp(out, "AIENOS-KERNEL-0\naienos_commit: 0123abcd\n", 40) == 0);
    const char *late = memmem(out, n, "LATE-COMPLETION", 15);
    CHECK(late != 0 && memmem(out, n, "local_model: starting public BOS", 32) != 0);
    CHECK(memmem(out, n, "SENTINEL", 8) == 0);
    /* Negative control: the old 16 KiB copy would have lost the completion. */
    memcpy(raw_cut, klog.buf, old_cap);
    size_t cn = ck_x86_report_filter(raw_cut, old_cap, out + n, sizeof out - n);
    CHECK(cn > 0 && memmem(out + n, cn, "LATE-COMPLETION", 15) == 0);
    CHECK(ck_x86_report_seal(out, n, sizeof out, klog.dropped) == n); /* nothing dropped, no note */

    /* Every part of it frames, carries no conversation, and rebuilds byte for byte. */
    unsigned parts = ck_x86_report_parts(n);
    CHECK(parts > 1 && parts <= 64); /* the raw log is past 16 KiB; the filtered report is what the allowlist keeps */
    size_t got = 0;
    for (unsigned p = 0; p < parts; p++) {
        size_t fl = ck_x86_report_frame(mac, 7, out, n, p, f, sizeof f), pl = 0;
        CHECK(fl >= 60 && fl <= 1514 && memmem(f, fl, "SENTINEL", 8) == 0);
        const uint8_t *d = fl ? unwrap(f, fl, &pl) : 0;
        CHECK(d != 0);
        const uint8_t *nl = d ? memchr(d, '\n', pl) : 0;
        CHECK(nl != 0);
        if (nl && got + pl - (size_t)(nl + 1 - d) <= sizeof rebuilt) {
            memcpy(rebuilt + got, nl + 1, pl - (size_t)(nl + 1 - d));
            got += pl - (size_t)(nl + 1 - d);
        }
    }
    CHECK(got == n && memcmp(rebuilt, out, n) == 0);

    /* A copy that fills: the head stays, the rest is counted, the report says so. */
    char head[64];
    memcpy(head, klog.buf, sizeof head);
    for (unsigned i = 0; klog.len < CK_X86_LOG_BYTES || i < 100; i++)
        say("pci: fn 00:%02x.1 id=8086:%04x class=0604 padding padding padding padding\n", i & 31, i);
    CHECK(klog.len == CK_X86_LOG_BYTES && klog.dropped > 0 && klog.len + klog.dropped == written);
    CHECK(memcmp(klog.buf, head, sizeof head) == 0);
    size_t m = ck_x86_report_filter(klog.buf, klog.len, out, sizeof out - CK_X86_REPORT_NOTE_MAX);
    size_t sealed = ck_x86_report_seal(out, m, sizeof out, klog.dropped);
    char note[80];
    int nn = snprintf(note, sizeof note, "report: console_copy_full dropped_bytes=%zu\n", klog.dropped);
    CHECK(nn > 0 && sealed == m + (size_t)nn && memcmp(out + m, note, (size_t)nn) == 0);
    CHECK(sealed <= CK_X86_REPORT_MAX && ck_x86_report_parts(sealed) > 1);
    CHECK(memcmp(out, "AIENOS-KERNEL-0\n", 16) == 0 && memmem(out, m, "LATE-COMPLETION", 15) != 0);
    CHECK(ck_x86_report_frame(mac, 8, out, sealed, ck_x86_report_parts(sealed) - 1, f, sizeof f) >= 60);
    CHECK(ck_x86_report_seal(out, m, m + 10, klog.dropped) == m); /* no room: unchanged, never overruns */

    /* Worst case: every line allowed, the copy overfull. The sealed report
     * still fits the 64 parts the listener rebuilds, and its last frame builds. */
    memset(&klog, 0, sizeof klog);
    written = 0;
    for (unsigned i = 0; klog.dropped == 0; i++)
        say("pci: fn 00:%02x.1 id=8086:%04x class=0604 padding padding padding padding\n", i & 31, i);
    m = ck_x86_report_filter(klog.buf, klog.len, out, sizeof out - CK_X86_REPORT_NOTE_MAX);
    sealed = ck_x86_report_seal(out, m, sizeof out, klog.dropped);
    CHECK(sealed > m && sealed <= CK_X86_REPORT_MAX && ck_x86_report_parts(sealed) == 64);
    CHECK(ck_x86_report_frame(mac, 9, out, sealed, 63, f, sizeof f) >= 60);
    CHECK(ck_x86_report_frame(mac, 9, out, sealed, 64, f, sizeof f) == 0);
    memcpy(full_report, out, sealed);
    full_len = sealed;
}

static const uint8_t *unwrap(const uint8_t *f, size_t n, size_t *plen)
{
    net_eth_frame e;
    net_ipv4_header ih;
    net_udp_header uh;
    const uint8_t *ip, *pl;
    size_t il;
    static const uint8_t bc[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    const net_ipv4 any = {{0, 0, 0, 0}}, bip = {{255, 255, 255, 255}};
    if (net_eth_parse(f, n, &e) != NET_OK || memcmp(e.destination.b, bc, 6) || memcmp(e.source.b, mac, 6) ||
        e.ethertype != NET_ETHERTYPE_IPV4)
        return 0;
    if (net_ipv4_parse(e.payload, e.payload_len, &ih, &ip, &il) != NET_OK || ih.protocol != NET_IPPROTO_UDP ||
        ih.ttl != 1 || memcmp(ih.source.b, any.b, 4) || memcmp(ih.destination.b, bip.b, 4))
        return 0;
    if (net_udp_parse(ip, il, ih.source, ih.destination, &uh, &pl, plen) != NET_OK ||
        uh.source_port != CK_X86_REPORT_PORT || uh.destination_port != CK_X86_REPORT_PORT)
        return 0;
    if (ip[6] == 0 && ip[7] == 0) /* the checksum must be present, not skipped */
        return 0;
    return pl;
}

int main(int argc, char **argv)
{
    for (size_t i = 0; i < sizeof log_text; i++)
        log_text[i] = (i % 61 == 60) ? '\n' : i % 61 < 5 ? "pci: "[i % 61] : (char)('A' + i % 26);
    size_t len = sizeof log_text;
    uint64_t run = 0x0123456789abcdefull;
    uint8_t f[1600];
    unsigned parts = ck_x86_report_parts(len);

    CHECK(ck_x86_report_parts(0) == 0);
    CHECK(ck_x86_report_parts(1) == 1);
    CHECK(ck_x86_report_parts(CK_X86_REPORT_CHUNK) == 1);
    CHECK(ck_x86_report_parts(CK_X86_REPORT_CHUNK + 1) == 2);
    CHECK(ck_x86_report_parts(CK_X86_REPORT_MAX) == CK_X86_REPORT_MAX / CK_X86_REPORT_CHUNK);
    CHECK(ck_x86_report_parts(CK_X86_REPORT_MAX + 1) == 0);
    CHECK(parts == 4);
    filter_checks();
    late_checks();
    /* The sample report is what the filter makes of itself: every line is kept. */
    static char filtered[sizeof log_text];
    CHECK(ck_x86_report_filter(log_text, len, filtered, sizeof filtered) == len &&
          memcmp(filtered, log_text, len) == 0);
    CHECK(ck_x86_report_frame(mac, run, log_text, len, parts, f, sizeof f) == 0);
    CHECK(ck_x86_report_frame(mac, run, log_text, len, 0, f, 100) == 0);
    CHECK(ck_x86_report_frame(0, run, log_text, len, 0, f, sizeof f) == 0);

    char rebuilt[sizeof log_text];
    size_t got = 0;
    for (unsigned p = 0; p < parts; p++) {
        size_t n = ck_x86_report_frame(mac, run, log_text, len, p, f, sizeof f);
        CHECK(n >= 60 && n <= 1514);
        size_t pl = 0;
        const uint8_t *d = unwrap(f, n, &pl);
        CHECK(d != 0);
        if (!d)
            continue;
        char want[128];
        int h = snprintf(want, sizeof want, "AIENOS-REPORT v=2 auth=none run=0123456789abcdef part=%u/%u bytes=%zu\n",
                         p,
                         parts, len);
        CHECK(pl > (size_t)h && memcmp(d, want, (size_t)h) == 0);
        if (pl > (size_t)h && got + pl - h <= sizeof rebuilt) {
            memcpy(rebuilt + got, d + h, pl - h);
            got += pl - h;
        }
    }
    CHECK(got == len && memcmp(rebuilt, log_text, len) == 0);

    if (argc == 4 && !strcmp(argv[1], "send")) {
        int lfd = open(argv[3], O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        FILE *lf = lfd < 0 ? 0 : fdopen(lfd, "w");
        CHECK(lf && fwrite(log_text, 1, len, lf) == len && fclose(lf) == 0);
        int s = socket(AF_INET, SOCK_DGRAM, 0);
        struct sockaddr_in a;
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_port = htons((unsigned short)atoi(argv[2]));
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        CHECK(s >= 0);
        for (unsigned round = 0; round < 2; round++)
            for (unsigned k = 0; k < parts; k++) {
                unsigned p = round ? k : parts - 1 - k;
                size_t n = ck_x86_report_frame(mac, run, log_text, len, p, f, sizeof f), pl = 0;
                const uint8_t *d = unwrap(f, n, &pl);
                CHECK(d && sendto(s, d, pl, 0, (struct sockaddr *)&a, sizeof a) == (ssize_t)pl);
                nanosleep(&(struct timespec){0, 20000000}, 0);
            }
        close(s);
    }
    if (argc == 4 && !strcmp(argv[1], "sendfull")) { /* all 64 parts of the sealed worst case, run id 9 */
        int lfd = open(argv[3], O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        FILE *lf = lfd < 0 ? 0 : fdopen(lfd, "w");
        CHECK(lf && fwrite(full_report, 1, full_len, lf) == full_len && fclose(lf) == 0);
        int s = socket(AF_INET, SOCK_DGRAM, 0);
        struct sockaddr_in a;
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_port = htons((unsigned short)atoi(argv[2]));
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        CHECK(s >= 0);
        for (unsigned p = 0; p < ck_x86_report_parts(full_len); p++) {
            size_t n = ck_x86_report_frame(mac, 9, full_report, full_len, p, f, sizeof f), pl = 0;
            const uint8_t *d = unwrap(f, n, &pl);
            CHECK(d && sendto(s, d, pl, 0, (struct sockaddr *)&a, sizeof a) == (ssize_t)pl);
            nanosleep(&(struct timespec){0, 2000000}, 0);
        }
        close(s);
    }
    if (argc == 3 && !strcmp(argv[1], "legacy")) { /* a forged or old v=1 raw-console part */
        static const char old[] = "AIENOS-IMAC-REPORT v=1 run=00000000000000ff part=0/1 bytes=5\nhello";
        int s = socket(AF_INET, SOCK_DGRAM, 0);
        struct sockaddr_in a;
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_port = htons((unsigned short)atoi(argv[2]));
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        CHECK(s >= 0 && sendto(s, old, sizeof old - 1, 0, (struct sockaddr *)&a, sizeof a) == (ssize_t)(sizeof old - 1));
        close(s);
    }
    return ck_t_verdict("test_imac_report");
}
