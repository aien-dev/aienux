#!/usr/bin/env bash
# gsp_contract_mutants.sh DEVC KERNEL_DIR: wrong-but-well-formed edits of gpu/contract/gb10_gsp.devc (offsets, bits,
# constants). The checker may accept them (they break no ordering rule); the generated twin must then FAIL
# test_gsp_mmio or differ from the hand twin in gsp_boot_trace. A mutant that survives = a hole. Exit 0 only if all die.
set -u
DEVC=$1 K=$2
CC=${CC:-cc}
src=$K/gpu/contract/gb10_gsp.devc
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
cc_() { $CC -std=c11 -O1 -I"$K/gpu" -I"$K/tests" -o "$1" "${@:2}" "$K/gpu/gsp_sim.c" > "$work/cc.log" 2>&1; }
cc_ "$work/trace_hand" "$K/tests/gsp_boot_trace.c" "$K/gpu/gsp_mmio.c" && "$work/trace_hand" > "$work/h.txt" || { echo "BASELINE FAILS"; exit 2; }
"$DEVC" gen "$src" "$work/gen" > /dev/null && cc_ "$work/t" "$K/tests/test_gsp_mmio.c" "$work/gen/gsb_contract.c" && "$work/t" > /dev/null &&
  cc_ "$work/tr" "$K/tests/gsp_boot_trace.c" "$work/gen/gsb_contract.c" && "$work/tr" | cmp -s - "$work/h.txt" || { echo "BASELINE CONTRACT FAILS"; exit 2; }
muts=(
  "reg sec2 QTAIL0 0xc04|reg sec2 QTAIL0 0xc0c"
  "reg sec2 EMEMD0 0xac4|reg sec2 EMEMD0 0xacc"
  "reg sec2 SEC2_MAILBOX0 0x040|reg sec2 SEC2_MAILBOX0 0x048"
  "reg gsp GSP_MAILBOX1 0x044|reg gsp GSP_MAILBOX1 0x04c"
  "reg gsp FALCON_OS 0x080|reg gsp FALCON_OS 0x084"
  "reg gsp HWCFG2 0x0f4|reg gsp HWCFG2 0x0f8"
  "reg gsp CPUCTL 0x1388|reg gsp CPUCTL 0x1384"
  "reg gsp GSP_QHEAD0 0xc00|reg gsp GSP_QHEAD0 0xc08"
  "window sec2 bar 0 base 0x840000|window sec2 bar 0 base 0x841000"
  "window gsp bar 0 base 0x110000|window gsp bar 0 base 0x114000"
  "bit LOCKDOWN HWCFG2 13|bit LOCKDOWN HWCFG2 12"
  "bit ACTIVE CPUCTL 7|bit ACTIVE CPUCTL 6"
  "bit AINCW EMEMC0 24|bit AINCW EMEMC0 25"
  "const SEC2_CMD_BUSY 0xc001cafe|const SEC2_CMD_BUSY 0xc001caff"
  "const PRIV_LOCKED_MASK 0xFFFFFF00|const PRIV_LOCKED_MASK 0xFFFFFFF0"
  "const EMEM_CHANNEL_BYTES 1024|const EMEM_CHANNEL_BYTES 1020"
  "budget polls_can_send else E_BUSY|budget polls_can_send else E_EMEM"
  "write FALCON_OS app_version|write FALCON_OS 0"
  "write GSP_QHEAD0 0|write GSP_QHEAD0 1"
  "into sec2_mb0 timeout-read SEC2_MAILBOX1 sec2_mb1|into sec2_mb0"
)
killed=0 total=0
for m in "${muts[@]}"; do
  from=${m%%|*} to=${m#*|}; total=$((total + 1))
  F=$from T=$to perl -0777 -ne '$i = index($_, $ENV{F}); exit 3 if $i < 0; substr($_, $i, length $ENV{F}) = $ENV{T}; print' \
    "$src" > "$work/gb10_gsp.devc" || { echo "MUTANT TEXT MISSING: $from"; exit 2; }
  rm -rf "$work/gen"
  if ! "$DEVC" gen "$work/gb10_gsp.devc" "$work/gen" > "$work/gen.log" 2>&1; then
    echo "mutant killed (checker): $from -> $to"; killed=$((killed + 1)); continue
  fi
  if ! cc_ "$work/t" "$K/tests/test_gsp_mmio.c" "$work/gen/gsb_contract.c" || ! cc_ "$work/tr" "$K/tests/gsp_boot_trace.c" "$work/gen/gsb_contract.c"; then
    echo "mutant killed (does not compile): $from -> $to"; killed=$((killed + 1)); continue
  fi
  if ! "$work/t" > /dev/null 2>&1; then echo "mutant killed (test_gsp_mmio): $from -> $to"; killed=$((killed + 1)); continue; fi
  "$work/tr" > "$work/c.txt"
  if ! cmp -s "$work/h.txt" "$work/c.txt"; then echo "mutant killed (trace differs): $from -> $to"; killed=$((killed + 1)); continue; fi
  echo "MUTANT SURVIVED: $from -> $to"
done
echo "gsp contract mutants: $killed of $total killed"
test "$killed" -eq "$total"
