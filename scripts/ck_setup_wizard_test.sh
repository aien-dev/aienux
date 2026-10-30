#!/bin/sh
# ck_setup_wizard_test.sh -- host test of Allen's setup wizard: the machine
# inventory and host-description reader (native/kernel/svc/setup_inv.c) and
# the wizard's screens, choices and records (native/kernel/svc/setup_wizard.c)
# and their two copies on disk (native/kernel/svc/setup_store.c),
# walked with keys and the clock against the three machine fixtures in
# native/kernel/svc/tests/fixtures/setup. See
# native/kernel/svc/tests/setup_wizard_test.c. Hosted only. Ends with
# "AIENOS_SETUP_WIZARD: PASS" or "AIENOS_SETUP_WIZARD: FAIL <why>".
set -u
cd "$(dirname "$0")/.."
K=native/kernel
N=native
W=$(mktemp -d "${TMPDIR:-/tmp}/ck_setup_wizard.XXXXXX")
trap 'rm -rf "$W"' EXIT
fail() { echo "AIENOS_SETUP_WIZARD: FAIL $*"; exit 1; }
CC=${HOSTCC:-cc}
$CC -std=c11 -D_DEFAULT_SOURCE -O2 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I$K/svc -I$N/disk -I$N/argus -o "$W/test" $K/svc/tests/setup_wizard_test.c $K/svc/setup_wizard.c $K/svc/setup_inv.c \
    $K/svc/setup_store.c $N/disk/disk.c $N/disk/disk_file.c $N/argus/sha256.c ||
    fail "test does not build"
"$W/test" $K/svc/tests/fixtures/setup "$W" || fail "wizard checks"
# freestanding: the two files build for the kernel with no libc
$CC -std=c11 -ffreestanding -fno-builtin -nostdlib -O2 -Wall -Wextra -Werror -I$K/svc -c -o "$W/inv.o" $K/svc/setup_inv.c ||
    fail "setup_inv.c is not freestanding"
$CC -std=c11 -ffreestanding -fno-builtin -nostdlib -O2 -Wall -Wextra -Werror -I$K/svc -c -o "$W/wiz.o" $K/svc/setup_wizard.c ||
    fail "setup_wizard.c is not freestanding"
ld -r -o "$W/both.o" "$W/inv.o" "$W/wiz.o" || fail "the two objects do not link together"
u=$(nm -u "$W/both.o" | grep -vE ' (memset|memcpy)$' || true)
[ -z "$u" ] || fail "freestanding objects call out: $u"
echo "AIENOS_SETUP_WIZARD: PASS"
