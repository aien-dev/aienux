#!/bin/bash
# gate.sh -- arch#314 boot fixture gate (shell referee; the programs under test are Omega and asm2).
#   gate.sh [OUTDIR]       runs every check under TCG, and under KVM when /dev/kvm is usable; prints RC=0 on pass.
# Checks, all on the same pinned tools (asm2, omc, audited Atlas ROM) and the pinned model file:
#  1. table-el1.s2: every privileged line decodes in objdump to the reviewed text in table-el1.expect.
#  2. echo.om and bytecopy.om (seed tests), preadp.om: fixture output == hosted output of the same ELF bytes.
#  3. probe.s2 (misbehaving EL0 program): every bad action refused or contained, runs relaunched, accounting exact.
#  4. chain controls: flipped bundle byte -> STAGE1 REFUSE; flipped stage1 byte -> ATLAS REFUSE;
#     writable+executable segment -> REFUSE program; bad ELF magic -> REFUSE program; bad params -> REFUSE params.
#  5. the Omega programs use no word the fixture does not serve (args, environment, clock).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
OMEGA=${OMEGA:-$HOME/workspace/arch314/omega-mv}
S=$OMEGA/seed; OMC=$S/out/omc; ASM2=$S/rung2/asm2.bin
OMC_PIN=734f0a39319fbfa930bcc62cb30dbf06d47d78e126372b74f5baf478e6126b61
MODEL=${MODEL:-$HOME/models/aien-mail/Llama-3.2-1B-Instruct-Q4_K_M.gguf}
MODEL_PIN=3f5a22426976ab26cfe84dba63c1d08391717abb1af893e10f1b2968d862dcc1
O=${1:-$HERE/evidence/run}; mkdir -p "$O"
LOG=$O/gate.log; : > "$LOG"
fail=0
say() { printf '%s\n' "$*" | tee -a "$LOG"; }
pass() { say "PASS $*"; }
bad() { say "FAIL $*"; fail=1; }
sha() { sha256sum "$1" | cut -d' ' -f1; }
FX=$HERE/fixture.sh
TABLES="$S/rung2/table.s2 $S/rung3/table3.s2 $S/rung4/table4.s2 $S/atlas/table5.s2 $HERE/table-el1.s2"
say "gate $(date -u +%FT%TZ) aienux $(git -C "$HERE" rev-parse HEAD 2>/dev/null) omega $(git -C "$OMEGA" rev-parse HEAD 2>/dev/null)"
say "host $(uname -srm) qemu $(qemu-system-aarch64 --version | head -1)"
[ "$(sha "$OMC")" = "$OMC_PIN" ] && pass "omc sha256 $OMC_PIN" || { bad "omc sha differs"; exit 1; }
[ "$(sha "$MODEL")" = "$MODEL_PIN" ] && pass "model sha256 $MODEL_PIN" || { bad "model sha differs"; exit 1; }

# 1. privileged table decode
grep '^=' "$HERE/table-el1.s2" | awk '{n=$2; f=$4; a=""; if (f ~ /^r0/) a=" x3"; else if (f ~ /^i/) a=" 1"; print "  " n a}' > "$O/el1all.s2"
cat $S/rung2/table.s2 "$HERE/table-el1.s2" "$O/el1all.s2" | "$ASM2" > "$O/el1all.bin"
paste -d'|' <(sed 's/^  //' "$O/el1all.s2") <(objdump -D -b binary -m aarch64 "$O/el1all.bin" | grep -E '^ +[0-9a-f]+:' | cut -f3- | tr '\t' ' ') > "$O/el1all.decoded"
cmp -s "$O/el1all.decoded" "$HERE/table-el1.expect" && pass "table-el1: $(wc -l < "$HERE/table-el1.expect") lines decode as reviewed" || bad "table-el1 decode differs"

# 5. no unserved words in the Omega programs
if grep -n -w -E 'argcnt|argread|envcnt|envread|clock|now|ticks' "$HERE/probes/preadp.om" "$HERE/demux.om" $S/rung4/tests/echo.om $S/rung4/tests/bytecopy.om > "$O/words.txt"; then
  bad "unserved words: $(head -1 "$O/words.txt")"; else pass "no args/environment/clock words in the Omega programs"; fi

# programs
for p in echo bytecopy; do "$OMC" < $S/rung4/tests/$p.om > "$O/$p" && chmod +x "$O/$p" || bad "omc $p"; done
"$OMC" < "$HERE/probes/preadp.om" > "$O/preadp" && chmod +x "$O/preadp" || bad "omc preadp"
"$OMC" < "$HERE/demux.om" > "$O/demux" && chmod +x "$O/demux" || bad "omc demux"
cat $TABLES "$HERE/probes/probe.s2" | "$ASM2" > "$O/probe" && truncate -s 4096 "$O/probe" || bad "asm2 probe"
printf 'hello fixture\nsecond line\nthird\n' > "$O/req-echo.txt"
head -c 60000 "$OMEGA/docs/one_layer/OMEGESE.md" | tr -d '\033' > "$O/req-bc.txt"
printf 's\nb\no\nt\nm\nk\nu\ng\na\nw\nd\ne\np\nl\nx\nz\no\n' > "$O/req-probe.txt"
: > "$O/req-none.txt"
"$O/echo" < "$O/req-echo.txt" > "$O/echo.hosted"
"$O/bytecopy" < "$O/req-bc.txt" > "$O/bytecopy.hosted"
/usr/bin/time -f '%e' -o "$O/preadp.hosted.time" "$O/preadp" 3< "$MODEL" > "$O/preadp.hosted"
say "hosted preadp: $(cat "$O/preadp.hosted") in $(cat "$O/preadp.hosted.time") s"

"$FX" build "$O/b-echo" "$O/echo" "$O/req-echo.txt" > /dev/null || bad "build echo"
"$FX" build "$O/b-bc" "$O/bytecopy" "$O/req-bc.txt" > /dev/null || bad "build bytecopy"
"$FX" build "$O/b-pread" "$O/preadp" "$O/req-none.txt" "$MODEL" 0 > /dev/null || bad "build preadp"
"$FX" build "$O/b-probe" "$O/probe" "$O/req-probe.txt" "" 500000 > /dev/null || bad "build probe"

ACCELS="tcg"; [ -r /dev/kvm ] && [ -w /dev/kvm ] && ACCELS="tcg kvm"
fieldval() { tr ' ' '\n' | awk -v k="$1" '$0==k{getline; print; exit}'; }
for A in $ACCELS; do
  for p in echo bc pread; do
    "$FX" run "$O/b-$p" "$A" > /dev/null 2>&1
    "$O/demux" < "$O/b-$p/serial-$A.txt" > "$O/b-$p/prog-$A.out" 2> "$O/b-$p/fixture-$A.txt" || bad "$A $p demux framing"
  done
  cmp -s "$O/b-echo/prog-$A.out" "$O/echo.hosted" && pass "$A echo: fixture output = hosted" || bad "$A echo differs"
  cmp -s "$O/b-bc/prog-$A.out" "$O/bytecopy.hosted" && pass "$A bytecopy: fixture output = hosted ($(stat -c %s "$O/bytecopy.hosted") bytes)" || bad "$A bytecopy differs"
  cmp -s "$O/b-pread/prog-$A.out" "$O/preadp.hosted" && pass "$A preadp: model length + hash = hosted" || bad "$A preadp differs"
  pb=$(grep -a ' RUN 1 END' "$O/b-pread/fixture-$A.txt" | fieldval pread_bytes)
  [ "$pb" = "$(stat -c %s "$MODEL")" ] && pass "$A preadp: fixture counted $pb model bytes read" || bad "$A pread_bytes $pb"
  say "$A preadp $(grep -a ' RUN 1 END' "$O/b-pread/fixture-$A.txt" | cut -c1-200) cntfrq $(grep -a ' BOOT' "$O/b-pread/fixture-$A.txt" | fieldval cntfrq)"
  # probe
  "$FX" run "$O/b-probe" "$A" > /dev/null 2>&1
  "$O/demux" < "$O/b-probe/serial-$A.txt" > "$O/b-probe/prog-$A.out" 2> "$O/b-probe/fixture-$A.txt" || bad "$A probe demux framing"
  printf 's ok\nb ok\no ok\n? unknown action\no ok\n' | cmp -s - "$O/b-probe/prog-$A.out" && pass "$A probe: answers exactly s ok, b ok, o ok, unknown, o ok (no SURVIVED)" || bad "$A probe output: $(tr '\n' '/' < "$O/b-probe/prog-$A.out")"
  got=$(grep -a ' END ' "$O/b-probe/fixture-$A.txt" | awk '{print $5 ":" $9}' | tr '\n' ' ')
  want="fault:36 fault:36 fault:36 fault:36 fault:36 fault:36 fault:36 fault:32 fault:0 fault:0 deadline:0 exit:0 exit:0 "
  [ "$got" = "$want" ] && pass "$A probe: 13 runs end as nucleus/model/params/uart/gic/unmapped/own-text data aborts, data-exec abort, eret+msr undefined, deadline, exit 7, exit 0" || bad "$A probe run ends: $got"
  grep -a ' RUN 12 END' "$O/b-probe/fixture-$A.txt" | grep -q 'exit status 7 ' && pass "$A probe: exit 7 reported" || bad "$A exit 7"
  grep -a ' RUN 1 END' "$O/b-probe/fixture-$A.txt" | grep -q 'svc 6 9 2 0 3 efault 5 in 8 out 15 ' && pass "$A probe: run 1 accounting svc 6 9 2 0 3 efault 5 in 8 out 15" || bad "$A probe run 1 accounting"
  grep -a ' HALT' "$O/b-probe/fixture-$A.txt" | grep -q 'HALT runs 13 faults 10 deadlines 1 ' && pass "$A probe: HALT runs 13 faults 10 deadlines 1" || bad "$A probe halt line"
done

# 4. chain controls (TCG)
flip() { local f=$1 off=$2; local b; b=$(od -An -tu1 -j "$off" -N1 "$f" | tr -d ' '); printf "\\x$(printf %02x $(( b ^ 1 )))" | dd of="$f" bs=1 seek="$off" conv=notrunc status=none; }
ctl() { local name=$1 dir=$2 want=$3; "$FX" run "$dir" tcg > /dev/null 2>&1
  grep -a -q -- "$want" "$dir/serial-tcg.txt" && ! grep -a -q ' RUN 1 START' "$dir/serial-tcg.txt" && pass "control $name -> $want, program never started" || bad "control $name: $(tail -c 200 "$dir/serial-tcg.txt" | tr -d '\033')"; }
cp -r "$O/b-echo" "$O/c-bundle"; flip "$O/c-bundle/bundle.bin" 2100; ctl "flipped nucleus byte" "$O/c-bundle" "STAGE1: REFUSE"
cp -r "$O/b-echo" "$O/c-elf"; flip "$O/c-elf/bundle.bin" $((1048576 + 300)); ctl "flipped program byte" "$O/c-elf" "STAGE1: REFUSE"
cp -r "$O/b-echo" "$O/c-stage1"; flip "$O/c-stage1/stage1.bin" 200; ctl "flipped stage1 byte" "$O/c-stage1" "ATLAS: REFUSE"
sed '0,/^  \.w 5$/s//  .w 7/' "$HERE/probes/probe.s2" > "$O/probe-wx.s2"
cat $TABLES "$O/probe-wx.s2" | "$ASM2" > "$O/probe-wx" && truncate -s 4096 "$O/probe-wx"
"$FX" build "$O/c-wx" "$O/probe-wx" "$O/req-probe.txt" > /dev/null; ctl "writable+executable segment" "$O/c-wx" "REFUSE program"
sed '0,/^  \.w 0x464c457f$/s//  .w 0x464c4570/' "$HERE/probes/probe.s2" > "$O/probe-magic.s2"
cat $TABLES "$O/probe-magic.s2" | "$ASM2" > "$O/probe-magic" && truncate -s 4096 "$O/probe-magic"
"$FX" build "$O/c-magic" "$O/probe-magic" "$O/req-probe.txt" > /dev/null; ctl "bad ELF magic" "$O/c-magic" "REFUSE program"
cp -r "$O/b-echo" "$O/c-params"; flip "$O/c-params/params.bin" 3; ctl "bad params magic" "$O/c-params" "REFUSE params"

[ $fail = 0 ] && say "RC=0" || say "RC=1"
exit $fail
