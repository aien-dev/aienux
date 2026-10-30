#!/usr/bin/env bash
# qemu_ck_gpu_bench_test.sh -- QEMU test for the GPU bench console, driven by the real Spark tool
# (tools/ck_gpu_bench.c) over a COM1 socket. QEMU is an emulator: the BARs are RAM stand-ins
# (CK_X86_BENCH_TEST=1, see native/kernel/x86/bench_x86.c), not PCI hardware. A PASS qualifies
# nothing about the AMD GPU, and the UDP/tg3 transport is not exercised here.
#
# The kernel is built as the lab image with the test transport and the test key (CK_TEST_SLOT_KEY=1
# is the swap owner key; the bench key is the test key in bench_proto.h). The tool connects, waits
# for "bench: GPU bench console ready", then sends the script below. Every reply is HMAC-checked by
# the tool with its own SHA-256 (a kernel/tool HMAC mismatch fails the run).
#
# Checks:
#   ping round trip; dword config read; BAR read and write (write logged before/after in the
#   kernel); allowlist refusal (RANGE); TSC poll (OK and TIMEOUT); read-modify-write;
#   replay refused (same counter); bad HMAC refused (AUTH); stale counter refused (AUTH);
#   all-ones reply stops the tool (device gone): the command after it is never sent.
# Final line: AIENOS_GPU_BENCH: PASS|FAIL|NOT_RUN   exit 0/1/2/3
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"
tag="AIENOS_GPU_BENCH"
code_fd="${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}"
vars_fd="${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}"
cross=""
if [ "$(uname -m)" != "x86_64" ]; then cross="x86_64-linux-gnu-"; fi
for t in qemu-system-x86_64 mformat mmd mcopy timeout "${cross}gcc" "${cross}ld"; do
    command -v "${t}" >/dev/null || { echo "${tag}: NOT_RUN (${t} missing)"; exit 2; }
done
[[ -r "${code_fd}" && -r "${vars_fd}" ]] || { echo "${tag}: NOT_RUN (no OVMF)"; exit 2; }

. "${repo_root}/scripts/lib_gate_hold.sh"
if ! gh_quiet_check || ! gh_lock_take qemu_ck_gpu_bench_test "${AIENOS_GATE_MINUTES:-60}"; then
    echo "${tag}: NOT_RUN (${gh_why})"
    exit 3
fi
work="$(mktemp -d)"
qpid=""
cleanup() {
    [[ -z "${qpid}" ]] || kill "${qpid}" 2>/dev/null || true
    # a red run keeps its logs for diagnosis
    if [[ "${fails:-0}" -gt 0 ]]; then
        keepdir="${HOME}/handoffs/gpu-bench-logs/${tag}-$(date -u +%Y%m%dT%H%M%SZ)"
        mkdir -p "${keepdir}" && cp "${work}"/*.log "${work}"/*.txt "${keepdir}/" 2>/dev/null && echo "logs kept: ${keepdir}"
    fi
    rm -rf "${work}"
    gh_lock_release
}
trap cleanup EXIT

out="${work}/out"
commit="$(git rev-parse HEAD 2>/dev/null || echo unknown)"
make -s -C native/kernel X86_CROSS="${cross}" OUT="${out}" AIENOS_COMMIT="${commit}" \
    CK_X86_REPORT=1 CK_TEST_SLOT_KEY=1 CK_X86_BENCH=1 CK_X86_BENCH_TEST=1 imac-kernel >/dev/null
make -s -C native/kernel OUT="${out}" ck-gpu-bench >/dev/null
TOOL="${out}/host/ck_gpu_bench"

img="${work}/esp.img"
truncate -s 64M "${img}"
mformat -i "${img}" -F ::
mmd -i "${img}" ::/EFI ::/EFI/BOOT
mcopy -i "${img}" "${out}/IMACKERN-LAB.EFI" ::/EFI/BOOT/BOOTX64.EFI

# The script. Register numbers are DWORD indexes: 18432 = 0x4800 (panel allowlist),
# 256 = 0x100 (not allowlisted), 4095 = 0x0FFF (the emulated all-ones register).
# Allowlist edges (dce_11_2_d.h, smu_7_1_3_d.h): 6660 = 0x1A04 GRPH_PRIMARY_SURFACE_ADDRESS and
# 6674 = 0x1A12 GRPH_FLIP_CONTROL (both ends of the DCP0 surface range), 148 = 0x94 SMC_MESSAGE_0,
# 149 = 0x95 SMC_RESP_0, 164 = 0xA4 SMC_MSG_ARG_0; refused neighbours 6659 = 0x1A03 GRPH_SWAP_CNTL,
# 6675 = 0x1A13, 150 = 0x96 SMC_MESSAGE_1, 165 = 0xA5 SMC_MSG_ARG_1.
# The two polls on 0x4800 (holding 0xABCD0001): the first matches the high half; the second waits
# for a low half of 0x0002 that never comes, so it must end in TIMEOUT.
cat > "${work}/test.bench" <<'BENCH'
ping
pci_cfg 0
bar5_read 16
bar5_write 18432 0xABCD0001
bar5_read 18432
bar5_write 256 1
bar0_write 5 0x42
bar0_read 5
bar5_poll 18432 0xFFFF0000 0xABCD0000 100
bar5_poll 18432 0x0000FFFF 0x00000002 50
bar5_rmw 18432 0x0000FF00 0x00005500
bar5_write 6660 0x11110001
bar5_write 6674 0x11110002
bar5_write 148 0x11110003
bar5_write 149 0x11110004
bar5_write 164 0x11110005
bar5_write 6659 1
bar5_write 6675 1
bar5_write 150 1
bar5_write 165 1
pci_cfg 256
replay
badauth ping
stale ping
ping
bar5_read 4095
ping
BENCH

sock="${work}/com1.sock"
serial_log="${work}/serial.log"
cp "${vars_fd}" "${work}/vars.fd"
qemu-system-x86_64 -M q35 -accel tcg -cpu max -smp 4 -m 1024 \
    -drive if=pflash,format=raw,readonly=on,file="${code_fd}" \
    -drive if=pflash,format=raw,file="${work}/vars.fd" \
    -drive if=none,id=esp,format=raw,file="${img}" -device ide-hd,drive=esp,bus=ide.0 \
    -device qemu-xhci -device usb-kbd \
    -nographic -monitor none -serial unix:"${sock}",server=on,wait=on -no-reboot >"${serial_log}" 2>&1 &
qpid=$!
for _ in $(seq 1 100); do [[ -S "${sock}" ]] && break; sleep 0.2; done

fails=0
pass() { echo "PASS  $1"; }
fail() { echo "FAIL  $1"; fails=$((fails + 1)); }
chk() { # name regex file
    if grep -aqE -- "$2" "$3"; then pass "$1"; else fail "$1 (no line matching: $2)"; fi
}

tool_rc=0
timeout "${AIENOS_BENCH_TOOL_TIMEOUT:-1500}" "${TOOL}" --serial "${sock}" \
    --transcript "${work}/transcript.txt" "${work}/test.bench" \
    >"${work}/tool.out" 2>"${work}/tool.err" || tool_rc=$?
kill "${qpid}" 2>/dev/null || true; wait "${qpid}" 2>/dev/null || true; qpid=""

tr -d '\r' < "${work}/tool.out" > "${work}/tool.txt"
echo "--- tool output ---"; cat "${work}/tool.txt"
echo "--- tool stderr ---"; cat "${work}/tool.err"

chk "kernel: bench console up (test build, emulated BARs)" "bench: GPU bench console ready" "${work}/transcript.txt"
chk "ping round trip" "^ping OK: result=0x00000000" "${work}/tool.txt"
chk "config read (dword 0 = 1002:67ef)" "^pci_cfg OK: result=0x67ef1002" "${work}/tool.txt"
chk "BAR5 read of a known word" "^bar5_read OK: result=0xc0ffee00" "${work}/tool.txt"
chk "BAR5 write on the allowlist (before 0, after ABCD0001)" "^bar5_write OK: result=0xabcd0001 before=0x00000000" "${work}/tool.txt"
chk "kernel logged the write before and after" "bench: bar5_write reg=0x4800 0x00000000 -> 0xabcd0001" "${work}/transcript.txt"
chk "BAR5 read back the written value" "^bar5_read OK: result=0xabcd0001" "${work}/tool.txt"
chk "BAR5 write off the allowlist refused (RANGE)" "^bar5_write ERR_RANGE: result=0x00000000" "${work}/tool.txt"
chk "GRPH primary surface address 0x1A04 writable" "^bar5_write OK: result=0x11110001 before=0x00000000" "${work}/tool.txt"
chk "GRPH flip control 0x1A12 writable (top of the range)" "^bar5_write OK: result=0x11110002 before=0x00000000" "${work}/tool.txt"
chk "SMC_MESSAGE_0 0x94 writable" "^bar5_write OK: result=0x11110003 before=0x00000000" "${work}/tool.txt"
chk "SMC_RESP_0 0x95 writable" "^bar5_write OK: result=0x11110004 before=0x00000000" "${work}/tool.txt"
chk "SMC_MSG_ARG_0 0xA4 writable" "^bar5_write OK: result=0x11110005 before=0x00000000" "${work}/tool.txt"
chk "kernel logged the GRPH write" "bench: bar5_write reg=0x1a04 0x00000000 -> 0x11110001" "${work}/transcript.txt"
ranges=$(grep -ac "^bar5_write ERR_RANGE" "${work}/tool.txt" || true)
if [[ "${ranges}" == "5" ]]; then pass "writes just outside each new range refused (0x1A03, 0x1A13, 0x96, 0xA5, plus 0x100): 5 RANGE"; else fail "bar5_write RANGE refusals: ${ranges} (expected 5)"; fi
chk "BAR0 write and read back" "^bar0_read OK: result=0x00000042" "${work}/tool.txt"
chk "TSC poll satisfied" "^bar5_poll OK: result=0xabcd0001" "${work}/tool.txt"
chk "TSC poll timed out" "^bar5_poll ERR_TIMEOUT" "${work}/tool.txt"
chk "read-modify-write" "^bar5_rmw OK: result=0xabcd5501 before=0xabcd0001" "${work}/tool.txt"
chk "config offset out of range refused" "^pci_cfg ERR_RANGE" "${work}/tool.txt"
chk "replay refused (same counter)" "^replay ERR_AUTH" "${work}/tool.txt"
chk "bad HMAC refused" "^badauth ping ERR_AUTH" "${work}/tool.txt"
chk "stale counter refused" "^stale ping ERR_AUTH" "${work}/tool.txt"
chk "all-ones read reported (device gone)" "^bar5_read ERR_ALLONES" "${work}/tool.txt"
if grep -aq "device gone (all-ones); stopping" "${work}/tool.err"; then pass "tool stopped at the all-ones reply"; else fail "tool stopped at the all-ones reply"; fi
pings=$(grep -ac "^ping " "${work}/tool.txt" || true)
if [[ "${pings}" == "2" ]]; then pass "the ping after the all-ones reply was never sent (2 ping replies, not 3)"; else fail "ping replies: ${pings} (expected 2)"; fi
reqs=$(grep -ac "^ABCHREQ " "${work}/transcript.txt" || true)
results=$(grep -acE " (OK|ERR_[A-Z]+): result=" "${work}/tool.txt" || true)
if [[ "${reqs}" == "${results}" && "${results}" -gt 0 ]]; then pass "one reply per request on the link (${results})"; else fail "requests ${reqs} vs results ${results}"; fi
if grep -aq "HMAC mismatch\|no response" "${work}/tool.err"; then fail "kernel and tool disagree on an HMAC or a reply"; else pass "every kernel reply carries the HMAC the tool expects"; fi
if [[ "${tool_rc}" == "1" ]]; then pass "tool exit 1 (the ALLONES stop)"; else fail "tool exit ${tool_rc} (expected 1)"; fi

[[ ${fails} -eq 0 ]] && echo "${tag}: PASS" && exit 0
echo "${tag}: FAIL (${fails} failures)"
exit 1
