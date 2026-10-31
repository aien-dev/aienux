#!/usr/bin/env bash
# qemu_ck_allen_memory_test.sh -- QEMU test for Allen's memory and notes on a hard
# disk (native/kernel/x86/memory_x86.c, dev/gpt_add.c, svc/allen_memory.c). QEMU is
# an emulator: a PASS here qualifies nothing about any real machine or disk.
#
# Builds IMACKERN.EFI (the default image) and boots it in QEMU q35 with OVMF, a
# usb-kbd and a second AHCI disk whose IDENTIFY model is "APPLE HDD ST1000DM003"
# (on the approved list). That disk starts as ck_gpt_image "one-old": one existing
# partition (sentinel-a, filled with a pattern) and the rest unused.
#   boot 1  the kernel lists the partition, adds a 1 GiB AIENOS partition in the
#           unused space, finds the memory empty and saves start 1; keys typed
#           from outside the guest: "my name is drake", "remember my favorite
#           color is blue", "save the gate code is 4471", "goodbye" (each
#           change saved, the note before the copy that counts it; QEMU powers
#           off). Then, on the host: the kernel's own parser finds the AIENOS
#           partition, and every block that changed is a GPT block, one of the
#           two memory copies or note 0; the sentinel partition is byte-identical.
#   boot 2  the same disk: the partition is found (not added again), memory
#           loads (1 fact, name known, 1 earlier start) and 1 note, Allen speaks
#           first ("Welcome back, Drake. I still remember 1 thing you taught me,
#           and your 1 note."), answers "what is my favorite color", "what is
#           the gate code" and "how many times have you restarted" from the
#           disk, then goodbye.
#   other   a copy of the disk after boot 1 on another machine, under a model
#           on no list ("WDC WD10EZEX"): the section alone makes it Allen's,
#           memory and the note load (booting anywhere).
#   fresh   a fresh "one-old" disk under that unlisted model: no section, not
#           approved, nothing written.
#   whole   an approved disk whose one partition fills it ("one-whole"): nothing
#           is written, Allen says it remembers only until the computer turns off.
# The disk holding the kernel (model "QEMU HARDDISK") must never be written.
# Final line: AIENOS_ALLEN_MEMORY_QEMU: PASS|FAIL|NOT_RUN  exit 0 / 1 / 2 (tools) / 3
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"
tag="AIENOS_ALLEN_MEMORY_QEMU"
code_fd="${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}"
vars_fd="${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}"
cross=""
if [ "$(uname -m)" != "x86_64" ]; then cross="x86_64-linux-gnu-"; fi
for t in qemu-system-x86_64 mformat mmd mcopy cmp sha256sum "${cross}gcc" "${cross}ld"; do
    command -v "${t}" >/dev/null || { echo "${tag}: NOT_RUN (${t} missing)"; exit 2; }
done
[[ -r "${code_fd}" && -r "${vars_fd}" ]] || { echo "${tag}: NOT_RUN (no OVMF)"; exit 2; }
out="${IMACKERN_OUT:-${repo_root}/target/native-kernel-allen-memory}"
commit="$(git rev-parse HEAD 2>/dev/null || echo unknown)"
make -s -C native/kernel X86_CROSS="${cross}" OUT="${out}" AIENOS_COMMIT="${commit}" imac-kernel >/dev/null
make -s -C native/kernel OUT="${out}" gpt-image >/dev/null
gpt="${out}/host/ck_gpt_image"

. "${repo_root}/scripts/lib_gate_hold.sh"
if ! gh_quiet_check || ! gh_lock_take qemu_ck_allen_memory_test "${AIENOS_GATE_MINUTES:-30}"; then
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

esp="${work}/esp.img"
truncate -s 64M "${esp}"
mformat -i "${esp}" -F ::
mmd -i "${esp}" ::/EFI ::/EFI/BOOT
mcopy -i "${esp}" "${out}/IMACKERN.EFI" ::/EFI/BOOT/BOOTX64.EFI
"${gpt}" create "${work}/hdd.img" 512 2048 one-old >"${work}/hdd.create"
"${gpt}" create "${work}/whole.img" 512 2048 one-whole >/dev/null
"${gpt}" create "${work}/fresh.img" 512 2048 one-old >/dev/null
cp --sparse=always "${work}/hdd.img" "${work}/hdd.before"
cp --sparse=always "${work}/whole.img" "${work}/whole.before"
cp --sparse=always "${work}/fresh.img" "${work}/fresh.before"
approved_model="APPLE HDD ST1000DM003"
other_model="WDC WD10EZEX"

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

start() { # name disk-image [disk-model]
    local name="$1" disk="$2" model="${3:-${approved_model}}"
    cp "${vars_fd}" "${work}/${name}.vars"
    cp "${esp}" "${work}/${name}.esp"
    mkfifo "${work}/${name}.mon.in" "${work}/${name}.mon.out"
    : >"${work}/${name}.log"
    qemu-system-x86_64 -M q35 -accel tcg -cpu max -smp 2 -m 1024 \
        -drive if=pflash,format=raw,readonly=on,file="${code_fd}" \
        -drive if=pflash,format=raw,file="${work}/${name}.vars" \
        -drive if=none,id=esp,format=raw,file="${work}/${name}.esp" -device ide-hd,drive=esp,bus=ide.0 \
        -drive if=none,id=hdd,format=raw,file="${disk}" \
        -device ide-hd,drive=hdd,bus=ide.1,model="${model}" \
        -device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0,port=1 \
        -chardev pipe,id=mon,path="${work}/${name}.mon" -mon chardev=mon,mode=readline \
        -display none -nic none -serial file:"${work}/${name}.log" -no-reboot 2>"${work}/${name}.err" &
    pids+=($!)
    eval "pid_${name}=$!"
    cat "${work}/${name}.mon.out" >/dev/null &
    pids+=($!)
}
running() { local v="pid_$1"; kill -0 "${!v}" 2>/dev/null; }
stop() { local v="pid_$1"; kill "${!v}" 2>/dev/null || true; }
# say name "text": type a line through the QEMU monitor (outside the guest), then Enter.
say() {
    local name="$1" text="$2" c k i
    exec 3>"${work}/${name}.mon.in"
    for ((i = 0; i < ${#text}; i++)); do
        c="${text:i:1}"
        case "${c}" in
            " ") k=spc ;;
            [a-z0-9]) k="${c}" ;;
            *) echo "say: no key for '${c}'" >&2; exit 2 ;;
        esac
        echo "sendkey ${k}" >&3
        sleep 0.15
    done
    echo "sendkey ret" >&3
    exec 3>&-
}
off() { # name: wait for QEMU to power off by itself after goodbye
    local i
    for i in $(seq 1 300); do running "$1" || return 0; sleep 1; done
    stop "$1"
    return 1
}
ready='^keyboard: type a line and press Enter'

# ---- boot 1 (and the whole-disk machine alongside) ----
start one "${work}/hdd.img"
start whole "${work}/whole.img"
b1_off=0
if wait_for one 300 "${ready}" && wait_for one 60 '^allen_memory: saved generation 1 '; then
    say one "my name is drake"
    wait_for one 120 '^allen_memory: saved generation 2 ' || true
    say one "remember my favorite color is blue"
    wait_for one 120 '^allen_memory: saved generation 3 ' || true
    say one "save the gate code is 4471"
    wait_for one 120 '^allen_memory: saved generation 4 ' || true
    say one "goodbye"
    off one && b1_off=1
fi
stop one
wait_for whole 300 "${ready}" || true
stop whole
txt one >"${work}/one.txt"
txt whole >"${work}/whole.txt"

echo "== boot 1: first start on a disk with one old partition and unused space =="
chk "1: the approved disk found" "${work}/one.txt" '^allen_memory: no section yet; the approved disk is disk [0-9]+ \([0-9]+ blocks of 512 bytes\)$'
chk "1: the old partition listed" "${work}/one.txt" '^ahci_part: disk=[0-9]+ entry=[0-9]+ type=[0-9a-f-]+ first_lba=2048 last_lba=18431 mib=8 name="'
chk "1: one partition counted" "${work}/one.txt" '^allen_memory: the disk has 1 partition$'
chk "1: AIENOS partition added in unused space" "${work}/one.txt" '^allen_memory: AIENOS partition added, entry [0-9]+, first_lba=[0-9]+ last_lba=[0-9]+ \(1024 MiB\); the existing partitions were not touched$'
chk "1: write ranges closed after the add" "${work}/one.txt" 'write ranges closed$'
chk "1: memory empty on a first start" "${work}/one.txt" '^allen_memory: empty \(a first start\); Allen starts fresh$'
chk "1: no notes yet" "${work}/one.txt" '^allen_memory: 0 notes loaded \(0 damaged\)$'
chk "1: start 1 saved" "${work}/one.txt" '^allen_memory: saved generation 1 to copy [AB] \(0 facts, 0 notes\) in [0-9]+ ms$'
chk "1: Allen learns the name" "${work}/one.txt" "^allen: Nice to meet you, Drake\. I'll remember that\.\$"
chk "1: the name saved" "${work}/one.txt" '^allen_memory: saved generation 2 to copy [AB] \(0 facts'
chk "1: Allen is taught a fact" "${work}/one.txt" "^allen: Got it: your favorite color is blue\. I'll remember that\.\$"
chk "1: the fact saved" "${work}/one.txt" '^allen_memory: saved generation 3 to copy [AB] \(1 fact, 0 notes\)'
chk "1: Allen saves a note" "${work}/one.txt" "^allen: Saved\. It's your first note\.\$"
chk "1: the note written" "${work}/one.txt" '^allen_memory: note 0 written in [0-9]+ ms$'
chk "1: then the copy that counts it" "${work}/one.txt" '^allen_memory: saved generation 4 to copy [AB] \(1 fact, 1 note\)'
if grep -anE '^allen_memory: (note 0 written|saved generation 4 )' "${work}/one.txt" | head -1 | grep -q 'note 0 written'; then
    pass "1: the note is on disk before the copy that counts it"
else
    fail "1: the note was not written first"
fi
chk "1: goodbye saves the line count" "${work}/one.txt" '^allen_memory: saved generation 5 to copy [AB] \(1 fact, 1 note\)'
chk "1: ACPI S5 power off" "${work}/one.txt" '^power: off via ACPI S5 '
[[ ${b1_off} == 1 ]] && pass "1: QEMU powered off by itself" || fail "1: QEMU still running"
nochk "1: no write refused by the fence" "${work}/one.txt" 'ahci_write: .*refused'
nochk "1: no failed save" "${work}/one.txt" 'save FAILED'

echo "== the disk after boot 1, read on the host =="
found="$("${gpt}" find "${work}/hdd.img" 512 || true)"
echo "${found}"
pf="$(sed -nE 's/^aienos first_lba=([0-9]+) last_lba=([0-9]+) blocks=[0-9]+$/\1/p' <<<"${found}")"
pl="$(sed -nE 's/^aienos first_lba=([0-9]+) last_lba=([0-9]+) blocks=[0-9]+$/\2/p' <<<"${found}")"
if [[ -n "${pf}" && $((pl - pf + 1)) == 2097152 ]]; then pass "host: the kernel's parser finds one 1 GiB AIENOS partition"; else fail "host: no 1 GiB AIENOS partition"; fi
if [[ -n "${pf}" ]] && grep -aqE "first_lba=${pf} last_lba=${pl} " "${work}/one.txt"; then pass "host: same range the kernel reported"; else fail "host: range differs from the kernel's"; fi
blocks=$(( $(stat -c %s "${work}/hdd.img") / 512 ))
# Every changed byte must be in LBA 1..33, the last 33 LBAs, the two 4096-byte copies
# or note 0 (the third 4096-byte unit): the first 24 blocks of the partition.
bad="$(cmp -l "${work}/hdd.before" "${work}/hdd.img" 2>/dev/null | awk -v pf="${pf:-0}" -v last=$((blocks - 1)) '
    { lba = int(($1 - 1) / 512); n++
      if (lba == 0) { print "lba 0 changed"; exit }
      if (lba >= 1 && lba <= 33) next
      if (lba >= last - 32 && lba <= last) next
      if (pf > 0 && lba >= pf && lba < pf + 24) next
      print "lba " lba " changed"; exit }
    END { if (n == 0) print "nothing changed" }' || true)"
[[ -z "${bad}" ]] && pass "host: only the GPT, the two memory copies and note 0 changed" || fail "host: ${bad}"
s0="$(dd if="${work}/hdd.before" bs=512 skip=2048 count=16384 status=none | sha256sum)"
s1="$(dd if="${work}/hdd.img" bs=512 skip=2048 count=16384 status=none | sha256sum)"
[[ "${s0}" == "${s1}" ]] && pass "host: the old partition is byte-identical" || fail "host: the old partition changed"
cmp -s "${work}/whole.before" "${work}/whole.img" && pass "whole: disk byte-identical" || fail "whole: disk was written"

echo "== whole: one partition fills the disk =="
chk "whole: the approved disk found" "${work}/whole.txt" '^allen_memory: no section yet; the approved disk is disk '
chk "whole: AIENOS partition not added, nothing written" "${work}/whole.txt" '^allen_memory: AIENOS partition not added: .*; nothing was written$'
chk "whole: Allen remembers only until power off" "${work}/whole.txt" '^allen_memory: no AIENOS partition; Allen remembers only until this computer turns off$'
nochk "whole: no save" "${work}/whole.txt" '^allen_memory: saved'

# ---- boot 2: the same disk; alongside, a copy of it on another machine, and a fresh unlisted disk ----
cp --sparse=always "${work}/hdd.img" "${work}/other.img"
start two "${work}/hdd.img"
start other "${work}/other.img" "${other_model}"
start fresh "${work}/fresh.img" "${other_model}"
b2_off=0
if wait_for two 300 "${ready}"; then
    wait_for two 60 '^allen: Welcome back' || true
    sleep 3
    say two "what is my favorite color"
    wait_for two 120 '^allen: Your favorite color is blue\.$' || true
    sleep 3
    say two "what is the gate code"
    wait_for two 120 '^allen: From your notes' || true
    sleep 3
    say two "how many times have you restarted"
    wait_for two 120 '^allen: This is start number' || true
    sleep 3
    say two "goodbye"
    off two && b2_off=1
fi
stop two
wait_for other 300 '^allen: Welcome back' || true
stop other
wait_for fresh 300 "${ready}" || true
stop fresh
txt two >"${work}/two.txt"
txt other >"${work}/other.txt"
txt fresh >"${work}/fresh.txt"

echo "== boot 2: the same disk after a power off =="
chk "2: Allen's section found" "${work}/two.txt" '^allen_memory: Allen'"'"'s section is on disk [0-9]+ \([0-9]+ blocks of 512 bytes\)$'
chk "2: AIENOS partition found, not added again" "${work}/two.txt" '^allen_memory: AIENOS partition found, entry [0-9]+, first_lba='"${pf:-x}"' last_lba='"${pl:-x}"'$'
nochk "2: no second partition" "${work}/two.txt" 'AIENOS partition added'
chk "2: memory loaded" "${work}/two.txt" '^allen_memory: loaded generation 5 \(2 of 2 copies valid\): 1 fact, name known, 1 earlier start$'
chk "2: the note loaded" "${work}/two.txt" '^allen_memory: 1 note loaded \(0 damaged\)$'
chk "2: start 2 saved" "${work}/two.txt" '^allen_memory: saved generation 6 '
chk "2: Allen welcomes Drake back first" "${work}/two.txt" '^allen: Welcome back, Drake\. I still remember 1 thing you taught me, and your 1 note\.$'
chk "2: the fact remembered across the power off" "${work}/two.txt" '^allen: Your favorite color is blue\.$'
chk "2: the note handed back across the power off" "${work}/two.txt" '^allen: From your notes: the gate code is 4471\.$'
chk "2: the start count remembered" "${work}/two.txt" "^allen: This is start number 2 for me, and I've answered [0-9]+ lines in all\.\$"
chk "2: ACPI S5 power off" "${work}/two.txt" '^power: off via ACPI S5 '
[[ ${b2_off} == 1 ]] && pass "2: QEMU powered off by itself" || fail "2: QEMU still running"
nochk "2: no write refused by the fence" "${work}/two.txt" 'ahci_write: .*refused'

echo "== other: the same section on another machine, under a model on no list =="
chk "other: the section alone makes the disk Allen's" "${work}/other.txt" '^allen_memory: Allen'"'"'s section is on disk '
chk "other: memory loaded" "${work}/other.txt" '^allen_memory: loaded generation 5 \(2 of 2 copies valid\): 1 fact, name known'
chk "other: the note loaded" "${work}/other.txt" '^allen_memory: 1 note loaded \(0 damaged\)$'
chk "other: Allen welcomes Drake back" "${work}/other.txt" '^allen: Welcome back, Drake\. I still remember 1 thing you taught me, and your 1 note\.$'
nochk "other: no partition added" "${work}/other.txt" 'AIENOS partition added'

echo "== fresh: an unlisted disk with no section =="
chk "fresh: nothing chosen" "${work}/fresh.txt" '^allen_memory: no disk holds Allen'"'"'s section and none is approved for one; Allen remembers only until this computer turns off$'
nochk "fresh: no save" "${work}/fresh.txt" '^allen_memory: saved'
cmp -s "${work}/fresh.before" "${work}/fresh.img" && pass "fresh: disk byte-identical" || fail "fresh: disk was written"

for r in one two whole other fresh; do
    nochk "${r}: no panic" "${work}/${r}.txt" '^(panic|power: still running|power: halted)'
    nochk "${r}: no iMac or Spark named on screen" "${work}/${r}.txt" '^(allen|allen_memory|screen): .*(iMac|Spark)'
done
for r in one two whole other fresh; do echo "---- ${r} (memory lines) ----"; grep -aE '^(allen_memory|ahci_part)' "${work}/${r}.txt" || true; done
if [[ ${fails} == 0 ]]; then
    echo "${tag}: PASS"
    exit 0
fi
echo "${tag}: FAIL (${fails})"
exit 1
