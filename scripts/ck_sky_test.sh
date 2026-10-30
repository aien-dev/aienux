#!/bin/sh
# ck_sky_test.sh -- host test of the real sky behind Allen (native/kernel/ui/
# sky_real.c and the generated sky_data.c): the projection against the
# generator, the integer view, sidereal time against published values, the
# zenith and precession, and the sky made at 3840x2160 (timed). Pictures land
# in target/native-kernel-sky/ for a person to look at. Hosted only. Ends
# with "AIENOS_SKY: PASS" or "AIENOS_SKY: FAIL <why>".
set -u
cd "$(dirname "$0")/.."
K=native/kernel
OUT=target/native-kernel-sky
W=$(mktemp -d "${TMPDIR:-/tmp}/ck_sky.XXXXXX")
trap 'rm -rf "$W"' EXIT
fail() { echo "AIENOS_SKY: FAIL $*"; exit 1; }
CC=${HOSTCC:-cc}
mkdir -p "$OUT"
$CC -std=gnu11 -O2 -Wall -Wextra -Werror -o "$W/test" $K/ui/tests/sky_test.c $K/ui/sky_real.c $K/ui/sky_data.c \
    $K/ui/night.c $K/ui/paint.c $K/ui/allen_font.c -lm || fail "test does not build"
"$W/test" "$OUT" || fail "sky checks"
echo "pictures: $OUT"
echo "AIENOS_SKY: PASS"
