#!/bin/sh
# ck_allen_ui_test.sh -- host test of Allen's home (native/kernel/ui: the
# night sky, his presence, the screen around them) and his first local
# answers (native/kernel/svc/allen_reply.c).
#  1. Allen's answers: "Hello Allen" (any case, punctuation, spacing) gives
#     "Hello Humanity."; anything it does not know gives "I don't know that yet."
#  2. Allen's typeface: every printable character rasterizes, measures and
#     has ink.
#  3. The startup at 3840x2160 BGRX: stars come out, two black holes merge,
#     Allen forms; the bar comes up when he wakes, takes a typed line, he
#     answers under him; left alone, only he stays on screen; a saved note
#     shows on the notes map and goes again. Drawing only what changed must
#     match drawing everything. 1280x800 RGBX with a padded pitch: red and
#     blue swap, nothing past the visible width, and three helper threads
#     draw exactly the same frames as one.
# Pictures of each state land in target/native-kernel-allen-ui/ for a person
# to look at. Hosted only. Ends with "AIENOS_ALLEN_UI: PASS" or
# "AIENOS_ALLEN_UI: FAIL <why>".
set -u
cd "$(dirname "$0")/.."
K=native/kernel
OUT=target/native-kernel-allen-ui
W=$(mktemp -d "${TMPDIR:-/tmp}/ck_allen_ui.XXXXXX")
trap 'rm -rf "$W"' EXIT
fail() { echo "AIENOS_ALLEN_UI: FAIL $*"; exit 1; }
CC=${HOSTCC:-cc}
mkdir -p "$OUT"
$CC -std=gnu11 -O2 -Wall -Wextra -Werror -pthread -o "$W/test" $K/ui/tests/allen_ui_test.c $K/ui/allen_ui.c \
    $K/ui/paint.c $K/ui/night.c $K/ui/mind.c \
    $K/ui/allen_font.c $K/svc/allen_reply.c $K/svc/allen_notes.c || fail "test does not build"
"$W/test" "$OUT" || fail "window checks"
echo "pictures: $OUT"
echo "AIENOS_ALLEN_UI: PASS"
