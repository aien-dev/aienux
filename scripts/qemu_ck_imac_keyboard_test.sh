#!/usr/bin/env bash
# qemu_ck_imac_keyboard_test.sh -- QEMU test for the iMac kernel's USB keyboard
# (native/kernel/x86/xhci_x86.c). QEMU is an emulator: a PASS here qualifies nothing
# about a 2017 iMac, its USB controller or any real keyboard.
#
# Builds IMACKERN.EFI (the default image) and boots it three times at once in QEMU q35
# with OVMF and a qemu-xhci controller:
#   hub     a usb-kbd behind a usb-hub (the shape of Apple's wired keyboards, which
#           carry their own hub); keys are typed from OUTSIDE the guest through the
#           QEMU monitor ("sendkey"): "Hello Allen" Enter, "xy" Backspace "z?!," Enter, then
#           after 70 s "goodbye" Enter.
#           The kernel must find the keyboard behind the hub (QEMU numbers its USB 2
#           ports after the USB 3 ones, so "port=1.1" shows as e.g. 5.1), echo both lines as
#           "keyboard_line: Hello Allen" and "keyboard_line: xz?!,", have Allen answer them
#           ("allen: Hello Humanity.", "allen: I don't know that yet.", plus the
#           content-free "screen: Allen answered (11 characters in, 15 out)"), switch the
#           screen to Allen's window (a QEMU screendump of it is kept as
#           $out/allen-qemu.png), keep it up with no idle countdown (still on 70 s
#           after the last key), answer "goodbye" and then power QEMU off by itself;
#   direct  a usb-kbd on a root port: found on that port;
#   none    the controller with nothing on it: "keyboard: none (no USB keyboard
#           found)" and the normal boot otherwise.
# The kernel's lines reach the serial log through COM1, which QEMU has and the iMac
# does not. Needs qemu-system-x86_64, OVMF, mtools and an x86_64 gcc. Final line:
#     AIENOS_IMAC_KEYBOARD: PASS|FAIL|NOT_RUN [(...)]   exit 0 / 1 / 2 (tools) / 3 (NOT_RUN)
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"
tag="AIENOS_IMAC_KEYBOARD"
code_fd="${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}"
vars_fd="${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}"
cross=""
if [ "$(uname -m)" != "x86_64" ]; then cross="x86_64-linux-gnu-"; fi
for t in qemu-system-x86_64 mformat mmd mcopy "${cross}gcc" "${cross}ld"; do
    command -v "${t}" >/dev/null || { echo "${tag}: NOT_RUN (${t} missing)"; exit 2; }
done
[[ -r "${code_fd}" && -r "${vars_fd}" ]] || { echo "${tag}: NOT_RUN (no OVMF)"; exit 2; }
out="${IMACKERN_OUT:-${repo_root}/target/native-kernel-imac-keyboard}"
commit="$(git rev-parse HEAD 2>/dev/null || echo unknown)"
make -s -C native/kernel X86_CROSS="${cross}" OUT="${out}" AIENOS_COMMIT="${commit}" imac-kernel >/dev/null

. "${repo_root}/scripts/lib_gate_hold.sh"
if ! gh_quiet_check || ! gh_lock_take qemu_ck_imac_keyboard_test "${AIENOS_GATE_MINUTES:-30}"; then
    echo "${tag}: NOT_RUN (${gh_why})"
    exit 3
fi
rm -f "${out}/allen-qemu.png"
work="$(mktemp -d)"
pids=()
cleanup() {
    local p
    for p in "${pids[@]}"; do kill "${p}" 2>/dev/null || true; done
    rm -rf "${work}"
    gh_lock_release
}
trap cleanup EXIT

img="${work}/esp.img"
truncate -s 64M "${img}"
mformat -i "${img}" -F ::
mmd -i "${img}" ::/EFI ::/EFI/BOOT
mcopy -i "${img}" "${out}/IMACKERN.EFI" ::/EFI/BOOT/BOOTX64.EFI

fails=0
chk() { # description, file, pattern
    if grep -aqE -- "$3" "$2"; then echo "PASS  $1"; else echo "FAIL  $1 (no match for: $3)"; fails=$((fails + 1)); fi
}
txt() { tr -d '\r' <"${work}/$1.log" 2>/dev/null || true; }
has() { txt "$1" | grep -aqE -- "$2"; }
# wait_for name seconds pattern: 0 once the serial log of run `name` matches.
wait_for() {
    local i
    for ((i = 0; i < $2 * 5; i++)); do
        has "$1" "$3" && return 0
        sleep 0.2
    done
    return 1
}

# start name usb-devices...: one QEMU in the background, monitor on a pipe.
start() {
    local name="$1"; shift
    cp "${vars_fd}" "${work}/${name}.vars"
    cp "${img}" "${work}/${name}.esp"
    mkfifo "${work}/${name}.mon.in" "${work}/${name}.mon.out"
    : >"${work}/${name}.log"
    qemu-system-x86_64 -M q35 -accel tcg -cpu max -smp 2 -m 1024 \
        -drive if=pflash,format=raw,readonly=on,file="${code_fd}" \
        -drive if=pflash,format=raw,file="${work}/${name}.vars" \
        -drive if=none,id=esp,format=raw,file="${work}/${name}.esp" -device ide-hd,drive=esp,bus=ide.0 \
        -device qemu-xhci,id=xhci "$@" \
        -chardev pipe,id=mon,path="${work}/${name}.mon" -mon chardev=mon,mode=readline \
        -display none -nic none -serial file:"${work}/${name}.log" -no-reboot 2>"${work}/${name}.err" &
    pids+=($!)
    eval "pid_${name}=$!"
    cat "${work}/${name}.mon.out" >/dev/null &   # drain monitor output so QEMU never blocks
    pids+=($!)
}
running() { local v="pid_$1"; kill -0 "${!v}" 2>/dev/null; }
stop() { local v="pid_$1"; kill "${!v}" 2>/dev/null || true; }

start hub -device usb-hub,bus=xhci.0,port=1 -device usb-kbd,bus=xhci.0,port=1.1
start direct -device usb-kbd,bus=xhci.0,port=1
start none

# Keys go in through the QEMU monitor (outside the guest), 0.15 s apart.
typed_ok=0
if wait_for hub 300 '^keyboard: type a line and press Enter'; then
    exec 3>"${work}/hub.mon.in"
    for k in shift-h e l l o spc shift-a l l e n ret; do echo "sendkey ${k}" >&3; sleep 0.15; done
    typed_ok=1
    # Allen's answer appears letter by letter; give it time, then photograph the screen.
    if wait_for hub 120 "^allen: Hello Humanity\.\$"; then
        sleep 4
        echo "screendump ${work}/hub.png -f png" >&3
        for _ in $(seq 1 50); do [[ -s "${work}/hub.png" ]] && break; sleep 0.2; done
        sleep 1
        [[ -s "${work}/hub.png" ]] && cp "${work}/hub.png" "${out}/allen-qemu.png"
    fi
    for k in x y backspace z shift-slash shift-1 comma ret; do echo "sendkey ${k}" >&3; sleep 0.15; done
fi
wait_for direct 300 '^power: Allen.s window stays on' || true
wait_for none 300 '^power: holding the screen' || true
stop direct
stop none
# Allen's window has no idle countdown: 70 s after the last key (the text
# screen's hold is 60 s) the hub run must still be up. Then "goodbye" ends it.
stayed=0
if [[ ${typed_ok} == 1 ]] && wait_for hub 60 "^allen: I don't know that yet\."; then
    sleep 70
    running hub && ! has hub '^power: off' && stayed=1
    for k in g o o d b y e ret; do echo "sendkey ${k}" >&3; sleep 0.15; done
fi
# After goodbye the hub run must power off by itself (ACPI S5).
self_off=0
for _ in $(seq 1 300); do   # generous: three emulated machines share the host
    if ! running hub; then has hub '^power: off via ACPI S5 ' && self_off=1; break; fi
    sleep 1
done
stop hub
for r in hub direct none; do txt "${r}" >"${work}/${r}.txt"; done

echo "== hub: usb-kbd behind usb-hub =="
chk "hub: controller running" "${work}/hub.txt" '^xhci: ctrl=0 running version=[0-9a-f]+ ports=[1-9][0-9]* slots=[1-8] context_bytes=(32|64) scratchpads=[0-9]+$'
chk "hub: hub walked" "${work}/hub.txt" '^usb: port [0-9]+: hub, [1-9][0-9]* ports, (full|high) speed$'
chk "hub: keyboard found behind the hub" "${work}/hub.txt" '^keyboard: ready \(controller 0, port [0-9]+\.1, full speed, slot [1-8], endpoint 0x8[1-9a-f]\)$'
chk "hub: typing prompt" "${work}/hub.txt" '^keyboard: type a line and press Enter; Allen.s window stays on until you type goodbye$'
chk "hub: no countdown while Allen's window is up" "${work}/hub.txt" '^power: Allen.s window stays on until goodbye \(or the power button\), then ACPI S5$'
[[ ${typed_ok} == 1 ]] && echo "PASS  hub: keys sent from outside the guest" || { echo "FAIL  hub: keys not sent"; fails=$((fails + 1)); }
chk "hub: Allen's window took the screen" "${work}/hub.txt" '^screen: Allen.s window \([0-9]+x[0-9]+, [0-9]+ MiB drawing memory\)$'
chk "hub: the night drawn by the secondary core, frames timed" "${work}/hub.txt" '^screen: night: [1-9][0-9]* frames before Allen woke, average [0-9]+\.[0-9] ms, slowest [0-9]+\.[0-9] ms \(drawn by a secondary core\)$'
chk "hub: first line typed" "${work}/hub.txt" '^keyboard_line: Hello Allen$'
chk "hub: Allen answers Hello Humanity." "${work}/hub.txt" '^allen: Hello Humanity\.$'
chk "hub: the answer reported by lengths only (11 characters in, 15 out)" "${work}/hub.txt" '^screen: Allen answered \(11 characters in, 15 out\)$'
[[ -s "${out}/allen-qemu.png" ]] && echo "PASS  hub: screenshot of Allen's window kept (${out}/allen-qemu.png)" || { echo "FAIL  hub: no screenshot"; fails=$((fails + 1)); }
chk "hub: backspace edits the line; shifted and plain symbols typed" "${work}/hub.txt" '^keyboard_line: xz\?!,$'
chk "hub: Allen says what it does not know" "${work}/hub.txt" "^allen: I don't know that yet\."
[[ ${stayed} == 1 ]] && echo "PASS  hub: still on 70 s after the last key (no idle countdown)" || { echo "FAIL  hub: Allen's window did not stay up"; fails=$((fails + 1)); }
chk "hub: Allen says goodbye" "${work}/hub.txt" '^allen: Goodbye\. Until next time\.$'
chk "hub: typing session ended on its own line" "${work}/hub.txt" '^keyboard: done \(lines=3\)$'
chk "hub: kernel alive" "${work}/hub.txt" '^kernel: alive$'
chk "hub: ACPI S5 power off" "${work}/hub.txt" '^power: off via ACPI S5 '
[[ ${self_off} == 1 ]] && echo "PASS  hub: QEMU powered off by itself after goodbye" || { echo "FAIL  hub: QEMU still running"; fails=$((fails + 1)); }

echo "== direct: usb-kbd on a root port =="
chk "direct: keyboard found on a root port" "${work}/direct.txt" '^keyboard: ready \(controller 0, port [0-9]+, (full|high) speed, slot 1, endpoint 0x8[1-9a-f]\)$'
chk "direct: Allen's window stays on" "${work}/direct.txt" '^power: Allen.s window stays on until goodbye \(or the power button\), then ACPI S5$'

echo "== none: controller, no keyboard =="
chk "none: controller found" "${work}/none.txt" '^pci: xhci [0-9a-f:.]+ id=1b36:000d '
chk "none: says there is no keyboard" "${work}/none.txt" '^keyboard: none \(no USB keyboard found\)$'
chk "none: kernel alive" "${work}/none.txt" '^kernel: alive$'
if grep -aqE '^keyboard: ready' "${work}/none.txt"; then
    echo "FAIL  none: claimed a keyboard"; fails=$((fails + 1))
else
    echo "PASS  none: no keyboard claimed"
fi
if grep -aqE '^screen: Allen' "${work}/none.txt"; then
    echo "FAIL  none: Allen's window without a keyboard"; fails=$((fails + 1))
else
    echo "PASS  none: text screen kept without a keyboard"
fi

for r in hub direct none; do
    if grep -aqE '^(panic|power: still running|power: halted)' "${work}/${r}.txt"; then
        echo "FAIL  ${r}: panic or failed power off"; fails=$((fails + 1))
    else
        echo "PASS  ${r}: no panic"
    fi
done
for r in hub direct none; do echo "---- ${r} serial (keyboard lines) ----"; grep -aE '^(pci: xhci|xhci|usb|keyboard)' "${work}/${r}.txt" || true; done
if [[ ${fails} == 0 ]]; then
    echo "${tag}: PASS"
    exit 0
fi
echo "${tag}: FAIL (${fails})"
exit 1
