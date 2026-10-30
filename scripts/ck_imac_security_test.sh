#!/usr/bin/env bash
# ck_imac_security_test.sh -- regression tests for the iMac security fixes (L872):
#   F1  the wizard's API key must never be queued on, nor stay in, the unencrypted Ethernet courier;
#   F2  the kernel must verify the owner's signature before it writes a boot section.
# Three small programs are built from the REAL production sources and run:
#   courier_secret_test  x86/courier_udp_x86.c (host build, mocked clock, link and frames)
#   setup_secret_test    svc/setup_wizard.c plus x86/setup_x86.c (the adapter), mocked kernel services
#   boot_verify_test     x86/memory_x86.c boot_section, mocked disk and GPT, real ck_slot_verify and Ed25519
# The last two are x86-64 code (rdtsc, cpuid): cross-built static and run under qemu-x86_64
# (run natively on an x86-64 host). No full-model QEMU, no disk, no network, no private key
# (RFC 8032 public test vectors only).
# PROD_ROOT=<checkout> builds the production sources from another tree (for the red run at the
# commit before the fix, e.g. a worktree of e3f1c6ac); the tests always come from this tree.
# Ends with "AIENOS_IMAC_SECURITY: PASS" or "AIENOS_IMAC_SECURITY: FAIL <why>".
set -u -o pipefail
cd "$(dirname "$0")/.."
HERE=$PWD
P=$(cd "${PROD_ROOT:-.}" && pwd)
K=native/kernel
tag=AIENOS_IMAC_SECURITY
bad=
fail() { echo "${tag}: FAIL $*"; exit 1; }
W=$(mktemp -d "${TMPDIR:-/tmp}/ck_imac_security.XXXXXX")
trap 'rm -rf "$W"' EXIT
T=$HERE/$K/x86/tests
HCC=${HOSTCC:-cc}
XCC=${X86CC:-x86_64-linux-gnu-gcc}
run_x86() {
    if [ "$(uname -m)" = x86_64 ]; then "$@"; else qemu-x86_64 "$@"; fi
}
# nice 19 on the shared Spark; the builds are small
nice_() { nice -n 19 "$@"; }

# 1. the courier (F1)
nice_ $HCC -std=gnu11 -O2 -Wall -Wextra -Werror -DCK_COURIER_HOST -DCOURIER_X86_C="\"$P/$K/x86/courier_udp_x86.c\"" \
    -I$P/$K/x86 -I$P/native/net -o "$W/courier" $T/courier_secret_test.c $P/native/net/aienos_net.c ||
    fail "courier_secret_test does not build"
"$W/courier" | grep -v '^courier: Ethernet' || bad="$bad courier_secret_test"

# 2. the wizard and its adapter (F1)
nice_ $XCC -std=gnu11 -O1 -static -Wall -Wextra -Werror -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -DSETUP_X86_C="\"$P/$K/x86/setup_x86.c\"" -I$P/$K/x86 -I$P/$K/svc -I$P/$K -I$P/$K/core -I$P/native/boot \
    -I$P/native/disk -I$P/native/argus -I$P/$K/dev -I$P/$K/include -I$P/$K/ui \
    -o "$W/setup" $T/setup_secret_test.c $P/$K/svc/setup_wizard.c $P/$K/svc/setup_inv.c ||
    fail "setup_secret_test does not build"
run_x86 "$W/setup" || bad="$bad setup_secret_test"

# 3. the boot section (F2)
nice_ $XCC -std=gnu11 -O1 -static -Wall -Wextra -Werror -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -DMEMORY_X86_C="\"$P/$K/x86/memory_x86.c\"" -I$P/$K/x86 -I$P/$K/svc -I$P/$K -I$P/$K/core -I$P/native/boot \
    -I$P/native/disk -I$P/native/sig -I$P/native/argus -I$P/$K/dev -I$P/$K/include \
    -o "$W/boot" $T/boot_verify_test.c $T/boot_verify_keys.c $P/native/boot/slot_select.c \
    $P/native/sig/ed25519.c $P/native/sig/sha512.c $P/native/sig/ct.c $P/native/argus/sha256.c ||
    fail "boot_verify_test does not build"
run_x86 "$W/boot" || bad="$bad boot_verify_test"

[ -z "$bad" ] || fail "red:$bad"
echo "${tag}: PASS"
