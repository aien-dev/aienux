#!/bin/sh
# ck_allen_ui_test.sh -- host test of Allen's home (native/kernel/ui: the
# night sky, his presence, the water and ink round him, the screen around them) and his first local
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
#  4. Goodbye, the dusk: six pictures through it (allen-8-horizon-1..6.png)
#     and the closed horizon; the light only falls after the disk's height,
#     the middle of the screen is brighter than its edges mid-dusk, the last
#     frame is pure black; then the dawn brings the sky back. The dusk's own
#     frame times are printed next to the whole run's.
#  5. Coordinates typed to Allen become his place (allen_place_test.c).
#  6. Every mind mode and replayed activity phase: no newly introduced warm or
#     white interior hot spots, bounded geometry, and meaningful visibility;
#     adversarial warm/white, enlarged-disc and missing-being controls.
#  7. Native Milky Way samples and compact catalogue/twinkle point footprints;
#     coarse-grid, static-bloom and dynamic-bloom controls must be rejected.
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
$CC -std=gnu11 -O2 -Wall -Wextra -Werror -pthread -o "$W/test" $K/ui/tests/allen_ui_test.c $K/ui/allen_ui.c $K/svc/allen_code.c \
    $K/ui/paint.c $K/ui/night.c $K/ui/sky_real.c $K/ui/sky_data.c $K/ui/mind.c $K/ui/flow.c \
    $K/ui/allen_font.c $K/ui/canvas.c $K/svc/allen_scene.c $K/svc/allen_reply.c $K/svc/allen_notes.c $K/svc/allen_intent.c \
    $K/svc/setup_wizard.c $K/svc/setup_inv.c || fail "test does not build"
"$W/test" "$OUT" || fail "window checks"
echo "pictures: $OUT"
# 5. Coordinates typed to Allen ("35 north 90 west", "35.1, -90.0") become his place; lines that are no place
#    (arithmetic, names, a bare pair of numbers) do not (native/kernel/svc/tests/allen_place_test.c).
$CC -std=gnu11 -O2 -Wall -Wextra -Werror -I $K/svc -o "$W/place" $K/svc/tests/allen_place_test.c $K/svc/allen_reply.c \
    $K/svc/allen_notes.c $K/svc/allen_intent.c || fail "place test does not build"
"$W/place" || fail "place checks"
sh scripts/ck_mind_phases_test.sh || fail "mind phase checks"
sh scripts/ck_sky_sharpness_test.sh || fail "native sky sharpness checks"
echo "AIENOS_ALLEN_UI: PASS"
