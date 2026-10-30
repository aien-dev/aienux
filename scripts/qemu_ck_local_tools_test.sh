#!/usr/bin/env bash
# Real CPL3 probes on two CPU models; runs only our QEMU instances.
set -euo pipefail
cd "$(dirname "$0")/.."
source scripts/lib_gate_hold.sh
if ! gh_quiet_check || ! gh_lock_take local_tools 15; then
    echo "LOCAL_TOOLS: NOT_RUN ($gh_why)"; exit 3
fi
relay_pid=""; oracle_pid=""
cleanup() {
    [[ -z "$relay_pid" ]] || kill "$relay_pid" 2>/dev/null || true
    [[ -z "$oracle_pid" ]] || kill "$oracle_pid" 2>/dev/null || true
    gh_lock_release
}
trap cleanup EXIT
out="${LOCAL_TOOLS_OUT:-$PWD/target/local-tools}"
mkdir -p "$out"
cross=""; [[ $(uname -m) = x86_64 ]] || cross=x86_64-linux-gnu-
make -s -j4 -C native/kernel X86_CROSS="$cross" OUT="$out/build" CK_LOCAL_TEST=1 imac-kernel
cargo build -q -p allen-courier
for cpu in qemu64 max; do
    run="$out/$cpu-courier"; mkdir -p "$run"
    rm -f "$run/oracle-port.txt" "$run/oracle-pass.txt" "$run/oracle-failure.txt" "$run/wire.sock"
    python3 scripts/local_tools_model_oracle.py "$run" >"$run/oracle.log" 2>&1 &
    oracle_pid=$!
    for ((i=0;i<100;i++)); do [[ ! -f "$run/oracle-port.txt" ]] || break; sleep .1; done
    port=$(cat "$run/oracle-port.txt")
    target/debug/allen-courier --socket "$run/wire.sock" --model "127.0.0.1:$port" \
        --work "$run/work" --log-dir "$run/logs" >"$run/relay.log" 2>&1 &
    relay_pid=$!
    image="$out/$cpu.img"
    truncate -s 64M "$image"
    mformat -i "$image" -F ::
    mmd -i "$image" ::/EFI ::/EFI/BOOT
    mcopy -i "$image" "$out/build/IMACKERN-LOCAL-TEST.EFI" ::/EFI/BOOT/BOOTX64.EFI
    cp /usr/share/OVMF/OVMF_VARS_4M.fd "$out/$cpu-vars.fd"
    timeout -k 3 60 qemu-system-x86_64 -M q35 -accel tcg -cpu "$cpu" -smp 4 -m 1024 \
        -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
        -drive if=pflash,format=raw,file="$out/$cpu-vars.fd" \
        -drive if=none,id=esp,format=raw,file="$image" -device ide-hd,drive=esp,bus=ide.0 \
        -display none -nic none -serial "file:$out/$cpu.log" \
        -serial "unix:$run/wire.sock,server=on,wait=off" -no-reboot >"$out/$cpu-qemu.log" 2>&1 || true
    grep -a 'local_test: PASS.*repeated=1000 real_cpl3=yes' "$out/$cpu.log"
    ! grep -a 'panic:' "$out/$cpu.log"
    grep -a 'local_courier: PASS real_cpl3_results=yes' "$out/$cpu.log"
    grep -a 'local_direct: PASS result=56088 .*real_cpl3=yes' "$out/$cpu.log"
    wait "$oracle_pid"; oracle_pid=""
    cat "$run/oracle-pass.txt"
    kill "$relay_pid" 2>/dev/null || true; wait "$relay_pid" 2>/dev/null || true; relay_pid=""
    echo "LOCAL_TOOLS: PASS cpu=$cpu"
done
