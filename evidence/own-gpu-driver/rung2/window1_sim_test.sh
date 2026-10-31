#!/usr/bin/env bash
# window1_sim_test.sh: host check of the window-1 start-up tool (native/linux/aien-gpu/aien_gsp_boot.c) built against the
# simulated SEC2 + GSP and a fake GSP on the status queue (-DAIEN_GSP_BOOT_SIM). No GPU, no module.
#   1. every verdict path: INIT-DONE, sequencer request, no answer, corrupt queue, SEC2 busy, FMC error, core dead,
#      plan only, a package with a wrong-size file, an arena too small for the plan;
#   2. every register the tool touched in those runs is on the module's allowlist (aien_gpu.c AIEN-GPU-ALLOW block):
#      reads on the read list, writes on the write list, and one SEC2 doorbell per start-up.
# Usage: window1_sim_test.sh SIM_BINARY AIEN_GPU_C
set -u
BIN=$1 SHIM=$2
T=$(mktemp -d)
trap 'rm -r "$T"' EXIT
fail=0
ok() { echo "PASS $*"; }
bad() { echo "FAIL $*"; fail=1; }

# fake package: fixed bytes of the right sizes (the simulator checks none of the firmware content)
P=$T/pkg; mkdir "$P"
mk() { head -c "$2" /dev/zero | tr '\0' "$3" > "$P/$1"; }
mk rm_elf.bin 3000000 '\001'; mk rm_sig.bin 4096 '\002'; mk bootloader.bin 196608 '\003'; mk fmc.bin 9000 '\004'
mk fmc_hash.bin 48 '\005'; mk fmc_pkey.bin 384 '\006'; mk fmc_sig.bin 384 '\007'; mk system_info.bin 936 '\010'

run() { # name expected-verdict expected-exit args...
    local name=$1 want=$2 wexit=$3; shift 3
    local out rc
    out=$("$BIN" --pkg "$P" --app-version 7 --out "$T/$name" --polls 50 --pause-us 0 --wait-ms 300 "$@" 2>"$T/$name.err")
    rc=$?
    if [[ "$out" == *"AIEN_GSP_BOOT: $want"* && $rc -eq $wexit ]]; then ok "$name: $want (exit $rc)"
    else bad "$name: got '$out' exit $rc, want '$want' exit $wexit ($(head -c 300 "$T/$name.err"))"; fi
}
run init      "INIT-DONE"             0 --sim-events init
run seq       "SEQUENCER-REQUESTED"   1 --sim-events seq
run seqregs   "INIT-DONE"             0 --sim-events seq-regs
run silent    "NO-INIT-DONE"          1 --sim-events none
run junk      "STATUS-QUEUE-ERROR"    1 --sim-events junk
run busy      "STOPPED-AT E_BUSY"     1 --sim-cfg busy
run fmcerr    "STOPPED-AT E_FMC"      1 --sim-cfg fmc-error
run dead      "STOPPED-AT E_RISCV"    1 --sim-cfg riscv-dead
run planonly  "PLAN-ONLY"             0 --plan-only
run small     "BUILD-REFUSED"         2 --sim-arena-mib 2
[[ ! -s "$T/small/regs.txt" ]] && ok "arena too small: no register touched" || bad "arena too small touched registers"

# the INIT-DONE run kept its evidence
for f in plan.txt result.txt messages.txt regs.txt cot_packet.bin log-LOGINIT.bin rpc-shared.bin; do
    [[ -s "$T/init/$f" ]] && ok "init wrote $f" || bad "init did not write $f"
done
grep -q "function=0x1001" "$T/init/messages.txt" && ok "INIT_DONE recorded in messages.txt" || bad "INIT_DONE not in messages.txt"
[[ $(stat -c %s "$T/init/cot_packet.bin") -eq 868 ]] && ok "COT packet 868 bytes" || bad "COT packet size"
grep -q "verdict=INIT-DONE" "$T/init/result.txt" && ok "result.txt carries the verdict" || bad "result.txt verdict"

# notify page: a package value is sent as is; a zero field gets the first arena page (sim arena pa 0x4000000000)
q64() { od -An -tx8 -j48 -N8 "$1" | tr -d ' '; }
[[ $(q64 "$T/init/system_info.sent.bin") == 0808080808080808 ]] && ok "package notify address sent unchanged" || bad "notify field changed: $(q64 "$T/init/system_info.sent.bin")"
cp -r "$P" "$T/pkg3"; printf '\0\0\0\0\0\0\0\0' | dd of="$T/pkg3/system_info.bin" bs=1 seek=48 conv=notrunc status=none
"$BIN" --pkg "$T/pkg3" --app-version 7 --out "$T/notify" --plan-only > /dev/null 2>&1
[[ $(q64 "$T/notify/system_info.sent.bin") == 0000004000000000 ]] && grep -q "notify_page pa=0x4000000000" "$T/notify/plan.txt" \
    && ok "zero notify field filled with the zeroed first arena page" || bad "notify page: $(q64 "$T/notify/system_info.sent.bin")"
cmp -s <(head -c 48 "$T/pkg3/system_info.bin") <(head -c 48 "$T/notify/system_info.sent.bin") \
    && cmp -s <(tail -c +57 "$T/pkg3/system_info.bin") <(tail -c +57 "$T/notify/system_info.sent.bin") \
    && ok "no other GspSystemInfo byte changed" || bad "notify patch touched other bytes"

# refusal before any register access: a 935-byte GspSystemInfo
cp -r "$P" "$T/pkg2"; head -c 935 "$P/system_info.bin" > "$T/pkg2/system_info.bin"
out=$("$BIN" --pkg "$T/pkg2" --app-version 7 --out "$T/short" 2>/dev/null); rc=$?
[[ "$out" == *"PACKAGE-REFUSED"* && $rc -eq 2 && ! -s "$T/short/regs.txt" ]] && ok "935-byte GspSystemInfo refused, no register touched" || bad "short system_info: '$out' rc $rc"

# allowlist: the module must let through what the tool uses, and nothing else it writes
sed -n '/AIEN-GPU-ALLOW-BEGIN/,/AIEN-GPU-ALLOW-END/p' "$SHIM" > "$T/allow.c"
rdl=$(sed -n '/aien_gpu_rd_ok/,/};/p' "$T/allow.c" | grep -o '0x[0-9a-f]\{8\}' | sed 's/^0x00//' | sort -u)
wrl=$(sed -n '/aien_gpu_wr_ok/,/};/p' "$T/allow.c" | grep -o '0x[0-9a-f]\{8\}' | sed 's/^0x00//' | sort -u)
used_r=$(cat "$T"/*/regs.txt | awk '$1=="R"{print $2}' | sort -u)
used_w=$(cat "$T"/*/regs.txt | awk '$1=="W"{print $2}' | sort -u)
miss_r=$(comm -23 <(echo "$used_r") <(echo "$rdl")); miss_w=$(comm -23 <(echo "$used_w") <(echo "$wrl"))
[[ -n "$rdl" && -z "$miss_r" ]] && ok "every read the tool made is on the module read list ($(echo "$used_r" | wc -l) registers)" || bad "reads not allowed: $miss_r"
[[ -n "$wrl" && -z "$miss_w" ]] && ok "every write the tool made is on the module write list ($(echo "$used_w" | wc -l) registers)" || bad "writes not allowed: $miss_w"
extra_w=$(comm -13 <(echo "$used_w") <(echo "$wrl") | tr '\n' ' ')
[[ -z "$extra_w" ]] && ok "every write-allowlist register is used by some path" || bad "write list extras: '$extra_w'"
init_qh=$(awk '$1=="W" && $2=="110c00"' "$T/init/regs.txt" | wc -l)
[[ $init_qh -eq 0 ]] && ok "start-up itself does not write GSP QHEAD0" || bad "init wrote QHEAD0 $init_qh times"
seq_qh=$(awk '$1=="W" && $2=="110c00"' "$T/seqregs/regs.txt" | wc -l)
[[ $seq_qh -ge 1 ]] && ok "sequencer path wrote GSP QHEAD0" || bad "seqregs did not write QHEAD0"
doorbells=$(awk '$1=="W" && $2=="840c00"' "$T/init/regs.txt" | wc -l)
[[ $doorbells -eq 1 ]] && ok "one SEC2 doorbell per start-up" || bad "doorbells in one start-up: $doorbells"

echo "WINDOW1_SIM_TEST: $([[ $fail -eq 0 ]] && echo PASS || echo FAIL)"
exit $fail
