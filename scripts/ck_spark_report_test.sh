#!/bin/sh
# ck_spark_report_test.sh -- host test of the Spark boot report (L198): frames
# built by native/net/aienos_report.c (tag AIENUX-SPARK-REPORT) for a 40 KiB
# report, sent out of order and twice over loopback to the shared receiver
# tools/ck_imac_report_listen.c, which must rebuild it byte for byte as
# spark-<run>.txt and still accept the iMac's parts (scripts/ck_imac_report_test.sh).
# Hosted only; the RTL8127 itself is covered by native/net tests/rtl8127_test.c.
# Ends with "AIENUX_SPARK_REPORT: PASS" or "AIENUX_SPARK_REPORT: FAIL <why>".
set -u
cd "$(dirname "$0")/.."
K=native/kernel
N=native/net
W=$(mktemp -d "${TMPDIR:-/tmp}/ck_spark_report.XXXXXX")
trap 'kill $LP 2>/dev/null; rm -rf "$W"' EXIT
LP=
fail() { echo "AIENUX_SPARK_REPORT: FAIL $*"; exit 1; }
CC=${HOSTCC:-cc}
CF="-std=gnu11 -O2 -Wall -Wextra -Werror"
$CC $CF -o "$W/listen" $K/tools/ck_imac_report_listen.c || fail "listener does not build"
$CC $CF -o "$W/send" $N/tests/report_send.c $N/aienos_report.c $N/aienos_net.c || fail "sender does not build"
i=0
while [ $i -lt 40960 ]; do printf 'spark boot report line %05d\n' $((i / 29)); i=$((i + 29)); done | head -c 40960 > "$W/sent.txt"
PORT=$((47900 + $$ % 90))
mkdir -p "$W/out"
"$W/listen" $PORT "$W/out" 20 &
LP=$!
i=0
until grep -q "^ready port=$PORT" "$W/out/listen.log" 2>/dev/null; do
    i=$((i + 1)); [ $i -gt 50 ] && fail "listener not ready: $(cat "$W/out/listen.log" 2>/dev/null)"
    sleep 0.1
done
"$W/send" $PORT "$W/sent.txt" >/dev/null || fail "send"
i=0
until grep -q "^complete run=fedcba9876543210 " "$W/out/listen.log"; do
    i=$((i + 1)); [ $i -gt 50 ] && fail "report not completed: $(cat "$W/out/listen.log")"
    sleep 0.1
done
cmp -s "$W/sent.txt" "$W/out/spark-fedcba9876543210.txt" || fail "rebuilt report differs"
[ "$(grep -c '^complete ' "$W/out/listen.log")" = 1 ] || fail "report written more than once"
grep -q '^ignored' "$W/out/listen.log" && fail "listener ignored a valid part"
echo "AIENUX_SPARK_REPORT: PASS"
