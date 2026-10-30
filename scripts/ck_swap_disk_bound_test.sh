#!/bin/sh
# ck_swap_disk_bound_test.sh -- host test of the QEMU test-disk swap inbox bounds
# (native/kernel/x86/swap_disk_read.h, used by swap_x86.c): a slot object is read
# only if manifest + image, rounded up to whole sectors, fits the swap buffer.
# Uses a mocked sector reader. See native/kernel/x86/tests/swap_disk_bound_test.c.
# Hosted only. Ends with "AIENOS_SWAP_DISK_BOUND: PASS" or "... FAIL <why>".
set -u
cd "$(dirname "$0")/.."
K=native/kernel
W=$(mktemp -d "${TMPDIR:-/tmp}/ck_swap_disk_bound.XXXXXX")
trap 'rm -rf "$W"' EXIT
fail() { echo "AIENOS_SWAP_DISK_BOUND: FAIL $*"; exit 1; }
CC=${HOSTCC:-cc}
$CC -std=c11 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -Wall -Wextra -Werror \
    -o "$W/test" $K/x86/tests/swap_disk_bound_test.c || fail "test does not build"
"$W/test" || fail "bound checks"
echo "AIENOS_SWAP_DISK_BOUND: PASS"
