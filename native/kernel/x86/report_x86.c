/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Drake Stapleton (AIEN / aien-dev)
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* report_x86.c -- LAB-ONLY: sends the iMac kernel's diagnostic lines over
 * Ethernet, so the owner does not have to photograph the screen.
 *
 * Built only into the opt-in lab image (make imac-kernel CK_X86_REPORT=1,
 * which writes IMACKERN-LAB.EFI). The default image links none of this file
 * nor the network library, and the Makefile fails a default image that has
 * its symbols; resident and service builds must never add it. Any future
 * service diagnostics go over the authenticated transport with an explicitly
 * authorized peer, never this broadcast.
 *
 * What is sent: ck_x86_report_filter's copy of the console's RAM copy
 * (console_x86.c, the first 64 KiB), which keeps only complete, printable,
 * bounded lines that start with a fixed kernel diagnostic key and drops
 * everything else whole. The raw console is never sent. The copy is cut into
 * parts of CK_X86_REPORT_CHUNK bytes; each part is one UDP datagram from
 * 0.0.0.0:PORT to 255.255.255.255:PORT in an Ethernet broadcast frame (no
 * address to configure, no ARP, nothing to answer), with a one-line header the
 * receiver checks:
 *   "AIENOS-REPORT v=2 auth=none run=<16 hex> part=<i>/<n> bytes=<total>\n"
 * auth=none says what it is: anyone on the LAN can read or forge one, and the
 * run id and checksum are not authenticity. A received report is never a
 * receipt, evidence or a command. The host side is
 * native/kernel/tools/ck_imac_report_listen.c. During the screen hold the whole
 * report is sent again every 2 s while the link is up, so a receiver that
 * missed a datagram gets it on the next round.
 *
 * ck_x86_report_filter and ck_x86_report_frame are pure functions of their
 * arguments (no device, no clock) and are host-tested by
 * scripts/ck_imac_report_test.sh. */
#ifdef CK_X86_REPORT_HOST
#include <string.h>
#else
#include "../core/ck_internal.h"
#include "x86.h"
#endif
#include "aienos_net.h"
#include "report_x86.h"
#include "console_log.h"

_Static_assert(CK_X86_REPORT_MAX >= CK_X86_LOG_BYTES, "the report must hold a full console copy");
_Static_assert(CK_X86_REPORT_MAX / CK_X86_REPORT_CHUNK <= 64, "the listener rebuilds at most 64 parts");

#define REPORT_HDR "AIENOS-REPORT v=2 auth=none "

/* The allowlist: every key the iMac kernel prints (the x86 sources, efi_gop.c,
 * the disk report's ahci_/nvme_ _read and _gpt lines, and Allen's memory
 * status "allen_memory:", which carries only counts and the disk layout,
 * and "boot_disk:" / "boot_time:", only sizes, commit ids and milliseconds),
 * minus "memory:", which describes the kernel's own memory layout, and
 * minus what is said to Allen ("keyboard_line:" and "allen:"): the conversation never leaves the
 * iMac; "screen: Allen answered" carries only lengths. A new key stays off the
 * report until it is added here. */
static const char *const allowed_keys[] = {
    "acpi_rsdp", "ahci", "ahci_disk", "ahci_gpt", "ahci_part", "ahci_port", "ahci_read", "ahci_write",
    "aienos_commit", "allen-code", "allen_memory", "apic", "bench", "boot_disk", "boot_time", "courier", "cpu", "descriptors", "dmar", "exit_boot_services",
    "firmware_cpl", "firmware_revision", "firmware_vendor", "gop", "guard_page", "handoff", "image", "kernel",
    "kernel_impl", "keyboard", "local_model", "local_tools", "madt", "net", "net_tg3", "note", "nvme", "nvme_disk", "nvme_gpt", "nvme_read", "paging",
    "panic", "pci", "power", "ram", "report", "report_kind", "screen", "smp", "sound", "stack", "storage", "summary",
    "timebase", "timer_irq", "timer_stats", "usb", "wizard", "xhci",
};
static const char *const allowed_lines[] = {
    "AIENOS-KERNEL-0",
    "AIENOS-KERNEL-0 DONE",
    "exiting firmware boot services",
};

static int line_allowed(const char *s, size_t n)
{
    if (n > CK_X86_REPORT_LINE_MAX)
        return 0;
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)s[i] < 0x20 || (unsigned char)s[i] > 0x7e)
            return 0;
    for (size_t k = 0; k < sizeof allowed_lines / sizeof allowed_lines[0]; k++)
        if (strlen(allowed_lines[k]) == n && !memcmp(s, allowed_lines[k], n))
            return 1;
    for (size_t k = 0; k < sizeof allowed_keys / sizeof allowed_keys[0]; k++) {
        size_t kl = strlen(allowed_keys[k]);
        if (n > kl && s[kl] == ':' && !memcmp(s, allowed_keys[k], kl))
            return 1;
    }
    return 0;
}

size_t ck_x86_report_filter(const char *log, size_t len, char *out, size_t cap)
{
    size_t w = 0;
    if (!log || !out)
        return 0;
    for (size_t i = 0; i < len;) {
        size_t e = i;
        while (e < len && log[e] != '\n')
            e++;
        if (e == len) /* unterminated last line: dropped */
            break;
        size_t n = e - i;
        if (line_allowed(log + i, n) && cap - w > n) {
            memcpy(out + w, log + i, n);
            out[w + n] = '\n';
            w += n + 1;
        }
        i = e + 1;
    }
    return w;
}
static void hex16(char *out, uint64_t v)
{
    for (int i = 15; i >= 0; i--, v >>= 4)
        out[i] = "0123456789abcdef"[v & 15];
}

static size_t dec(char *out, unsigned v)
{
    char t[10];
    size_t n = 0, k = 0;
    do
        t[n++] = (char)('0' + v % 10);
    while ((v /= 10) && n < sizeof t);
    while (n)
        out[k++] = t[--n];
    return k;
}

size_t ck_x86_report_seal(char *out, size_t len, size_t cap, size_t dropped)
{
    static const char head[] = "report: console_copy_full dropped_bytes=";
    if (!out || !dropped || cap - len < sizeof head + 11) /* digits of a 32-bit count, '\n' */
        return len;
    memcpy(out + len, head, sizeof head - 1);
    len += sizeof head - 1;
    len += dec(out + len, dropped > 0xffffffffu ? 0xffffffffu : (unsigned)dropped);
    out[len++] = '\n';
    return len;
}

unsigned ck_x86_report_parts(size_t len)
{
    if (!len || len > CK_X86_REPORT_MAX)
        return 0;
    return (unsigned)((len + CK_X86_REPORT_CHUNK - 1) / CK_X86_REPORT_CHUNK);
}

size_t ck_x86_report_frame(const uint8_t mac[6], uint64_t run, const char *report, size_t len, unsigned part,
                           uint8_t *out, size_t cap)
{
    static uint8_t data[160 + CK_X86_REPORT_CHUNK], udp[8 + sizeof data], ip[20 + sizeof udp];
    unsigned parts = ck_x86_report_parts(len);
    if (!mac || !report || !out || part >= parts)
        return 0;
    size_t off = (size_t)part * CK_X86_REPORT_CHUNK;
    size_t n = len - off < CK_X86_REPORT_CHUNK ? len - off : CK_X86_REPORT_CHUNK;
    size_t h = sizeof REPORT_HDR - 1;
    memcpy(data, REPORT_HDR "run=", h + 4);
    h += 4;
    hex16((char *)data + h, run);
    h += 16;
    memcpy(data + h, " part=", 6);
    h += 6;
    h += dec((char *)data + h, part);
    data[h++] = '/';
    h += dec((char *)data + h, parts);
    memcpy(data + h, " bytes=", 7);
    h += 7;
    h += dec((char *)data + h, (unsigned)len);
    data[h++] = '\n';
    memcpy(data + h, report + off, n);

    const net_ipv4 any = {{0, 0, 0, 0}}, bcast_ip = {{255, 255, 255, 255}};
    net_udp_header uh = {CK_X86_REPORT_PORT, CK_X86_REPORT_PORT};
    size_t uw = 0, iw = 0, ew = 0;
    if (net_udp_build(&uh, data, h + n, any, bcast_ip, udp, sizeof udp, &uw) != NET_OK)
        return 0;
    net_ipv4_header ih;
    ih.dscp_ecn = 0;
    ih.identification = (uint16_t)part;
    ih.ttl = 1; /* this network only */
    ih.protocol = NET_IPPROTO_UDP;
    ih.source = any;
    ih.destination = bcast_ip;
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

#ifndef CK_X86_REPORT_HOST
/* Lab build only. Holds for `seconds` (longer while ck_x86_hold_step moves the
 * end out: typing, or Allen's window, which stays up through sleep), sending the allowlisted report every
 * 2 s while the link is up. Without a working network it only waits. Returns
 * the rounds sent. */
unsigned ck_x86_report_hold(uint64_t tsc_hz, unsigned seconds)
{
    static uint8_t frame[1600];
    static int busy; /* a fault while sending ends up here again: only wait */
    uint64_t t0 = ck_rdtsc(), end = t0 + (uint64_t)seconds * tsc_hz, next = t0;
    if (busy++ || !ck_x86_tg3_ready()) {
        while (ck_rdtsc() < end) {
            ck_x86_hold_step(&end);
            ck_pause();
        }
        return 0;
    }
    ck_printf("report: LAB BUILD, sending allowlisted diagnostic lines to UDP port %u (broadcast, "
              "unauthenticated, auth=none) every 2 s during the hold\n",
              (unsigned)CK_X86_REPORT_PORT);
    static char report[CK_X86_REPORT_MAX];
    size_t len = 0;
    unsigned parts = 0, rounds = 0, failed = 0;
    uint8_t mac[6];
    ck_x86_tg3_mac(mac);
    uint64_t run = 0;
    while (ck_rdtsc() < end) {
        ck_x86_hold_step(&end); /* typing keeps the hold going */
        if (ck_rdtsc() < next) {
            ck_pause();
            continue;
        }
        next += 2 * tsc_hz;
        if (!ck_x86_tg3_link())
            continue;
        /* A fresh filtered copy each round, so lines typed during the hold go
         * out too; a new copy gets a new run id. Only the filtered copy leaves
         * the machine; the raw console never does. */
        size_t raw_len;
        const char *raw = ck_x86_console_log(&raw_len);
        size_t n_len = ck_x86_report_filter(raw, raw_len, report, sizeof report - CK_X86_REPORT_NOTE_MAX);
        n_len = ck_x86_report_seal(report, n_len, sizeof report, ck_x86_console_log_dropped());
        if (n_len != len || !run) {
            len = n_len;
            parts = ck_x86_report_parts(len);
            run = ck_rdtsc() ^ (uint64_t)len << 48;
        }
        for (unsigned p = 0; p < parts; p++) {
            size_t n = ck_x86_report_frame(mac, run, report, len, p, frame, sizeof frame);
            if (!n || ck_x86_tg3_send(frame, n))
                failed++;
        }
        if (ck_x86_tg3_drain(100))
            failed++;
        rounds++;
    }
    /* Printed after the last copy, so only the screen shows it. */
    ck_printf("report: rounds=%u parts=%u bytes=%u send_failures=%u\n", rounds, parts, (unsigned)len, failed);
    return rounds;
}
#endif

#ifndef CK_X86_REPORT_HOST
/* Allen's courier over Ethernet (courier_udp.h) bound to the chip: lab image
 * only, like everything else in this file. The far side's address is the
 * broadcast address unless the lab image was built with another
 * (make imac-kernel CK_X86_REPORT=1 CK_COURIER_PEER=192.168.1.108). */
#include "courier_udp.h"
#ifndef CK_COURIER_PEER_STR
#define CK_COURIER_PEER_STR "255.255.255.255"
#endif
static int nio_send(void *c, const uint8_t *f, size_t n) { (void)c; return ck_x86_tg3_send(f, n); }
static size_t nio_recv(void *c, uint8_t *f, size_t cap) { (void)c; return ck_x86_tg3_recv(f, cap); }
static int nio_link(void *c) { (void)c; return ck_x86_tg3_link(); }
static uint64_t nio_now(void *c) { (void)c; return ck_rdtsc(); }
static const ck_netio nio = {0, nio_send, nio_recv, nio_link, nio_now};

static void dotted(const char *s, uint8_t out[4])
{
    for (int i = 0; i < 4; i++) {
        unsigned v = 0;
        while (*s >= '0' && *s <= '9')
            v = v * 10 + (unsigned)(*s++ - '0');
        out[i] = (uint8_t)v;
        if (*s == '.')
            s++;
    }
}

const ck_courier *ck_x86_courier_udp(uint64_t tsc_hz)
{
    if (!ck_x86_tg3_ready())
        return 0;
    uint8_t mac[6], peer[4];
    static const uint8_t none[4] = {0, 0, 0, 0};
    ck_x86_tg3_mac(mac);
    dotted(CK_COURIER_PEER_STR, peer);
    return ck_courier_udp_init(&nio, mac, none, peer, CK_COURIER_PORT, tsc_hz);
}
#endif
