#!/bin/sh
# ck_allen_memory_test.sh -- host test of Allen's lasting memory: the record
# format and two-copy store (native/kernel/svc/allen_memory.c), the AIENOS
# partition added in unused space next to an existing one
# (native/kernel/dev/gpt_add.c), power cuts during both, and Allen taught,
# restarted from the bytes on disk and asked (svc/allen_reply.c). See
# native/kernel/svc/tests/allen_memory_test.c. Hosted only. Ends with
# "AIENOS_ALLEN_MEMORY: PASS" or "AIENOS_ALLEN_MEMORY: FAIL <why>".
set -u
cd "$(dirname "$0")/.."
K=native/kernel
N=native
W=$(mktemp -d "${TMPDIR:-/tmp}/ck_allen_memory.XXXXXX")
trap 'rm -rf "$W"' EXIT
fail() { echo "AIENOS_ALLEN_MEMORY: FAIL $*"; exit 1; }
CC=${HOSTCC:-cc}
$CC -std=c11 -D_DEFAULT_SOURCE -O2 -Wall -Wextra -Werror -I$K/svc -I$K/dev -I$K/tools -I$N/disk -I$N/argus \
    -o "$W/test" $K/svc/tests/allen_memory_test.c $K/svc/allen_memory.c $K/svc/allen_reply.c $K/svc/allen_notes.c \
    $K/dev/gpt_add.c $K/dev/disk_part.c $K/tools/gpt_write.c $N/disk/disk.c $N/disk/disk_file.c \
    $N/argus/sha256.c || fail "test does not build"
"$W/test" "$W" || fail "memory checks"
echo "AIENOS_ALLEN_MEMORY: PASS"
