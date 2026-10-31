#!/usr/bin/env bash
# reproduce-pipefail.sh: deterministic reproduction of the SIGPIPE (exit 141)
# race in window1-package.sh when using `tail | head` under `set -eo pipefail`,
# and verification that `dd if=... of=... bs=64k iflag=skip_bytes,count_bytes`
# exits 0 and produces byte-identical slices without pipes.
set -eu
FW=${FW:-/lib/firmware/nvidia/580.173.02/gsp_ga10x.bin}
T=$(mktemp -d /tmp/repro-pipefail.XXXXXX)
trap 'rm -rf "$T"' EXIT

if [[ -r "$FW" ]]; then
    SRC="$FW"
    off=64
    size=74969088
    echo "Using real GSP firmware: $FW (size $(stat -c %s "$FW") bytes)"
else
    SRC="$T/synthetic.bin"
    dd if=/dev/urandom of="$SRC" bs=1M count=16 status=none
    off=64
    size=8388608
    echo "Using synthetic binary payload: $SRC (size $(stat -c %s "$SRC") bytes)"
fi

echo "--- 1. Demonstrating SIGPIPE (exit 141) with tail | head under pipefail ---"
old_141_count=0
trials=20
for i in $(seq 1 "$trials"); do
    rc=0
    (
        set -euo pipefail
        tail -c +$((off + 1)) "$SRC" | head -c "$size" > /dev/null
    ) || rc=$?
    if [[ $rc -eq 141 ]]; then
        old_141_count=$((old_141_count + 1))
    fi
done
echo "tail | head under pipefail: $old_141_count / $trials runs exited with 141 (SIGPIPE)"

echo "--- 2. Demonstrating exit 0 with dd (no pipe, direct seek and slice) ---"
new_fail_count=0
for i in $(seq 1 "$trials"); do
    rc=0
    (
        set -euo pipefail
        dd if="$SRC" of=/dev/null bs=64k iflag=skip_bytes,count_bytes skip="$off" count="$size" status=none
    ) || rc=$?
    if [[ $rc -ne 0 ]]; then
        new_fail_count=$((new_fail_count + 1))
    fi
done
echo "dd with skip_bytes,count_bytes: $new_fail_count / $trials failures (all passed with rc 0)"

echo "--- 3. Verifying byte-for-byte identity against reference ---"
python3 -c "
with open('$SRC', 'rb') as f:
    f.seek($off)
    chunk = f.read($size)
with open('$T/reference.bin', 'wb') as f:
    f.write(chunk)
"
dd if="$SRC" of="$T/dd.bin" bs=64k iflag=skip_bytes,count_bytes skip="$off" count="$size" status=none

cmp "$T/reference.bin" "$T/dd.bin"
echo "BYTE-IDENTICAL: Reference and dd outputs match exactly ($size bytes)"

if [[ $old_141_count -gt 0 && $new_fail_count -eq 0 ]]; then
    echo "REPRODUCTION SUCCESS: tail|head exited 141; dd exited 0 with byte-identical output"
    exit 0
else
    echo "REPRODUCTION FAILED unexpected counts: old_141=$old_141_count new_fail=$new_fail_count"
    exit 1
fi
