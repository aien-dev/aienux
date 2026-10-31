#!/usr/bin/env bash
# gsp_mmio_mutants.sh KERNEL_DIR: wrong-but-compilable edits of gpu/gsp_mmio.c (the GB10B GSP start-up). Each must
# make test_gsp_mmio FAIL against the simulated SEC2 + GSP. A mutant that passes = a hole in the tests.
# Register offsets and bit positions live in gsp_mmio.h, which the simulator shares, so they cannot be mutated here;
# the gpu-boot device contract checks those against the driver source independently. Exit 0 only if all are killed.
set -u
K=$1
CC=${CC:-cc}
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
mkdir -p "$work/gpu"
cp "$K/gpu/gsp_mmio.h" "$K/gpu/gsp_sim.h" "$K/gpu/gsp_sim.c" "$K/gpu/gmmu.h" "$work/gpu/"
build() { $CC -std=c11 -O1 -I"$work/gpu" -I"$K/tests" -o "$work/t" "$K/tests/test_gsp_mmio.c" "$1" "$work/gpu/gsp_sim.c" > "$work/cc.log" 2>&1; }
build "$K/gpu/gsp_mmio.c" && "$work/t" > /dev/null 2>&1 || { echo "BASELINE FAILS"; exit 2; }
muts=(
  "if (head == rd(r, CK_GSP_SEC2_QTAIL0))|if (head != rd(r, CK_GSP_SEC2_QTAIL0))"
  "if (n >= in->polls_can_send) {|if (n > in->polls_can_send) {"
  "wr(r, CK_GSP_SEC2_EMEMC0, CK_GSP_EMEMC_AINCW);|wr(r, CK_GSP_SEC2_EMEMC0, 0);"
  "(uint32_t)in->packet[i + 3] << 24|(uint32_t)in->packet[i + 3] << 16"
  "v |= (uint32_t)in->packet[i + k] << (8 * k);|v |= (uint32_t)in->packet[i + k] << (8 * (3 - k));"
  "if (emem_dwords(o->ememc_after) - start != o->dwords)|if (0)"
  "wr(r, CK_GSP_SEC2_QTAIL0, o->dwords * 4 - 4);\n    wr(r, CK_GSP_SEC2_QHEAD0, 0);|wr(r, CK_GSP_SEC2_QHEAD0, 0);\n    wr(r, CK_GSP_SEC2_QTAIL0, o->dwords * 4 - 4);"
  "o->dwords * 4 - 4|o->dwords * 4"
  "if (o->sec2_mb0 != CK_GSP_SEC2_CMD_BUSY)|if (1)"
  "return hwcfg2 != 0 && (hwcfg2|return (hwcfg2"
  "return hwcfg2 != 0 && (hwcfg2 & CK_GSP_PRIV_LOCKED_MASK) != CK_GSP_PRIV_LOCKED;|return hwcfg2 != 0;"
  "            return 0; /* boot arguments|            return 1; /* boot arguments"
  "if (o->gsp_mb1 == (uint32_t)(in->fmc_args_pa >> 32))|if (1)"
  "|| o->gsp_mb0 != 0;|;"
  "return !(o->hwcfg2 & CK_GSP_HWCFG2_LOCKDOWN)|return (o->hwcfg2 & CK_GSP_HWCFG2_LOCKDOWN)"
  "    o->gsp_mb0 = rd(r, CK_GSP_FLCN_MAILBOX0);\n    if (o->gsp_mb0 != 0)\n        return o->stage = CK_GSP_E_FMC;|"
  "wr(r, CK_GSP_FLCN_OS, in->app_version);|wr(r, CK_GSP_FLCN_OS, 0);"
  "if (!(o->cpuctl & CK_GSP_CPUCTL_ACTIVE))|if (0)"
  "in->len > CK_GSP_EMEM_CHANNEL_BYTES|in->len >= CK_GSP_EMEM_CHANNEL_BYTES"
  "wr(r, CK_GSP_QHEAD0, 0);|wr(r, CK_GSP_QHEAD0, 1);"
)
killed=0 total=0
for m in "${muts[@]}"; do
  from=${m%%|*} to=${m#*|}; total=$((total + 1))
  f=$(printf '%b' "$from") t=$(printf '%b' "$to")
  F=$f T=$t perl -0777 -ne '$i = index($_, $ENV{F}); exit 3 if $i < 0; substr($_, $i, length $ENV{F}) = $ENV{T}; print' \
    "$K/gpu/gsp_mmio.c" > "$work/gpu/gsp_mmio.c" || { echo "MUTANT TEXT MISSING: $from"; exit 2; }
  if ! build "$work/gpu/gsp_mmio.c"; then echo "mutant killed (does not compile): $from -> $to"; killed=$((killed + 1)); continue; fi
  if ! "$work/t" > /dev/null 2>&1; then echo "mutant killed (test_gsp_mmio): $from -> $to"; killed=$((killed + 1)); continue; fi
  echo "MUTANT SURVIVED: $from -> $to"
done
echo "gsp_mmio mutants: $killed of $total killed"
test "$killed" -eq "$total"
