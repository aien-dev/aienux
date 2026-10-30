#!/bin/sh
# ck_allen_canvas_test.sh -- host test of Allen's canvas: the scene language
# the model writes (native/kernel/svc/allen_scene.c) and the way it is painted
# round Allen (native/kernel/ui/canvas.c, hooked into ui/allen_ui.c).
#  1. Kernel build rules: both files compile freestanding with general
#     registers only (no floating point anywhere) and call nothing outside
#     the kernel but memset/memcpy/memmove.
#  2. The parser on fixed scenes, then a fuzz pass (random bytes, mutated
#     scenes, word soup; each input in a buffer of exactly its length) under
#     AddressSanitizer and UndefinedBehaviorSanitizer, with counts.
#  3. Frames at 3840x2160: Allen forms, then plain answers and each sample
#     scene after a line; partial redraws must match full ones; pictures to
#     target/native-kernel-allen-canvas/*.png; frame times printed.
#  4. --live: asks the model server (127.0.0.1:18091, one request at a time,
#     waiting politely while it is busy) the questions in
#     evidence/allen-canvas/questions.txt with the prompt in
#     crates/allen-courier/scene_prompt.txt, keeps every raw reply in
#     $LIVE_DIR (default evidence/allen-canvas/replies; a round cut off part
#     way resumes there), then parses and paints each one
#     (live-*.png) and reports how many parsed cleanly.
#     --replies DIR: the same from replies already kept (no model needed).
# Ends with "AIENOS_ALLEN_CANVAS: PASS" or "AIENOS_ALLEN_CANVAS: FAIL <why>".
set -u
cd "$(dirname "$0")/.."
K=native/kernel
OUT=target/native-kernel-allen-canvas
EVID=evidence/allen-canvas
LIVE=0
REPLIES=""
while [ $# -gt 0 ]; do
    case "$1" in
    --live) LIVE=1 ;;
    --replies) REPLIES="$2"; shift ;;
    *) echo "usage: $0 [--live] [--replies DIR]"; exit 2 ;;
    esac
    shift
done
W=$(mktemp -d "${TMPDIR:-/tmp}/ck_allen_canvas.XXXXXX")
trap 'rm -rf "$W"' EXIT
fail() { echo "AIENOS_ALLEN_CANVAS: FAIL $*"; exit 1; }
CC=${HOSTCC:-cc}
mkdir -p "$OUT"

# 1. as the kernel builds it
for f in svc/allen_scene ui/canvas; do
    $CC -std=gnu11 -O2 -ffreestanding -fno-builtin -mgeneral-regs-only -fno-stack-protector -Wall -Wextra -Werror \
        -c -o "$W/$(basename $f).o" "$K/$f.c" || fail "$f.c does not build freestanding with general registers only"
    extra=$(nm -u "$W/$(basename $f).o" | awk '{print $2}' | grep -v -E '^(ck_|memset$|memcpy$|memmove$)' || true)
    [ -z "$extra" ] || fail "$f.c calls outside the kernel: $extra"
done
echo "kernel rules: allen_scene.c and canvas.c build freestanding, no floating point, no outside calls"

SRC="$K/ui/tests/canvas_test.c $K/ui/canvas.c $K/svc/allen_scene.c $K/ui/allen_ui.c $K/svc/allen_code.c $K/ui/paint.c $K/ui/night.c \
    $K/ui/mind.c $K/ui/flow.c $K/ui/allen_font.c $K/ui/sky_real.c $K/ui/sky_data.c"

# 2. parser and fuzz under the sanitizers
$CC -std=gnu11 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -Wall -Wextra \
    -Werror -o "$W/san" $SRC || fail "test does not build with the sanitizers"
"$W/san" --parse-only --fuzz "${CANVAS_FUZZ:-300000}" || fail "parser or fuzz"

# 4a. live: ask the model first (its replies are kept as evidence)
if [ "$LIVE" = 1 ]; then
    cargo build -q --release -p allen-courier --bin allen-scene || fail "allen-scene does not build"
    REPLIES="${LIVE_DIR:-$EVID/replies}"
    mkdir -p "$REPLIES"
    target/release/allen-scene --questions "$EVID/questions.txt" --out "$REPLIES" \
        --prompt crates/allen-courier/scene_prompt.txt --wait-max 1800 || fail "the model could not be asked"
fi

# 3 (and 4b). frames, at full speed
$CC -std=gnu11 -O2 -Wall -Wextra -Werror -o "$W/test" $SRC || fail "test does not build"
if [ -n "$REPLIES" ]; then
    "$W/test" "$OUT" --fuzz 20000 --replies "$REPLIES" > "$W/log" 2>&1
    rc=$?
    cat "$W/log"
    [ $rc = 0 ] || fail "canvas checks"
    grep -E '^(live |LIVE:|note: frames|note: startup)' "$W/log" > "$REPLIES/parse-report.txt"
else
    "$W/test" "$OUT" --fuzz 20000 || fail "canvas checks"
fi
echo "pictures: $OUT"
echo "AIENOS_ALLEN_CANVAS: PASS"
