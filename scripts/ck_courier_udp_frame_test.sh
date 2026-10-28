#!/usr/bin/env bash
# Actual kernel-produced Ethernet frames, real relay UDP answers. All network
# setup and offload changes are confined to a fresh namespace; no live service,
# interface, firewall, model or hardware is touched. Requires Linux iproute2,
# ethtool, unshare and sudo -n (or root); host cc, rustc and cargo offline cache.
# KEEP_LOGS must name a new output directory if evidence is to be retained.
# COURIER_CAPTURE_SELF_LOOP=1 additionally records loopback UDP and waits for
# the relay's existing 10 s drop log after a failing baseline/mutant run.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD
if [[ ${1:-} == --namespace ]]; then
    W=$2
    cleanup_namespace() {
        kill ${relay_pid:-} ${model_pid:-} ${capture_pid:-} 2>/dev/null || true
        wait ${relay_pid:-} ${model_pid:-} ${capture_pid:-} 2>/dev/null || true
        chown -R "${SUDO_UID:-0}:${SUDO_GID:-0}" "$W"
    }
    trap cleanup_namespace EXIT
    ip link set lo up
    ip link add imac0 type veth peer name spark0
    ip address add 192.168.1.108/24 broadcast 192.168.1.255 dev spark0
    ip link set imac0 up
    ip link set spark0 up
    # AF_PACKET sees final bytes, not veth CHECKSUM_PARTIAL metadata.
    ethtool -K spark0 tx off
    if [[ ${COURIER_CAPTURE_SELF_LOOP:-0} == 1 ]]; then
        tcpdump -U -n -i lo -w "$W/self-loop.pcap" 'udp port 47033' >"$W/capture.log" 2>&1 &
        capture_pid=$!
    fi
    "$W/model" "$W/model.port" 0 0 >"$W/model.log" 2>&1 &
    model_pid=$!
    for _ in {1..100}; do
        [[ -s "$W/model.port" ]] && break
        kill -0 "$model_pid"
        sleep .02
    done
    [[ -s "$W/model.port" ]]
    model=$(cat "$W/model.port")
    "$W/relay" --udp 47033 --udp-answer 192.168.1.255:47033 \
        --model "127.0.0.1:$model" --work "$W/work" --log-dir "$W/conversations" >"$W/relay.log" 2>&1 &
    relay_pid=$!
    # Wait for the socket, not an assumed server startup delay.
    for _ in {1..100}; do
        if ss -H -uln 'sport = :47033' | read -r _; then break; fi
        kill -0 "$relay_pid"
        sleep .02
    done
    mkdir "$W/frames"
    test_rc=0
    "$W/test" imac0 "$W/frames" || test_rc=$?
    if [[ $test_rc != 0 ]]; then
        if [[ ${COURIER_CAPTURE_SELF_LOOP:-0} == 1 ]]; then
            sleep 7 # observe the existing LINE drop after its 10 s deadline
        fi
        exit "$test_rc"
    fi
    # A valid emitted GOT intentionally withheld at the frame-device seam must
    # make the same test fail on the relay's actual LINE retransmission.
    mkdir "$W/negative"
    if "$W/test" imac0 "$W/negative" drop-got >"$W/drop-got.log" 2>&1; then
        echo 'COURIER_FRAMES: FAIL dropped GOT control unexpectedly passed'
        exit 1
    fi
    if ! [[ $(cat "$W/drop-got.log") == *'relay retransmitted LINE after GOT'* ]]; then
        cat "$W/drop-got.log"
        echo 'COURIER_FRAMES: FAIL negative control failed for unrelated reason'
        exit 1
    fi
    echo 'COURIER_FRAME_ACK_CONTROL: PASS withholding GOT triggers real LINE retransmission'
    exit
fi
W=$(mktemp -d "${TMPDIR:-/tmp}/ck_courier_frames.XXXXXX")
cleanup() {
    if [[ -n ${KEEP_LOGS:-} ]]; then
        cp -r "$W" "$KEEP_LOGS"
    fi
    rm -rf "$W"
}
trap cleanup EXIT
${HOSTCC:-cc} -std=c11 -D_DEFAULT_SOURCE -O2 -Wall -Wextra -Werror -DCK_COURIER_HOST \
    -Inative/kernel/x86 -Inative/net -o "$W/test" \
    native/kernel/x86/tests/courier_udp_frame_test.c native/kernel/x86/courier_udp_x86.c native/net/aienos_net.c
rustc --edition=2021 -D warnings -O -o "$W/model" native/kernel/x86/tests/courier_model_fixture.rs
if [[ -n ${ALLEN_RELAY:-} ]]; then
    cp "$ALLEN_RELAY" "$W/relay"
else
    CARGO_TARGET_DIR="$W/target" cargo build --release --offline -p allen-courier
    cp "$W/target/release/allen-courier" "$W/relay"
fi
if [[ $EUID == 0 ]]; then
    COURIER_CAPTURE_SELF_LOOP=${COURIER_CAPTURE_SELF_LOOP:-0} unshare --net -- bash "$ROOT/scripts/ck_courier_udp_frame_test.sh" --namespace "$W"
else
    sudo -n env COURIER_CAPTURE_SELF_LOOP="${COURIER_CAPTURE_SELF_LOOP:-0}" unshare --net -- bash "$ROOT/scripts/ck_courier_udp_frame_test.sh" --namespace "$W"
fi
