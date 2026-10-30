#!/usr/bin/env bash
# qemu_ck_imac_keyboard_test.sh -- QEMU test for the iMac kernel's USB keyboard
# (native/kernel/x86/xhci_x86.c). QEMU is an emulator: a PASS here qualifies nothing
# about a 2017 iMac, its USB controller or any real keyboard.
#
# Builds IMACKERN.EFI (the default image) and boots it three times at once in QEMU q35
# with OVMF and a qemu-xhci controller, plus a fourth machine on the same kernel built
# with a 25 s idle time (CK_X86_IDLE_S=25, own object directory):
#   hub     a usb-kbd behind a usb-hub (the shape of Apple's wired keyboards, which
#           carry their own hub); keys are typed from OUTSIDE the guest through the
#           QEMU monitor ("sendkey"): "Hello Allen" Enter, "xy" Backspace "z?!," Enter, then
#           after 70 s "goodbye" Enter. QEMU's unsupported display selects ACPI S5,
#           confirmed by a guest-shutdown QMP event and clean process exit, not a reset.
#           The kernel must find the keyboard behind the hub (QEMU numbers its USB 2
#           ports after the USB 3 ones, so "port=1.1" shows as e.g. 5.1), echo both lines as
#           "keyboard_line: Hello Allen" and "keyboard_line: xz?!,", have Allen answer them
#           ("allen: Hello Humanity.", "allen: I don't know that yet.", plus the
#           content-free "screen: Allen answered (11 characters in, 15 out)"), switch the
#           screen to Allen's window (a QEMU screendump of it is kept as
#           $out/allen-qemu.png), keep it up with no countdown to power-off (still on and
#           awake 70 s after the last key, the default idle time being 600 s), then
#           power off on "goodbye" rather than falsely claim any-key panel wake;
#   direct  a usb-kbd on a root port: found on that port;
#   none    the controller with nothing on it: "keyboard: none (no USB keyboard
#           found)" and the normal boot otherwise;
#   idle    a usb-kbd on a root port and no key typed: 25 s after Allen's window
#           opens the computer requests rest, then powers off through ACPI S5.
#           The same QMP and process checks distinguish shutdown from reset/halt.
# Reversible panel-off/any-key wake needs the real supported AMD path: NOT_RUN here.
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
make -s -C native/kernel X86_CROSS="${cross}" OUT="${out}/idle" AIENOS_COMMIT="${commit}" CK_X86_IDLE_S=25 imac-kernel >/dev/null

. "${repo_root}/scripts/lib_gate_hold.sh"
if ! gh_quiet_check || ! gh_lock_take qemu_ck_imac_keyboard_test "${AIENOS_GATE_MINUTES:-30}"; then
    echo "${tag}: NOT_RUN (${gh_why})"
    exit 3
fi
rm -f "${out}/allen-qemu.png"
work="$(mktemp -d)"
pids=()
qmp_fds=()
cleanup() {
    local p fd
    for p in "${pids[@]}"; do kill "${p}" 2>/dev/null || true; done
    for fd in "${qmp_fds[@]}"; do exec {fd}>&-; done
    if [[ -n "${AIENOS_KEEP_WORK:-}" ]]; then echo "work kept: ${work}"; else rm -rf "${work}"; fi
    gh_lock_release
}
trap cleanup EXIT

img="${work}/esp.img"
truncate -s 64M "${img}"
mformat -i "${img}" -F ::
mmd -i "${img}" ::/EFI ::/EFI/BOOT
mcopy -i "${img}" "${out}/IMACKERN.EFI" ::/EFI/BOOT/BOOTX64.EFI
idle_img="${work}/idle-esp.img"
truncate -s 64M "${idle_img}"
mformat -i "${idle_img}" -F ::
mmd -i "${idle_img}" ::/EFI ::/EFI/BOOT
mcopy -i "${idle_img}" "${out}/idle/IMACKERN.EFI" ::/EFI/BOOT/BOOTX64.EFI

fails=0
chk() { # description, file, pattern
    if grep -aqE -- "$3" "$2"; then echo "PASS  $1"; else echo "FAIL  $1 (no match for: $3)"; fails=$((fails + 1)); fi
}
txt() { tr -d '\r' <"${work}/$1.log" 2>/dev/null || true; }
nohas() {
    if has "$1" "$2"; then echo "FAIL  $3"; fails=$((fails + 1)); else echo "PASS  $3"; fi
}
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
# QMP's greeting establishes that QEMU has opened the FIFO. Sending before
# that point and closing the sole O_RDWR descriptor can discard the command.
# The writer must also stay open after negotiation so events remain enabled.
qmp_wait() {
    local name="$1" pattern="$2" i
    for ((i = 0; i < 150; i++)); do
        if grep -aqE -- "${pattern}" "${work}/${name}.qmp.log"; then return 0; fi
        running "${name}" || return 1
        sleep 0.2
    done
    return 1
}


# A serial S5 write is only an attempt. Require QEMU's guest shutdown event
# and a clean process exit before cleanup can send any signal. -no-reboot
# alone cannot distinguish successful S5 from a reset/triple-fault exit.
wait_off() {
    local name="$1" v="pid_$1" i rc
    wait_for "${name}" 120 '^power: off via ACPI S5 ' || return 1
    for ((i = 0; i < 300; i++)); do
        # Wait for both independently: the QMP drain can run after the
        # QEMU child exits. Match fields on one event without key-order assumptions.
        if ! running "${name}" &&
            grep -aE '"event":[[:space:]]*"SHUTDOWN"' "${work}/${name}.qmp.log" |
                grep -aqE '"reason":[[:space:]]*"guest-shutdown"'; then
            if wait "${!v}"; then rc=0; else rc=$?; fi
            printf '%s\n' "${rc}" >"${work}/${name}.exit"
            [[ ${rc} == 0 ]]
            return
        fi
        sleep 0.2
    done
    return 1
}

# start name usb-devices...: one QEMU in the background, monitor on a pipe
# (START_IMG: another ESP image than the default kernel's).
start() {
    local name="$1" qmp_fd; shift
    cp "${vars_fd}" "${work}/${name}.vars"
    cp "${START_IMG:-${img}}" "${work}/${name}.esp"
    mkfifo "${work}/${name}.mon.in" "${work}/${name}.mon.out"
    mkfifo "${work}/${name}.qmp.in" "${work}/${name}.qmp.out"
    : >"${work}/${name}.qmp.log"
    cat "${work}/${name}.qmp.out" >"${work}/${name}.qmp.log" &
    pids+=($!)
    : >"${work}/${name}.log"
    qemu-system-x86_64 -M q35 -accel tcg -cpu max -smp 2 -m 1024 \
        -drive if=pflash,format=raw,readonly=on,file="${code_fd}" \
        -drive if=pflash,format=raw,file="${work}/${name}.vars" \
        -drive if=none,id=esp,format=raw,file="${work}/${name}.esp" -device ide-hd,drive=esp,bus=ide.0 \
        -device qemu-xhci,id=xhci "$@" \
        -chardev pipe,id=mon,path="${work}/${name}.mon" -mon chardev=mon,mode=readline \
        -qmp pipe:"${work}/${name}.qmp" \
        -display none -nic none -serial file:"${work}/${name}.log" -no-reboot 2>"${work}/${name}.err" &
    pids+=($!)
    eval "pid_${name}=$!"
    cat "${work}/${name}.mon.out" >/dev/null &   # drain monitor output so QEMU never blocks
    pids+=($!)
    # Hold a separate writer for every machine until cleanup, even after
    # capabilities succeeds. A one-command FD can close before QEMU opens
    # the FIFO, losing the command and therefore every asynchronous event.
    exec {qmp_fd}<>"${work}/${name}.qmp.in"
    qmp_fds+=("${qmp_fd}")
    if ! qmp_wait "${name}" '"QMP":[[:space:]]*\{'; then
        echo "FAIL  ${name}: QMP greeting missing"
        cat "${work}/${name}.qmp.log"
        fails=$((fails + 1))
        return
    fi
    printf '{"execute":"qmp_capabilities","id":"caps"}\n' >&"${qmp_fd}"
    if qmp_wait "${name}" '"id":[[:space:]]*"caps"' &&
        grep -aE '"id":[[:space:]]*"caps"' "${work}/${name}.qmp.log" |
            grep -aqE '"return":[[:space:]]*\{[[:space:]]*\}'; then
        echo "PASS  ${name}: QMP capabilities acknowledged before guest shutdown"
    else
        echo "FAIL  ${name}: QMP capabilities not acknowledged"
        cat "${work}/${name}.qmp.log"
        fails=$((fails + 1))
    fi
}
running() { local v="pid_$1"; kill -0 "${!v}" 2>/dev/null; }
stop() { local v="pid_$1"; kill "${!v}" 2>/dev/null || true; }

start hub -device usb-hub,bus=xhci.0,port=1 -device usb-kbd,bus=xhci.0,port=1.1
start direct -device usb-kbd,bus=xhci.0,port=1,id=kbd0
start none
START_IMG="${idle_img}" start idle -device usb-kbd,bus=xhci.0,port=1

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
# direct: unplug the keyboard from outside the guest, plug one back in, type on it.
replug_ok=0
if wait_for direct 300 '^keyboard: type a line and press Enter'; then
    exec 4>"${work}/direct.mon.in"
    echo "device_del kbd0" >&4
    if wait_for direct 30 '^keyboard: unplugged'; then
        sleep 1
        echo "device_add usb-kbd,bus=xhci.0,port=1,id=kbd1" >&4
        for _ in $(seq 1 150); do
            [[ $(txt direct | grep -acE '^keyboard: ready ') -ge 2 ]] && { replug_ok=1; break; }
            sleep 0.2
        done
        if [[ ${replug_ok} == 1 ]]; then
            sleep 1
            for k in o k ret; do echo "sendkey ${k}" >&4; sleep 0.15; done
            wait_for direct 60 '^keyboard_line: ok$' || true
        fi
    fi
fi
wait_for direct 300 '^power: Allen.s window stays on' || true
wait_for none 300 '^power: holding the screen' || true
stop direct
stop none
# idle: no key is typed. The unsupported display must select S5 on its own.
idle_off=0
if wait_off idle; then idle_off=1; fi
stop idle
# Allen's window has no countdown to power-off, and the default idle time is
# 600 s: 70 s after the last key (the text screen's hold is 60 s) the hub run
# must still be up and awake. Then "goodbye" must power off through S5:
# no panel-off/any-key-wake claim and no automatic reboot.
stayed=0
hub_off=0
if [[ ${typed_ok} == 1 ]] && wait_for hub 60 "^allen: I don't know that yet\."; then
    sleep 70
    running hub && ! has hub '^power: off' && ! has hub '^power: (no key|Allen asleep)' && stayed=1
    for k in g o o d b y e ret; do echo "sendkey ${k}" >&3; sleep 0.15; done
    if wait_off hub; then hub_off=1; fi
fi
stop hub

for r in hub direct none idle; do txt "${r}" >"${work}/${r}.txt"; done

echo "== hub: usb-kbd behind usb-hub =="
chk "hub: controller running" "${work}/hub.txt" '^xhci: ctrl=0 running version=[0-9a-f]+ ports=[1-9][0-9]* slots=[1-8] context_bytes=(32|64) scratchpads=[0-9]+$'
chk "hub: hub walked" "${work}/hub.txt" '^usb: port [0-9]+: hub, [1-9][0-9]* ports, (full|high) speed$'
chk "hub: keyboard found behind the hub" "${work}/hub.txt" '^keyboard: ready \(controller 0, port [0-9]+\.1, full speed, slot [1-8], endpoint 0x8[1-9a-f]\)$'
chk "hub: honest typing prompt" "${work}/hub.txt" '^keyboard: type a line and press Enter; goodbye switches the display off; any-key wake only after confirmed panel off, otherwise ACPI S5 \(power button to start\)$'
chk "hub: no countdown, with explicit S5 fallback" "${work}/hub.txt" '^power: Allen.s window stays on; goodbye, idle or a power-button tap switches the display off; confirmed panel off permits any-key wake, otherwise ACPI S5$'
chk "hub: power button watched" "${work}/hub.txt" '^power: button watched at PM1 status port 0x[0-9a-f]+ '
chk "hub: default idle time 600 s" "${work}/hub.txt" '^power: this computer rests after 600 s without a key$'
# QEMU is not the supported AMD display; no MMIO panel writes are permitted.
chk "hub: display chip listed" "${work}/hub.txt" '^pci: display [0-9a-f:.]+ id=1234:1111 class=030000 bar0=0x[0-9a-f]+ bar5=0x[0-9a-f]+ mem_decode=[01]$'
chk "hub: unsupported panel reports S5 policy" "${work}/hub.txt" '^screen: panel: the display chip is not an AMD VGA controller; reversible panel off unavailable \(rest uses ACPI S5\)$'

[[ ${typed_ok} == 1 ]] && echo "PASS  hub: keys sent from outside the guest" || { echo "FAIL  hub: keys not sent"; fails=$((fails + 1)); }
chk "hub: Allen's window took the screen" "${work}/hub.txt" '^screen: Allen.s window \([0-9]+x[0-9]+, [0-9]+ MiB drawing memory\)$'
chk "hub: the night drawn by the secondary core, frames timed" "${work}/hub.txt" '^screen: night: [1-9][0-9]* frames before Allen woke, average [0-9]+\.[0-9] ms, slowest [0-9]+\.[0-9] ms \(drawn by a secondary core\)$'
chk "hub: first line typed" "${work}/hub.txt" '^keyboard_line: Hello Allen$'
chk "hub: Allen answers Hello Humanity." "${work}/hub.txt" '^allen: Hello Humanity\.$'
chk "hub: the answer reported by lengths only (11 characters in, 15 out)" "${work}/hub.txt" '^screen: Allen answered \(11 characters in, 15 out\)$'
[[ -s "${out}/allen-qemu.png" ]] && echo "PASS  hub: screenshot of Allen's window kept (${out}/allen-qemu.png)" || { echo "FAIL  hub: no screenshot"; fails=$((fails + 1)); }
chk "hub: backspace edits the line; shifted and plain symbols typed" "${work}/hub.txt" '^keyboard_line: xz\?!,$'
chk "hub: Allen says what it does not know" "${work}/hub.txt" "^allen: I don't know that yet\."
[[ ${stayed} == 1 ]] && echo "PASS  hub: still on and awake 70 s after the last key (idle time 600 s)" || { echo "FAIL  hub: Allen's window did not stay up and awake"; fails=$((fails + 1)); }
chk "hub: Allen says goodbye" "${work}/hub.txt" '^allen: Goodbye\. Until next time\.$'
chk "hub: kernel alive" "${work}/hub.txt" '^kernel: alive$'
[[ ${hub_off} == 1 ]] && echo "PASS  hub: guest S5 shutdown and clean QEMU exit after goodbye" || { echo "FAIL  hub: no confirmed S5 shutdown after goodbye"; fails=$((fails + 1)); }
chk "hub: S5 selected instead of black-only rest" "${work}/hub.txt" '^power: panel off unsupported; ACPI S5 fallback \(power button to start; no any-key wake\)$'
chk "hub: actual ACPI S5 write attempted" "${work}/hub.txt" '^power: off via ACPI S5 '
nohas hub '^power: (Allen asleep|Allen awake|restart|ACPI S5 failed)' "hub: no false sleep/wake, restart or failed S5"
nohas hub '^screen: panel (off|on)' "hub: no unsupported panel writes"


echo "== direct: usb-kbd on a root port =="
chk "direct: keyboard found on a root port" "${work}/direct.txt" '^keyboard: ready \(controller 0, port [0-9]+, (full|high) speed, slot 1, endpoint 0x8[1-9a-f]\)$'
chk "direct: Allen's window stays on" "${work}/direct.txt" '^power: Allen.s window stays on; goodbye, idle or a power-button tap switches the display off; confirmed panel off permits any-key wake, otherwise ACPI S5$'

chk "direct: unplug noticed" "${work}/direct.txt" '^keyboard: unplugged'
[[ ${replug_ok} == 1 ]] && echo "PASS  direct: keyboard found again after replug" || { echo "FAIL  direct: no second keyboard: ready after replug"; fails=$((fails + 1)); }
chk "direct: line typed on the replugged keyboard" "${work}/direct.txt" '^keyboard_line: ok$'

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

echo "== idle: nobody types, idle time 25 s =="
chk "idle: keyboard found" "${work}/idle.txt" '^keyboard: ready \(controller 0, port [0-9]+, (full|high) speed, slot 1, endpoint 0x8[1-9a-f]\)$'
chk "idle: idle time 25 s" "${work}/idle.txt" '^power: this computer rests after 25 s without a key$'
# Either way of resting is accepted: whether QEMU's -cpu max lists MONITOR/MWAIT is QEMU's choice.
chk "idle: how the cores rest, meter off under a hypervisor" "${work}/idle.txt" '^power: asleep, the drawing cores (rest in mwait \(hint 0x[0-9a-f]+\) and the boot core in mwait|spin \(no MONITOR/MWAIT\) and the boot core halts) between keyboard polls every 20 ms; meter: none \(under a hypervisor\)$'
chk "idle: the idle timer requested rest" "${work}/idle.txt" '^power: no key for 25 s; the computer rests$'
chk "idle: unsupported panel selected S5" "${work}/idle.txt" '^power: panel off unsupported; ACPI S5 fallback \(power button to start; no any-key wake\)$'
[[ ${idle_off} == 1 ]] && echo "PASS  idle: guest S5 shutdown and clean QEMU exit without keys" || { echo "FAIL  idle: no confirmed S5 shutdown without keys"; fails=$((fails + 1)); }
nohas idle '^power: (Allen asleep|Allen awake|restart|ACPI S5 failed)' "idle: no false sleep/wake, restart or failed S5"
nohas idle '^keyboard_line:' "idle: no input needed for S5"
nohas idle '^screen: panel (off|on)' "idle: no unsupported panel writes"
if grep -aqE '^power: processor ' "${work}/idle.txt"; then echo "FAIL  idle: processor meter read under a hypervisor"; fails=$((fails + 1)); else echo "PASS  idle: no processor meter under a hypervisor"; fi
chk "idle: kernel alive" "${work}/idle.txt" '^kernel: alive$'

for r in hub direct none idle; do
    if grep -aqE '^(panic|power: still running|power: halted|power: ACPI S5 failed)' "${work}/${r}.txt"; then
        echo "FAIL  ${r}: panic or failed power off"; fails=$((fails + 1))
    else
        echo "PASS  ${r}: no panic"
    fi
done
for r in hub idle; do
    boots=$(grep -acE '^report_kind: pre_exit$' "${work}/${r}.txt" || true)
    if [[ ${boots} == 1 ]]; then echo "PASS  ${r}: one boot, no reboot"; else
        echo "FAIL  ${r}: expected one boot, saw ${boots}"; fails=$((fails + 1))
    fi
    echo "---- ${r} QMP power events ----"
    grep -aE '"event".*"(SHUTDOWN|RESET)"' "${work}/${r}.qmp.log" || true
done
for r in hub direct none idle; do echo "---- ${r} serial (keyboard, power and panel lines) ----"; grep -aE '^(pci: xhci|pci: display|xhci|usb|keyboard|power|screen: panel)' "${work}/${r}.txt" || true; done
if [[ ${fails} == 0 ]]; then
    echo "${tag}: PASS"
    exit 0
fi
echo "${tag}: FAIL (${fails})"
exit 1
