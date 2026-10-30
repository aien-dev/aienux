#!/bin/bash
# scripts/gpu_bench/check_regs.sh -- check every AMD register number in the iMac GPU driver
# against AMD's own MIT headers in the Linux kernel, so a made-up number cannot slip in again.
#
# What it checks
#   1. native/kernel/gpu/amd_gpu_regs.h: every `#define mm...` register index and every
#      `*_MASK` / `*__SHIFT` field constant must equal the value in the real header, found
#      by name. When the line carries a citation `/* header.h:LINE */` the cited line must
#      be that exact define (so the citation cannot drift either). A define with no
#      citation is a failure.
#   2. the SMU message ids and firmware-header offset cited from smu7_ppsmc.h / smu7.h.
#   3. scripts/gpu_bench/STEP*.bench: a `bar5_*` line whose preceding comment names a
#      register (`# mmNAME, byte 0x...`) must use that register's real index, and the byte
#      offset in the comment must be index * 4. Any other BAR5 index must be a value that
#      amd_gpu_regs.h defines (or the line is marked with a `# raw` comment above it).
#
# Headers: Linux v6.12, drivers/gpu/drm/amd (asic_reg/* and pm/powerplay/inc/*), downloaded
# once into $AMD_HDR_DIR (default ~/.cache/aienos-amd-hdr). Set AMD_HDR_DIR to a directory
# that already holds the files to run offline.
#
# Usage: check_regs.sh        Exit: 0 all match, 1 any mismatch or uncited define, 2 no headers.

set -u
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
regs=$root/native/kernel/gpu/amd_gpu_regs.h
hdr=${AMD_HDR_DIR:-$HOME/.cache/aienos-amd-hdr}
tag=${AMD_HDR_TAG:-v6.12}
base=https://raw.githubusercontent.com/torvalds/linux/$tag/drivers/gpu/drm/amd

mkdir -p "$hdr"
for f in include/asic_reg/dce/dce_11_2_d.h include/asic_reg/dce/dce_11_2_sh_mask.h \
         include/asic_reg/gmc/gmc_8_1_d.h include/asic_reg/gmc/gmc_8_1_sh_mask.h \
         include/asic_reg/gca/gfx_8_0_d.h include/asic_reg/gca/gfx_8_0_sh_mask.h \
         include/asic_reg/oss/oss_3_0_d.h include/asic_reg/oss/oss_3_0_sh_mask.h \
         include/asic_reg/smu/smu_7_1_3_d.h include/asic_reg/smu/smu_7_1_3_sh_mask.h \
         include/asic_reg/bif/bif_5_1_d.h include/asic_reg/bif/bif_5_1_sh_mask.h \
         pm/powerplay/inc/smu7_ppsmc.h pm/powerplay/inc/smu7.h; do
    b=$(basename "$f")
    [ -s "$hdr/$b" ] && continue
    curl -sfL -o "$hdr/$b" "$base/$f" || { echo "cannot fetch $base/$f"; rm -f "$hdr/$b"; exit 2; }
done

fail=0
checked=0
bad() { echo "FAIL $*"; fail=$((fail + 1)); }

# real_line NAME -> "file:line:value" of the unique `#define NAME value` in the headers
real_line() {
    grep -n "^#define $1[[:space:]]" "$hdr"/*.h | head -1 |
        awk -F'[: \t]+' '{ f=$1; sub(".*/", "", f); printf "%s:%s:%s\n", f, $2, $5 }'
}

# ---- 1. amd_gpu_regs.h -------------------------------------------------------------------
while IFS= read -r line; do
    name=$(echo "$line" | awk '{print $2}')
    case "$name" in
        mm[A-Za-z0-9_]*|*_MASK|*__SHIFT) ;;
        *) continue ;;
    esac
    val=$(echo "$line" | awk '{print $3}')
    case "$val" in 0x[0-9a-fA-F]*) ;; *) continue ;; esac     # macros are not literal numbers
    val=${val%u}
    cite=$(echo "$line" | sed -n 's|.*/\* \([a-z0-9_]*\.h\):\([0-9]*\).*|\1:\2|p')
    checked=$((checked + 1))
    real=$(real_line "$name")
    if [ -z "$real" ]; then bad "$name is not in any AMD header (ours $val)"; continue; fi
    rfile=${real%%:*}; rest=${real#*:}; rline=${rest%%:*}; rval=${rest#*:}
    if [ $((val)) -ne $((rval)) ]; then
        printf 'FAIL %s ours=%s real=%s (%s:%s)\n' "$name" "$val" "$rval" "$rfile" "$rline"
        fail=$((fail + 1)); continue
    fi
    if [ -z "$cite" ]; then bad "$name has no header:line citation (should be $rfile:$rline)"; continue; fi
    if [ "$cite" != "$rfile:$rline" ]; then bad "$name cites $cite but the define is at $rfile:$rline"; fi
done < <(grep '^#define ' "$regs")

# ---- 2. SMU ids and firmware header layout -----------------------------------------------
# Run against the same cites the header gives: message id by name, struct layout by text.
smu_msg() {   # NAME EXPECTED_HEX
    local r; r=$(grep -n "^#define $1[[:space:]]" "$hdr/smu7_ppsmc.h" | head -1)
    local v; v=$(echo "$r" | grep -o '0x[0-9A-Fa-f]*' | head -1)
    checked=$((checked + 1))
    [ -n "$v" ] && [ $((v)) -eq $(($2)) ] || bad "$1 expected $2, smu7_ppsmc.h has '${v:-nothing}'"
}
for pair in $(grep -o '^#define PPSMC_[A-Za-z_]* *0x[0-9a-fA-F]*u\?' "$regs" | awk '{print $2 ":" $3}'); do
    v=${pair#*:}; smu_msg "${pair%%:*}" "${v%u}"
done
checked=$((checked + 1))
loc=$(grep -o 'define SMU7_FIRMWARE_HEADER_LOCATION *0x[0-9a-fA-F]*' "$hdr/smu7.h" | awk '{print $3}')
ours=$(grep '^#define SMU7_FIRMWARE_HEADER_LOCATION' "$regs" | awk '{print $3}'); ours=${ours%u}
[ -n "$loc" ] && [ -n "$ours" ] && [ $((loc)) -eq $((ours)) ] || bad "SMU7_FIRMWARE_HEADER_LOCATION ours=${ours:-none} real=${loc:-none}"
# Version is the 6th dword of SMU7_Firmware_Header (after Digest[5]): byte offset 0x14.
checked=$((checked + 1))
if grep -q 'uint32_t Digest\[5\];' "$hdr/smu7.h" &&
   [ "$(grep -A1 'uint32_t Digest\[5\];' "$hdr/smu7.h" | tail -1 | tr -d ' ;')" = "uint32_tVersion" ]; then
    want=$((5 * 4))
else
    want=-1
fi
ours=$(grep '^#define SMU7_FIRMWARE_HEADER_Version_offset' "$regs" | grep -o '(\?[0-9u* ]*)\?' | head -1)
oursn=$(grep '^#define SMU7_FIRMWARE_HEADER_Version_offset' "$regs" | sed 's/^#define [A-Za-z0-9_]* *//; s|/\*.*||; s/u//g')
[ -n "$oursn" ] && [ $((oursn)) -eq "$want" ] || bad "SMU7_FIRMWARE_HEADER_Version_offset ours='${oursn:-none}' real=$want"

# ---- 3. bench scripts --------------------------------------------------------------------
defined=" "
while IFS= read -r line; do
    v=$(echo "$line" | awk '{print $3}'); v=${v%u}
    case "$v" in 0x[0-9a-fA-F]*) defined="$defined$((v)) " ;; esac
done < <(grep '^#define mm' "$regs")

for f in "$here"/STEP*.bench; do
    bn=$(basename "$f"); n=0; lastname=""; lastbyte=""; raw=0
    while IFS= read -r line || [ -n "$line" ]; do
        n=$((n + 1))
        case "$line" in
            '#'*)
                nm=$(echo "$line" | grep -o 'mm[A-Z0-9_]*' | head -1)
                [ -n "$nm" ] && { lastname=$nm; lastbyte=$(echo "$line" | sed -n 's/.*byte \(0x[0-9A-Fa-f]*\).*/\1/p'); }
                case "$line" in '# raw'*) raw=1 ;; esac
                continue ;;
        esac
        set -- $line
        case "${1:-}" in bar5_read|bar5_write|bar5_rmw|bar5_poll) ;; *) lastname=""; lastbyte=""; raw=0; continue ;; esac
        idx=$(($2)); checked=$((checked + 1))
        if [ -n "$lastname" ]; then
            real=$(real_line "$lastname")
            if [ -z "$real" ]; then bad "$bn:$n comment names $lastname, which is not in any AMD header"
            elif [ $((${real##*:})) -ne "$idx" ]; then
                bad "$bn:$n $lastname is ${real##*:} in ${real%%:*}, the script uses $(printf '0x%x' "$idx")"
            fi
            if [ -n "$lastbyte" ] && [ $((lastbyte)) -ne $((idx * 4)) ]; then
                bad "$bn:$n comment byte $lastbyte is not index $(printf '0x%x' "$idx") * 4"
            fi
        elif [ "$raw" -eq 0 ]; then
            case "$defined" in *" $idx "*) ;; *) bad "$bn:$n index $(printf '0x%x' "$idx") is not a register in amd_gpu_regs.h (add a '# mmNAME, byte' comment or '# raw')" ;; esac
        fi
        lastname=""; lastbyte=""; raw=0
    done < "$f"
done

if [ "$fail" -ne 0 ]; then echo "check_regs: $fail problem(s) in $checked check(s)"; exit 1; fi
echo "check_regs: $checked check(s), all match the AMD headers ($tag)"
