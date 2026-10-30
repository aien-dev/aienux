#!/usr/bin/env bash
# qemu_ck_allen_wizard_test.sh -- Allen's setup wizard end to end in QEMU, on
# the three test machines of native/kernel/svc/tests/fixtures/setup.
#
# The launcher image (make imac-kernel CK_X86_HOSTDESC=1, IMACKERN-HOST.EFI)
# boots with the machine's host description handed over as the fw_cfg file
# opt/aienos/host, a disk the kernel may write Allen's section on, and keys
# typed from outside through the QEMU monitor:
#
#   games PC  "Play games smoothly" (1), the scan, then the power goes off at
#             the consent; the next start carries on there ("Welcome back"),
#             "Set it up", the games path, the one AI nearby picked by itself
#             after 10 s, ready; a third start shows no wizard at all;
#   AI PC     Enter past hello, the AI by its number (2), "Show me each one"
#             with one item skipped, two models here (the running one picked
#             by its number), ready moves on by itself;
#   the Spark the AI (2), "Not now", "Choose another", "Connect my own",
#             "Another service" at http://127.0.0.1:18091 with a made-up key:
#             the key goes down the second serial port to the relay
#             (crates/allen-courier), which makes the one test request.
#
# Checked: the wizard's log lines ("wizard: ..."), its records on the disk
# (llm.conf and consent, read back from the image), screenshots of the
# screens, and that the key appears in none of: the console log, the relay's
# log, the relay's work folder, the disk. The default image is built too and
# must carry no fw_cfg code (the Makefile's link check).
#
# Needs qemu-system-x86_64, OVMF, mtools, an x86_64 gcc, cargo, and the model
# server at 127.0.0.1:18091 (otherwise NOT_RUN). Takes the gate lock like
# every QEMU test. Final line: AIENOS_ALLEN_WIZARD: PASS | FAIL | NOT_RUN (reason)
set -u
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"
tag="AIENOS_ALLEN_WIZARD"
code_fd="${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}"
vars_fd="${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}"
server="${ALLEN_MODEL:-127.0.0.1:18091}"
fx="${repo_root}/native/kernel/svc/tests/fixtures/setup"
key="sk-test-5150-wizard"
cross=""
if [ "$(uname -m)" != "x86_64" ]; then cross="x86_64-linux-gnu-"; fi
for t in qemu-system-x86_64 mformat mmd mcopy cargo curl "${cross}gcc" "${cross}ld"; do
    command -v "${t}" >/dev/null || { echo "${tag}: NOT_RUN (${t} missing)"; exit 2; }
done
[[ -r "${code_fd}" && -r "${vars_fd}" ]] || { echo "${tag}: NOT_RUN (no OVMF)"; exit 2; }
if ! curl -s --max-time 5 "http://${server}/v1/models" | grep -q '"id"'; then
    echo "${tag}: NOT_RUN (no model server at ${server})"
    exit 2
fi
out="${repo_root}/target/native-kernel-allen-wizard"
mkdir -p "${out}"
commit="$(git rev-parse HEAD 2>/dev/null || echo unknown)"
make -s -C native/kernel X86_CROSS="${cross}" OUT="${out}" AIENOS_COMMIT="${commit}" imac-kernel >"${out}/make0.log" 2>&1 ||
    { echo "${tag}: FAIL (the default kernel does not build)"; tail -5 "${out}/make0.log"; exit 1; }
make -s -C native/kernel X86_CROSS="${cross}" OUT="${out}" AIENOS_COMMIT="${commit}" CK_X86_HOSTDESC=1 imac-kernel \
    >"${out}/make1.log" 2>&1 || { echo "${tag}: FAIL (the launcher kernel does not build)"; tail -5 "${out}/make1.log"; exit 1; }
make -s -C native/kernel OUT="${out}" gpt-image >/dev/null || { echo "${tag}: FAIL (gpt-image does not build)"; exit 1; }
gpt="${out}/host/ck_gpt_image"
cargo build -q --release --offline -p allen-courier 2>"${out}/cargo.err" ||
    { echo "${tag}: FAIL (relay does not build)"; tail -5 "${out}/cargo.err"; exit 1; }
relay="${repo_root}/target/release/allen-courier"

. "${repo_root}/scripts/lib_gate_hold.sh"
if ! gh_quiet_check || ! gh_lock_take qemu_ck_allen_wizard_test "${AIENOS_GATE_MINUTES:-40}"; then
    echo "${tag}: NOT_RUN (${gh_why})"
    exit 3
fi
work="$(mktemp -d)"
pids=()
cleanup() {
    local p f
    for p in "${pids[@]}"; do kill "${p}" 2>/dev/null || true; done
    for f in "${work}"/*.log; do [[ -e "${f}" ]] && cp "${f}" "${out}/" 2>/dev/null; done
    rm -rf "${work}"
    gh_lock_release
}
trap cleanup EXIT

esp="${work}/esp.img"
truncate -s 64M "${esp}"
mformat -i "${esp}" -F ::
mmd -i "${esp}" ::/EFI ::/EFI/BOOT
mcopy -i "${esp}" "${out}/IMACKERN-HOST.EFI" ::/EFI/BOOT/BOOTX64.EFI
for m in games ai spark; do "${gpt}" create "${work}/${m}.hdd" 512 2048 one-old >/dev/null; done

fails=0
pass() { echo "PASS  $1"; }
fail() { echo "FAIL  $1"; fails=$((fails + 1)); }
txt() { tr -d '\r' <"${work}/$1.log" 2>/dev/null || true; }
has() { txt "$1" | grep -aqE -- "$2"; }
chk() { # run, description, pattern
    if has "$1" "$3"; then pass "$1: $2"; else fail "$1: $2 (no match for: $3)"; fi
}
nochk() {
    if has "$1" "$3"; then fail "$1: $2 (unexpected: $3)"; else pass "$1: $2"; fi
}
wait_for() { # run seconds pattern
    local i
    for ((i = 0; i < $2 * 5; i++)); do
        has "$1" "$3" && return 0
        sleep 0.2
    done
    echo "      (waited $2 s for: $3)"
    return 1
}

# start run fixture disk wire(0|1): one boot; the console goes to <run>.log
start() {
    local run="$1" fixture="$2" disk="$3" wire="$4" extra=()
    cp "${vars_fd}" "${work}/${run}.vars"
    cp "${esp}" "${work}/${run}.esp"
    rm -f "${work}/${run}.mon.in" "${work}/${run}.mon.out"
    mkfifo "${work}/${run}.mon.in" "${work}/${run}.mon.out"
    : >"${work}/${run}.log"
    if [[ ${wire} == 1 ]]; then extra=(-serial unix:"${work}/${run}.sock",server=on,wait=off); fi
    qemu-system-x86_64 -M q35 -accel tcg -cpu max -smp 2 -m 1024 \
        -drive if=pflash,format=raw,readonly=on,file="${code_fd}" \
        -drive if=pflash,format=raw,file="${work}/${run}.vars" \
        -drive if=none,id=esp,format=raw,file="${work}/${run}.esp" -device ide-hd,drive=esp,bus=ide.0 \
        -drive if=none,id=hdd,format=raw,file="${disk}" \
        -device ide-hd,drive=hdd,bus=ide.1,model="APPLE HDD ST1000DM003" \
        -device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0,port=1 \
        -fw_cfg name=opt/aienos/host,file="${fixture}" \
        -chardev pipe,id=mon,path="${work}/${run}.mon" -mon chardev=mon,mode=readline \
        -display none -nic none -serial file:"${work}/${run}.log" "${extra[@]}" \
        -no-reboot 2>"${work}/${run}.err" &
    pids+=($!)
    eval "pid_${run}=$!"
    cat "${work}/${run}.mon.out" >/dev/null &
    pids+=($!)
    if [[ ${wire} == 1 ]]; then
        sleep 1
        "${relay}" --socket "${work}/${run}.sock" --model "${server}" --work "${work}/${run}.answers" \
            >"${work}/${run}-relay.log" 2>&1 &
        pids+=($!)
    fi
}
stop() { local v="pid_$1"; kill "${!v}" 2>/dev/null || true; wait "${!v}" 2>/dev/null || true; }
mon() { # run line...: monitor commands
    local run="$1" l
    shift
    exec 3>"${work}/${run}.mon.in"
    for l in "$@"; do echo "${l}" >&3; sleep 0.15; done
    exec 3>&-
}
keys() { # run qemu-key...
    local run="$1" k cmds=()
    shift
    for k in "$@"; do cmds+=("sendkey ${k}"); done
    mon "${run}" "${cmds[@]}"
}
type_text() { # run text: letters, digits, space and : / . -
    local run="$1" text="$2" c i ks=()
    for ((i = 0; i < ${#text}; i++)); do
        c="${text:i:1}"
        case "${c}" in
            " ") ks+=(spc) ;;
            [a-z0-9]) ks+=("${c}") ;;
            :) ks+=(shift-semicolon) ;;
            /) ks+=(slash) ;;
            .) ks+=(dot) ;;
            -) ks+=(minus) ;;
            *) echo "type_text: no key for '${c}'" >&2; exit 2 ;;
        esac
    done
    keys "${run}" "${ks[@]}"
}
shot() { # run name: a screenshot once Allen's words have settled
    sleep "${3:-3}"
    rm -f "${work}/shot.png"
    mon "$1" "screendump ${work}/shot.png -f png"
    for _ in $(seq 1 50); do [[ -s "${work}/shot.png" ]] && break; sleep 0.2; done
    sleep 0.5
    if [[ -s "${work}/shot.png" ]]; then cp "${work}/shot.png" "${out}/$2.png"; pass "$1: screenshot $2.png"; else fail "$1: no screenshot $2"; fi
}
# the record on the disk: the newer of the two AIENSET1 copies, as text
record() { # disk
    LC_ALL=C grep -aqF AIENSET1 "$1" || return 1
    local off best="" bg=-1 g
    for off in $(LC_ALL=C grep -aobE 'AIENSET1' "$1" | cut -d: -f1); do
        g=$(od -An -t u8 -j $((off + 16)) -N 8 "$1" | tr -d ' ')
        if ((g > bg)); then bg=${g}; best=${off}; fi
    done
    local n
    n=$(od -An -t u4 -j $((best + 12)) -N 4 "$1" | tr -d ' ')
    dd if="$1" bs=1 skip=$((best + 64)) count="${n}" 2>/dev/null
}
ready_re='^wizard: step intent$'

# ---- the games PC: a power cut at the consent, then a second and a third start ----
start games1 "${fx}/games-pc.txt" "${work}/games.hdd" 0
if wait_for games1 300 '^wizard: step hello$' && wait_for games1 30 "${ready_re}"; then
    shot games1 wizard-qemu-games-1-intent 2
    keys games1 1
    wait_for games1 20 '^wizard: step scan$'
    keys games1 ret
    wait_for games1 20 '^wizard: saved at consent '
fi
stop games1
chk games1 "the host description was read" '^wizard: host description [0-9]+ bytes, [0-9]+ items, 0 not understood$'
chk games1 "a first start says hello" '^wizard: step hello$'
chk games1 "the question by its number" '^wizard: intent games$'
chk games1 "saved after every step" '^wizard: saved at scan '
chk games1 "saved at the consent before the power went" '^wizard: saved at consent '

start games2 "${fx}/games-pc.txt" "${work}/games.hdd" 0
if wait_for games2 300 '^wizard: resumed at consent$' && wait_for games2 30 '^wizard: step consent$'; then
    shot games2 wizard-qemu-games-2-consent
    keys games2 ret
    wait_for games2 20 '^wizard: step games$'
    shot games2 wizard-qemu-games-3-games
    keys games2 ret
    wait_for games2 20 '^wizard: step llm$'
    wait_for games2 40 '^wizard: step ready$'
    shot games2 wizard-qemu-games-4-ready
    keys games2 ret
    wait_for games2 20 '^wizard: finished'
fi
stop games2
chk games2 "carries on at the consent after the power cut" '^wizard: resumed at consent$'
chk games2 "Set it up" '^wizard: consent all$'
chk games2 "the one AI nearby is picked after 10 s" '^wizard: llm auto-picked after 10 s$'
chk games2 "the peer is the AI" '^wizard: llm picked peer qwen3.8-27b-fp8$'
chk games2 "done" '^wizard: step done$'
chk games2 "saved as done" '^wizard: saved at done '
rec="$(record "${work}/games.hdd" || true)"
grep -qx 'step 16' <<<"${rec}" && pass "games: the disk says setup is done" || fail "games: the disk record: ${rec:0:80}"
grep -qx 'base_url http://127.0.0.1:18091' <<<"${rec}" && grep -qx 'key_env none' <<<"${rec}" &&
    pass "games: llm.conf on the disk names the peer" || fail "games: no llm.conf for the peer on the disk"
grep -qx 'screen-refresh yes' <<<"${rec}" && pass "games: the consent record is on the disk" || fail "games: no consent record"

start games3 "${fx}/games-pc.txt" "${work}/games.hdd" 0
wait_for games3 300 '^keyboard: type a line and press Enter' || true
stop games3
chk games3 "a finished setup is not shown again" '^wizard: setup finished on an earlier start'
nochk games3 "no hello from the wizard" '^wizard: step hello$'

# ---- the AI PC ----
start ai "${fx}/ai-pc.txt" "${work}/ai.hdd" 0
if wait_for ai 300 '^wizard: step hello$'; then
    sleep 1
    keys ai ret
    wait_for ai 20 "${ready_re}"
    keys ai 2
    wait_for ai 20 '^wizard: step scan$'
    keys ai ret
    wait_for ai 20 '^wizard: step consent$'
    keys ai down ret
    wait_for ai 20 '^wizard: step each$'
    keys ai ret
    sleep 1
    keys ai 2
    sleep 1
    keys ai ret
    wait_for ai 20 '^wizard: step ai$'
    shot ai wizard-qemu-ai-1-models
    keys ai ret
    wait_for ai 20 '^wizard: step llm$'
    shot ai wizard-qemu-ai-2-pick
    keys ai 1
    wait_for ai 20 '^wizard: step ready$'
    wait_for ai 40 '^wizard: step done$'
fi
stop ai
chk ai "Enter moves past hello" '^wizard: step intent$'
chk ai "the AI by its number" '^wizard: intent ai$'
chk ai "each one, one skipped" '^wizard: consent skip ai-gpu$'
chk ai "two found: chosen by hand" '^wizard: llm picked local qwen2.5-7b-instruct$'
nochk ai "two found: nothing picked by itself" '^wizard: llm auto-picked'
chk ai "ready moves on by itself" '^wizard: step done$'
rec="$(record "${work}/ai.hdd" || true)"
grep -qx 'base_url http://127.0.0.1:1234' <<<"${rec}" && pass "ai: llm.conf names the running model" || fail "ai: llm.conf: ${rec:0:120}"
grep -qx 'ai-gpu no' <<<"${rec}" && pass "ai: the skipped item is a no" || fail "ai: consent record"

# ---- the Spark: connect my own, the key test through the relay ----
start spark "${fx}/spark.txt" "${work}/spark.hdd" 1
if wait_for spark 300 "${ready_re}"; then
    wait_for spark 30 '^courier: ready, the far side said hello$'
    keys spark 2
    wait_for spark 20 '^wizard: step scan$'
    keys spark ret
    wait_for spark 20 '^wizard: step consent$'
    keys spark 3
    wait_for spark 20 '^wizard: step ai$'
    keys spark ret
    wait_for spark 20 '^wizard: step llm$'
    keys spark 2
    sleep 1
    keys spark 2
    wait_for spark 20 '^wizard: step service$'
    keys spark 3
    wait_for spark 20 '^wizard: step url$'
    type_text spark "http://${server}"
    keys spark ret
    wait_for spark 20 '^wizard: step key$'
    type_text spark "${key}"
    shot spark wizard-qemu-spark-1-key 1
    keys spark ret
    wait_for spark 60 '^wizard: step ready$'
    shot spark wizard-qemu-spark-2-ready
    keys spark ret
    wait_for spark 20 '^wizard: step done$'
fi
stop spark
chk spark "the wire to the host is up" '^courier: ready, the far side said hello$'
chk spark "the AI only" '^wizard: intent ai$'
chk spark "Not now" '^wizard: consent not now$'
chk spark "the key test went down the wire" '^wizard: key test 1 sent \(openai\)$'
chk spark "the key test passed" '^wizard: key test ok$'
chk spark "done" '^wizard: step done$'
if grep -aqE "^courier: key test 1 \(openai, ${#key} chars of key\) -> ok in [0-9]+ ms$" "${work}/spark-relay.log"; then
    pass "spark: the relay made the one request and printed only the key's length"
else
    fail "spark: the relay's key test line: $(grep -a 'key test' "${work}/spark-relay.log" | head -1)"
fi
rec="$(record "${work}/spark.hdd" || true)"
grep -qx "base_url http://${server}" <<<"${rec}" && grep -qx 'key_env AIENOS_LLM_KEY' <<<"${rec}" &&
    pass "spark: llm.conf names the service and where its key comes from" || fail "spark: llm.conf: ${rec:0:160}"

# ---- the key is nowhere ----
leak=""
for f in "${work}"/*.log; do LC_ALL=C grep -aqF "${key}" "${f}" && leak+=" $(basename "${f}")"; done
[[ -d "${work}/spark.answers" ]] && LC_ALL=C grep -raqF "${key}" "${work}/spark.answers" && leak+=" relay-work-folder"
for d in "${work}"/*.hdd; do LC_ALL=C grep -aqF "${key}" "${d}" && leak+=" $(basename "${d}")"; done
[[ -z "${leak}" ]] && pass "the key is in no log, no relay file and on no disk" || fail "the key leaked into:${leak}"
if grep -aE '^wizard: ' "${work}"/*.log | grep -aqE 'http://|127\.0\.0\.1'; then
    fail "an address typed into the wizard reached its log lines"
else
    pass "nothing typed reaches the wizard's log lines"
fi

# ---- the default image carries no fw_cfg ----
if [[ -s "${out}/IMACKERN.EFI" ]] && ! LC_ALL=C grep -aqE 'opt/aienos/host|QEMU0002' "${out}/IMACKERN.EFI"; then
    pass "the default image has no host description reader"
else
    fail "the default image carries fw_cfg strings"
fi
LC_ALL=C grep -aqF 'launcher image: the setup wizard reads the host description' "${out}/IMACKERN-HOST.EFI" &&
    pass "the launcher image says what it reads" || fail "the launcher image lacks its announcement"

echo "logs and screenshots: ${out}"
if [[ ${fails} == 0 ]]; then echo "${tag}: PASS"; else echo "${tag}: FAIL (${fails})"; exit 1; fi
