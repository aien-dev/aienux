#!/usr/bin/env bash
# qemu_ck_imac_bootdisk_test.sh -- QEMU test of the boot section: the network
# loader's verified kernel installs itself on the computer's own hard disk
# (native/kernel/x86/memory_x86.c boot_section, dev/fat_boot.c, dev/gpt_add.c,
# boot/net_loader.c struct nb_pass), and the firmware then starts it from that
# disk with no stick and no network. QEMU is an emulator: a PASS here
# qualifies nothing about any real machine or its firmware.
#
# One q35 + OVMF machine; a fresh firmware variable store for every start (no
# boot entries remembered). The hard disk is ck_gpt_image "one-old" (one
# partition full of a pattern, the rest unused) under the approved model
# "APPLE HDD ST1000DM003".
#   start 1  stick (NETLDR.EFI) + network: the server offers the signed
#            default kernel; it adds Allen's section and the boot section
#            (EFI System Partition, 256 MiB) in unused space, installs itself
#            as \EFI\BOOT\BOOTX64.EFI (slot A), reports boot_time; goodbye selects ACPI S5.
#            Host: fsck.fat finds nothing to repair, mtools reads BOOTX64.EFI
#            equal to the kernel image, the old partition and the stick are
#            byte-identical.
#   start 2  the hard disk alone, no network: the firmware starts the kernel
#            from it; boot_time says no loader; Allen's memory loads (start 2).
#   start 3  stick + network, a second kernel (another commit stamp): it goes
#            to slot B.
#   start 4  the hard disk alone: the second kernel is the one that runs.
# Final line: AIENOS_IMAC_BOOTDISK: PASS|FAIL|NOT_RUN  exit 0 / 1 / 2 (tools) / 3
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"
tag="AIENOS_IMAC_BOOTDISK"
code_fd="${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}"
vars_fd="${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}"
cross=""
if [ "$(uname -m)" != "x86_64" ]; then cross="x86_64-linux-gnu-"; fi
for t in qemu-system-x86_64 mformat mmd mcopy mtype cmp sha256sum "${cross}gcc" "${cross}ld"; do
    command -v "${t}" >/dev/null || { echo "${tag}: NOT_RUN (${t} missing)"; exit 2; }
done
fsck_fat="$(command -v fsck.fat || echo /usr/sbin/fsck.fat)"
[[ -x "${fsck_fat}" ]] || { echo "${tag}: NOT_RUN (fsck.fat missing)"; exit 2; }
[[ -r "${code_fd}" && -r "${vars_fd}" ]] || { echo "${tag}: NOT_RUN (no OVMF)"; exit 2; }

. "${repo_root}/scripts/lib_gate_hold.sh"
if ! gh_quiet_check || ! gh_lock_take qemu_ck_imac_bootdisk_test "${AIENOS_GATE_MINUTES:-30}"; then
    echo "${tag}: NOT_RUN (${gh_why})"
    exit 3
fi
work="$(mktemp -d)"
pids=()
cleanup() {
    local p
    for p in "${pids[@]}"; do kill "${p}" 2>/dev/null || true; done
    rm -rf "${work}"
    gh_lock_release
}
trap cleanup EXIT

out="${work}/out"
out2="${work}/out2"
commit="$(git rev-parse HEAD 2>/dev/null || echo unknown)"
second="0000000000000000000000000000000000000b0b"
mk() { make -s -C native/kernel X86_CROSS="${cross}" OUT="${out}" AIENOS_COMMIT="${commit}" "$@" >/dev/null; }
mk slot-tool imac-netboot-tool gpt-image
nb="${out}/host/ck_imac_netboot"
gpt="${out}/host/ck_gpt_image"
"${nb}" keygen "${work}/test.key" "${work}/test.pub" >/dev/null
mk CK_NETLDR_KEYS="${work}/test.pub" CK_NETLDR_FALLBACK_S=8 imac-netldr
mk imac-kernel
make -s -C native/kernel X86_CROSS="${cross}" OUT="${out2}" AIENOS_COMMIT="${second}" imac-kernel >/dev/null
push() { NETBOOT_TOOL_OUT="${out}" bash scripts/ck_imac_netboot_push.sh "$1" "${work}/test.key" "${work}/spool" >/dev/null; }

stick="${work}/stick.img"
truncate -s 64M "${stick}"
mformat -i "${stick}" -F ::
mmd -i "${stick}" ::/EFI ::/EFI/BOOT
mcopy -i "${stick}" "${out}/NETLDR.EFI" ::/EFI/BOOT/BOOTX64.EFI
"${gpt}" create "${work}/hdd.img" 512 2048 one-old >/dev/null
cp --sparse=always "${work}/hdd.img" "${work}/hdd.before"
stick_sum="$(sha256sum <"${stick}")"

fails=0
pass() { echo "PASS  $1"; }
fail() { echo "FAIL  $1"; fails=$((fails + 1)); }
chk() { # description, file, pattern
    if grep -aqE -- "$3" "$2"; then pass "$1"; else fail "$1 (no match for: $3)"; fi
}
nochk() {
    if grep -aqE -- "$3" "$2"; then fail "$1 (unexpected: $3)"; else pass "$1"; fi
}
txt() { tr -d '\r' <"${work}/$1.log" 2>/dev/null || true; }
has() { txt "$1" | grep -aqE -- "$2"; }
wait_for() { # name seconds pattern
    local i
    for ((i = 0; i < $2 * 5; i++)); do
        has "$1" "$3" && return 0
        sleep 0.2
    done
    return 1
}
lp=$((40000 + RANDOM % 10000))
rp=$((lp + 1))
start() { # name with-stick(0/1)
    local name="$1" net=()
    cp "${vars_fd}" "${work}/${name}.vars"
    mkfifo "${work}/${name}.mon.in" "${work}/${name}.mon.out"
    : >"${work}/${name}.log"
    local disks=(-drive if=none,id=hdd,format=raw,file="${work}/hdd.img"
        -device ide-hd,drive=hdd,bus=ide.1,model="APPLE HDD ST1000DM003",bootindex=2)
    if [[ "$2" == 1 ]]; then
        disks+=(-drive if=none,id=stick,format=raw,file="${stick}" -device ide-hd,drive=stick,bus=ide.0,bootindex=1)
        net=(-netdev dgram,id=n0,local.type=inet,local.host=127.0.0.1,local.port="${lp}",remote.type=inet,remote.host=127.0.0.1,remote.port="${rp}"
            -device virtio-net-pci,netdev=n0,mac=52:54:00:12:34:56)
    else
        net=(-nic none)
    fi
    qemu-system-x86_64 -M q35 -accel tcg -cpu max -smp 2 -m 1024 \
        -drive if=pflash,format=raw,readonly=on,file="${code_fd}" \
        -drive if=pflash,format=raw,file="${work}/${name}.vars" \
        "${disks[@]}" "${net[@]}" \
        -device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0,port=1 \
        -chardev pipe,id=mon,path="${work}/${name}.mon" -mon chardev=mon,mode=readline \
        -display none -serial file:"${work}/${name}.log" -no-reboot 2>"${work}/${name}.err" &
    pids+=($!)
    eval "pid_${name}=$!"
    cat "${work}/${name}.mon.out" >/dev/null &
    pids+=($!)
}
running() { local v="pid_$1"; kill -0 "${!v}" 2>/dev/null; }
stop() { local v="pid_$1"; kill "${!v}" 2>/dev/null || true; }
say() { # name "lower-case words": typed through the QEMU monitor, then Enter
    local name="$1" text="$2" c k i
    exec 3>"${work}/${name}.mon.in"
    for ((i = 0; i < ${#text}; i++)); do
        c="${text:i:1}"
        case "${c}" in " ") k=spc ;; [a-z0-9]) k="${c}" ;; *) echo "say: no key for '${c}'" >&2; exit 2 ;; esac
        echo "sendkey ${k}" >&3
        sleep 0.15
    done
    echo "sendkey ret" >&3
    exec 3>&-
}
off() { wait_for "$1" 60 '^power: off via ACPI S5 '; } # QEMU has no supported panel: goodbye selects S5
ready='^keyboard: type a line and press Enter'
run() { # name with-stick: start, wait for Allen, goodbye (S5), cleanup; result in name.txt
    start "$1" "$2"
    if wait_for "$1" 400 "${ready}"; then
        if [[ "$1" == one ]]; then
            # A fresh disk now opens setup before chat. Complete its real
            # keyboard path without granting optional device/model consent.
            # Sending goodbye here used to type it into setup instead.
            local step
            for step in intent scan consent llm ready; do
                if wait_for "$1" 40 "^wizard: step ${step}$"; then
                    printf 'sendkey esc\n' >"${work}/$1.mon.in"
                    sleep 0.2
                else
                    fail "1: setup reached ${step} before chat"
                    break
                fi
            done
            wait_for "$1" 40 '^wizard: finished' || fail "1: setup finished before goodbye"
        else
            chk "$1: completed setup stays closed" "${work}/$1.log" '^wizard: setup finished on an earlier start'
        fi
        say "$1" "goodbye"
        off "$1" || true
    fi
    stop "$1"
    txt "$1" >"${work}/$1.txt"
}

mkdir -p "${work}/spool"
"${nb}" serve "${work}/spool" --qemu-dgram "${rp}:${lp}" >"${work}/serve.log" 2>&1 &
pids+=($!)

echo "== start 1: stick + network, install =="
push "${out}/IMACKERN.EFI"
run one 1
chk "1: the network kernel started" "${work}/one.txt" '^netldr: starting the network kernel \([0-9]+ bytes\)$'
chk "1: Allen's section added" "${work}/one.txt" '^allen_memory: AIENOS partition added, entry [0-9]+, '
chk "1: boot section added in unused space" "${work}/one.txt" '^boot_disk: boot section added, entry [0-9]+, first_lba=[0-9]+ last_lba=[0-9]+ \(256 MiB, EFI System Partition\); the existing partitions were not touched$'
chk "1: installed in slot A, read back" "${work}/one.txt" "^boot_disk: installed this kernel \\(commit ${commit:0:12}, generation [0-9]+, [0-9]+ bytes\\) as .EFI.BOOT.BOOTX64\\.EFI, slot A, new FAT32 volume, [0-9]+ blocks in [0-9]+ ms, read back exact; "
chk "1: boot times reported with the loader" "${work}/one.txt" '^boot_time: since power on: firmware until the loader [0-9]+ ms, loader [0-9]+ ms, kernel [0-9]+ ms; Allen.s window at [0-9]+ ms$'
chk "1: ACPI S5 after goodbye" "${work}/one.txt" '^power: off via ACPI S5 '
nochk "1: no write refused by the fence" "${work}/one.txt" 'ahci_write: .*refused'
first="$(grep -aoE '^boot_disk: boot section added, entry [0-9]+, first_lba=[0-9]+' "${work}/one.txt" | grep -oE '[0-9]+$' || echo 0)"
if [[ "${first}" -gt 0 ]]; then
    dd if="${work}/hdd.img" of="${work}/esp1.img" bs=512 skip="${first}" count=$((256 * 2048)) status=none
    if "${fsck_fat}" -n "${work}/esp1.img" >"${work}/fsck1" 2>&1; then pass "1: fsck.fat finds nothing to repair"
    else cat "${work}/fsck1"; fail "1: fsck.fat finds faults"; fi
    if MTOOLS_SKIP_CHECK=1 mtype -i "${work}/esp1.img" ::/EFI/BOOT/BOOTX64.EFI >"${work}/got1" &&
        cmp -s "${work}/got1" "${out}/IMACKERN.EFI"; then pass "1: mtools reads BOOTX64.EFI equal to the kernel image"
    else fail "1: BOOTX64.EFI differs from the kernel image"; fi
else
    fail "1: no boot section first LBA"
fi
if cmp -s -i $((2048 * 512)) -n $((16384 * 512)) "${work}/hdd.before" "${work}/hdd.img"; then
    pass "1: the old partition is byte-identical"
else fail "1: the old partition changed"; fi
[[ "$(sha256sum <"${stick}")" == "${stick_sum}" ]] && pass "1: the stick was not written" || fail "1: the stick changed"

echo "== start 2: the hard disk alone =="
run two 0
nochk "2: no network loader" "${work}/two.txt" '^netldr: '
chk "2: the kernel ran from the hard disk" "${work}/two.txt" "^aienos_commit: ${commit}\$"
chk "2: boot times without a loader" "${work}/two.txt" '^boot_time: since power on: firmware until this kernel [0-9]+ ms \(no loader\), kernel [0-9]+ ms; Allen.s window at [0-9]+ ms$'
chk "2: the boot section reported, slot A" "${work}/two.txt" '^boot_disk: .EFI.BOOT.BOOTX64\.EFI on disk [0-9]+: [0-9]+ bytes \(slot A\)$'
chk "2: Allen's memory loaded (second start)" "${work}/two.txt" '^allen_memory: loaded generation [0-9]+ \(2 of 2 copies valid\): .*1 earlier start$'
nochk "2: nothing added" "${work}/two.txt" 'partition added|boot section added'

echo "== start 3: stick + network, a second kernel =="
push "${out2}/IMACKERN.EFI"
run three 1
chk "3: the second kernel ran" "${work}/three.txt" "^aienos_commit: ${second}\$"
chk "3: installed in slot B" "${work}/three.txt" '^boot_disk: installed this kernel \(commit [0-9a-f]{12}, generation [0-9]+, [0-9]+ bytes\) as .EFI.BOOT.BOOTX64\.EFI, slot B, [0-9]+ blocks in [0-9]+ ms, read back exact; '
nochk "3: no new partition" "${work}/three.txt" 'partition added|boot section added'

echo "== start 4: the hard disk alone =="
run four 0
chk "4: the second kernel runs from the hard disk" "${work}/four.txt" "^aienos_commit: ${second}\$"
chk "4: slot B reported" "${work}/four.txt" '^boot_disk: .EFI.BOOT.BOOTX64\.EFI on disk [0-9]+: [0-9]+ bytes \(slot B\)$'
dd if="${work}/hdd.img" of="${work}/esp4.img" bs=512 skip="${first:-0}" count=$((256 * 2048)) status=none
if "${fsck_fat}" -n "${work}/esp4.img" >"${work}/fsck4" 2>&1; then pass "4: fsck.fat finds nothing to repair"
else cat "${work}/fsck4"; fail "4: fsck.fat finds faults"; fi

grep -ahE '^boot_time:|^boot_disk:' "${work}"/one.txt "${work}"/two.txt "${work}"/three.txt "${work}"/four.txt | sed 's/^/note: /'
if [[ "${fails}" -ne 0 ]]; then
    for n in one two three four; do echo "---- ${n} (last 30 lines) ----"; tail -30 "${work}/${n}.txt"; done
    echo "${tag}: FAIL (${fails} checks)"
    exit 1
fi
echo "${tag}: PASS"
