#!/usr/bin/env bash
# AIENOS_CK_PIPE gate (AIENOS-NATIVE-PIPE-1, aienos-mv#428): the native pipe primitive (native/kernel/core/pipe.c) driven
# from inside the C kernel. A TEST-ONLY image (make CK_PIPE_TEST=1, refused with CK_HARDWARE_STAGING) runs
# core/pipe_test.c after "kernel: alive": a producer and a consumer task, step functions interleaved by the kernel's
# run-queue scheduler, each holding one pipe end in its own capability table, then the negative cases. The boot then
# goes on as usual to its report and PSCI reset.
#
# Label: QEMU (aarch64 virt), TEST build, not physical. Nothing here ran on a Spark or any real machine; a PASS
# qualifies nothing physical.
# Checks (one boot):
#   payload_nul_byte_for_byte            160 KiB with every fifth byte NUL arrives byte for byte, no temporary file
#   backpressure_would_block             a slow consumer fills the 64 KiB pipe; the producer sees WOULD_BLOCK
#   eof_after_last_writer                the consumer reads EOF (0 bytes, OK) after the producer closes
#   bounded_memory                       never more than the pipe capacity buffered
#   blocking_wait_wake_through_scheduler the blocking variant parks and is woken through the scheduler
#   broken_pipe_no_reader                write with no reader -> BROKEN_PIPE, no bytes
#   revoked_write_refused_no_bytes_after write after revoke -> REVOKED, nothing delivered after it
#   revoke_midstream_no_data_after       the consumer revokes mid-transfer; no read succeeds afterwards
#   stale_handle_after_close             a closed handle stays STALE after its slot is reused
#   unknown_op_not_supported             unknown op / other ABI version -> NOT_SUPPORTED before any effect
#   cancel_interrupts_once               cancel -> INTERRUPTED once with no byte moved
# every check: both capability tables and the pipe pool empty afterwards (tables_empty=1).
# Last line: AIENOS_CK_PIPE: PASS|FAIL|NOT_RUN.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"
verdict=AIENOS_CK_PIPE
code_fd="${AAVMF_CODE:-/usr/share/AAVMF/AAVMF_CODE.no-secboot.fd}"
vars_fd="${AAVMF_VARS:-/usr/share/AAVMF/AAVMF_VARS.fd}"
command -v qemu-system-aarch64 >/dev/null || { echo "qemu-system-aarch64 not installed"; echo "${verdict}: NOT_RUN"; exit 2; }
[[ -r "${code_fd}" && -r "${vars_fd}" ]] || { echo "AAVMF firmware not found"; echo "${verdict}: NOT_RUN"; exit 2; }

cross=""
if [ "$(uname -m)" != "aarch64" ]; then cross="aarch64-linux-gnu-"; fi
commit="$(git rev-parse HEAD 2>/dev/null || echo unknown)"
out="${AIENOS_CK_OUT_PIPE:-${repo_root}/target/native-kernel-test-pipe}"
make -s -C native/kernel CROSS="${cross}" OUT="${out}" AIENOS_COMMIT="${commit}" CK_PIPE_TEST=1 >/dev/null

# Machine quiet flag (read only) and the QEMU gate lock (one gate at a time):
# scripts/lib_gate_hold.sh (aienos#278).
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib_gate_hold.sh"
if ! gh_quiet_check || ! gh_lock_take qemu_ck_pipe_test "${AIENOS_GATE_MINUTES:-60}"; then
    echo "NOT_RUN  ${gh_why}"
    echo "${verdict}: NOT_RUN"
    exit 3
fi
# Release only this run's own gate lock record, at most once.
release_flag() { gh_lock_release; }
work="$(mktemp -d)"
cleanup() { rm -rf "${work}"; release_flag; }
trap cleanup EXIT
mkdir -p "${work}/esp/EFI/BOOT" "${work}/esp/EFI/AIENOS"
touch "${work}/esp/EFI/AIENOS/BOOTREPORT.TXT"
cp "${out}/BOOTAA64.EFI" "${work}/esp/EFI/BOOT/BOOTAA64.EFI"
cp "${vars_fd}" "${work}/vars.fd"
log="${work}/serial.log"

started=$(date +%s)
set +e
timeout "${AIENOS_QEMU_TIMEOUT:-300}" qemu-system-aarch64 \
    -M virt,virtualization=on,gic-version=3 -accel tcg,thread=single -cpu max -smp 4 -m 2048 \
    -drive if=pflash,format=raw,readonly=on,file="${code_fd}" \
    -drive if=pflash,format=raw,file="${work}/vars.fd" \
    -drive if=none,id=esp,format=raw,file=fat:rw:"${work}/esp" \
    -device virtio-blk-pci,drive=esp \
    -device ramfb -display none -nic none \
    -serial file:"${log}" -no-reboot
qemu_status=$?
set -e
elapsed=$(( $(date +%s) - started ))
release_flag

serial="${work}/serial.txt"
tr -d '\r' <"${log}" >"${serial}"
[[ -z "${AIENOS_LOG_DIR:-}" ]] || cp "${serial}" "${AIENOS_LOG_DIR}/qemu_ck_pipe_serial.log"
failed=0
pass() { echo "PASS  $1"; }
fail() { echo "FAIL  $1"; failed=1; }
has() { grep -qE -- "$2" "$1"; }
check() { if has "${serial}" "$2"; then pass "$1"; else fail "$1"; fi; }
echo "qemu exit ${qemu_status} after ${elapsed} s (commit ${commit:0:12})"
[[ "${qemu_status}" == 0 ]] && pass "QEMU exit status 0 (PSCI reset)" || fail "QEMU exit status ${qemu_status} (124 = timeout)"

label='QEMU \(aarch64 virt\), TEST build, not physical'
check "left firmware and entered the kernel" "kernel: alive"
check "the image announces the TEST-ONLY pipe self-test" "^pipe_test: TEST-ONLY pipe self-test begin abi_version=1 capacity=65536 pool_pipes=4 payload=163840; ${label}\$"
for c in payload_nul_byte_for_byte backpressure_would_block eof_after_last_writer bounded_memory \
         blocking_wait_wake_through_scheduler broken_pipe_no_reader revoked_write_refused_no_bytes_after \
         revoke_midstream_no_data_after stale_handle_after_close unknown_op_not_supported cancel_interrupts_once; do
    check "${c}" "^pipe_check: name=${c} result=PASS tables_empty=1; ${label}\$"
done
check "payload detail: all 163840 bytes, NUL bytes counted, zero mismatches" "^pipe_detail: nonblocking bytes=163840 sent=163840 nul=[1-9][0-9]* expected_nul=[1-9][0-9]* mismatches=0 would_block=[1-9]"
check "blocking detail: waits and wakeups observed" "^pipe_detail: blocking bytes=163840 .* waits=[1-9][0-9]* wakeups=[1-9]"
check "revoked write refused, zero bytes delivered" "^pipe_detail: revoked write=REVOKED n=0 read=REVOKED n=0 bytes_delivered=0\$"
check "broken pipe detail" "^pipe_detail: broken_pipe write_after_reader_close=BROKEN_PIPE n=0\$"
check "self-test summary: 11 checks, 0 failed" "^pipe_test: done checks=11 failed=0 capacity=65536 pool_pipes=4; ${label}\$"
if has "${serial}" "^pipe_check: .* result=FAIL"; then fail "no pipe check failed"; else pass "no pipe check failed"; fi
check "the boot went on to its report" "report_kind: final"
if has "${serial}" "report_kind: (panic|fault)"; then fail "panic or fault reported"; else pass "no kernel panic or fault report"; fi

if [[ "${failed}" != 0 || -n "${AIENOS_QEMU_VERBOSE:-}" ]]; then
    echo "---- serial console (pipe lines) ----"
    grep -E "^(pipe_|kernel: alive|report_kind|panic)" "${serial}" || true
fi
[[ "${failed}" == 0 ]] && { echo "${verdict}: PASS"; exit 0; }
echo "${verdict}: FAIL"
exit 1
