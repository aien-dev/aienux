#!/usr/bin/env bash
# contract_mutants.sh DEVC_BIN KERNEL_DIR OUT: wrong-but-well-formed edits of gb10_chan.devc. The checker accepts each
# (they break no ordering rule); the generated twin must then FAIL test_gpu_sim or differ in the differential trace.
# A mutant that passes both = a hole in the tests. Exit 0 only if every mutant is killed.
set -u
DEVC=$1 K=$2 OUT=$3
src=$K/gpu/contract/gb10_chan.devc
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
mkdir -p "$work/gpu/contract/gen"
cp "$src" "$work/orig.devc"
muts=(
  "TARGET=TARGET_SYS_COH|TARGET=1"
  "BIG_PAGE_64K=1|BIG_PAGE_64K=0"
  "LENGTH=nwords LEVEL=1|LENGTH=nwords LEVEL=0"
  "const USERD_GPPUT 0x8C|const USERD_GPPUT 0x88"
  "const RAMIN_PDB_WORD 128|const RAMIN_PDB_WORD 130"
  "format TOKEN 32 RUNLIST_ID 22:16|format TOKEN 32 RUNLIST_ID 23:17"
  "reg vf DOORBELL 0x30090 4 wo|reg vf DOORBELL 0x30094 4 wo"
  "LENGTH 62:42|LENGTH 63:43"
  "PTR_HI 51:32 place|PTR_HI 51:32"
  "  fence\n  bind|  bind\n  fence\n  bind"
)
killed=0 total=0
for m in "${muts[@]}"; do
  from=${m%%|*} to=${m#*|}; total=$((total + 1))
  if ! grep -qF "$(printf '%b' "$from" | head -1)" "$work/orig.devc"; then echo "MUTANT TEXT MISSING: $from"; exit 2; fi
  awk -v f="$(printf '%b' "$from")" -v t="$(printf '%b' "$to")" 'BEGIN{RS="\0"} {i=index($0,f); if(i) $0=substr($0,1,i-1) t substr($0,i+length(f)); printf "%s", $0}' "$work/orig.devc" > "$work/gb10_chan.devc"
  if ! "$DEVC" gen "$work/gb10_chan.devc" "$work/gen" > "$work/gen.log" 2>&1; then
    echo "mutant killed (checker): $from -> $to"; killed=$((killed + 1)); continue
  fi
  make -s -C "$K" OUT="$OUT" GPU_CONTRACT_C="$work/gen/gch_contract.c" \
    "$OUT/host/test_gpu_sim_contract" "$OUT/host/gpu_chan_trace_hand" "$OUT/host/gpu_chan_trace_contract" -B > "$work/mk.log" 2>&1 \
    || { echo "mutant killed (does not compile): $from -> $to"; killed=$((killed + 1)); continue; }
  if ! "$OUT/host/test_gpu_sim_contract" > /dev/null 2>&1; then echo "mutant killed (test_gpu_sim): $from -> $to"; killed=$((killed + 1)); continue; fi
  "$OUT/host/gpu_chan_trace_hand" > "$work/h.txt"; "$OUT/host/gpu_chan_trace_contract" > "$work/c.txt"
  if ! cmp -s "$work/h.txt" "$work/c.txt"; then echo "mutant killed (trace differs): $from -> $to"; killed=$((killed + 1)); continue; fi
  echo "MUTANT SURVIVED: $from -> $to"
done
echo "contract mutants: $killed of $total killed"
test "$killed" -eq "$total"
