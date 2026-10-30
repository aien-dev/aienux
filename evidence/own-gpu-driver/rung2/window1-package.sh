#!/usr/bin/env bash
# window1-package.sh: build the boot package aien_gsp_boot reads (L332 rung 2 window 1). Reads only; writes only into OUT.
# NVIDIA's signed firmware stays on loan: every byte comes from the pinned 580.173.02 files on this box, cut out by our
# own extractors and checked against the pins in native/kernel/Makefile (GSP_FW_SHA256, GSP_BINDATA_BOOT_PROD_SHA256,
# GSP_BINDATA_FMC_IMAGE_SHA256). Nothing is sent to any device.
#
#   window1-package.sh OUT [TOOLS] [SYSINFO_TXT]
#     TOOLS        directory with ck_gsp_fw_inspect, ck_gsp_bindata_extract, ck_gsp_sysinfo_encode
#                  (default /tmp/l332-k/host, from make -C native/kernel OUT=/tmp/l332-k gsp-fw-inspect
#                  gsp-bindata-extract $OUT/host/ck_gsp_sysinfo_encode)
#     SYSINFO_TXT  the GspSystemInfo field values (default rung2/system_info.txt next to this script)
# OUT gets rm_elf.bin rm_sig.bin bootloader.bin fmc.bin fmc_hash.bin fmc_pkey.bin fmc_sig.bin system_info.bin,
# app_version.txt and MANIFEST.txt (sizes + SHA-256 + where each came from). Exit 0 only when every check holds.
set -euo pipefail
OUT=$1
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
TOOLS=${2:-/tmp/l332-k/host}
SI_TXT=${3:-$HERE/system_info.txt}
FW=/lib/firmware/nvidia/580.173.02/gsp_ga10x.bin
GEN=${OGKM_DIR:-$HOME/workspace/_ext/open-gpu-kernel-modules-580.173.02}/src/nvidia/generated
MK=$REPO/native/kernel/Makefile
pin() { sed -n "s/^$1 := \([0-9a-f]\{64\}\)$/\1/p" "$MK"; }
die() { echo "WINDOW1_PACKAGE: FAIL ($*)"; exit 1; }
sha() { sha256sum "$1" | cut -d' ' -f1; }
for t in ck_gsp_fw_inspect ck_gsp_bindata_extract ck_gsp_sysinfo_encode; do [[ -x $TOOLS/$t ]] || die "missing tool $TOOLS/$t"; done
mkdir -p "$OUT"
[[ -z $(ls -A "$OUT") ]] || die "$OUT is not empty"
W=$OUT/work; mkdir "$W"

# 1. GSP-RM: the .fwimage section and the gb10y signature section of the container
[[ $(sha "$FW") == $(pin GSP_FW_SHA256) ]] || die "$FW does not match GSP_FW_SHA256"
"$TOOLS/ck_gsp_fw_inspect" inspect "$FW" --family gb10y --expect-version 580.173.02 > "$W/inspect.txt" || die "fw inspect refused"
grep -q '^RESULT OK' "$W/inspect.txt" || die "fw inspect not OK"
cutsec() { # section-name out
    local off size
    read -r off size < <(awk -v n="$1" '$1=="section" && $3==n { sub("offset=","",$5); sub("size=","",$6); print $5, $6 }' "$W/inspect.txt")
    [[ -n ${off:-} && -n ${size:-} ]] || die "section $1 not in the inspect output"
    dd if="$FW" of="$2" bs=64k iflag=skip_bytes,count_bytes skip="$off" count="$size" status=none
    [[ $(stat -c %s "$2") -eq $size ]] || die "short cut of $1"
}
cutsec .fwimage "$OUT/rm_elf.bin"
cutsec .fwsignature_gb10y "$OUT/rm_sig.bin"
[[ $(sha "$OUT/rm_elf.bin") == $(awk '$1=="sha256_fwimage"{print $2}' "$W/inspect.txt") ]] || die ".fwimage cut differs from the inspected hash"
[[ $(sha "$OUT/rm_sig.bin") == $(awk '$1=="sha256_signature"{print $2}' "$W/inspect.txt") ]] || die "signature cut differs from the inspected hash"

# 2. GspRmBoot_GB10B (PROD image + its descriptor) and GspRmFmcGfwProdSigned_GB10B (image, hash, key, signature)
"$TOOLS/ck_gsp_bindata_extract" extract "$GEN/g_bindata_kgspGetBinArchiveGspRmBoot_GB10B.c" --out "$W" > "$W/boot.txt" || die "boot bindata refused"
"$TOOLS/ck_gsp_bindata_extract" extract "$GEN/g_bindata_kgspGetBinArchiveGspRmFmcGfwProdSigned_GB10B.c" --out "$W" > "$W/fmc.txt" || die "fmc bindata refused"
B=$W/kgspGetBinArchiveGspRmBoot_GB10B M=$W/kgspGetBinArchiveGspRmFmcGfwProdSigned_GB10B
cp "$B.UCODE_IMAGE_PROD.bin" "$OUT/bootloader.bin"
cp "$M.UCODE_IMAGE.bin" "$OUT/fmc.bin"
cp "$M.UCODE_HASH.bin" "$OUT/fmc_hash.bin"
cp "$M.UCODE_PKEY.bin" "$OUT/fmc_pkey.bin"
cp "$M.UCODE_SIG.bin" "$OUT/fmc_sig.bin"
[[ $(sha "$OUT/bootloader.bin") == $(pin GSP_BINDATA_BOOT_PROD_SHA256) ]] || die "boot PROD image does not match its pin"
[[ $(sha "$OUT/fmc.bin") == $(pin GSP_BINDATA_FMC_IMAGE_SHA256) ]] || die "FMC image does not match its pin"
grep -q '^hash384 UCODE_HASH equals sha384(UCODE_IMAGE)' "$W/fmc.txt" || die "FMC hash not checked against the image"
app=$(sed -n 's/^desc UCODE_DESC_PROD .* app_version=\([0-9]*\) .*/\1/p' "$W/boot.txt")
[[ -n $app ]] || die "no app_version in the PROD descriptor"
echo "$app" > "$OUT/app_version.txt"

# 3. GspSystemInfo
"$TOOLS/ck_gsp_sysinfo_encode" encode "$SI_TXT" "$OUT/system_info.bin" > "$W/sysinfo.txt" || die "system info refused: $(tail -1 "$W/sysinfo.txt")"

# 4. manifest
{
    echo "# window-1 boot package, built $(date -u +%FT%TZ) on $(hostname) from repo $(git -C "$REPO" rev-parse HEAD)"
    echo "# firmware $FW sha256 $(sha "$FW"); bindata from $GEN; system info from $SI_TXT sha256 $(sha "$SI_TXT")"
    echo "app_version $app (RM_RISCV_UCODE_DESC.appVersion of UCODE_DESC_PROD, GspRmBoot_GB10B)"
    for f in rm_elf rm_sig bootloader fmc fmc_hash fmc_pkey fmc_sig system_info; do
        printf '%-16s %10s %s\n' "$f.bin" "$(stat -c %s "$OUT/$f.bin")" "$(sha "$OUT/$f.bin")"
    done
} > "$OUT/MANIFEST.txt"
cat "$OUT/MANIFEST.txt"
echo "WINDOW1_PACKAGE: PASS ($OUT)"
