#!/bin/sh
# ck_allen_code_test.sh -- host test of code from the Spark: recorded model
# replies (native/kernel/svc/tests/fixtures/code) turned into wire lines by
# the courier itself (allen-courier --split, crates/allen-courier/src/code.rs)
# and taken by the kernel's side (native/kernel/svc/allen_code.c): short code,
# long code in many pieces, a lost piece asked again, a spoiled piece, hostile
# lines, a slow and a silent Spark, the screen's rows and scroll, and Allen's
# coding requests told from talk (svc/allen_intent.c). See
# native/kernel/svc/tests/allen_code_test.c. Hosted only. Ends with
# "AIENOS_ALLEN_CODE: PASS" or "AIENOS_ALLEN_CODE: FAIL <why>".
set -u
cd "$(dirname "$0")/.."
K=native/kernel
F=$K/svc/tests/fixtures/code
W=$(mktemp -d "${TMPDIR:-/tmp}/ck_allen_code.XXXXXX")
trap 'rm -rf "$W"' EXIT
fail() { echo "AIENOS_ALLEN_CODE: FAIL $*"; exit 1; }
CC=${HOSTCC:-cc}
cargo build -q --release -p allen-courier --bin allen-courier || fail "allen-courier does not build"
for n in short long; do
    target/release/allen-courier --split "$F/$n.txt" > "$W/$n.wire" || fail "allen-courier --split $n"
done
$CC -std=c11 -D_DEFAULT_SOURCE -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I$K/svc -o "$W/test" $K/svc/tests/allen_code_test.c $K/svc/allen_code.c $K/svc/allen_intent.c \
    $K/svc/allen_reply.c $K/svc/allen_notes.c || fail "test does not build"
"$W/test" "$F" "$W" || fail "code checks"
# freestanding: the kernel side builds with no libc and no floating point
$CC -std=c11 -ffreestanding -fno-builtin -nostdlib -O2 -Wall -Wextra -Werror -mgeneral-regs-only -I$K/svc \
    -c -o "$W/code.o" $K/svc/allen_code.c || fail "allen_code.c is not freestanding"
u=$(nm -u "$W/code.o" | grep -vE ' (memset|memcpy)$' || true)
[ -z "$u" ] || fail "allen_code.c calls out: $u"
echo "AIENOS_ALLEN_CODE: PASS"
