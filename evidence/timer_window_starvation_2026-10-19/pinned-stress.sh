#!/bin/bash
# usage: pinned-stress.sh WORKTREE N HOGS TAG : N boot tests pinned to CPU 3 with HOGS busy loops on CPU 3 (same nice), counts FAILs
cd "$1" || exit 1
D=$HOME/workspace/hermes-desk/allen-logs/$4; mkdir -p "$D"; : > "$D/fails.txt"
for i in $(seq 1 "$3"); do timeout 1200 taskset -c 3 nice -n 19 sh -c 'while :; do :; done' & done
pass=0; fail=0
for i in $(seq 1 "$2"); do
  taskset -c 3 nice -n 19 bash scripts/qemu_ck_boot_test.sh > "$D/run-$i.txt" 2>&1
  rc=$?
  if [ $rc = 0 ]; then pass=$((pass+1)); else fail=$((fail+1)); echo "run $i rc=$rc" >> "$D/fails.txt"; grep -h "^FAIL" "$D/run-$i.txt" >> "$D/fails.txt"; fi
  grep -h "timer_irq\|timer_stats" "$D/run-$i.txt" | tr '\n' ' ' >> "$D/stats.txt"; echo >> "$D/stats.txt"
done
echo "$4 pass=$pass fail=$fail" >> $HOME/workspace/hermes-desk/allen-logs/summary.txt
kill $(jobs -p) 2>/dev/null
