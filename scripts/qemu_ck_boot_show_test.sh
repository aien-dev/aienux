#!/usr/bin/env bash
# qemu_ck_boot_show_test.sh -- the boot sequence (native/kernel/ui/boot_show.c:
# the night, two black holes circling and merging, the chirp) in the aarch64
# VM the aienos-run window shows: QEMU virt + AAVMF, the screen a
# virtio-gpu-pci and the speakers a virtio-sound-pci, both behind the smmuv3
# vIOMMU (their DMA confined like every device's), sound captured to a WAV.
#   usage: qemu_ck_boot_show_test.sh [aarch64|x86_64] [tcg|kvm]   (default aarch64 tcg; kvm: aarch64 on an
#   aarch64 host with /dev/kvm). x86_64: QEMU q35 + OVMF, the screen virtio-vga (OVMF gives its linear
#   framebuffer, the kernel draws there), the speakers virtio-sound-pci; that kernel has no IOMMU support
#   yet, so its sound DMA is not confined (it says so on its console), and check 1 is the GOP screen line.
# Checks:
#  1. the screen came up on virtio-gpu inside its confined DMA window
#  2. the night ran its full 15.2 s and the receipt line counts frames and
#     dropped 60 Hz slots (printed: the frame-rate receipt for this machine)
#  3. a screendump taken mid-night is not blank (stars are lit)
#  4. the sound went out on virtio-sound: started, fed, stopped, nothing refused
#  5. the WAV QEMU wrote holds the whole chirp, sample for sample the formula
#     the kernel played from (ui/chirp.c), checked by ui/tools/boot_show_check.c
# Not proven here (stated, not hidden): the speed of any real machine; TCG
# emulates the CPU, so its frame rate is QEMU's, not the hardware's.
# Ends with "AIENOS_CK_BOOT_SHOW: PASS|FAIL|NOT_RUN".
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"
arch="${1:-aarch64}"
accel="${2:-tcg}"
case "${arch}" in tcg | kvm) accel="${arch}"; arch=aarch64 ;; esac # the one-argument form: aarch64
command -v socat >/dev/null || { echo "socat not installed (screendump)"; echo "AIENOS_CK_BOOT_SHOW: NOT_RUN"; exit 2; }
commit="$(git rev-parse HEAD 2>/dev/null || echo unknown)"
dev="disable-legacy=on"
case "${arch}" in
aarch64)
    code_fd="${AAVMF_CODE:-/usr/share/AAVMF/AAVMF_CODE.no-secboot.fd}"
    vars_fd="${AAVMF_VARS:-/usr/share/AAVMF/AAVMF_VARS.fd}"
    qemu=qemu-system-aarch64
    case "${accel}" in
    tcg) accel_args=(-accel tcg -cpu max); machine="virt,virtualization=on,gic-version=3,iommu=smmuv3" ;;
    kvm)
        [[ -w /dev/kvm && "$(uname -m)" == aarch64 ]] || { echo "NOT_RUN  no usable /dev/kvm on an aarch64 host"; echo "AIENOS_CK_BOOT_SHOW: NOT_RUN"; exit 2; }
        accel_args=(-accel kvm -cpu host); machine="virt,gic-version=3,iommu=smmuv3" ;;
    *) echo "usage: $0 [aarch64|x86_64] [tcg|kvm]"; exit 2 ;;
    esac
    dev="disable-legacy=on,iommu_platform=on" # behind the smmuv3: DMA confined
    out="${repo_root}/target/native-kernel"
    cross=""
    if [ "$(uname -m)" != "aarch64" ]; then cross="aarch64-linux-gnu-"; fi
    make -s -C native/kernel CROSS="${cross}" OUT="${out}" AIENOS_COMMIT="${commit}" full CK_BOOT_SHOW=1 >/dev/null
    efi="${out}/full-boot-show/BOOTAA64.EFI"
    screen_line='^virtio_gpu: screen '
    ;;
x86_64)
    code_fd="${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}"
    vars_fd="${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}"
    qemu=qemu-system-x86_64
    [[ "${accel}" == tcg ]] || { echo "NOT_RUN  x86_64 runs under tcg on this host"; echo "AIENOS_CK_BOOT_SHOW: NOT_RUN"; exit 2; }
    accel_args=(-accel tcg -cpu max); machine=q35
    out="${repo_root}/target/native-kernel-imac-kernel"
    cross=""
    if [ "$(uname -m)" != "x86_64" ]; then cross="x86_64-linux-gnu-"; fi
    make -s -C native/kernel X86_CROSS="${cross}" OUT="${out}" AIENOS_COMMIT="${commit}" imac-kernel CK_BOOT_SHOW=1 >/dev/null
    efi="${out}/IMACKERN-BOOT-SHOW.EFI"
    screen_line='^screen: gop '
    ;;
*) echo "usage: $0 [aarch64|x86_64] [tcg|kvm]"; exit 2 ;;
esac
command -v "${qemu}" >/dev/null || { echo "${qemu} not installed"; echo "AIENOS_CK_BOOT_SHOW: NOT_RUN"; exit 2; }
[[ -r "${code_fd}" && -r "${vars_fd}" ]] || { echo "UEFI firmware not found"; echo "AIENOS_CK_BOOT_SHOW: NOT_RUN"; exit 2; }
make -s -C native/kernel OUT="${out}" gpt-image >/dev/null
check="${out}/host/boot_show_check"
mkdir -p "${out}/host"
cc -std=gnu11 -O2 -Wall -Wextra -Werror -o "${check}" native/kernel/ui/tools/boot_show_check.c native/kernel/ui/chirp.c -lm

. "${repo_root}/scripts/lib_gate_hold.sh"
if ! gh_quiet_check || ! gh_lock_take "qemu_ck_boot_show_test" "${AIENOS_GATE_MINUTES:-30}"; then
    echo "NOT_RUN  ${gh_why}"
    echo "AIENOS_CK_BOOT_SHOW: NOT_RUN"
    exit 3
fi
top="$(mktemp -d)"
qpid=""
cleanup() {
    if [[ -n "${qpid}" ]]; then kill "${qpid}" 2>/dev/null || true; wait "${qpid}" 2>/dev/null || true; fi
    rm -rf "${top}"
    gh_lock_release
}
trap cleanup EXIT

keep="${repo_root}/target/boot-show/${arch}-${accel}"
mkdir -p "${keep}"
image="${top}/nvme.img"
"${out}/host/ck_gpt_image" create "${image}" 512 64 aienos-middle >/dev/null
mkdir -p "${top}/esp/EFI/BOOT" "${top}/esp/EFI/AIENOS"
touch "${top}/esp/EFI/AIENOS/BOOTREPORT.TXT"
if [[ "${arch}" == aarch64 ]]; then cp "${efi}" "${top}/esp/EFI/BOOT/BOOTAA64.EFI"; else cp "${efi}" "${top}/esp/EFI/BOOT/BOOTX64.EFI"; fi
cp "${vars_fd}" "${top}/vars.fd"
log="${top}/serial.log"
if [[ "${arch}" == aarch64 ]]; then
    disks=(-drive if=none,id=esp,format=raw,file=fat:rw:"${top}/esp" -device virtio-blk-pci,drive=esp
        -drive if=none,id=nvme0,format=raw,file="${image}" -device nvme,drive=nvme0,serial=aienos-boot-show)
    screen=(-device "virtio-gpu-pci,${dev}")
    mem=2048
else
    disks=(-drive if=none,id=esp,format=raw,file=fat:rw:"${top}/esp" -device ide-hd,drive=esp,bus=ide.0)
    screen=(-device virtio-vga)
    mem=1024
fi
"${qemu}" -M "${machine}" "${accel_args[@]}" -smp 4 -m "${mem}" \
    -drive if=pflash,format=raw,readonly=on,file="${code_fd}" \
    -drive if=pflash,format=raw,file="${top}/vars.fd" \
    "${disks[@]}" "${screen[@]}" \
    -audiodev "wav,id=snd0,path=${top}/chirp.wav,out.frequency=48000,out.channels=2,out.format=s16" \
    -device "virtio-sound-pci,audiodev=snd0,${dev}" \
    -display none -nic none -serial "file:${log}" -qmp "unix:${top}/qmp.sock,server,nowait" -no-reboot &
qpid=$!

qmp() { printf '{"execute":"qmp_capabilities"}\n%s\n' "$1" | socat - "UNIX-CONNECT:${top}/qmp.sock" >/dev/null 2>&1 || true; }
wait_for() { # pattern, seconds
    local end=$((SECONDS + $2))
    while ((SECONDS < end)); do
        grep -qE "$1" "${log}" 2>/dev/null && return 0
        kill -0 "${qpid}" 2>/dev/null || return 1
        sleep 0.2
    done
    return 1
}
fail=0
ok() { echo "PASS  $*"; }
bad() { echo "FAIL  $*"; fail=1; }

if wait_for "${screen_line}" "${AIENOS_QEMU_TIMEOUT:-300}"; then
    ok "$(grep -m1 "${screen_line}" "${log}")"
    if [[ "${arch}" == aarch64 ]]; then
        grep -m1 '^dma_gate: virtio_gpu granted' "${log}" >/dev/null && ok "$(grep -m1 '^dma_gate: virtio_gpu granted' "${log}")" ||
            bad "virtio_gpu DMA not confined"
    fi
    wait_for '^boot-show: the night starts' 120 || bad "the night never started"
    sleep 8 # 8 s into the night: both holes are circling and the stars are out
    qmp "{\"execute\":\"screendump\",\"arguments\":{\"filename\":\"${top}/night.ppm\"}}"
else
    bad "no screen line (${screen_line})"
    grep -E '^(virtio_gpu|dma_gate|boot-show|stage)' "${log}" | head -20 || true
fi
if wait_for '^boot-show: [0-9]+x[0-9]+ via' 120; then
    line="$(grep -m1 -E '^boot-show: [0-9]+x[0-9]+ via' "${log}")"
    ok "${line}"
    [[ "${line}" =~ "in 15200 ms" && "${line}" =~ "of 912 slots" ]] || bad "the night did not run its 15.2 s"
else
    bad "no boot-show receipt line"
    grep -E '^(boot-show|virtio|sound)' "${log}" | head -20 || true
fi
if wait_for '^(sound: |virtio_snd: )' 30 && grep -q '^sound: virtio played the chirp' "${log}"; then
    sl="$(grep -m1 '^sound: virtio played' "${log}")"
    ok "${sl}"
    [[ "${sl}" =~ ", 0 refused, stop ok" ]] || bad "sound stream refused or not stopped cleanly"
else
    bad "no virtio sound played line"
    grep -E '^(sound|virtio_snd|dma_gate: virtio_snd)' "${log}" | head -10 || true
fi
qmp '{"execute":"quit"}'
wait "${qpid}" 2>/dev/null || true
qpid=""
cp "${log}" "${keep}/serial.log"
[[ -f "${top}/chirp.wav" ]] && cp "${top}/chirp.wav" "${keep}/chirp.wav"
if [[ -f "${top}/night.ppm" ]]; then
    cp "${top}/night.ppm" "${keep}/night.ppm"
    if r="$("${check}" ppm "${top}/night.ppm")"; then ok "${r} (screendump 8 s into the night)"; else bad "${r}"; fi
else
    bad "no screendump"
fi
if r="$("${check}" wav "${top}/chirp.wav" 2>&1)"; then ok "${r}"; else bad "${r}"; fi
echo "kept: ${keep} (serial.log, night.ppm, chirp.wav)"
if ((fail)); then echo "AIENOS_CK_BOOT_SHOW: FAIL"; exit 1; fi
echo "AIENOS_CK_BOOT_SHOW: PASS"
