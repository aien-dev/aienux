#!/usr/bin/env bash
# ck_courier_udp_test.sh -- host test of Allen's Ethernet courier.
#
# The kernel's own courier code (native/kernel/x86/courier_udp_x86.c, built with
# -DCK_COURIER_HOST) talks UDP to the real relay (crates/allen-courier --udp),
# which asks a model server. Five runs: a clean wire; wires that lose about 1 frame
# in 3 and 1 in 2 each way; a model server that answers "busy" (503) twice before
# it answers; and one that sits silent for 4 s before it starts (the relay must
# keep sending WAIT meanwhile). The model here is a small stand-in so the run is
# exact and quick. With ALLEN_MODEL=host:port (the real server) a last run
# sends the same job to the real model. The real kernel cannot run this in QEMU
# (QEMU has no Broadcom chip), so this is the check of the courier's protocol.
# Ends with "AIENOS_COURIER_UDP: PASS" or "AIENOS_COURIER_UDP: FAIL <why>".
set -u
cd "$(dirname "$0")/.."
K=native/kernel
tag=AIENOS_COURIER_UDP
fail() { echo "${tag}: FAIL $*"; exit 1; }
W=$(mktemp -d "${TMPDIR:-/tmp}/ck_courier_udp.XXXXXX")
pids=()
cleanup() {
    for p in ${pids[@]+"${pids[@]}"}; do kill "$p" 2>/dev/null || true; done
    [ -n "${KEEP_LOGS:-}" ] && cp -r "$W" "${KEEP_LOGS}" 2>/dev/null
    rm -rf "$W"
}
trap cleanup EXIT

${HOSTCC:-cc} -std=c11 -D_DEFAULT_SOURCE -O2 -Wall -Wextra -Werror -DCK_COURIER_HOST -I$K/x86 -Inative/net \
    -o "$W/test" $K/x86/tests/courier_udp_test.c $K/x86/courier_udp_x86.c native/net/aienos_net.c ||
    fail "the test does not build"
if [ -n "${ALLEN_RELAY:-}" ]; then
    relay="$ALLEN_RELAY" # a relay built elsewhere
else
    cargo build -q --release --offline -p allen-courier 2>"$W/cargo.err" || { tail -5 "$W/cargo.err"; fail "the relay does not build"; }
    relay="${CARGO_TARGET_DIR:-target}/release/allen-courier"
fi

rustc --edition=2021 -D warnings -O -o "$W/model-fixture" "$K/x86/tests/courier_model_fixture.rs" ||
    fail "the model fixture does not build"
free_port() { "$W/model-fixture" --free-udp-port; }

# The fixture binds loopback before publishing its port. Its readiness wait
# is bounded and also notices an early failure, instead of guessing a delay.
start_model() { # port-file busy-count delay-seconds
    "$W/model-fixture" "$1" "$2" "$3" >"$1.log" 2>&1 &
    model_pid=$!
    pids+=("$model_pid")
    for _ in {1..100}; do
        [[ -s "$1" ]] && return 0
        kill -0 "$model_pid" 2>/dev/null || fail "model fixture exited before binding"
        sleep 0.05
    done
    fail "model fixture did not publish its port"
}

printf 'JOB 1 chat Answer as Allen.\thello allen, how was your day\tself=Allen\tname=Drake\tstarts=20\tnote asked: hi / said: Hello\n' >"$W/job.txt"

run() { # label model drop_every
    local label="$1" model="$2" drop="$3" rp
    rp=$(free_port)
    "$relay" --udp "$rp" --model "$model" --work "$W/work" --log-dir "$W/log-$label" --busy-wait "${ALLEN_WAIT:-90}" >"$W/relay-$label.log" 2>&1 &
    rpid=$!
    pids+=("$rpid")
    sleep 0.5
    "$W/test" "$rp" "$drop" "$W/job.txt" "${ALLEN_WAIT:-90}" | tee "$W/test-$label.log" | grep -a -E "hello after|first part|PASS|FAIL" | sed "s/^/[$label] /"
    grep -aq "COURIER_UDP: PASS" "$W/test-$label.log" || fail "run $label"
    sleep 0.4 # the relay writes its log line a moment after the last part
    kill "$rpid" 2>/dev/null || true
    wait "$rpid" 2>/dev/null
}

start_model "$W/clean.port" 0 0
mp=$(cat "$W/clean.port")
fake1=$model_pid
run clean "127.0.0.1:$mp" 0
run lossy "127.0.0.1:$mp" 3
run lossy2 "127.0.0.1:$mp" 2
grep -aq "job resends [1-9]" "$W/test-lossy2.log" || fail "the lost job was not sent again"
kill "$fake1" 2>/dev/null || true
wait "$fake1" 2>/dev/null
start_model "$W/busy.port" 2 0
mp2=$(cat "$W/busy.port")
run busy "127.0.0.1:$mp2" 0
grep -aqE "waits 2|waits [2-9]" "$W/relay-busy.log" || fail "the relay did not wait out the busy answers"
echo "[busy] relay waited out the busy answers: $(grep -a 'waits' "$W/relay-busy.log" | head -1 | sed 's/.*first in/first in/')"
start_model "$W/slow.port" 0 4
mp3=$(cat "$W/slow.port")
run slow "127.0.0.1:$mp3" 0
grep -aqE "[1-9][0-9]* waits" "$W/test-slow.log" || fail "the relay kept no word going while the model sat on the request"
echo "[slow] the relay sent WAIT while the model sat silent: $(grep -aoE '[0-9]+ waits' "$W/test-slow.log" | head -1)"
[ -s "$W"/log-clean/*.jsonl ] || fail "no line in the learning log"
grep -aq '"ask":"hello allen, how was your day"' "$W"/log-clean/*.jsonl || fail "the learning log lacks the question"
echo "learning log line: $(head -c 300 "$W"/log-clean/*.jsonl)"
if [ -n "${ALLEN_MODEL:-}" ]; then
    run real "$ALLEN_MODEL" 0
fi
echo "${tag}: PASS"
