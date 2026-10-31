#!/bin/sh
# ck_fat_boot_test.sh -- host test of the AIENOS boot section: the EFI System
# Partition added in unused space (native/kernel/dev/gpt_add.c) and the FAT32
# volume holding \EFI\BOOT\BOOTX64.EFI (native/kernel/dev/fat_boot.c), with
# power cuts. See native/kernel/dev/tests/fat_boot_test.c. The volumes it
# writes are then read by outside tools as judges: fsck.fat must find nothing
# to repair and mtools must read back BOOTX64.EFI byte for byte. Hosted only.
# Ends with "AIENOS_FAT_BOOT: PASS" or "AIENOS_FAT_BOOT: FAIL <why>".
set -u
cd "$(dirname "$0")/.."
K=native/kernel
N=native
W=$(mktemp -d "${TMPDIR:-/tmp}/ck_fat_boot.XXXXXX")
trap 'rm -rf "$W"' EXIT
fail() { echo "AIENOS_FAT_BOOT: FAIL $*"; exit 1; }
CC=${HOSTCC:-cc}
$CC -std=c11 -D_DEFAULT_SOURCE -O2 -Wall -Wextra -Werror -I$K/dev -I$K/tools -I$N/disk -I$N/argus \
    -o "$W/test" $K/dev/tests/fat_boot_test.c $K/dev/fat_boot.c $K/dev/gpt_add.c $K/dev/disk_part.c \
    $K/tools/gpt_write.c $N/disk/disk.c $N/disk/disk_file.c $N/argus/sha256.c || fail "test does not build"
"$W/test" "$W" || fail "boot section checks"
FSCK=$(command -v fsck.fat || echo /usr/sbin/fsck.fat)
for v in part1 part2 part3; do
    "$FSCK" -n "$W/$v.img" >"$W/$v.fsck" 2>&1 || { cat "$W/$v.fsck"; fail "fsck.fat finds faults in $v"; }
done
"$FSCK" -n -v "$W/part1.img" | grep -q "2 FATs, 32 bit entries" || fail "fsck.fat does not see FAT32"
export MTOOLS_SKIP_CHECK=1
mtype -i "$W/part1.img" ::/EFI/BOOT/BOOTX64.EFI >"$W/got1" || fail "mtools cannot read BOOTX64.EFI (part1)"
cmp -s "$W/got1" "$W/img1.bin" || fail "BOOTX64.EFI read by mtools differs from the image (part1)"
mtype -i "$W/part2.img" ::/EFI/BOOT/BOOTX64.EFI >"$W/got2" || fail "mtools cannot read BOOTX64.EFI (part2)"
cmp -s "$W/got2" "$W/img2.bin" || fail "BOOTX64.EFI read by mtools differs from the image (part2)"
mtype -i "$W/part3.img" ::/EFI/BOOT/BOOTX64.EFI >"$W/got3" || fail "mtools cannot read BOOTX64.EFI (part3)"
cmp -s "$W/got3" "$W/img1.bin" || fail "BOOTX64.EFI read by mtools differs from the image (part3)"
mdir -i "$W/part2.img" -b ::/EFI/BOOT | grep -qx "::/EFI/BOOT/BOOTX64.MAN" || fail "no BOOTX64.MAN"
[ "$(mdir -i "$W/part2.img" -b ::/EFI/BOOT | wc -l)" = 2 ] || fail "\\EFI\\BOOT holds more than the two files"
echo "note: fsck.fat clean on all three volumes; mtools reads BOOTX64.EFI exactly"
echo "AIENOS_FAT_BOOT: PASS"
