#!/usr/bin/env bash
# Complete native Omega inference under the actual CPL3 kernel entry path.
set -euo pipefail
cd "$(dirname "$0")/.."
source scripts/lib_gate_hold.sh
if ! gh_quiet_check || ! gh_lock_take local_model 15; then
    echo "LOCAL_MODEL: NOT_RUN ($gh_why)"; exit 3
fi
trap gh_lock_release EXIT
out=${LOCAL_MODEL_OUT:?set LOCAL_MODEL_OUT to an isolated output directory}
weights=${LOCAL_MODEL_WEIGHTS:?set LOCAL_MODEL_WEIGHTS to pinned stories15M.bin}
tokenizer=${LOCAL_MODEL_TOKENIZER:?set LOCAL_MODEL_TOKENIZER to pinned tokenizer.bin}
[[ $(sha256sum "$weights" | cut -d' ' -f1) == cd590644d963867a2b6e5a1107f51fad663c41d79c149fbecbbb1f95fa81f49a ]]
[[ $(sha256sum "$tokenizer" | cut -d' ' -f1) == 50a52ef822ee9e83de5ce9d0be0a025a773d019437f58b5ff9dcafb063ece361 ]]
cross=""; [[ $(uname -m) == x86_64 ]] || cross=x86_64-linux-gnu-
make -s -j4 -C native/kernel X86_CROSS="$cross" OUT="$out/build" CK_LOCAL_TEST=1 \
    CK_LOCAL_MODEL=1 CK_LOCAL_MODEL_FILE="$weights" CK_LOCAL_TOKENIZER_FILE="$tokenizer" imac-kernel
image="$out/model.img"
truncate -s 256M "$image"
mformat -i "$image" -F ::
mmd -i "$image" ::/EFI ::/EFI/BOOT
mcopy -i "$image" "$out/build/IMACKERN-LOCAL-TEST-MODEL.EFI" ::/EFI/BOOT/BOOTX64.EFI
for cpu in ${LOCAL_MODEL_CPUS:-qemu64 max}; do
    cp /usr/share/OVMF/OVMF_VARS_4M.fd "$out/$cpu-vars.fd"
    rc=0
    timeout -k 3 240 qemu-system-x86_64 -M q35 -accel tcg -cpu "$cpu" -smp 4 -m 1024 \
        -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
        -drive if=pflash,format=raw,file="$out/$cpu-vars.fd" \
        -drive if=none,id=esp,format=raw,file="$image" -device ide-hd,drive=esp,bus=ide.0 \
        -display none -nic none -serial "file:$out/$cpu.log" -no-reboot >"$out/$cpu-qemu.log" 2>&1 || rc=$?
    [[ $rc == 0 || $rc == 124 ]]
    grep -a 'local_model_test: generation PASS tokens=32 .*real_cpl3=yes' "$out/$cpu.log"
    grep -a 'local_model_test: PASS isolation=1 quotas=1 cancellation=1 cleanup=1' "$out/$cpu.log"
    grep -a 'local_test: PASS.*repeated=1000 real_cpl3=yes' "$out/$cpu.log"
    ! grep -a 'panic:' "$out/$cpu.log"
    echo "LOCAL_MODEL: PASS cpu=$cpu"
done
