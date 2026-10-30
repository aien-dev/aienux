#!/bin/sh
# ck_boot_show_test.sh -- host test of the boot sequence's frame receipt
# (native/kernel/ui/boot_show.c on ui/allen_ui.c): frames drawn and 60 Hz
# slots dropped on a fake clock (fast and slow), one present per frame, the
# chirp kept on the night's clock. With BOOT_SHOW_SIZES="WxH ..." it also
# plays the whole night at each size on one core of this host with the real
# clock and prints the receipt line (host fixtures, e.g. the iMac's 5120x2880).
# Hosted only. Ends with "AIENOS_BOOT_SHOW: PASS" or "AIENOS_BOOT_SHOW: FAIL <why>".
set -u
cd "$(dirname "$0")/.."
K=native/kernel
W=$(mktemp -d "${TMPDIR:-/tmp}/ck_boot_show.XXXXXX")
trap 'rm -rf "$W"' EXIT
fail() { echo "AIENOS_BOOT_SHOW: FAIL $*"; exit 1; }
CC=${HOSTCC:-cc}
$CC -std=gnu11 -O2 -Wall -Wextra -Werror -pthread -o "$W/test" $K/ui/tests/boot_show_test.c $K/ui/boot_show.c \
    $K/ui/allen_ui.c $K/svc/allen_code.c $K/ui/paint.c $K/ui/night.c $K/ui/sky_real.c $K/ui/sky_data.c $K/ui/mind.c $K/ui/flow.c \
    $K/ui/allen_font.c $K/ui/canvas.c $K/svc/allen_scene.c || fail "test does not build"
"$W/test" || fail "receipt checks"
for s in ${BOOT_SHOW_SIZES:-}; do
    "$W/test" "$s" | grep "^host one core" || fail "host run at $s"
done
echo "AIENOS_BOOT_SHOW: PASS"
