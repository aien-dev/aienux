#!/usr/bin/env bash
# allen_courier_up.sh -- starts the relay that answers Allen's courier, on the Spark.
#
# The relay listens on UDP port 47033 (Allen's Ethernet courier, native/kernel/x86/courier_udp.h)
# and asks the model server on this machine (127.0.0.1:18091), one request at a time,
# waiting and asking again with growing pauses while the server says it is busy (503). Every
# exchange is kept, one line each, in ~/.local/state/aienos/allen/conversations/<date>.jsonl:
# that is Allen's learning corpus (a data pipeline, not weight training).
#
# It is a plain background process (nohup), not a systemd unit, on purpose: it opens a port on
# the LAN that anyone there can talk to (the courier has no password yet), so it should run
# only while Allen is being tried, and stop with one command:   pkill -f 'allen-courier.*--udp 47033'
# Safe to run twice: it does nothing when the relay already runs. Never touches the model server.
set -u
cd "$(dirname "$0")/.."
state="${HOME}/.local/state/aienos/allen"
mkdir -p "${state}/bin" "${state}/conversations"
if pgrep -f 'allen-courier.*--udp 47033' >/dev/null; then
    echo "the relay is already running: $(pgrep -af 'allen-courier.*--udp 47033' | head -1)"
    exit 0
fi
export PATH="${HOME}/.cargo/bin:${PATH}"
if [ -z "${ALLEN_RELAY_BIN:-}" ]; then
    cargo build -q --release --offline -p allen-courier || { echo "the relay does not build"; exit 1; }
    sha="$(sha256sum "${CARGO_TARGET_DIR:-target}/release/allen-courier" | cut -c1-12)"
    cp "${CARGO_TARGET_DIR:-target}/release/allen-courier" "${state}/bin/allen-courier-${sha}"
    ALLEN_RELAY_BIN="${state}/bin/allen-courier-${sha}"
fi
nohup "${ALLEN_RELAY_BIN}" --udp 47033 --model "${ALLEN_MODEL:-127.0.0.1:18091}" \
    --log-dir "${state}/conversations" --work "${state}/work" \
    >>"${state}/relay.log" 2>&1 </dev/null &
sleep 1
if pgrep -f 'allen-courier.*--udp 47033' >/dev/null; then
    echo "the relay is up: $(pgrep -af 'allen-courier.*--udp 47033' | head -1)"
    echo "log: ${state}/relay.log   learning log: ${state}/conversations/"
else
    echo "the relay did not stay up; see ${state}/relay.log"
    exit 1
fi
