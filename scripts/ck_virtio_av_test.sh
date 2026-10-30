#!/bin/sh
# ck_virtio_av_test.sh -- host test of the VM screen and speaker drivers'
# pure parts: the virtio-gpu and virtio-sound commands checked field by field
# against the Linux UAPI copy of the virtio spec, the reply parsers, and the
# polled request core (native/kernel/dev/virtio_q.c) compiled clean.
# Hosted only. Ends with "AIENOS_VIRTIO_AV: PASS" or "AIENOS_VIRTIO_AV: FAIL <why>".
set -u
cd "$(dirname "$0")/.."
K=native/kernel
W=$(mktemp -d "${TMPDIR:-/tmp}/ck_virtio_av.XXXXXX")
trap 'rm -rf "$W"' EXIT
fail() { echo "AIENOS_VIRTIO_AV: FAIL $*"; exit 1; }
CC=${HOSTCC:-cc}
$CC -std=gnu11 -O2 -Wall -Wextra -Werror -DCK_HOSTED -Inative/net -o "$W/test" $K/dev/tests/virtio_av_test.c \
    $K/dev/virtio_gpu.c $K/dev/virtio_snd.c $K/dev/virtio_q.c || fail "test does not build"
"$W/test" || fail "layout checks"
echo "AIENOS_VIRTIO_AV: PASS"
