#!/usr/bin/env bash
# UDP adapter oracle for the local-tools courier path, synthetic result bytes.
# Real CPL3 execution is covered separately by scripts/qemu_ck_local_tools_test.sh.
# Builds the C model oracle and C wire test, then runs them against the Rust relay.
set -euo pipefail
cd "$(dirname "$0")/.."
tmp=$(mktemp -d "${TMPDIR:-/tmp}/local_tools_udp.XXXXXX")
trap 'rm -rf "$tmp"' EXIT
cc="${HOSTCC:-cc}"
for unit in local_tools_model_oracle local_tools_udp_test; do
    "$cc" -std=c11 -D_DEFAULT_SOURCE -O2 -Wall -Wextra -Werror \
        -o "$tmp/$unit" "native/kernel/x86/tests/$unit.c"
done
cargo build -q -p allen-courier
"$tmp/local_tools_udp_test" "$tmp/local_tools_model_oracle" "${ALLEN_RELAY:-${CARGO_TARGET_DIR:-target}/debug/allen-courier}"
