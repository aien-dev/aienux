#!/usr/bin/env bash
# qemu_ck_allen_code_test.sh -- Allen sends a coding request to the Spark and
# shows the code that comes back, end to end, in QEMU.
#
# The test-computer kernel (native/kernel/x86) boots in QEMU with its second
# serial port (COM2) joined to the relay crates/allen-courier. Keys go in
# through the QEMU monitor. In Allen's normal chat:
#   "hello allen"                Allen answers himself (talk is not code)
#   "make me a calculator"       Allen says at once that he needs to code it and
#                                sends it to the Spark; the code comes back in
#                                pieces with a checksum and is shown (svc/allen_code.h)
#   "now add a square root key"  a change to the code on the screen (code-more);
#                                the reply is long and one piece is lost on the
#                                way, so the kernel asks AGAIN and the whole set comes
#   Page Down, End, Home, Up     the code scrolls; Escape puts it away
#   (the relay stops)            "write a snake game in python": the plain line at
#                                20 s, the give-up line after 30 s of silence
#   "goodbye"                    the chat goes on
# By default the relay answers code jobs from recorded replies
# (native/kernel/svc/tests/fixtures/code, --code-reply), so the code shown is
# compared with the recording line by line and by checksum. ALLEN_CODE_LIVE=1
# asks the language-model server instead (127.0.0.1:18091, shared: a busy 503
# waits exactly its Retry-After) with "make me a calculator in python", "now add
# a square root key" and "make me a calculator in c", and reports the times:
# keystroke to Allen's answer (ms) and request to code on the screen (s).
#
# Needs qemu-system-x86_64, OVMF, mtools, an x86_64 gcc and cargo (live: the
# model server up, otherwise NOT_RUN). Takes the gate lock like every QEMU test.
# Final line: AIENOS_ALLEN_CODE_QEMU: PASS | FAIL | NOT_RUN (reason)
set -u
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"
tag="AIENOS_ALLEN_CODE_QEMU"
code_fd="${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}"
vars_fd="${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}"
model="${ALLEN_MODEL:-127.0.0.1:18091}"
live="${ALLEN_CODE_LIVE:-0}"
answer_wait="${ALLEN_WAIT:-900}"
fx="${repo_root}/native/kernel/svc/tests/fixtures/code"
cross=""
if [ "$(uname -m)" != "x86_64" ]; then cross="x86_64-linux-gnu-"; fi
for t in qemu-system-x86_64 mformat mmd mcopy cargo curl "${cross}gcc" "${cross}ld"; do
    command -v "${t}" >/dev/null || { echo "${tag}: NOT_RUN (${t} missing)"; exit 2; }
done
[[ -r "${code_fd}" && -r "${vars_fd}" ]] || { echo "${tag}: NOT_RUN (no OVMF)"; exit 2; }
if [[ "${live}" == 1 ]]; then
    if curl -s --max-time 5 "http://${model}/health" | grep -q '"status":"ok"'; then
        echo "model server at ${model}: up"
    else
        echo "${tag}: NOT_RUN (no model server at ${model})"
        exit 2
    fi
fi
out="${repo_root}/target/native-kernel-allen-code"
[[ "${live}" == 1 ]] && out="${out}-live"
mkdir -p "${out}"
commit="$(git rev-parse HEAD 2>/dev/null || echo unknown)"
make -s -C native/kernel X86_CROSS="${cross}" OUT="${out}" AIENOS_COMMIT="${commit}" imac-kernel >/dev/null ||
    { echo "${tag}: FAIL (kernel does not build)"; exit 1; }
cargo build -q --release --offline -p allen-courier 2>"${out}/cargo.err" ||
    { echo "${tag}: FAIL (relay does not build)"; tail -5 "${out}/cargo.err"; exit 1; }
relay="${repo_root}/target/release/allen-courier"

. "${repo_root}/scripts/lib_gate_hold.sh"
if ! gh_quiet_check || ! gh_lock_take qemu_ck_allen_code_test "${AIENOS_GATE_MINUTES:-30}"; then
    echo "${tag}: NOT_RUN (${gh_why})"
    exit 3
fi
work="$(mktemp -d)"
pids=()
relay_pid=""
cleanup() {
    local p
    for p in "${pids[@]}"; do kill "${p}" 2>/dev/null || true; done
    cp "${work}/one.log" "${out}/serial.log" 2>/dev/null || true
    cp "${work}/relay.log" "${out}/relay.log" 2>/dev/null || true
    rm -rf "${work}"
    gh_lock_release
}
trap cleanup EXIT

img="${work}/esp.img"
truncate -s 64M "${img}"
mformat -i "${img}" -F ::
mmd -i "${img}" ::/EFI ::/EFI/BOOT
mcopy -i "${img}" "${out}/IMACKERN.EFI" ::/EFI/BOOT/BOOTX64.EFI

# the recorded replies: 1 the calculator, 2 the long change with piece 50 lost the first time
mkdir -p "${work}/replies"
cp "${fx}/short.txt" "${work}/replies/1.txt"
cp "${fx}/long.txt" "${work}/replies/2.txt"
echo 50 >"${work}/replies/2.drop"

fails=0
pass() { echo "PASS  $1"; }
flunk() { echo "FAIL  $1"; fails=$((fails + 1)); }
chk() { # description, file, pattern
    if grep -aqE -- "$3" "$2"; then pass "$1"; else flunk "$1 (no match for: $3)"; fi
}
txt() { tr -d '\r' <"${work}/one.log" 2>/dev/null || true; }
has() { txt | grep -aqE -- "$1"; }
count() { txt | grep -acE -- "$1"; }
wait_for() { # seconds pattern
    local i
    for ((i = 0; i < $1 * 5; i++)); do
        has "$2" && return 0
        sleep 0.2
    done
    return 1
}
wait_count() { # seconds pattern n: until the pattern has matched n times
    local i
    for ((i = 0; i < $1 * 5; i++)); do
        [[ "$(count "$2")" -ge "$3" ]] && return 0
        sleep 0.2
    done
    return 1
}
key() { echo "sendkey $1" >&3; sleep 0.3; }
say() { # text: typed through the monitor, then Enter (letters, digits, spaces)
    local text="$1" c k i
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
}
shot() { # name
    rm -f "${work}/$1.png"
    echo "screendump ${work}/$1.png -f png" >&3
    for _ in $(seq 1 50); do [[ -s "${work}/$1.png" ]] && break; sleep 0.2; done
    sleep 0.5
    [[ -s "${work}/$1.png" ]] && cp "${work}/$1.png" "${out}/$1.png"
}
# the code| lines the kernel echoed after its n-th "code shown"
shown() { txt | awk -v n="$1" '/^allen-code: code shown/{b++} b==n && /^code\|/{sub(/^code\|/,""); print}'; }
# the first 40 lines of a recording's code block
recorded() { awk 'f && /^```/{exit} f{print} /^```/{f=1}' "$1" | head -40; }

cp "${vars_fd}" "${work}/one.vars"
mkfifo "${work}/one.mon.in" "${work}/one.mon.out"
: >"${work}/one.log"
qemu-system-x86_64 -M q35 -accel tcg -cpu max -smp 2 -m 1024 \
    -drive if=pflash,format=raw,readonly=on,file="${code_fd}" \
    -drive if=pflash,format=raw,file="${work}/one.vars" \
    -drive if=none,id=esp,format=raw,file="${img}" -device ide-hd,drive=esp,bus=ide.0 \
    -device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0,port=1 \
    -chardev pipe,id=mon,path="${work}/one.mon" -mon chardev=mon,mode=readline \
    -display none -nic none \
    -serial file:"${work}/one.log" \
    -serial unix:"${work}/wire.sock",server=on,wait=off \
    -no-reboot 2>"${work}/one.err" &
pids+=($!)
cat "${work}/one.mon.out" >/dev/null &
pids+=($!)
if [[ "${live}" == 1 ]]; then
    "${relay}" --socket "${work}/wire.sock" --model "${model}" --work "${work}/answers" --log-dir "${work}/log" \
        --busy-wait "${answer_wait}" >"${work}/relay.log" 2>&1 &
else
    "${relay}" --socket "${work}/wire.sock" --model 127.0.0.1:9 --work "${work}/answers" --log-dir "${work}/log" \
        --code-reply "${work}/replies" >"${work}/relay.log" 2>&1 &
fi
relay_pid=$!
pids+=("${relay_pid}")

typed=0
if wait_for 300 '^keyboard: type a line and press Enter'; then
    exec 3>"${work}/one.mon.in"
    wait_for 30 '^courier: ready, the far side said hello' || true
    wait_for 60 '^screen: night: .* by 20 s after Allen woke' || true
    say "hello allen"
    typed=1
    wait_for 60 '^allen: Hello Humanity\.$' || true
    if [[ "${live}" == 1 ]]; then
        asks=("make me a calculator in python" "now add a square root key" "make me a calculator in c")
        for i in 0 1 2; do
            say "${asks[$i]}"
            wait_count "${answer_wait}" '^allen-code: (code shown|the Spark stopped|the Spark answered without|lost)' $((i + 1)) || true
            sleep 2
            shot "code-live-$((i + 1))"
            key pgdn
            sleep 1
            shot "code-live-$((i + 1))-pgdn"
        done
        key esc
    else
        say "make me a calculator"
        wait_count 120 '^allen-code: (code shown|the Spark stopped|lost)' 1 || true
        sleep 2
        shot code-short
        say "now add a square root key"
        wait_count 120 '^allen-code: (code shown|the Spark stopped|lost)' 2 || true
        sleep 2
        shot code-long
        key pgdn
        key pgdn
        sleep 1.5
        shot code-long-pgdn
        key end
        key home
        key down
        key up
        key esc
        wait_for 10 '^allen-code: code put away' || true
        sleep 1
        shot code-away
        # the Spark goes quiet: the relay stops
        kill "${relay_pid}" 2>/dev/null || true
        sleep 1
        say "write a snake game in python"
        wait_for 25 '^allen-code: the Spark is slow' || true
        shot code-waiting
        wait_for 20 '^allen-code: the Spark stopped answering' || true
        say "goodbye"
        wait_for 30 '^power: off via ACPI S5 ' || true
    fi
    exec 3>&-
fi
txt >"${work}/one.txt"
T="${work}/one.txt"

[[ ${typed} == 1 ]] && pass "keys typed from outside the guest" || flunk "the keyboard never came up"
chk "the far side said hello" "$T" '^courier: ready, the far side said hello$'
chk "talk is still talk: Allen answers the greeting himself" "$T" '^allen: Hello Humanity\.$'
if [[ "${live}" == 1 ]]; then
    chk "Allen says at once he needs to code it" "$T" '^allen: I need to code that\. Let me send it off to the Spark\.$'
    chk "the change goes as a change" "$T" '^allen: Let me take that change to the Spark\.$'
    [[ "$(count '^allen-code: code shown, [1-9][0-9]* lines in [0-9]+ ms$')" == 3 ]] && pass "three answers shown as code" ||
        flunk "not three answers shown as code ($(count '^allen-code: code shown'))"
    chk "a python calculator" "$T" '^allen-code: [0-9]+ bytes, crc32 [0-9a-f]{8}, [0-9]+ pieces, language python$'
    chk "a C calculator" "$T" '^allen-code: [0-9]+ bytes, crc32 [0-9a-f]{8}, [0-9]+ pieces, language c$'
    if shown 2 | grep -aqiE 'sqrt|square'; then pass "the change added a square root"; else flunk "no square root in the changed code"; fi
    chk "the relay asked the model for code" "${work}/relay.log" '^courier: serial job [0-9]+ \(code, '
    chk "the relay asked for a change with the code before" "${work}/relay.log" '^courier: serial job [0-9]+ \(code-more, '
else
    chk "Allen says at once he needs to code it" "$T" '^allen: I need to code that\. Let me send it off to the Spark\.$'
    chk "the request reported by length only" "$T" '^allen-code: asked the Spark \(20 chars\)$'
    chk "the acknowledgement timed" "$T" '^allen-code: acknowledged in [0-9]+ ms \(job 1, new code\)$'
    chk "the calculator shown" "$T" '^allen-code: code shown, 29 lines in [0-9]+ ms$'
    crc1=$("${relay}" --split "${fx}/short.txt" | head -1 | awk '{print $5}')
    crc2=$("${relay}" --split "${fx}/long.txt" | head -1 | awk '{print $5}')
    chk "its checksum is the recording's" "$T" "^allen-code: 768 bytes, crc32 ${crc1}, 6 pieces, language python$"
    if diff <(shown 1) <(recorded "${fx}/short.txt") >"${work}/d1"; then pass "the code read back is the recording, line by line"; else flunk "the code read back differs: $(head -3 "${work}/d1" | tr '\n' ' ')"; fi
    chk "Allen's own sentence after the code" "$T" '^allen: Here is a small calculator you can run with python3 calc\.py\.$'
    chk "the change goes as a change" "$T" '^allen: Let me take that change to the Spark\.$'
    chk "the relay left piece 50 off" "${work}/relay.log" '^courier: code job 2: piece 50 left off'
    chk "the kernel saw the piece missing and asked again" "$T" '^allen-code: a piece went missing, asked again$'
    chk "the relay sent the set again" "${work}/relay.log" '^courier: serial: code job 2 sent again \(143 lines\)$'
    chk "the long code shown whole" "$T" '^allen-code: code shown, 730 lines in [0-9]+ ms$'
    chk "its checksum is the recording's" "$T" "^allen-code: 18056 bytes, crc32 ${crc2}, 142 pieces, language c$"
    if diff <(shown 2) <(recorded "${fx}/long.txt") >"${work}/d2"; then pass "the long code read back is the recording"; else flunk "the long code read back differs: $(head -3 "${work}/d2" | tr '\n' ' ')"; fi
    pg=$(txt | grep -aE '^allen-code: rows ' | head -1)
    [[ "${pg}" =~ ^allen-code:\ rows\ ([0-9]+)\ to\ ([0-9]+)\ of\ ([0-9]+)\ shown$ ]] && (( BASH_REMATCH[1] > 1 )) &&
        pass "Page Down moves the code (${pg#allen-code: })" || flunk "Page Down did not move the code (${pg})"
    last=$(txt | grep -aE '^allen-code: rows ' | sed -n 3p)
    [[ "${last}" =~ to\ ([0-9]+)\ of\ ([0-9]+)\ shown$ ]] && [[ "${BASH_REMATCH[1]}" == "${BASH_REMATCH[2]}" ]] &&
        pass "End shows the last row (${last#allen-code: })" || flunk "End did not reach the end (${last})"
    chk "Home goes back to the top" "$T" '^allen-code: rows 1 to [0-9]+ of [0-9]+ shown$'
    chk "Escape puts the code away" "$T" '^allen-code: code put away$'
    chk "with the Spark quiet: the plain line at 20 s" "$T" '^allen: The Spark is still writing it\. Longer code takes a little while\.$'
    chk "and the give-up line after 30 s of silence" "$T" '^allen: The Spark stopped answering, so I couldn.t get the code\. Try again in a minute\.$'
    chk "the chat goes on after it" "$T" '^allen: Goodbye\. Until next time\.$'
    for f in code-short code-long code-long-pgdn code-away code-waiting; do
        [[ -s "${out}/${f}.png" ]] && pass "screenshot ${f}.png" || flunk "no screenshot ${f}.png"
    done
fi
# the request's words and the code never reach a line the lab report may carry
if grep -aE '^(screen|allen-code|courier): ' "$T" | grep -aqiE 'calculator|square|snake|import |#include'; then
    flunk "words of the request or code leaked into a report line"
else
    pass "report lines carry lengths and times only"
fi
if LC_ALL=C grep -aqE '^allen: .*[^ -~]' "$T"; then flunk "a character the typeface cannot draw reached Allen's words"; else pass "Allen's words are plain letters"; fi
echo "the kernel's times: $(grep -aE '^allen-code: (acknowledged|code shown)' "$T" | sed 's/^allen-code: //' | tr '\n' ';')"
echo "the relay: $(grep -aE '^courier: (serial job|code job)' "${work}/relay.log" | tr '\n' ';')"
echo "serial log: ${out}/serial.log; relay log: ${out}/relay.log; screenshots: ${out}/*.png"
if [[ ${fails} == 0 ]]; then echo "${tag}: PASS"; else echo "${tag}: FAIL (${fails})"; exit 1; fi
