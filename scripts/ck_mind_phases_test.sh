#!/bin/sh
# Focused host-only production mind renderer regression; no UI boot or image build.
set -eu
cd "$(dirname "$0")/.."
K=native/kernel
OUT=${1:-target/native-kernel-mind-phases}
mkdir -p "$OUT"
CC=${HOSTCC:-cc}
"$CC" -std=gnu11 -O2 -Wall -Wextra -Werror -I "$K/ui" -o "$OUT/mind-phases-test" \
    "$K/ui/tests/mind_phases_test.c" "${MIND_SOURCE:-$K/ui/mind.c}" \
    "$K/ui/paint.c" "$K/ui/night.c" "$K/ui/sky_real.c" "$K/ui/sky_data.c" "$K/ui/allen_font.c"
"$OUT/mind-phases-test" "$OUT" "${2:-}"

if [ "$#" -lt 2 ]; then
    for control in warm large-disc suppressed halo; do
        if "$OUT/mind-phases-test" - "--$control-control" > "$OUT/control-$control.log" 2>&1; then
            echo "MIND_PHASES: FAIL detector accepted $control control"
            exit 1
        else
            status=$?
            [ "$status" -eq 1 ] || exit "$status"
        fi
        echo "MIND_PHASES: rejected $control control"
    done
    # Exact source mutants exercise the production point/tail/heart paths.
    # Python is a hosted test oracle only; no production dependency is added.
    python3 - "$K/ui/mind.c" "$OUT" <<'PY'
from pathlib import Path
import sys
source = Path(sys.argv[1]).read_text()
mutations = {
    "warm-tail": ("16 - k * 4, CK_RGB(48, 112, 192)", "16 - k * 4, CK_RGB(255, 222, 176)"),
    "white-heart": ("ck_mix(CK_RGB(32, 72, 144), CK_RGB(48, 112, 192), m->heart)",
                    "ck_mix(CK_RGB(255, 210, 170), CK_RGB(255, 250, 240), m->heart)"),
    "bright-point": ("ck_mix(CK_RGB(32, 72, 144), CK_RGB(48, 112, 192), m->act[i])",
                     "ck_mix(CK_RGB(206, 222, 255), CK_RGB(255, 214, 160), m->act[i])"),
    "large-point": ("int32_t rad = 16;", "int32_t rad = 96;"),
    "large-heart": ("m->hr = 24;", "m->hr = 160;"),
}
for name, (old, new) in mutations.items():
    if source.count(old) != 1:
        raise SystemExit("mutant anchor changed: " + name)
    Path(sys.argv[2], "mutant-" + name + ".c").write_text(source.replace(old, new))
PY
    for mutant in warm-tail white-heart bright-point large-point large-heart; do
        "$CC" -std=gnu11 -O2 -Wall -Wextra -Werror -I "$K/ui" -o "$OUT/mutant-$mutant-test" \
            "$K/ui/tests/mind_phases_test.c" "$OUT/mutant-$mutant.c" \
            "$K/ui/paint.c" "$K/ui/night.c" "$K/ui/sky_real.c" "$K/ui/sky_data.c" "$K/ui/allen_font.c"
        if "$OUT/mutant-$mutant-test" - > "$OUT/mutant-$mutant.log" 2>&1; then
            echo "MIND_PHASES: FAIL accepted source mutant $mutant"
            exit 1
        else
            status=$?
            [ "$status" -eq 1 ] || exit "$status"
        fi
        echo "MIND_PHASES: rejected source mutant $mutant"
    done
fi