#!/usr/bin/env bash
# ck_imac_bsdp_test.sh -- host test for tools/ck_imac_bsdp.c (Apple NetBoot answerer).
# Runs the answerer on loopback high ports and plays the iMac with a small C client that
# sends the INFORM[LIST] the real iMac18,3 sent on 2026-10-17 (vendor class
# "AAPLBSDPC/i386/iMac18,3", option 43 = 01 01 01 02 02 01 01) plus a reply port, then an
# INFORM[SELECT] for the offered image; checks every answer field; then fetches the booter
# with curl over TFTP (blksize + tsize) and compares bytes; checks a wrong file name and a
# DHCP DISCOVER get nothing. A second and third answerer run with --reply-source-port: the answers
# must leave from that source port (the default run: from the listening port), with the same
# fields, client identifier echo, MAC restriction and TFTP. Loopback says nothing about the iMac
# firmware.
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
spids=()
trap '[[ -z "${spid}" ]] || kill "${spid}" 2>/dev/null || true; for p in "${spids[@]}"; do kill "${p}" 2>/dev/null || true; done; rm -rf "${W}"' EXIT
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
/* client SERVER_PORT REPLY_PORT list|select|discover [none|short|max] [mac|other] -> prints the answer's fields
 * and, last, src_port=N (the UDP source port the answer came from).
 * The fourth argument picks the request's DHCP client identifier (option 61): none = absent,
 * short = the iMac's 7 bytes (01 + MAC), max = 255 binary bytes (including 00 and ff).
 * The fifth: other = a different client MAC (the server's --mac restriction must ignore it). */
int main(int argc, char **argv)
{
    int sp = atoi(argv[1]), rp = atoi(argv[2]);
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in me = {.sin_family = AF_INET, .sin_port = htons(rp), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(sp), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    if (bind(s, (struct sockaddr *)&me, sizeof me) != 0)
        return 3;
    if (strcmp(argv[3], "flood") == 0)
        return flood(to, s);
    uint8_t p[800] = {0};
    size_t n = 240;
    p[0] = 1, p[1] = 1, p[2] = 6;
    p[4] = 0xd7, p[5] = 0xfe, p[6] = 0xa7, p[7] = 0x48;
    p[12] = 127, p[15] = 1; /* ciaddr 127.0.0.1 (the iMac sent its leased address) */
    uint8_t mac[6] = {0x78, 0x7b, 0x8a, 0xbd, 0xd0, 0x51};
    if (argc > 5 && strcmp(argv[5], "other") == 0)
        mac[5] = 0x52;
    memcpy(p + 28, mac, 6);
    p[236] = 0x63, p[237] = 0x82, p[238] = 0x53, p[239] = 0x63;
    int disc = strcmp(argv[3], "discover") == 0, sel = strcmp(argv[3], "select") == 0;
    p[n++] = 53, p[n++] = 1, p[n++] = disc ? 1 : 8;
    p[n++] = 55, p[n++] = 2, p[n++] = 43, p[n++] = 60;
    p[n++] = 57, p[n++] = 2, p[n++] = 0x05, p[n++] = 0xdc;
    const char *cid = argc > 4 ? argv[4] : "none";
    if (strcmp(cid, "short") == 0) {
        p[n++] = 61, p[n++] = 7, p[n++] = 1, memcpy(p + n, mac, 6), n += 6;
    } else if (strcmp(cid, "max") == 0) {
        p[n++] = 61, p[n++] = 255;
        for (int i = 0; i < 255; i++)
            p[n++] = (uint8_t)(i * 7 + 255); /* i=0 gives ff, i=1 gives 06, ... 00 appears too */
    }
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
    uint8_t r[1600];
    struct sockaddr_in from;
    socklen_t fl = sizeof from;
    ssize_t k = recvfrom(s, r, sizeof r, 0, (struct sockaddr *)&from, &fl);
    printf("op=%u xid=%02x%02x%02x%02x siaddr=%u.%u.%u.%u chaddr_ok=%d file=%s sname=%s\n", r[0], r[4], r[5], r[6],
           r[7], r[20], r[21], r[22], r[23], memcmp(r + 28, mac, 6) == 0, (char *)r + 108, (char *)r + 44);
    ssize_t i = 240;
    for (; i + 1 < k && r[i] != 255;) {
        uint8_t c = r[i], l = r[i + 1];
        if (i + 2 + l > k)
            break;
        printf("opt %u:", c);
        for (int j = 0; j < l; j++)
            printf(c == 60 || c == 17 ? "%c" : " %02x", r[i + 2 + j]);
        printf("\n");
        i += 2 + l;
    }
    printf("end=%d len=%zd\n", i < k && r[i] == 255, k);
    printf("src_port=%u\n", ntohs(from.sin_port));
    return 0;
}
EOF
cc -std=gnu11 -O2 -Wall -o "${W}/client" "${W}/client.c"
head -c 45056 /dev/urandom >"${W}/booter.efi"
printf 'MZ' | dd of="${W}/booter.efi" conv=notrunc status=none
bp=$((41000 + RANDOM % 4000)); tp=$((bp + 1)); rp=$((bp + 2))
"${W}/bsdp" serve --booter "${W}/booter.efi" --server-ip 127.0.0.1 --mac 78:7b:8a:bd:d0:51 \
    --router 127.0.0.2 --bsdp-port "${bp}" --tftp-port "${tp}" --seconds 60 >"${W}/log" 2>&1 &
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

# DHCP client identifier (option 61): RFC 6842 section 3 says a server that gets one must echo it
# byte for byte; no identifier in, none out. Short = the iMac's own 7 bytes, max = 255 binary bytes.
cid_short=" 01 78 7b 8a bd d0 51"
cid_max=""
for i in $(seq 0 254); do cid_max+="$(printf ' %02x' $(((i * 7 + 255) & 255)))"; done
for kind in list select; do
    for cid in none short max; do
        "${W}/client" "${bp}" "${rp}" "${kind}" "${cid}" >"${W}/${kind}.${cid}"
        f="${W}/${kind}.${cid}"
        grep -q '^op=2 xid=d7fea748 siaddr=127.0.0.1 chaddr_ok=1 ' "${f}" || fail "${kind}/${cid}: no answer: $(head -1 "${f}")"
        grep -q '^end=1 ' "${f}" || fail "${kind}/${cid}: answer options cut off or overran: $(tail -1 "${f}")"
        grep -q '^opt 43: ' "${f}" || fail "${kind}/${cid}: answer lost its BSDP options"
        [[ "${kind}" == list ]] || grep -q '^opt 17:tftp:127.0.0.1:/aienos/none$' "${f}" || fail "${kind}/${cid}: answer lost its root path"
        n61="$(grep -c '^opt 61:' "${f}" || true)"
        case "${cid}" in
        none) [[ "${n61}" == 0 ]] || fail "${kind}/none: answer invented a client identifier: $(grep '^opt 61' "${f}")" ;;
        short) [[ "$(grep '^opt 61:' "${f}")" == "opt 61:${cid_short}" && "${n61}" == 1 ]] || fail "${kind}/short: client identifier not echoed exactly: $(grep '^opt 61' "${f}")" ;;
        max) [[ "$(grep '^opt 61:' "${f}")" == "opt 61:${cid_max}" && "${n61}" == 1 ]] || fail "${kind}/max: 255-byte client identifier not echoed exactly (${n61} copies): $(grep '^opt 61' "${f}" | cut -c1-80)" ;;
        esac
    done
done
pass "client identifier (option 61) echoed byte for byte on LIST and SELECT: absent stays absent, 7 bytes, 255 bytes, answer complete"

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

# Default source port: the answers leave from the listening socket, so the source port is --bsdp-port.
for f in list sel; do
    grep -q "^src_port=${bp}\$" "${W}/${f}" || fail "default: ${f} answer did not leave from the listening port ${bp}: $(grep '^src_port' "${W}/${f}")"
done
pass "default run: BSDP answers leave from the listening port (source port ${bp}), unchanged"
"${W}/client" "${bp}" "${rp}" list none other >"${W}/other"
grep -q '^no answer$' "${W}/other" || fail "a client with another MAC got an answer from the --mac restricted server"
pass "--mac restriction: a client with another MAC is ignored"

# --reply-source-port: same answers, from another UDP source port; requests still arrive on
# --bsdp-port, TFTP is untouched. Run 2 uses a distinct port, run 3 names the listening port itself
# (same as the default), then the unusable values must be refused.
start_server() { # name bsdp-port tftp-port extra-args...
    local name="$1" b="$2" t="$3"; shift 3
    "${W}/bsdp" serve --booter "${W}/booter.efi" --server-ip 127.0.0.1 --mac 78:7b:8a:bd:d0:51 \
        --router 127.0.0.2 --bsdp-port "${b}" --tftp-port "${t}" --seconds 60 "$@" >"${W}/${name}.log" 2>&1 &
    spids+=($!)
    for _ in $(seq 1 50); do grep -q 'serving BSDP' "${W}/${name}.log" && return 0; sleep 0.1; done
    fail "${name}: server did not start: $(cat "${W}/${name}.log")"
}
check_reply_port() { # name bsdp-port tftp-port expected-source-port
    local name="$1" b="$2" t="$3" want="$4" kind cid f
    for kind in list select; do
        for cid in none short max; do
            f="${W}/${name}.${kind}.${cid}"
            "${W}/client" "${b}" "${rp}" "${kind}" "${cid}" >"${f}"
            grep -q '^op=2 xid=d7fea748 siaddr=127.0.0.1 chaddr_ok=1 ' "${f}" || fail "${name} ${kind}/${cid}: no answer: $(head -1 "${f}")"
            grep -q "^src_port=${want}\$" "${f}" || fail "${name} ${kind}/${cid}: answer source port is not ${want}: $(grep '^src_port' "${f}")"
            grep -q '^end=1 ' "${f}" || fail "${name} ${kind}/${cid}: answer options cut off or overran"
            grep -q '^opt 53: 05$' "${f}" && grep -q '^opt 54: 7f 00 00 01$' "${f}" && grep -q '^opt 60:AAPLBSDPC$' "${f}" &&
                grep -q '^opt 1: ff ff ff 00$' "${f}" || fail "${name} ${kind}/${cid}: ACK, server id, vendor class or mask changed"
            if [[ "${kind}" == list ]]; then
                grep -q '^opt 43: 01 01 01 04 02 80 00 07 04 81 00 00 01 09 0b 81 00 00 01 06 41 49 45 4e 4f 53$' "${f}" ||
                    fail "${name} list/${cid}: BSDP options changed: $(grep 'opt 43' "${f}")"
            else
                grep -q '^op=2 xid=d7fea748 siaddr=127.0.0.1 chaddr_ok=1 file=aienos/netldr.efi sname=spark$' "${f}" || fail "${name} select/${cid}: boot file changed"
                grep -q '^opt 43: 01 01 02 08 04 81 00 00 01 82 06 41 49 45 4e 4f 53$' "${f}" || fail "${name} select/${cid}: BSDP options changed"
                grep -q '^opt 17:tftp:127.0.0.1:/aienos/none$' "${f}" || fail "${name} select/${cid}: root path changed"
            fi
            case "${cid}" in
            none) [[ "$(grep -c '^opt 61:' "${f}" || true)" == 0 ]] || fail "${name} ${kind}/none: invented a client identifier" ;;
            short) [[ "$(grep '^opt 61:' "${f}")" == "opt 61:${cid_short}" ]] || fail "${name} ${kind}/short: client identifier not echoed exactly" ;;
            max) [[ "$(grep '^opt 61:' "${f}")" == "opt 61:${cid_max}" ]] || fail "${name} ${kind}/max: 255-byte client identifier not echoed exactly" ;;
            esac
        done
    done
    "${W}/client" "${b}" "${rp}" discover >"${W}/${name}.disc"
    grep -q '^no answer$' "${W}/${name}.disc" || fail "${name}: a DHCP DISCOVER got an answer"
    "${W}/client" "${b}" "${rp}" list none other >"${W}/${name}.other"
    grep -q '^no answer$' "${W}/${name}.other" || fail "${name}: a client with another MAC got an answer"
    curl -s --tftp-blksize 1428 -o "${W}/${name}.got" "tftp://127.0.0.1:${t}/aienos/netldr.efi" || fail "${name}: TFTP fetch failed"
    cmp -s "${W}/${name}.got" "${W}/booter.efi" || fail "${name}: TFTP bytes differ"
    if curl -s -o "${W}/${name}.nope" "tftp://127.0.0.1:${t}/etc/passwd"; then fail "${name}: TFTP served a file it should refuse"; fi
}
sp=$((bp + 3)); bp2=$((bp + 4)); tp2=$((bp + 5)); bp3=$((bp + 6)); tp3=$((bp + 7))
start_server rport "${bp2}" "${tp2}" --reply-source-port "${sp}"
grep -q "BSDP answers are sent from UDP source port ${sp}" "${W}/rport.log" || fail "reply source port not logged: $(cat "${W}/rport.log")"
check_reply_port rport "${bp2}" "${tp2}" "${sp}"
pass "--reply-source-port ${sp}: LIST and SELECT answers leave from source port ${sp} (requests read on ${bp2}), same fields and client identifier echo; DISCOVER and other MACs ignored; TFTP byte for byte"
start_server same "${bp3}" "${tp3}" --reply-source-port "${bp3}"
check_reply_port same "${bp3}" "${tp3}" "${bp3}"
grep -q 'BSDP answers are sent from UDP source port' "${W}/same.log" && fail "naming the listening port opened a second socket"
pass "--reply-source-port equal to --bsdp-port: same listening socket, source port ${bp3}"
for bad in 0 65536 -1 abc "" "${tp2}"; do
    set +e
    "${W}/bsdp" serve --booter "${W}/booter.efi" --server-ip 127.0.0.1 --bsdp-port "$((bp + 8))" --tftp-port "${tp2}" \
        --reply-source-port "${bad}" --seconds 1 >"${W}/bad.log" 2>&1
    rc=$?
    set -e
    [[ "${rc}" == 2 ]] || fail "--reply-source-port '${bad}' was not refused with usage (exit ${rc})"
done
pass "--reply-source-port refuses 0, 65536, -1, text, empty and the TFTP port (exit 2)"
echo "---- server log ----"; cat "${W}/log"; echo "---- reply-source-port server log ----"; cat "${W}/rport.log"
echo "${tag}: PASS"
