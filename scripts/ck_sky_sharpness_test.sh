#!/bin/sh
# Hosted production-renderer native-pixel contract and adversarial controls.
# Usage: sh scripts/ck_sky_sharpness_test.sh [OUTDIR]
# Historical red: sh scripts/ck_sky_sharpness_test.sh --baseline REV OUTDIR
set -eu
cd "$(dirname "$0")/.."
K=native/kernel
CC=${HOSTCC:-cc}
BASE=
if [ "${1:-}" = --baseline ]; then BASE=$2; shift 2; fi
OUT=${1:-target/native-kernel-sky-sharpness}
mkdir -p "$OUT"
OUT=$(realpath "$OUT")
WORK=$(mktemp -d "${TMPDIR:-/tmp}/ck_sky_sharpness.XXXXXX")
trap 'rm -rf "$WORK"' EXIT HUP INT TERM
mkdir -p "$WORK/$K/ui/tests"
# Only scoped render sources; no service, GPU, image build or full suite.
cp "$K/ui/paint.c" "$K/ui/paint.h" "$K/ui/allen_font.c" "$K/ui/allen_font.h" \
   "$K/ui/sky_data.c" "$K/ui/sky_data.h" "$WORK/$K/ui/"
if [ -n "$BASE" ]; then
    git archive "$BASE" "$K/ui/night.c" "$K/ui/night.h" "$K/ui/sky_real.c" "$K/ui/sky_real.h" | tar -x -C "$WORK"
else
    cp "$K/ui/night.c" "$K/ui/night.h" "$K/ui/sky_real.c" "$K/ui/sky_real.h" "$WORK/$K/ui/"
fi
cp "$K/ui/tests/sky_sharpness_test.c" "$WORK/$K/ui/tests/"
build() {
    "$CC" -std=gnu11 -O2 -Wall -Wextra -Werror ${FLAGS:-} -o "$WORK/test" \
        "$WORK/$K/ui/tests/sky_sharpness_test.c" "$WORK/$K/ui/night.c" \
        "$WORK/$K/ui/sky_data.c" "$WORK/$K/ui/paint.c" "$WORK/$K/ui/allen_font.c"
}
if [ -n "$BASE" ]; then
    FLAGS=-DCK_SKY_BASELINE
    build
    status=0
    "$WORK/test" "$OUT" > "$OUT/baseline.log" || status=$?
    cat "$OUT/baseline.log"
    exit "$status"
fi
build
status=0
"$WORK/test" "$OUT" > "$OUT/after.log" || status=$?
cat "$OUT/after.log"
[ "$status" = 0 ] || exit "$status"
cp "$WORK/$K/ui/sky_real.c" "$WORK/sky.clean"
cp "$WORK/$K/ui/night.c" "$WORK/night.clean"
# Mutate copies, never the checkout. Historical cubic lobes and quarter
# screen grid are taken verbatim from the rejected pinned source. Each
# independent defect must be rejected, as must suppressing the point layer.
# init-fog and view-fog reintroduce the diffuse Milky Way into the UI sky in
# ck_night_init and ck_night_view; each must fail its own clear-sky assertion.
for control in static-bloom dynamic-bloom quarter-grid invisible init-fog view-fog; do
    cp "$WORK/sky.clean" "$WORK/$K/ui/sky_real.c"
    cp "$WORK/night.clean" "$WORK/$K/ui/night.c"
    python3 - "$WORK/$K/ui" "$control" <<'PY'
from pathlib import Path
import subprocess, sys
root, control = Path(sys.argv[1]), sys.argv[2]
sky = (root/'sky_real.c').read_text()
night = (root/'night.c').read_text()
pin = '735c40bff7270cc4d2a88f663ba4b663986b468c'
def historical(name):
    return subprocess.check_output(['git','show',pin+':native/kernel/ui/'+name], text=True)
def section(text, start, end):
    return text[text.index(start):text.index(end)]
def replace_once(text, before, after):
    assert text.count(before) == 1, before
    return text.replace(before,after)
if control == 'static-bloom':
    old = historical('sky_real.c')
    lobe = section(old, 'static void light_add(', 'static uint32_t srgb_of(')
    sky = replace_once(sky, 'static void point_add(', lobe+'static void point_add(')
    sky = replace_once(sky, '        point_add(cv, &all, x16, y16, core, col);',
        '        point_add(cv, &all, x16, y16, core, col);\n'
        '        uint32_t k = 512;\n'
        '        int32_t r0 = (int32_t)(k * 3 / 40);\n'
        '        if (r0 < 12) r0 = 12;\n'
        '        light_add(cv, x16, y16, r0 * 4, (core * 15) >> 7, col);\n'
        '        light_add(cv, x16, y16, r0 * 12, (core * 5) >> 7, col);')
elif control == 'dynamic-bloom':
    night = replace_once(night, '        ck_sky_twinkle(out, r, s, a);',
        '        int32_t r0 = (int32_t)(n->k * 3 / 40);\n'
        '        if (r0 < 12) r0 = 12;\n'
        '        ck_glow(out, r, s->x, s->y, r0 * 4, 0xffffff, a);')
elif control == 'quarter-grid':
    oldsky, oldnight = historical('sky_real.c'), historical('night.c')
    grid = section(oldsky, 'void ck_sky_milkyway_q(', '/* ---- stars ---- */')
    upscale = section(oldnight, 'static void scale_up(', '/* ---- coming out of the dark ---- */')
    replacement = grid+upscale+'''void ck_sky_draw_milkyway(ck_canvas *cv, int32_t cx, int32_t cy, const ck_sky_view *v)
{
    int32_t qw = cv->w/4+2, qh = cv->h/4+2;
    uint16_t *q = malloc((size_t)qw*qh*3*sizeof *q);
    if (!q) abort();
    ck_sky_milkyway_q(q,qw,qh,cv->w,cx,cy,v);
    ck_night n = {.w=cv->w,.h=cv->h,.sky=cv->px};
    scale_up(&n,q,qw);
    free(q);
}

'''
    before = section(sky,'void ck_sky_draw_milkyway(', '/* ---- stars ---- */')
    sky = replace_once(sky,before,replacement)
elif control == 'invisible':
    sky = replace_once(sky,'        point_add(cv, &all, x16, y16, core, col);',
        '        point_add(cv, &all, x16, y16, core * 0, col);')
    sky = replace_once(sky,'    point_add(cv, clip, s->x, s->y, (uint64_t)s->peak * amount >> 8, s->col);',
        '    point_add(cv, clip, s->x, s->y, (uint64_t)s->peak * amount * 0, s->col);')
elif control == 'init-fog':
    night = replace_once(night,'    ck_sky_draw_stars(&cv, cx, cy, &n->view,',
        '    ck_sky_draw_milkyway(&cv, cx, cy, &n->view);\n'
        '    ck_sky_draw_stars(&cv, cx, cy, &n->view,')
elif control == 'view-fog':
    night = replace_once(night,'    ck_sky_draw_stars(&cv, n->cx, n->cy, &n->view,',
        '    ck_sky_draw_milkyway(&cv, n->cx, n->cy, &n->view);\n'
        '    ck_sky_draw_stars(&cv, n->cx, n->cy, &n->view,')
(root/'sky_real.c').write_text(sky)
(root/'night.c').write_text(night)
PY
    build
    status=0
    "$WORK/test" > "$OUT/control-$control.log" || status=$?
    [ "$status" = 1 ] || { echo "SKY_SHARPNESS: FAIL control $control returned $status, expected rejection"; exit 1; }
    # The fog controls are rejected by their own assertion only.
    case $control in
    init-fog) own="clear sky after init"; other="clear sky after view change" ;;
    view-fog) own="clear sky after view change"; other="clear sky after init" ;;
    *) own=; other= ;;
    esac
    if [ -n "$own" ]; then
        grep -q "^FAIL $own " "$OUT/control-$control.log" &&
            grep -q "^PASS $other " "$OUT/control-$control.log" &&
            grep -q "^PASS direct milkyway render " "$OUT/control-$control.log" || {
            echo "SKY_SHARPNESS: FAIL control $control was not rejected by its own assertion ($own)"; exit 1; }
    fi
    echo "PASS rejected control: $control (details $OUT/control-$control.log)"
done
echo "AIENOS_SKY_SHARPNESS: PASS"
