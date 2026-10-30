#!/usr/bin/env bash
# L332 rung 0, one spark-window:
#  cell (a) idle control, Omega launch-cost test unprobed/probed/strace at two rep counts, idle again;
#  cell (b) the live 35B server on 18091: attach perf+bpftrace (no ptrace) while one fixed greedy request decodes, and an
#           attached idle control of the same length. The server is never restarted or signalled.
set -u
here=$(cd "$(dirname "$0")" && pwd)
BIN=${BIN:-$HOME/workspace/targets/l332-omega/gpu_launch_cost_test}
OUT=${OUT:-$here/a-$(date -u +%Y%m%dT%H%M%SZ)}
PORT=${PORT:-18091}
mkdir -p "$OUT"
while [ ! -x "$BIN" ]; do sleep 30; done
sha256sum "$BIN" > "$OUT/bin.sha256"
ledger note L332 "measure: in line for spark-window l332-rung0 ($OUT)" >/dev/null 2>&1
spark-window open l332-rung0 --ledger L332 --gb10 exclusive --lease 20 --wait > "$OUT/window.open" 2>&1 || { echo "window open failed" >> "$OUT/window.open"; exit 1; }
R="spark-window run $here/rung0.sh $OUT"
$R idle idle1 30
$R run  run-r3  -- "$BIN" --reps 3
$R run  run-r30 -- "$BIN" --reps 30
$R probe probe-r3  -- "$BIN" --reps 3
$R probe probe-r30 -- "$BIN" --reps 30
$R strace strace-r3 -- "$BIN" --reps 3
$R idle idle2 30
spark-window close > "$OUT/window.close" 2>&1
# cell (b): own window without the chip lock (the server takes /tmp/aien-gb10.lock per request itself), quiet lock for timing
spark-window open l332-rung0b --ledger L332 --gb10 none --lease 20 --wait > "$OUT/window-b.open" 2>&1 || { echo "window b open failed" >> "$OUT/window-b.open"; exit 1; }
# cell (b): 35B decode on the live server
SPID=$(ss -ltnpH "sport = :$PORT" | grep -o "pid=[0-9]*" | head -1 | cut -d= -f2)
if [ -n "$SPID" ]; then
  tr "\0" " " < /proc/$SPID/cmdline > "$OUT/server.cmdline"
  curl -s -m 5 127.0.0.1:$PORT/v1/models > "$OUT/server.models.json"
  MODEL=$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["data"][0]["id"])' "$OUT/server.models.json")
  LOG=$(readlink /proc/$SPID/fd/1); wc -l < "$LOG" > "$OUT/server.log.line0"
  SECS=45
  $R attach srv-idle "$SPID" "$SECS"
  BODY='{"model":"'"$MODEL"'","temperature":0,"max_tokens":4096,"messages":[{"role":"user","content":"Write a long, detailed history of the bicycle, chapter by chapter."}]}'
  ( sleep 4; date +%s%N > "$OUT/srv-decode.req_start_ns"
    curl -s -m $((SECS-6)) 127.0.0.1:$PORT/v1/chat/completions -H "content-type: application/json" -d "$BODY" \
      > "$OUT/srv-decode.response.json"; echo "curl_exit $?" > "$OUT/srv-decode.curl"; date +%s%N > "$OUT/srv-decode.req_done_ns" ) &
  $R attach srv-decode "$SPID" "$SECS"
  wait
  tail -n +$(( $(cat "$OUT/server.log.line0") + 1 )) "$LOG" > "$OUT/server.log.window"
else
  echo "no listener on $PORT" > "$OUT/server.absent"
fi
spark-window close > "$OUT/window-b.close" 2>&1
sudo find "$OUT" -maxdepth 1 -name '*.perf.data*' -delete; sudo chown -R "$(id -un)" "$OUT"
ledger note L332 "measure: rung0 window done, results $OUT" >/dev/null 2>&1
echo DONE > "$OUT/DONE"
