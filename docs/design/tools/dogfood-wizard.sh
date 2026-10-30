#!/usr/bin/env bash
# dogfood-wizard.sh -- use Allen's setup wizard (L267) the way a person would, in QEMU only, and keep what they
# would see and hear: a screenshot of every screen (and one every 2 s while the night plays), the sound as a
# .wav, and when each screen arrived. Six first runs: both intents (games, AI) times the three ways of getting
# an AI (a model on this computer, an AienOS computer nearby, connect my own service through the relay).
#
#   quiet-build bash docs/design/tools/dogfood-wizard.sh SRC_DIR OUT_DIR
#
# SRC_DIR is a clean copy of the wizard's branch (git archive of a pushed commit), never someone's worktree.
# It is built there with the wizard's own make targets. Output per run: OUT_DIR/<run>/NN-<step>.png,
# boot-TTT.png, sound.wav, console.log, relay.log, steps.tsv (seconds since power-on, step), and
# OUT_DIR/summary.tsv. Exit 0 when every run reached "done", 1 otherwise, 2 when it could not run.
set -u
src="$(cd "${1:?SRC_DIR}" && pwd)"
out="${2:?OUT_DIR}"
mkdir -p "${out}"
out="$(cd "${out}" && pwd)"
code_fd="${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}"
vars_fd="${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}"
server="${ALLEN_MODEL:-127.0.0.1:18091}"
key="sk-Dogfood_0000-not-real"
cross=""
[[ "$(uname -m)" != "x86_64" ]] && cross="x86_64-linux-gnu-"
for t in qemu-system-x86_64 mformat mmd mcopy cargo curl "${cross}gcc"; do
    command -v "${t}" >/dev/null || { echo "dogfood: NOT_RUN (${t} missing)"; exit 2; }
done
curl -s --max-time 5 "http://${server}/v1/models" | grep -q '"id"' || { echo "dogfood: NOT_RUN (no model server at ${server})"; exit 2; }

cd "${src}"
build="${out}/build"
mkdir -p "${build}"
make -s -C native/kernel X86_CROSS="${cross}" OUT="${build}" AIENOS_COMMIT="$(cat "${src}/.commit" 2>/dev/null || echo unknown)" \
    CK_X86_HOSTDESC=1 imac-kernel >"${build}/make.log" 2>&1 || { echo "dogfood: FAIL (launcher kernel does not build)"; tail -5 "${build}/make.log"; exit 1; }
make -s -C native/kernel OUT="${build}" gpt-image >>"${build}/make.log" 2>&1 || { echo "dogfood: FAIL (gpt-image)"; exit 1; }
CARGO_TARGET_DIR="${build}/cargo" cargo build -q --release --offline -p allen-courier 2>"${build}/cargo.err" ||
    { echo "dogfood: FAIL (relay does not build)"; tail -5 "${build}/cargo.err"; exit 1; }
relay="${build}/cargo/release/allen-courier"
fx="${src}/native/kernel/svc/tests/fixtures/setup"

. "${src}/scripts/lib_gate_hold.sh"
if ! gh_quiet_check || ! gh_lock_take dogfood_wizard_l282 "${AIENOS_GATE_MINUTES:-60}"; then
    echo "dogfood: NOT_RUN (${gh_why})"
    exit 3
fi
work="$(mktemp -d)"
qpid="" rpid="" cpid=""
cleanup() {
    for p in ${qpid} ${rpid} ${cpid}; do kill "${p}" 2>/dev/null || true; done
    rm -rf "${work}"
    gh_lock_release
}
trap cleanup EXIT

esp="${work}/esp.img"
truncate -s 64M "${esp}"
mformat -i "${esp}" -F ::
mmd -i "${esp}" ::/EFI ::/EFI/BOOT
mcopy -i "${esp}" "${build}/IMACKERN-HOST.EFI" ::/EFI/BOOT/BOOTX64.EFI

t0=0
now() { awk -v a="$(date +%s.%N)" -v b="${t0}" 'BEGIN { printf "%.1f", a - b }'; }
mon() { local l; exec 3>"${work}/mon.in"; for l in "$@"; do echo "${l}" >&3; sleep 0.15; done; exec 3>&-; }
keys() { local k c=(); for k in "$@"; do c+=("sendkey ${k}"); done; mon "${c[@]}"; echo "$(now)	keys $*" >>"${dir}/steps.tsv"; }
type_text() {
    local c i ks=()
    for ((i = 0; i < ${#1}; i++)); do
        c="${1:i:1}"
        case "${c}" in
            " ") ks+=(spc) ;; [a-z0-9]) ks+=("${c}") ;; :) ks+=(shift-semicolon) ;; /) ks+=(slash) ;;
            .) ks+=(dot) ;; -) ks+=(minus) ;; _) ks+=(shift-minus) ;; [A-Z]) ks+=("shift-${c,,}") ;; *) echo "type_text: no key for '${c}'" >&2; return 1 ;;
        esac
    done
    keys "${ks[@]}"
}
shot() { # name
    rm -f "${work}/shot.png"
    mon "screendump ${work}/shot.png -f png"
    # QEMU writes the png slowly under emulation: take it only once the file ends with its closing chunk (IEND)
    for _ in $(seq 1 100); do
        [[ -s "${work}/shot.png" ]] && tail -c 4 "${work}/shot.png" | xxd -p | grep -q '^ae426082$' && break
        sleep 0.2
    done
    if tail -c 4 "${work}/shot.png" 2>/dev/null | xxd -p | grep -q '^ae426082$'; then
        cp "${work}/shot.png" "${dir}/$1.png"
    else
        echo "$(now)	shot $1 incomplete, dropped" >>"${dir}/steps.tsv"
    fi
}
step() { tr -d '\r' <"${dir}/console.log" | grep -aoE '^wizard: step [a-z]+' | tail -1 | cut -d' ' -f3; }
wait_step() { # seconds: wait for a step other than ${cur}
    local i s
    for ((i = 0; i < $1 * 5; i++)); do s="$(step)"; [[ -n "${s}" && "${s}" != "${cur}" ]] && { echo "${s}"; return 0; }; sleep 0.2; done
    echo "${cur}"
}

# run name fixture intent(games|ai) source(local|peer|own) wire(0|1)
run() {
    local name="$1" fixture="$2" intent="$3" source="$4" wire="$5" n=0 s cur="" boot=0 extra=() stuck=0
    dir="${out}/${name}"
    mkdir -p "${dir}"
    : >"${dir}/console.log"
    printf 'seconds\tevent\n' >"${dir}/steps.tsv"
    cp "${vars_fd}" "${work}/vars"
    cp "${esp}" "${work}/esp"
    "${build}/host/ck_gpt_image" create "${work}/disk.hdd" 512 2048 one-old >/dev/null
    rm -f "${work}/mon.in" "${work}/mon.out"
    mkfifo "${work}/mon.in" "${work}/mon.out"
    [[ ${wire} == 1 ]] && extra=(-serial unix:"${work}/wire.sock",server=on,wait=off)
    t0="$(date +%s.%N)"
    qemu-system-x86_64 -M q35 -accel tcg -cpu max -smp 2 -m 1024 \
        -drive if=pflash,format=raw,readonly=on,file="${code_fd}" \
        -drive if=pflash,format=raw,file="${work}/vars" \
        -drive if=none,id=esp,format=raw,file="${work}/esp" -device ide-hd,drive=esp,bus=ide.0 \
        -drive if=none,id=hdd,format=raw,file="${work}/disk.hdd" \
        -device ide-hd,drive=hdd,bus=ide.1,model="APPLE HDD ST1000DM003" \
        -device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0,port=1 \
        -audiodev wav,id=snd,path="${dir}/sound.wav" -device ich9-intel-hda -device hda-output,audiodev=snd \
        -fw_cfg name=opt/aienos/host,file="${fixture}" \
        -chardev pipe,id=mon,path="${work}/mon" -mon chardev=mon,mode=readline \
        -display none -nic none -serial file:"${dir}/console.log" "${extra[@]}" \
        -no-reboot 2>"${dir}/qemu.err" &
    qpid=$!
    cat "${work}/mon.out" >/dev/null &
    cpid=$!
    if [[ ${wire} == 1 ]]; then
        sleep 1
        "${relay}" --socket "${work}/wire.sock" --model "${server}" --work "${work}/answers" >"${dir}/relay.log" 2>&1 &
        rpid=$!
    fi
    # the night and Allen's arrival: a picture every 2 s until the wizard's first screen (at most 300 s)
    while [[ -z "$(step)" ]] && ((boot < 300)); do
        sleep 2; boot=$((boot + 2)); shot "boot-$(printf %03d ${boot})"
        kill -0 "${qpid}" 2>/dev/null || break
    done
    cur=""
    while :; do
        s="$(wait_step 30)"
        if [[ "${s}" == "${cur}" ]]; then
            stuck=1; echo "$(now)	stuck at ${cur} (no new step in 30 s)" >>"${dir}/steps.tsv"; shot "$(printf %02d ${n})-stuck-${cur}"; break
        fi
        cur="${s}"; n=$((n + 1))
        echo "$(now)	step ${cur}" >>"${dir}/steps.tsv"
        sleep 3; shot "$(printf %02d ${n})-${cur}"   # after the words have eased in
        case "${cur}:${intent}:${source}" in
            hello:*) s="$(wait_step 12)"; [[ "${s}" == "${cur}" ]] && keys ret ;;     # does hello move on by itself?
            intent:games:*) keys 1 ;;
            intent:ai:*) keys down; sleep 1; shot "$(printf %02d ${n})-${cur}-down"; keys ret ;;  # the arrow path
            scan:* | consent:* | each:* | games:* | ai:*) keys ret ;;
            llm:*:peer) s="$(wait_step 15)" ;;                                      # one AI nearby: picked by itself
            llm:*:local) keys 1 ;;
            llm:*:own) keys 2; sleep 1; shot "$(printf %02d ${n})-${cur}-another"; keys 2 ;;
            service:*) keys 3 ;;
            url:*) type_text "http://${server}"; sleep 1; shot "$(printf %02d ${n})-${cur}-typed"; keys ret ;;
            key:*) type_text "${key}"; sleep 1; shot "$(printf %02d ${n})-${cur}-typed"; keys ret ;;
            ready:*) keys ret ;;
            done:*) sleep 4; shot "$(printf %02d ${n})-after-done"; break ;;
            *) keys ret ;;
        esac
    done
    kill "${qpid}" "${cpid}" ${rpid} 2>/dev/null; wait "${qpid}" 2>/dev/null; rpid=""
    grep -aqF "${key}" "${dir}/console.log" "${dir}/relay.log" 2>/dev/null && echo "$(now)	KEY LEAKED into a log" >>"${dir}/steps.tsv"
    printf '%s\t%s\t%s\t%s\t%s\t%s\n' "${name}" "${intent}" "${source}" "${cur}" "${stuck}" "$(stat -c %s "${dir}/sound.wav" 2>/dev/null || echo 0)" >>"${out}/summary.tsv"
}

printf 'run\tintent\tsource\tlast_step\tstuck\twav_bytes\n' >"${out}/summary.tsv"
run games-peer  "${fx}/games-pc.txt" games peer  0
run games-local "${fx}/ai-pc.txt"    games local 0
run games-own   "${fx}/spark.txt"    games own   1
run ai-local    "${fx}/ai-pc.txt"    ai    local 0
run ai-peer     "${fx}/games-pc.txt" ai    peer  0
run ai-own      "${fx}/spark.txt"    ai    own   1
cat "${out}/summary.tsv"
awk -F'\t' 'NR > 1 && $4 != "done" { bad = 1 } END { exit bad }' "${out}/summary.tsv"
