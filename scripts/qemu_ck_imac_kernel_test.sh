#!/usr/bin/env bash
# qemu_ck_imac_kernel_test.sh -- QEMU test for IMACKERN.EFI (AIENOS-KERNEL-0,
# native/kernel/x86). QEMU is an emulator: a PASS here qualifies nothing about a
# 2017 iMac or any other real x86_64 machine.
#
# Builds IMACKERN.EFI, puts it on a FAT image as EFI/BOOT/BOOTX64.EFI attached as a
# SATA disk, and boots it twice in QEMU q35 with OVMF: "-cpu max" and "-cpu qemu64"
# (two CPU models; the kernel drives the local APIC in xAPIC mode on both). Each run
# must leave boot services, switch to the kernel's own page tables, prove the guard
# page, measure the TSC against the HPET, take 20 local APIC timer interrupts, print
# start the three other cores (-smp 4) drawing the night sky of the startup, find both SATA disks behind
# the q35 AHCI controller and an NVMe disk with its own GPT and read them without
# writing (the ESP, and a data disk with a GPT whose AIENOS partition the kernel
# must locate where ck_gpt_image wrote it; the data disk must be byte-identical
# afterwards), print "kernel: alive", hold the screen and power QEMU off through
# ACPI S5 by itself.
# The kernel's report reaches the serial log through COM1, which QEMU has and the
# iMac does not. Needs qemu-system-x86_64, OVMF, mtools and an x86_64 gcc. Final line:
#     AIENOS_IMAC_KERNEL: PASS|FAIL|NOT_RUN [(...)]      exit 0 / 1 / 2 (tools) / 3 (NOT_RUN)
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"
tag="AIENOS_IMAC_KERNEL"
# Opt-in firmware-inventory coverage. This device advertises a DMAR table;
# the kernel only reads it and never enables or configures DMA remapping.
iommu=()
case "${AIENOS_IMAC_DMAR:-0}" in
    0) ;;
    1) iommu=(-device intel-iommu,intremap=on,caching-mode=on) ;;
    *) echo "${tag}: FAIL (AIENOS_IMAC_DMAR must be 0 or 1)"; exit 1 ;;
esac
code_fd="${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}"
vars_fd="${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}"
cross=""
if [ "$(uname -m)" != "x86_64" ]; then cross="x86_64-linux-gnu-"; fi
for t in qemu-system-x86_64 mformat mmd mcopy "${cross}gcc" "${cross}ld"; do
    command -v "${t}" >/dev/null || { echo "${tag}: NOT_RUN (${t} missing)"; exit 2; }
done
[[ -r "${code_fd}" && -r "${vars_fd}" ]] || { echo "${tag}: NOT_RUN (no OVMF)"; exit 2; }
out="${IMACKERN_OUT:-${repo_root}/target/native-kernel-imac-kernel}"
commit="$(git rev-parse HEAD 2>/dev/null || echo unknown)"
make -s -C native/kernel X86_CROSS="${cross}" OUT="${out}" AIENOS_COMMIT="${commit}" imac-kernel >/dev/null
make -s -C native/kernel OUT="${out}" gpt-image >/dev/null

. "${repo_root}/scripts/lib_gate_hold.sh"
if ! gh_quiet_check || ! gh_lock_take qemu_ck_imac_kernel_test "${AIENOS_GATE_MINUTES:-60}"; then
    echo "${tag}: NOT_RUN (${gh_why})"
    exit 3
fi
work="$(mktemp -d)"
qpid=""
cleanup() {
    [[ -z "${qpid}" ]] || kill "${qpid}" 2>/dev/null || true
    rm -rf "${work}"
    gh_lock_release
}
trap cleanup EXIT

img="${work}/esp.img"
truncate -s 64M "${img}"
mformat -i "${img}" -F ::
mmd -i "${img}" ::/EFI ::/EFI/BOOT
mcopy -i "${img}" "${out}/IMACKERN.EFI" ::/EFI/BOOT/BOOTX64.EFI
data="${work}/data.img"
gpt_line="$("${out}/host/ck_gpt_image" create "${data}" 512 64 aienos-middle)"
read -r a_first a_last < <(sed -E "s/.*aienos_first_lba=([0-9]+) aienos_last_lba=([0-9]+).*/\1 \2/" <<<"${gpt_line}")
data_sum="$(sha256sum "${data}" | cut -d" " -f1)"
nvm="${work}/nvme.img"
nvm_line="$("${out}/host/ck_gpt_image" create "${nvm}" 512 32 aienos-middle)"
read -r n_first n_last < <(sed -E "s/.*aienos_first_lba=([0-9]+) aienos_last_lba=([0-9]+).*/\1 \2/" <<<"${nvm_line}")
nvm_sum="$(sha256sum "${nvm}" | cut -d" " -f1)"

fails=0
chk() { # description, file, pattern
    if grep -aqE -- "$3" "$2"; then echo "PASS  $1"; else echo "FAIL  $1 (no match for: $3)"; fails=$((fails + 1)); fi
}
run() { # name, -cpu model, expected APIC mode
    local name="$1" cpu="$2" mode="$3" log="${work}/$1.log" txt="${work}/$1.txt"
    cp "${vars_fd}" "${work}/vars.fd"
    # AIENOS_IMAC_KERNEL_KEEP_DIR: keep each run's serial log (.txt) and a recording of its sound (.wav) there
    local audio="none,id=snd0" keep="${AIENOS_IMAC_KERNEL_KEEP_DIR:-}"
    [[ -z "${keep}" ]] || audio="wav,id=snd0,path=${keep}/${name}.wav,out.frequency=48000,out.channels=2,out.format=s16"
    qemu-system-x86_64 -M q35 -accel tcg -cpu "${cpu}" -smp 4 -m 1024 \
        "${iommu[@]}" \
        -drive if=pflash,format=raw,readonly=on,file="${code_fd}" \
        -drive if=pflash,format=raw,file="${work}/vars.fd" \
        -drive if=none,id=esp,format=raw,file="${img}" -device ide-hd,drive=esp,bus=ide.0 \
        -drive if=none,id=data,format=raw,file="${data}" -device ide-hd,drive=data,bus=ide.1 \
        -drive if=none,id=nvm,format=raw,file="${nvm}" -device nvme,drive=nvm,serial=AIENOSQEMU \
        -audiodev "${audio}" -device ich9-intel-hda -device hda-duplex,audiodev=snd0 \
        -display none -nic none -serial file:"${log}" -no-reboot &
    qpid=$!
    local self_exit=0
    for _ in $(seq 1 "${AIENOS_IMAC_KERNEL_WAIT:-300}"); do
        if ! kill -0 "${qpid}" 2>/dev/null; then self_exit=1; break; fi
        sleep 1
    done
    kill "${qpid}" 2>/dev/null || true
    wait "${qpid}" 2>/dev/null || true
    qpid=""
    tr -d '\r' <"${log}" >"${txt}" 2>/dev/null || : >"${txt}"
    [[ -z "${keep}" ]] || cp "${txt}" "${keep}/${name}.txt"
    echo "== run ${name} (-cpu ${cpu}) =="
    chk "${name}: title" "${txt}" '^AIENOS-KERNEL-0$'
    chk "${name}: commit" "${txt}" "^aienos_commit: ${commit}\$"
    chk "${name}: GOP found" "${txt}" '^gop: ok '
    chk "${name}: RSDP" "${txt}" '^acpi_rsdp: 0x[0-9a-f]*[1-9a-f][0-9a-f]*$'
    if [[ "${AIENOS_IMAC_DMAR:-0}" == 1 ]]; then
        chk "${name}: DMAR firmware inventory" "${txt}" '^dmar: inventory bytes=[0-9]+ address_bits=[0-9]+ flags=[0-9]+ units=[1-9][0-9]* regions=[0-9]+ scopes=[1-9][0-9]* other=[0-9]+$'
        chk "${name}: DMAR remapping unit" "${txt}" '^dmar: unit=0 segment=0 base=0x[0-9a-f]+ register_bytes=[0-9]+ include_all=[01]$'
        chk "${name}: DMAR IOAPIC path" "${txt}" '^dmar: scope=[0-9]+ owner=unit:0 segment=0 type=3 enum=[0-9]+ start_bus=[0-9]+ path=[0-9a-f.]+$'
        chk "${name}: inventory grants no protection" "${txt}" '^dmar: inventory-only register_access=none; DMA confinement NOT QUALIFIED$'
    else
        chk "${name}: absent DMAR grants no protection" "${txt}" '^dmar: absent-or-invalid; DMA confinement NOT QUALIFIED$'
    fi
    chk "${name}: PCI segment and requester reported" "${txt}" '^pci: fn .* segment=0 requester=[0-9a-f]{4}$'
    chk "${name}: left boot services" "${txt}" '^exit_boot_services: ok attempts=[1-8]$'
    chk "${name}: descriptor tables" "${txt}" '^descriptors: gdt=7 tss=yes ist1=df,nmi,mc idt=256$'
    chk "${name}: handoff checked" "${txt}" '^handoff: CHANDOF3 ok$'
    chk "${name}: screen on the framebuffer" "${txt}" '^screen: gop [0-9]+x[0-9]+ .* map=wc scale=[1-4] grid=[0-9]+x[0-9]+$'
    chk "${name}: own page tables" "${txt}" '^paging: root=0x[0-9a-f]+ firmware_cr3=0x[0-9a-f]+ switched=yes top_gib=[0-9]+ pages_2m_wb=[1-9][0-9]* pages_2m_wc=[1-9][0-9]* pages_2m_uc=[1-9][0-9]* pt_frames=[0-9]+ fb=wc pat1=wc$'
    chk "${name}: heap" "${txt}" '^memory: frame_ranges=[1-9][0-9]* free_bytes=[1-9][0-9]* .* heap_test=ok$'
    chk "${name}: guard page fault contained" "${txt}" '^guard_page: ok fault=contained \(#PF cr2=0x[0-9a-f]+ error=0x[0-9a-f]+, not present\)$'
    chk "${name}: TSC timed by the HPET" "${txt}" '^timebase: hpet period_fs=[1-9][0-9]* tsc_hz=[1-9][0-9]*$'
    chk "${name}: APIC mode ${mode}" "${txt}" "^apic: mode=${mode} id=0 "
    chk "${name}: MADT four CPUs" "${txt}" '^madt: cpus=4 ioapics=1 ioapic_lines_masked=[1-9][0-9]*$'
    chk "${name}: 20 timer interrupts" "${txt}" '^timer_irq: 20 ticks in [0-9]+ ms$'
    chk "${name}: AHCI controller found" "${txt}" "^pci: ahci [0-9a-f:.]+ id=8086:2922 "
    chk "${name}: AHCI polled" "${txt}" "^ahci: ctrl=0 abar=0x[0-9a-f]+ version=1\\.[0-9]+ ports=[0-9]+ implemented=0x[0-9a-f]+ s64a=[01] polled=yes\$"
    chk "${name}: ESP disk identified" "${txt}" "^ahci_disk: ctrl=0 disk=0 port=0 link=gen[0-9] model=\"QEMU HARDDISK\" blocks=131072 block=512 mib=64 access=read-only\$"
    chk "${name}: ESP sector 0 read" "${txt}" "^ahci_read: ctrl=0 disk=0 lba=0 bytes=512 boot_sig=55aa "
    chk "${name}: ESP has no GPT" "${txt}" "^ahci_gpt: ctrl=0 disk=0 refused: no protective MBR\$"
    chk "${name}: data disk identified" "${txt}" "^ahci_disk: ctrl=0 disk=1 port=1 link=gen[0-9] model=\"QEMU HARDDISK\" blocks=131072 block=512 mib=64 access=read-only\$"
    chk "${name}: data disk AIENOS partition where written" "${txt}" "^ahci_gpt: ctrl=0 disk=1 gpt=valid used=3 aienos first_lba=${a_first} last_lba=${a_last}\$"
    chk "${name}: three secondary cores started, drawing the night" "${txt}" "^smp: cpus=4 started=3 failed=0 bsp_id=0 ap_ids=[0-9]+,[0-9]+,[0-9]+ trampoline=0x[0-9a-f]+ state=drawing helpers=[0-2]\$"
    chk "${name}: NVMe controller found" "${txt}" "^pci: nvme [0-9a-f:.]+ id=1b36:0010 "
    chk "${name}: NVMe disk identified" "${txt}" "^nvme_disk: ctrl=0 disk=0 version=1\\.[0-9]+ nsid=1 blocks=65536 block=512 mib=32 access=read-only\$"
    chk "${name}: NVMe AIENOS partition where written" "${txt}" "^nvme_gpt: ctrl=0 disk=0 gpt=valid used=3 aienos first_lba=${n_first} last_lba=${n_last}\$"
    chk "${name}: the night began at boot" "${txt}" "^screen: night: the sky made in [0-9]+ ms \([0-9]+ MiB drawing memory\); stars, black holes, then Allen\$"
    chk "${name}: sound controller found" "${txt}" "^pci: sound [0-9a-f:.]+ id=8086:293e class=0403[0-9a-f]{2} "
    chk "${name}: sound controller looked at" "${txt}" "^sound: ctrl=0 version=1\\.0 streams out=[1-9][0-9]* "
    chk "${name}: codec mapped through the command rings" "${txt}" "^sound: codec=0 id=1af4:[0-9a-f]{4} sub=[0-9a-f]{4}:[0-9a-f]{4} rev=0x[0-9a-f]{8} afg=1 power=D0 widgets=0x02\\.\\.0x05 "
    chk "${name}: output path opened" "${txt}" "^sound: ctrl=0 opened 1 output pin\\(s\\): codec 0 pins 0x03\\(line\\)<-0x02\$"
    chk "${name}: chirp stream running" "${txt}" "^sound: ctrl=0 stream 4 playing the chirp from night [0-9]+ ms .* position after 2 ms [1-9][0-9]* bytes, status 0x[0-9a-f]{2}\$"
    chk "${name}: chirp stream stopped after the chirp" "${txt}" "^sound: chirp over; stream stopped at night [0-9]+ ms "
    chk "${name}: microphone path opened" "${txt}" "^sound: codec=0 microphone: pin 0x05 \\(line in\\) -> converter 0x04\$"
    chk "${name}: microphone stream recording" "${txt}" "^sound: ctrl=0 stream 0 recording 1000 ms from the microphone \\(pin 0x05 -> converter 0x04, format 0x0011, 192000 bytes\\); position after 2 ms [0-9]+ bytes, status 0x[0-9a-f]{2}\$"
    chk "${name}: microphone recording summed up" "${txt}" "^sound: microphone recorded 1000 ms: 96000 samples, [0-9]+% nonzero \\(first at [0-9]+\\), peak [0-9]+, rms [0-9]+, dc -?[0-9]+ \\(16-bit scale\\), position [0-9]+ bytes, status 0x[0-9a-f]{2}\$"
    chk "${name}: no Ethernet in this VM" "${txt}" "^net: no Ethernet controller\$"
    chk "${name}: storage summary" "${txt}" "^storage: pci_functions=[1-9][0-9]* ahci_controllers=1 disks=2 nvme_controllers=1 nvme_disks=1 gpt_valid=2 writes=none\$"
    chk "${name}: one-line summary" "${txt}" "^summary: cores=4/4 timer=ok sata_disks=2 nvme_disks=1 gpt_valid=2 writes=none net=none\$"
    [[ "$(sha256sum "${nvm}" | cut -d" " -f1)" == "${nvm_sum}" ]] && echo "PASS  ${name}: NVMe disk unchanged" || { echo "FAIL  ${name}: NVMe disk changed"; fails=$((fails + 1)); }
    [[ "$(sha256sum "${data}" | cut -d" " -f1)" == "${data_sum}" ]] && echo "PASS  ${name}: data disk unchanged" || { echo "FAIL  ${name}: data disk changed"; fails=$((fails + 1)); }
    chk "${name}: W^X probes (#409)" "${txt}" '^wx: ok nx=on wp=on text=rx ro=r data=rw-nx text_write=fault ro_write=fault heap_exec=fault stack_exec=fault$'
    chk "${name}: W^X audit, only code executable" "${txt}" '^wx_audit: ok wx_leaves=0 exec_pages=([1-9][0-9]*) text_pages=\1 pages_4k=[1-9][0-9]* trampoline=retired$'
    chk "${name}: kernel alive" "${txt}" '^kernel: alive$'
    chk "${name}: DONE" "${txt}" '^AIENOS-KERNEL-0 DONE$'
    chk "${name}: ACPI S5 power off" "${txt}" '^power: off via ACPI S5 pm1a=0x[0-9a-f]+ '
    if grep -aqE '^(panic|power: still running|power: halted)' "${txt}"; then
        echo "FAIL  ${name}: panic or failed power off"; fails=$((fails + 1))
    else
        echo "PASS  ${name}: no panic"
    fi
    # The default image carries no network report (lab-only, CK_X86_REPORT=1).
    if grep -aqE '^report: ' "${txt}"; then
        echo "FAIL  ${name}: default image ran the lab-only network report"; fails=$((fails + 1))
    else
        echo "PASS  ${name}: no network report in the default image"
    fi
    [[ ${self_exit} == 1 ]] && echo "PASS  ${name}: QEMU powered off by itself" || { echo "FAIL  ${name}: QEMU still running"; fails=$((fails + 1)); }
    echo "---- ${name} serial ----"; cat "${txt}"; echo "----"
}
run max max xapic
run qemu64 qemu64 xapic
# Negative controls (#409): builds with W^X deliberately broken must make the kernel's own
# probes panic. The panic comes before the clock is measured, so the kernel halts instead
# of powering off: wait for the line, then stop QEMU.
mut() { # mode, expected panic pattern
    local m="$1" mo="${work}/mut$1" mi="${work}/mut$1.img" log="${work}/mut$1.log" hit=0
    make -s -C native/kernel X86_CROSS="${cross}" OUT="${mo}" AIENOS_COMMIT="${commit}" CK_X86_WX_MUTATE="${m}" imac-kernel >/dev/null
    truncate -s 64M "${mi}"
    mformat -i "${mi}" -F ::
    mmd -i "${mi}" ::/EFI ::/EFI/BOOT
    mcopy -i "${mi}" "${mo}/IMACKERN.EFI" ::/EFI/BOOT/BOOTX64.EFI
    cp "${vars_fd}" "${work}/vars.fd"
    qemu-system-x86_64 -M q35 -accel tcg -cpu max -smp 1 -m 1024 \
        -drive if=pflash,format=raw,readonly=on,file="${code_fd}" \
        -drive if=pflash,format=raw,file="${work}/vars.fd" \
        -drive if=none,id=esp,format=raw,file="${mi}" -device ide-hd,drive=esp,bus=ide.0 \
        -display none -nic none -serial file:"${log}" -no-reboot &
    qpid=$!
    for _ in $(seq 1 120); do
        if tr -d '\r' <"${log}" 2>/dev/null | grep -aqE -- "$2"; then hit=1; break; fi
        kill -0 "${qpid}" 2>/dev/null || break
        sleep 1
    done
    kill "${qpid}" 2>/dev/null || true
    wait "${qpid}" 2>/dev/null || true
    qpid=""
    if [[ ${hit} == 1 ]] && tr -d '\r' <"${log}" | grep -aq '^wx: MUTATED TEST BUILD'; then
        echo "PASS  mutation ${m}: broken W^X caught by the kernel's probes"
    else
        echo "FAIL  mutation ${m}: no '$2' panic"; fails=$((fails + 1))
        tr -d '\r' <"${log}" | tail -20
    fi
}
mut 1 '^panic: wx: FAIL .*heap_exec=ALLOWED stack_exec=ALLOWED$'
mut 2 '^panic: wx: FAIL .*text_write=ALLOWED '
if [[ ${fails} == 0 ]]; then echo "${tag}: PASS"; exit 0; fi
echo "${tag}: FAIL (${fails} checks)"; exit 1
