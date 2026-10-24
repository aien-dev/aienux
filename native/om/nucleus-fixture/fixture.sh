#!/bin/bash
# fixture.sh -- build and boot the arch#314 BOOT FIXTURE (test harness, not the kernel) on QEMU virt.
#   fixture.sh build OUT PROGRAM_ELF REQUESTS [MODEL [DEADLINE_MICROSECONDS]]
#   fixture.sh run OUT tcg|kvm            -> OUT/serial-<accel>.txt, stops QEMU at the first terminal line
# Chain: Atlas ROM variant (only its pinned digest differs from seed/atlas/atlas.s2) checks stage1 (256 B);
# stage1 pins the bundle length and SHA-256 and checks it with the ROM's sha256_compute; the nucleus loads the
# program ELF from the bundle. Every image is built by the seed assembler asm2 from plain-text sources here.
# Model and request bytes are emulator-placed storage (QEMU loader), not a disk driver.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
OMEGA=${OMEGA:-$HOME/workspace/arch314/omega-mv}
S=$OMEGA/seed
ASM2=$S/rung2/asm2.bin
ASM2_PIN=6988abe072fac3cdef2240d5ad13a3ecd8fb576fa09112d344b9e7c2315969c4
ATLAS_PIN=f7802501b410a0c19eff7b8fca8865c9ba9c96bfa4f8065ca8f508b67748b9a5
TABLES="$S/rung2/table.s2 $S/rung3/table3.s2 $S/rung4/table4.s2 $S/atlas/table5.s2 $HERE/table-el1.s2"
die() { echo "fixture: $*" >&2; exit 1; }
sha() { sha256sum "$1" | cut -d' ' -f1; }
u64le() { local v=$1 i s=; for i in 0 1 2 3 4 5 6 7; do s="$s\\x$(printf %02x $(( (v >> (8*i)) & 255 )))"; done; printf "$s"; }

build() {
  local out=$1 prog=$2 req=$3 model=${4:-} dl=${5:-0}
  [ "$(sha $ASM2)" = "$ASM2_PIN" ] || die "asm2 sha differs from pin"
  [ "$(sha $S/atlas/atlas.bin)" = "$ATLAS_PIN" ] || die "seed atlas.bin differs from the audited pin"
  mkdir -p "$out"
  cat $TABLES "$HERE/nucleus.s2" | "$ASM2" > "$out/nucleus.bin" || die "asm2 nucleus"
  local n; n=$(stat -c %s "$out/nucleus.bin")
  [ "$n" -le 1048576 ] || die "nucleus larger than 1 MiB"
  cp "$out/nucleus.bin" "$out/bundle.bin"; truncate -s 1048576 "$out/bundle.bin"
  cat "$prog" >> "$out/bundle.bin"
  local len dg; len=$(stat -c %s "$out/bundle.bin"); dg=$(sha "$out/bundle.bin")
  local sedargs=(-e "s/@LEN@/$len/")
  local i; for i in 0 1 2 3; do sedargs+=(-e "s/@LEN$i@/$(( (len >> (16*i)) & 65535 ))/"); done
  for i in 0 1 2 3 4 5 6 7; do sedargs+=(-e "s/@D$i@/0x${dg:$((8*i)):8}/"); done
  sed "${sedargs[@]}" "$HERE/stage1.tmpl.s2" > "$out/stage1.s2"
  grep -q @ "$out/stage1.s2" && die "stage1 placeholder left"
  cat $TABLES "$out/stage1.s2" | "$ASM2" > "$out/stage1.bin" || die "asm2 stage1"
  [ "$(stat -c %s "$out/stage1.bin")" -le 256 ] || die "stage1 over 256 bytes"
  truncate -s 256 "$out/stage1.bin"
  local s1; s1=$(sha "$out/stage1.bin")
  awk -v d="$s1" '/^:pinned_digest/{print; for(i=0;i<8;i++){getline; print "  .w 0x" substr(d,8*i+1,8)}; next} {print}' \
    "$S/atlas/atlas.s2" > "$out/atlas-variant.s2"
  [ "$(diff "$S/atlas/atlas.s2" "$out/atlas-variant.s2" | grep -c '^>')" -le 8 ] || die "atlas variant differs beyond the digest"
  cat $S/rung2/table.s2 $S/rung3/table3.s2 $S/atlas/table5.s2 "$out/atlas-variant.s2" | "$ASM2" > "$out/atlas-variant.bin" || die "asm2 atlas"
  local ml=0; [ -n "$model" ] && ml=$(stat -c %s "$model")
  local rl; rl=$(stat -c %s "$req")
  { printf 'N314PAR1'; u64le "$ml"; u64le "$rl"; u64le 0; u64le "$dl"; } > "$out/params.bin"
  cp "$req" "$out/requests.bin"
  { echo "built $(date -u +%FT%TZ)"
    echo "asm2 $ASM2_PIN"; echo "atlas-audited $ATLAS_PIN"
    echo "nucleus.bin $n $(sha "$out/nucleus.bin")"
    echo "program $(stat -c %s "$prog") $(sha "$prog") $prog"
    echo "bundle $len $dg"; echo "stage1 $s1"
    echo "atlas-variant $(sha "$out/atlas-variant.bin")"
    echo "requests $rl $(sha "$req")"
    [ -n "$model" ] && echo "model $ml $(sha "$model") $model"
    echo "deadline_us $dl"; } > "$out/manifest.txt"
  [ -n "$model" ] && ln -sf "$(readlink -f "$model")" "$out/model.bin"
  cat "$out/manifest.txt"
}

run() {
  local out=$1 accel=$2 cpu
  case $accel in tcg) cpu=cortex-a76 ;; kvm) cpu=host ;; *) die "accel tcg|kvm" ;; esac
  local log=$out/serial-$accel.txt; : > "$log"
  local dev=(-device loader,file=$out/bundle.bin,addr=0x100000000,force-raw=on
             -device loader,file=$out/stage1.bin,addr=0x40200000,force-raw=on
             -device loader,file=$out/params.bin,addr=0x180000000,force-raw=on)
  [ -s "$out/requests.bin" ] && dev+=(-device loader,file=$out/requests.bin,addr=0x180100000,force-raw=on)
  [ -e "$out/model.bin" ] && dev+=(-device loader,file=$out/model.bin,addr=0x140000000,force-raw=on)
  local t0; t0=$(date +%s.%N)
  qemu-system-aarch64 -machine virt,gic-version=3 -accel "$accel" -cpu "$cpu" -m 6G -smp 1 \
    -display none -monitor none -serial "file:$log" -bios "$out/atlas-variant.bin" "${dev[@]}" ${QEMU_EXTRA:-} &
  local q=$!
  # Stop only at a terminal line the fixture itself prints (HALT, REFUSE, PANIC); no clock cut-off.
  while kill -0 $q 2>/dev/null; do
    if grep -a -q -E $'\eN (HALT|REFUSE|PANIC)|REFUSE' "$log"; then sleep 0.2; kill $q; break; fi
    sleep 0.1
  done
  wait $q 2>/dev/null || true
  local t1; t1=$(date +%s.%N)
  echo "wall_seconds_start $t0 end $t1" >> "$out/runs-$accel.txt"
  echo "serial: $log"
}

case ${1:-} in
  build) shift; build "$@" ;;
  run) shift; run "$@" ;;
  *) die "usage: fixture.sh build OUT PROGRAM_ELF REQUESTS [MODEL [DEADLINE_MICROSECONDS]] | run OUT tcg|kvm" ;;
esac
