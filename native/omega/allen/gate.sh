#!/bin/sh
# External build/output comparison only. All behavioral tests execute Omega ELFs.
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/../../.." && pwd)
cd "$root"
out=$1
mkdir -p "$out"
[ -z "$(ls -A "$out")" ] || { echo 'gate requires fresh output directory' >&2; exit 73; }
compiler=${OMEGA_COMPILER:-"$root/build/allen-toolchain/seed/out/omc"}
sh native/omega/allen/test-build.sh "$compiler" "$out/build-checks"
core="$out/core.om"
cat native/omega/allen/base.om native/omega/allen/vendor/sha256-core.om \
 native/omega/allen/runtime.om native/omega/allen/code.om \
 native/omega/allen/courier.om native/omega/allen/scheduler.om native/omega/allen/memory.om \
 native/omega/allen/intent.om native/omega/allen/reply.om \
 native/omega/allen/view.om native/omega/allen/scene.om native/omega/allen/paint.om native/omega/allen/application.om native/omega/allen/scene-input.om native/omega/allen/context.om native/omega/allen/font-data.om native/omega/allen/font.om native/omega/allen/look-paint.om native/omega/allen/surface.om native/omega/allen/presentation.om native/omega/allen/provider-core.om > "$core"
for name in runtime code courier scheduler application provider-core presentation font surface memory context intent reply view scene scene-input paint edges fixture golden render font-output font-render surface-render; do
 cat "$core" > "$out/$name.om"
 case "$name" in golden|render|font-output|font-render|surface-render) ;; *) cat native/omega/allen/tests/check.om >> "$out/$name.om" ;; esac
 cat "native/omega/allen/tests/$name.om" >> "$out/$name.om"
 native/omega/allen/build-one.sh "$out/$name.om" "$out/$name"
 case "$name" in
 fixture)
  "$out/$name" < evidence/allen-omega-L694/short.wire > "$out/short.log"
  "$out/$name" < evidence/allen-omega-L694/long.wire > "$out/long.log"
  ;;
 golden) "$out/$name" > "$out/memory-native.bin" ;;
 render) "$out/$name" > "$out/mind-component.ppm" ;;
 font-output) "$out/$name" > "$out/font.bin" ;;
 font-render) "$out/$name" > "$out/font-specimen.ppm" ;;
 surface-render) "$out/$name" > "$out/provider-surface.ppm" ;;
 *) "$out/$name" > "$out/$name.log" ;;
 esac
 printf '%s compile=0 native-exit=0\n' "$name"
done
cmp "$out/font.bin" evidence/allen-omega-L694/L758/legacy-font.bin
printf 'font native masks match frozen external legacy oracle\n'
native/omega/allen/golden.sh "$out/reference"
cmp "$out/memory-native.bin" "$out/reference/memory-golden.bin"
cat "$core" native/omega/allen/main.om > "$out/allen.om"
native/omega/allen/build-one.sh "$out/allen.om" "$out/allen"
"$out/allen" < evidence/allen-omega-L694/stdio-input.txt > "$out/stdio-job.txt" 2> "$out/stdio-display.txt"
cmp "$out/stdio-job.txt" evidence/allen-omega-L694/stdio-job.txt
cmp "$out/stdio-display.txt" evidence/allen-omega-L694/stdio-display.txt
printf 'stdio native-exit=0 golden-cmp=0\n'
# The fixture generator is Omega too. Refuse the oversized operation and
# never execute the valid ASK following it (L719/L722 regression).
native/omega/allen/build-one.sh native/omega/allen/tests/stdio-overflow.om "$out/stdio-overflow-input"
"$out/stdio-overflow-input" > "$out/stdio-overflow.input"
set +e
"$out/allen" < "$out/stdio-overflow.input" > "$out/stdio-overflow.stdout" 2> "$out/stdio-overflow.stderr"
rc=$?
set -e
[ "$rc" = 65 ] || { echo "stdio overflow was not refused: exit=$rc" >&2; exit 1; }
[ ! -s "$out/stdio-overflow.stdout" ] || { echo 'stdio overflow executed a later job' >&2; exit 1; }
[ ! -s "$out/stdio-overflow.stderr" ] || { echo 'stdio overflow displayed a later answer' >&2; exit 1; }
printf 'stdio overflow native-exit=65 no-later-command=PASS\n'
# Real native provider entry, independent native input/output fixtures.
cat "$core" native/omega/allen/provider.om > "$out/provider.om"
native/omega/allen/build-one.sh "$out/provider.om" "$out/provider"
for name in provider-input provider-output provider-process; do
 cat "$core" native/omega/allen/tests/check.om "native/omega/allen/tests/$name.om" > "$out/$name.om"
 native/omega/allen/build-one.sh "$out/$name.om" "$out/$name"
done
printf 0 | "$out/provider-input" > "$out/provider.input"
"$out/provider" < "$out/provider.input" > "$out/provider.output" 2> "$out/provider.stderr"
{ printf '7\n0\n'; stat -c %s "$out/provider.output" "$out/provider.stderr"; cat "$out/provider.output" "$out/provider.stderr"; } > "$out/provider-process.receipt"
"$out/provider-process" < "$out/provider-process.receipt" > "$out/provider-process.log"
"$out/provider-output" < "$out/provider.output" > "$out/provider-stream.log"
# Byte-at-a-time native producer through a real pipe; collect both exits explicitly.
printf 7 > "$out/provider.mode"
mkfifo "$out/provider.pipe"
"$out/provider-input" < "$out/provider.mode" > "$out/provider.pipe" 2> "$out/provider-producer.stderr" &
producer=$!
set +e
"$out/provider" < "$out/provider.pipe" > "$out/provider-fragment.output" 2> "$out/provider-fragment.stderr"
consumer_rc=$?
wait "$producer"
producer_rc=$?
set -e
{ printf '7\n%s\n' "$consumer_rc"; stat -c %s "$out/provider-fragment.output" "$out/provider-fragment.stderr"; cat "$out/provider-fragment.output" "$out/provider-fragment.stderr"; } > "$out/provider-consumer.receipt"
"$out/provider-process" < "$out/provider-consumer.receipt" > "$out/provider-consumer.log"
{ printf '7\n%s\n0\n' "$producer_rc"; stat -c %s "$out/provider-producer.stderr"; cat "$out/provider-producer.stderr"; } > "$out/provider-producer.receipt"
"$out/provider-process" < "$out/provider-producer.receipt" > "$out/provider-producer.log"
cmp "$out/provider.output" "$out/provider-fragment.output"
"$out/provider-output" < "$out/provider-fragment.output" > "$out/provider-fragment.log"
printf 8 | "$out/provider-input" > "$out/provider-boot.expected"
for mode in 1 2 3 4 5 6; do
 printf '%s' "$mode" | "$out/provider-input" > "$out/provider-bad-$mode.input"
 set +e
 "$out/provider" < "$out/provider-bad-$mode.input" > "$out/provider-bad-$mode.output" 2> "$out/provider-bad-$mode.stderr"
 rc=$?
 set -e
 { printf '%s\n%s\n' "$mode" "$rc"; stat -c %s "$out/provider-bad-$mode.output" "$out/provider-bad-$mode.stderr"; cat "$out/provider-bad-$mode.output" "$out/provider-bad-$mode.stderr"; } > "$out/provider-bad-$mode.receipt"
 "$out/provider-process" < "$out/provider-bad-$mode.receipt" > "$out/provider-bad-$mode.log"
 printf 'provider malformed mode=%s exit=65 no-later-command=PASS\n' "$mode"
done
printf 'provider native-exit=0 fragmented-pipe=0 output-verifier=0\n'
# Compiled behavioral mutants must be valid ELFs and fail the original gates.
sed 's/if or eq get R 1 0 ne seq get R 0/if eq get R 1 0/' "$core" > "$out/stale-mutant.om"
cat native/omega/allen/tests/check.om native/omega/allen/tests/runtime.om >> "$out/stale-mutant.om"
sed 's/if ne ulong codecrc get C 3/if false/' "$core" > "$out/crc-mutant.om"
cat native/omega/allen/tests/check.om native/omega/allen/tests/code.om >> "$out/crc-mutant.om"
sed 's/ret eq d 0/ret true/' "$core" > "$out/hash-mutant.om"
cat native/omega/allen/tests/check.om native/omega/allen/tests/memory.om >> "$out/hash-mutant.om"
sed 's/if ne d 0/if false/' "$core" > "$out/readback-mutant.om"
cat native/omega/allen/tests/check.om native/omega/allen/tests/memory.om >> "$out/readback-mutant.om"
sed 's/if ne run get Q 0/if false/' "$core" > "$out/session-mutant.om"
cat native/omega/allen/tests/check.om native/omega/allen/tests/courier.om >> "$out/session-mutant.om"
sed 's/delay = ulong 10000/delay = ulong 2000/' "$core" > "$out/keepalive-mutant.om"
cat native/omega/allen/tests/check.om native/omega/allen/tests/scheduler.om >> "$out/keepalive-mutant.om"
sed 's/put T 6 1/put T 6 0/' "$core" > "$out/ackstop-mutant.om"
cat native/omega/allen/tests/check.om native/omega/allen/tests/scheduler.om >> "$out/ackstop-mutant.om"
sed 's/put Outb i get Appwire i/put Outb i get Outb i/' "$core" > "$out/pendingwire-mutant.om"
cat native/omega/allen/tests/check.om native/omega/allen/tests/application.om >> "$out/pendingwire-mutant.om"
sed 's/if or le get R 2 1 ne get C 0 get R 0/if false/' "$core" > "$out/appcode-mutant.om"
cat native/omega/allen/tests/check.om native/omega/allen/tests/application.om >> "$out/appcode-mutant.om"
sed 's/if hide/if false/' "$core" > "$out/thinking-mutant.om"
cat native/omega/allen/tests/check.om native/omega/allen/tests/scene-input.om >> "$out/thinking-mutant.om"
sed 's/if lt get Nm 8 12/if true/' "$core" > "$out/scenecap-mutant.om"
cat native/omega/allen/tests/check.om native/omega/allen/tests/scene-input.om >> "$out/scenecap-mutant.om"
sed 's/if and eq get Ctxgone index 0 lt given limit/if lt given limit/' "$core" > "$out/contextnote-mutant.om"
cat native/omega/allen/tests/check.om native/omega/allen/tests/context.om >> "$out/contextnote-mutant.om"
sed 's/if not ctxappend/if false/' "$core" > "$out/contextjob-mutant.om"
cat native/omega/allen/tests/check.om native/omega/allen/tests/context.om >> "$out/contextjob-mutant.om"
native/omega/allen/build-one.sh native/omega/allen/tests/provider-mutants.om "$out/provider-mutants"
{ printf 0; cat "$core"; } | "$out/provider-mutants" > "$out/autosent-mutant.om"
cat native/omega/allen/tests/check.om native/omega/allen/tests/provider-core.om >> "$out/autosent-mutant.om"
{ printf 1; cat "$core"; } | "$out/provider-mutants" > "$out/framecheck-mutant.om"
cat native/omega/allen/tests/check.om native/omega/allen/tests/provider-core.om >> "$out/framecheck-mutant.om"
native/omega/allen/build-one.sh native/omega/allen/tests/presentation-mutants.om "$out/presentation-mutants"
{ printf 0; cat "$core"; } | "$out/presentation-mutants" > "$out/staleview-mutant.om"
cat native/omega/allen/tests/check.om native/omega/allen/tests/presentation.om >> "$out/staleview-mutant.om"
{ printf 1; cat "$core"; } | "$out/presentation-mutants" > "$out/inputtail-mutant.om"
cat native/omega/allen/tests/check.om native/omega/allen/tests/presentation.om >> "$out/inputtail-mutant.om"
native/omega/allen/build-one.sh native/omega/allen/tests/font-mutants.om "$out/font-mutants"
{ printf 0; cat "$core"; } | "$out/font-mutants" > "$out/fontroot-mutant.om"
cat native/omega/allen/tests/check.om native/omega/allen/tests/font.om >> "$out/fontroot-mutant.om"
{ printf 1; cat "$core"; } | "$out/font-mutants" > "$out/fontalpha-mutant.om"
cat native/omega/allen/tests/check.om native/omega/allen/tests/font.om >> "$out/fontalpha-mutant.om"
native/omega/allen/build-one.sh native/omega/allen/tests/provider-display-mutants.om "$out/provider-display-mutants"
{ printf 0; cat "$core"; } | "$out/provider-display-mutants" > "$out/rawask-mutant.om"
cat native/omega/allen/tests/check.om native/omega/allen/tests/provider-core.om >> "$out/rawask-mutant.om"
{ printf 1; cat "$core"; } | "$out/provider-display-mutants" > "$out/rawrecv-mutant.om"
cat native/omega/allen/tests/check.om native/omega/allen/tests/provider-core.om >> "$out/rawrecv-mutant.om"
{ printf 2; cat "$core"; } | "$out/provider-display-mutants" > "$out/rawreset-mutant.om"
cat native/omega/allen/tests/check.om native/omega/allen/tests/provider-core.om >> "$out/rawreset-mutant.om"
native/omega/allen/build-one.sh native/omega/allen/tests/surface-mutants.om "$out/surface-mutants"
{ printf 0; cat "$core"; } | "$out/surface-mutants" > "$out/clipright-mutant.om"
cat native/omega/allen/tests/check.om native/omega/allen/tests/surface.om >> "$out/clipright-mutant.om"
{ printf 1; cat "$core"; } | "$out/surface-mutants" > "$out/blendbg-mutant.om"
cat native/omega/allen/tests/check.om native/omega/allen/tests/surface.om >> "$out/blendbg-mutant.om"
for name in stale crc hash readback session keepalive ackstop pendingwire appcode thinking scenecap contextnote contextjob autosent framecheck staleview inputtail fontroot fontalpha rawask rawrecv rawreset clipright blendbg; do
 native/omega/allen/build-one.sh "$out/$name-mutant.om" "$out/$name-mutant"
 set +e
 "$out/$name-mutant" > "$out/$name-mutant.log"
 rc=$?
 set -e
 if [ "$rc" -le 0 ] || [ "$rc" -ge 100 ]; then echo "mutant did not fail behaviorally: $name exit=$rc" >&2; exit 1; fi
 printf '%s mutant compile=0 rejected-exit=%s\n' "$name" "$rc"
done
find native/omega/allen -type f -print0 | sort -z | xargs -0 sha256sum > "$out/source.sha256"
find "$out" -maxdepth 1 -type f -perm -100 -exec sha256sum {} + > "$out/binaries.sha256"
printf 'ALLEN_OMEGA_HOST: PASS; target=NOT_RUN; live=NOT_RUN\n'
