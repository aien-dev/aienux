#!/usr/bin/env bash
# ck_imac_bsdp_test.sh -- host test for tools/ck_imac_bsdp.c (Apple NetBoot answerer).
# Runs the answerer on loopback high ports and plays the iMac with a small C client that
# sends the INFORM[LIST] the real iMac18,3 sent on 2026-10-17 (vendor class
# "AAPLBSDPC/i386/iMac18,3", option 43 = 01 01 01 02 02 01 01) plus a reply port, then an
# INFORM[SELECT] for the offered image; checks every answer field; then fetches the booter
# with curl over TFTP (blksize + tsize) and compares bytes; checks a wrong file name and a
# DHCP DISCOVER get nothing. Hostile-input fixtures (CodeQL alerts on this tool): a TFTP
# read request that repeats blksize until a chained option answer would pass the end of its
# 128-byte buffer, and a clock gmtime cannot convert (LD_PRELOAD time shim). The server is
# built with ASan + UBSan, so any write past a buffer ends the test. Loopback says nothing
# about the iMac firmware.
# Final line: AIENOS_IMAC_BSDP: PASS|FAIL   exit 0 / 1
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tag="AIENOS_IMAC_BSDP"
W="$(mktemp -d)"
spid=""
trap '[[ -z "${spid}" ]] || kill "${spid}" 2>/dev/null || true; rm -rf "${W}"' EXIT
fail() { echo "FAIL  $1"; echo "${tag}: FAIL"; exit 1; }
pass() { echo "PASS  $1"; }
command -v curl >/dev/null || { echo "${tag}: NOT_RUN (curl missing)"; exit 2; }
san=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
cc -std=gnu11 -O2 -Wall -Wextra -Werror "${san[@]}" -o "${W}/bsdp" "${repo_root}/native/kernel/tools/ck_imac_bsdp.c"
cc -std=gnu11 -O2 -Wall -Wextra -Werror -o "${W}/bsdp_plain" "${repo_root}/native/kernel/tools/ck_imac_bsdp.c"
cat >"${W}/client.c" <<'EOF'
#include <arpa/inet.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <stdint.h>
/* client SERVER_PORT REPLY_PORT list|select|discover -> prints the answer's fields
 * client TFTP_PORT REPLY_PORT flood -> a read request with blksize repeated 12 times;
 * prints the option answer (oack_len=N, then name=value lines) or "no answer" */
static int flood(struct sockaddr_in to, int s)
{
    uint8_t q[512];
    size_t n = 0;
    memcpy(q, "\0\1aienos/netldr.efi\0octet", 26), n = 26; /* includes the NUL after "octet" */
    for (int i = 0; i < 12; i++)
        memcpy(q + n, "blksize\0" "1428", 13), n += 13;
    memcpy(q + n, "tsize\0" "0", 8), n += 8;
    sendto(s, q, n, 0, (struct sockaddr *)&to, sizeof to);
    struct pollfd pf = {s, POLLIN, 0};
    if (poll(&pf, 1, 3000) <= 0) {
        printf("no answer\n");
        return 0;
    }
    uint8_t r[1600];
    struct sockaddr_in from;
    socklen_t fl = sizeof from;
    ssize_t k = recvfrom(s, r, sizeof r, 0, (struct sockaddr *)&from, &fl);
    if (k < 2 || r[1] != 6) {
        printf("not an option answer (opcode %d)\n", k >= 2 ? r[1] : -1);
        return 0;
    }
    printf("oack_len=%zd\n", k);
    for (ssize_t i = 2; i < k;) {
        const char *name = (const char *)r + i;
        size_t a = strnlen(name, (size_t)(k - i));
        if (i + (ssize_t)a + 1 >= k)
            break;
        const char *val = name + a + 1;
        size_t b = strnlen(val, (size_t)(k - i - (ssize_t)a - 1));
        printf("%.*s=%.*s\n", (int)a, name, (int)b, val);
        i += (ssize_t)(a + b + 2);
    }
    uint8_t err[] = {0, 5, 0, 0, 's', 't', 'o', 'p', 0}; /* end the transfer */
    sendto(s, err, sizeof err, 0, (struct sockaddr *)&from, sizeof from);
    return 0;
}
int main(int argc, char **argv)
{
    int sp = atoi(argv[1]), rp = atoi(argv[2]);
    (void)argc;
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in me = {.sin_family = AF_INET, .sin_port = htons(rp), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(sp), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    if (bind(s, (struct sockaddr *)&me, sizeof me) != 0)
        return 3;
    if (strcmp(argv[3], "flood") == 0)
        return flood(to, s);
    uint8_t p[400] = {0};
    size_t n = 240;
    p[0] = 1, p[1] = 1, p[2] = 6;
    p[4] = 0xd7, p[5] = 0xfe, p[6] = 0xa7, p[7] = 0x48;
    p[12] = 127, p[15] = 1; /* ciaddr 127.0.0.1 (the iMac sent its leased address) */
    uint8_t mac[6] = {0x78, 0x7b, 0x8a, 0xbd, 0xd0, 0x51};
    memcpy(p + 28, mac, 6);
    p[236] = 0x63, p[237] = 0x82, p[238] = 0x53, p[239] = 0x63;
    int disc = strcmp(argv[3], "discover") == 0, sel = strcmp(argv[3], "select") == 0;
    p[n++] = 53, p[n++] = 1, p[n++] = disc ? 1 : 8;
    p[n++] = 55, p[n++] = 2, p[n++] = 43, p[n++] = 60;
    p[n++] = 57, p[n++] = 2, p[n++] = 0x05, p[n++] = 0xdc;
    p[n++] = 60, p[n++] = 23;
    memcpy(p + n, "AAPLBSDPC/i386/iMac18,3", 23), n += 23;
    if (sel) {
        uint8_t v[] = {1, 1, 2, 2, 2, 1, 1, 5, 2, (uint8_t)(rp >> 8), (uint8_t)rp, 8, 4, 0x81, 0, 0, 1, 3, 4, 127, 0, 0, 1};
        p[n++] = 43, p[n++] = sizeof v, memcpy(p + n, v, sizeof v), n += sizeof v;
    } else {
        uint8_t v[] = {1, 1, 1, 2, 2, 1, 1, 5, 2, (uint8_t)(rp >> 8), (uint8_t)rp};
        p[n++] = 43, p[n++] = sizeof v, memcpy(p + n, v, sizeof v), n += sizeof v;
    }
    p[n++] = 255;
    sendto(s, p, n, 0, (struct sockaddr *)&to, sizeof to);
    struct pollfd pf = {s, POLLIN, 0};
    if (poll(&pf, 1, 2000) <= 0) {
        printf("no answer\n");
        return 0;
    }
    uint8_t r[600];
    ssize_t k = recv(s, r, sizeof r, 0);
    printf("op=%u xid=%02x%02x%02x%02x siaddr=%u.%u.%u.%u chaddr_ok=%d file=%s sname=%s\n", r[0], r[4], r[5], r[6],
           r[7], r[20], r[21], r[22], r[23], memcmp(r + 28, mac, 6) == 0, (char *)r + 108, (char *)r + 44);
    for (ssize_t i = 240; i < k && r[i] != 255;) {
        uint8_t c = r[i], l = r[i + 1];
        printf("opt %u:", c);
        for (int j = 0; j < l; j++)
            printf(c == 60 || c == 17 ? "%c" : " %02x", r[i + 2 + j]);
        printf("\n");
        i += 2 + l;
    }
    return 0;
}
EOF
cc -std=gnu11 -O2 -Wall -o "${W}/client" "${W}/client.c"
head -c 45056 /dev/urandom >"${W}/booter.efi"
printf 'MZ' | dd of="${W}/booter.efi" conv=notrunc status=none
bp=$((41000 + RANDOM % 4000)); tp=$((bp + 1)); rp=$((bp + 2))
"${W}/bsdp" serve --booter "${W}/booter.efi" --server-ip 127.0.0.1 --mac 78:7b:8a:bd:d0:51 \
    --bsdp-port "${bp}" --tftp-port "${tp}" --seconds 60 >"${W}/log" 2>&1 &
spid=$!
for _ in $(seq 1 50); do grep -q '^.* serving BSDP' "${W}/log" && break; sleep 0.1; done
grep -q 'serving BSDP' "${W}/log" || fail "server did not start: $(cat "${W}/log")"

"${W}/client" "${bp}" "${rp}" list >"${W}/list"
grep -q '^op=2 xid=d7fea748 siaddr=127.0.0.1 chaddr_ok=1 ' "${W}/list" || fail "LIST answer header: $(cat "${W}/list")"
grep -q '^opt 53: 05$' "${W}/list" || fail "LIST answer is not a DHCP ACK"
grep -q '^opt 54: 7f 00 00 01$' "${W}/list" || fail "LIST answer server id"
grep -q '^opt 60:AAPLBSDPC$' "${W}/list" || fail "LIST answer vendor class"
grep -q "^opt 1: ff ff ff 00$" "${W}/list" || fail "LIST answer subnet mask"
grep -q '^opt 43: 01 01 01 04 02 80 00 07 04 81 00 00 01 09 0b 81 00 00 01 06 41 49 45 4e 4f 53$' "${W}/list" ||
    fail "LIST answer BSDP options: $(grep 'opt 43' "${W}/list")"
pass "INFORM[LIST] from iMac18,3 answered: ACK, one default image AIENOS (id 0x81000001, install image)"

"${W}/client" "${bp}" "${rp}" select >"${W}/sel"
grep -q '^op=2 xid=d7fea748 siaddr=127.0.0.1 chaddr_ok=1 file=aienos/netldr.efi sname=spark$' "${W}/sel" ||
    fail "SELECT answer header: $(head -1 "${W}/sel")"
grep -q '^opt 43: 01 01 02 08 04 81 00 00 01 82 06 41 49 45 4e 4f 53$' "${W}/sel" || fail "SELECT answer BSDP options: $(grep 'opt 43' "${W}/sel")"
grep -q '^opt 17:tftp:127.0.0.1:/aienos/none$' "${W}/sel" || fail "SELECT answer root path"
pass "INFORM[SELECT] answered: boot file aienos/netldr.efi from this server"

"${W}/client" "${bp}" "${rp}" discover >"${W}/disc"
grep -q '^no answer$' "${W}/disc" || fail "a DHCP DISCOVER got an answer (the router must stay the DHCP server)"
pass "DHCP DISCOVER ignored"

curl -s --tftp-blksize 1428 -o "${W}/got" "tftp://127.0.0.1:${tp}/aienos/netldr.efi" || fail "TFTP fetch failed: $(tail -3 "${W}/log")"
cmp -s "${W}/got" "${W}/booter.efi" || fail "TFTP bytes differ"
pass "TFTP delivered the booter byte for byte (blksize 1428)"
curl -s -o "${W}/got512" "tftp://127.0.0.1:${tp}/aienos/netldr.efi" && cmp -s "${W}/got512" "${W}/booter.efi" ||
    fail "TFTP default block size"
pass "TFTP delivered the booter with the default 512-byte blocks"
if curl -s -o "${W}/nope" "tftp://127.0.0.1:${tp}/etc/passwd"; then fail "TFTP served a file it should refuse"; fi
grep -q 'asked for "etc/passwd" (octet): refused' "${W}/log" || fail "refusal not logged"
pass "TFTP refused any other file"

"${W}/client" "${tp}" "${rp}" flood >"${W}/flood"
grep -q '^oack_len=' "${W}/flood" || fail "no option answer to a repeated-blksize request: $(cat "${W}/flood"); $(tail -5 "${W}/log")"
olen="$(sed -n 's/^oack_len=//p' "${W}/flood")"
((olen <= 128)) || fail "option answer of ${olen} bytes exceeds its 128-byte buffer"
[[ "$(grep -c '^blksize=' "${W}/flood")" == 1 ]] || fail "blksize not acknowledged exactly once: $(cat "${W}/flood")"
grep -q '^blksize=1428$' "${W}/flood" || fail "blksize answer: $(cat "${W}/flood")"
grep -q '^tsize=45056$' "${W}/flood" || fail "tsize answer: $(cat "${W}/flood")"
sleep 0.5
kill -0 "${spid}" 2>/dev/null || fail "server died on a repeated-blksize request: $(tail -20 "${W}/log")"
if grep -qE 'AddressSanitizer|runtime error' "${W}/log"; then fail "sanitizer report: $(grep -E -A5 'AddressSanitizer|runtime error' "${W}/log")"; fi
pass "TFTP request with blksize repeated 12 times: one bounded option answer (${olen} bytes), server still serving"

cat >"${W}/timeshim.c" <<'EOF'
#include <time.h>
/* A clock gmtime cannot convert (year beyond int); advances so --seconds still ends. */
static time_t now = (time_t)1 << 62;
time_t time(time_t *t)
{
    time_t v = now++;
    if (t)
        *t = v;
    return v;
}
EOF
cc -std=gnu11 -O2 -Wall -shared -fPIC -o "${W}/timeshim.so" "${W}/timeshim.c"
rc=0
LD_PRELOAD="${W}/timeshim.so" "${W}/bsdp_plain" serve --booter "${W}/booter.efi" --server-ip 127.0.0.1 \
    --mac 78:7b:8a:bd:d0:51 --bsdp-port "$((bp + 10))" --tftp-port "$((bp + 11))" --seconds 3 >"${W}/log_clock" 2>&1 || rc=$?
[[ "${rc}" == 0 ]] || fail "server exited ${rc} on a clock gmtime cannot convert: $(cat "${W}/log_clock")"
grep -q 'serving BSDP' "${W}/log_clock" && grep -q 'deadline reached' "${W}/log_clock" ||
    fail "log lines missing under an unconvertible clock: $(cat "${W}/log_clock")"
pass "unconvertible clock: log lines still written, clean exit"
echo "---- server log ----"; cat "${W}/log"
echo "${tag}: PASS"
