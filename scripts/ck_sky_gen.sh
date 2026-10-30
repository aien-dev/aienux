#!/bin/sh
# ck_sky_gen.sh -- makes native/kernel/ui/sky_data.c, the real sky Allen
# opens on, from real astronomical data (see native/kernel/tools/ck_sky_gen.c
# and docs/ALLEN_SKY_SCIENCE.md for the sources, their licences and credits).
#
# Downloads each input once into $SKY_CACHE (default ~/.cache/aienos-sky),
# checks its sha256 (a file that does not match is refused), unpacks the two
# .gz files, builds the generator (plain C, needs only cc, libm, curl, gzip and
# sha256sum) and writes the C tables plus a plain-text report of what it found.
# The kernel itself never touches the network: the tables are compiled in.
#
# Settings (environment; the defaults are the committed sky):
#   SKY_RA, SKY_DEC  view centre, degrees (default: GW150914's most probable point)
#   SKY_FOV          degrees of sky across the screen's width (80)
#   SKY_ROLL         degrees the view is turned from north up (0)
#   SKY_MAG          faintest star, V magnitude (6.5, the naked-eye limit)
#   SKY_GRID         Milky Way samples across the screen's width (240)
#   SKY_EXPOSURE     linear light at the middle of a magnitude 6.5 star (see the tool)
#   SKY_GAIN         a test multiplier on the Milky Way and Clouds light (1.0: the physical scale; no anchor, see the report)
#   SKY_SKYGLOW      the dark sky's own glow, V magnitudes per square arcsecond (22.0)
#   SKY_HPX_LEVEL    Gaia HEALPix level of the Milky Way aggregates: 7 (196608 cells, the default) or 6
#                    (49152 cells; the level-7 chunks are fetched if missing)
#   SKY_OUT          where the tables go (default native/kernel/ui/sky_data.c)
set -eu
cd "$(dirname "$0")/.."
C=${SKY_CACHE:-$HOME/.cache/aienos-sky}
OUT=${SKY_OUT:-native/kernel/ui/sky_data.c}
REPORT=${OUT%.c}.txt
mkdir -p "$C"
W=$(mktemp -d "${TMPDIR:-/tmp}/ck_sky_gen.XXXXXX")
trap 'rm -rf "$W"' EXIT
OPEN=be3cd6c5498806a2fbb2f7e0452ef802dbb02be0 # OpenNGC commit the inputs are pinned to
: > "$W/inputs"
fetch() { # key file url sha256
    if [ ! -f "$C/$2" ] || [ "$(sha256sum "$C/$2" | cut -d' ' -f1)" != "$4" ]; then
        echo "ck_sky_gen: downloading $3"
        curl -fsSL --retry 3 -m 900 -o "$C/$2.part" "$3"
        mv "$C/$2.part" "$C/$2"
    fi
    [ "$(sha256sum "$C/$2" | cut -d' ' -f1)" = "$4" ] || { echo "ck_sky_gen: $2 does not match its sha256"; exit 1; }
    echo "input $1: $3 sha256 $4" >> "$W/inputs"
}
fetch bsc5 catalog.gz https://cdsarc.cds.unistra.fr/ftp/V/50/catalog.gz 3dc44b1e90be8fbe5bcc7656032560f51275f985c7e3f783c9028e1838ec7bed
# Gaia DR3 Milky Way: full-sky HEALPix level-6 aggregate (49152 cells)
# Query: source counts + summed G/BP/RP flux (stars V>7), grouped by HPX6
# ESA/Gaia/DPAC, CC BY-NC 3.0 IGO.  Credit: ESA/Gaia/DPAC.
GAIA_TAP=https://gea.esac.esa.int/tap-server/tap/async
GAIA_Q1="SELECT GAIA_HEALPIX_INDEX(6, source_id) AS hpx6, COUNT(*) AS nsrc, SUM(phot_g_mean_flux) AS g_flux, SUM(phot_bp_mean_flux) AS bp_flux, SUM(phot_rp_mean_flux) AS rp_flux FROM gaiadr3.gaia_source WHERE phot_g_mean_flux IS NOT NULL AND phot_g_mean_mag > 7.0 GROUP BY hpx6 ORDER BY hpx6"
GAIA_Q2="SELECT GAIA_HEALPIX_INDEX(6, source_id) AS hpx6, AVG(ag_gspphot) AS mean_ag, COUNT(*) AS n FROM gaiadr3.astrophysical_parameters WHERE ag_gspphot IS NOT NULL GROUP BY hpx6 ORDER BY hpx6"
gaia_fetch() { # filename query sha256
    if [ ! -f "$C/$1" ] || [ "$(sha256sum "$C/$1" | cut -d' ' -f1)" != "$3" ]; then
        rm -f "$C/$1"
        echo "ck_sky_gen: querying Gaia DR3 ($1)..."
        JOB=$(curl -fsS --max-time 30 -X POST "$GAIA_TAP" \
            --data-urlencode "REQUEST=doQuery" --data-urlencode "LANG=ADQL" \
            --data-urlencode "FORMAT=csv" --data-urlencode "PHASE=RUN" \
            --data-urlencode "QUERY=$2" -w "%{redirect_url}" 2>/dev/null | tail -1)
        [ -n "$JOB" ] || { echo "ck_sky_gen: Gaia TAP job submission failed"; exit 1; }
        for i in $(seq 1 120); do
            PHASE=$(curl -fsS --max-time 10 "$JOB/phase" 2>/dev/null)
            case "$PHASE" in COMPLETED) break ;; ERROR|ABORTED) echo "ck_sky_gen: Gaia job $PHASE"; exit 1 ;; esac
            sleep 3
        done
        curl -fsSL --max-time 300 -L "$JOB/results/result" -o "$C/$1.part"
        mv "$C/$1.part" "$C/$1"
        echo "ck_sky_gen: downloaded $C/$1 ($(wc -l < "$C/$1") rows)"
    fi
    [ "$(sha256sum "$C/$1" | cut -d' ' -f1)" = "$3" ] || { echo "ck_sky_gen: $1 does not match its sha256"; exit 1; }
    echo "input $1: $GAIA_TAP ADQL (query in docs/ALLEN_SKY_SCIENCE.md) sha256 $3" >> "$W/inputs"
}
gaia_fetch gaia_hpx6.csv "$GAIA_Q1" 334bde2b6359747e2cc3d7f3db7bb3776d3273836215804249a4e9730935c80e
gaia_fetch gaia_dust.csv "$GAIA_Q2" 105f8f51de8cc473ff34fd8a0ffd455179c0d12f85de59165fafe9551f015e25
# Level 7 (SKY_HPX_LEVEL=7): the four TAP chunks by HEALPix range (scripts/l7 chunk queries, see
# docs/ALLEN_SKY_SCIENCE.md section 2) are joined into one table each, header once, and checked.
LEVEL=${SKY_HPX_LEVEL:-7}
if [ "$LEVEL" = 7 ]; then
    # the four TAP chunks (49152 cells each, HEALPix ranges), fetched once if missing
    gaia_chunk() { # filename query
        [ -f "$C/$1" ] && return 0
        echo "ck_sky_gen: querying Gaia DR3 level 7 ($1)..."
        JOB=$(curl -fsS --max-time 60 -X POST "$GAIA_TAP" --data-urlencode "REQUEST=doQuery" \
            --data-urlencode "LANG=ADQL" --data-urlencode "FORMAT=csv" --data-urlencode "PHASE=RUN" \
            --data-urlencode "QUERY=$2" -w "%{redirect_url}" 2>/dev/null | tail -1)
        [ -n "$JOB" ] || { echo "ck_sky_gen: Gaia TAP job submission failed"; exit 1; }
        for i in $(seq 1 600); do
            PHASE=$(curl -fsS --max-time 20 "$JOB/phase" 2>/dev/null)
            case "$PHASE" in COMPLETED) break ;; ERROR|ABORTED) echo "ck_sky_gen: Gaia job $PHASE"; exit 1 ;; esac
            sleep 5
        done
        curl -fsSL --max-time 600 -L "$JOB/results/result" -o "$C/$1.part" && mv "$C/$1.part" "$C/$1"
    }
    for c in 1 2 3 4; do
        LO=$(( (c - 1) * 49152 )); HI=$(( c * 49152 - 1 ))
        gaia_chunk "gaia_hpx7_flux_$c.csv" "SELECT GAIA_HEALPIX_INDEX(7, source_id) AS hpx7, COUNT(*) AS nsrc, SUM(phot_g_mean_flux) AS g_flux, SUM(phot_bp_mean_flux) AS bp_flux, SUM(phot_rp_mean_flux) AS rp_flux FROM gaiadr3.gaia_source WHERE phot_g_mean_flux IS NOT NULL AND phot_g_mean_mag > 7.0 AND GAIA_HEALPIX_INDEX(7, source_id) BETWEEN $LO AND $HI GROUP BY hpx7 ORDER BY hpx7"
        gaia_chunk "gaia_hpx7_dust_$c.csv" "SELECT GAIA_HEALPIX_INDEX(7, source_id) AS hpx7, AVG(ag_gspphot) AS mean_ag, COUNT(*) AS n FROM gaiadr3.astrophysical_parameters WHERE ag_gspphot IS NOT NULL AND GAIA_HEALPIX_INDEX(7, source_id) BETWEEN $LO AND $HI GROUP BY hpx7 ORDER BY hpx7"
    done
    for k in flux dust; do
        : > "$C/gaia_hpx7_${k}_all.part"
        for c in 1 2 3 4; do
            [ -f "$C/gaia_hpx7_${k}_$c.csv" ] || { echo "ck_sky_gen: missing $C/gaia_hpx7_${k}_$c.csv (run the level-7 chunk queries first)"; exit 1; }
            if [ $c = 1 ]; then cat "$C/gaia_hpx7_${k}_$c.csv"; else tail -n +2 "$C/gaia_hpx7_${k}_$c.csv"; fi
        done > "$C/gaia_hpx7_${k}_all.part"
    done
    mv "$C/gaia_hpx7_flux_all.part" "$C/gaia_hpx7.csv"; mv "$C/gaia_hpx7_dust_all.part" "$C/gaia_dust7.csv"
    for f in gaia_hpx7.csv gaia_dust7.csv; do
        want=4d1412d3df02f6ee34120b8fd9b96bde6766fcaf50c569a94b45b72d4ec151cd
        [ $f = gaia_dust7.csv ] && want=ce46146f06fa33791d81cde3f02f19a7ac259e5555b13c03cdb8092e75174d89
        [ "$(sha256sum "$C/$f" | cut -d' ' -f1)" = "$want" ] || { echo "ck_sky_gen: $f (joined chunks) does not match its sha256"; exit 1; }
        echo "input $f: $GAIA_TAP ADQL level 7, four HEALPix-range chunks joined, sha256 $want" >> "$W/inputs"
    done
fi
fetch ngc NGC.csv https://raw.githubusercontent.com/mattiaverga/OpenNGC/$OPEN/database_files/NGC.csv 54abea0b57b0a103831bfc7a8e3a99a23acfbf4bb581fcf3d8af539cbb826c84
fetch ngc_addendum addendum.csv https://raw.githubusercontent.com/mattiaverga/OpenNGC/$OPEN/database_files/addendum.csv c93f983dda6780b76b7b2e9271a856a892627710148a982ee98101a54c642fdd
fetch gw150914 GW150914_skymap.fits.gz https://dcc.ligo.org/public/0157/P1800381/007/GW150914_skymap.fits.gz a8a4bdeb46aade1389e594e466ee6372a401157188df636eae2bbb3eab2805f2
gzip -dc "$C/catalog.gz" > "$W/bsc5.dat"
gzip -dc "$C/GW150914_skymap.fits.gz" > "$W/GW150914_skymap.fits"
ln -s "$C/NGC.csv" "$C/addendum.csv" "$W/" && cp "$C/gaia_hpx$LEVEL.csv" "$W/gaia_hpx$LEVEL.csv" && cp "$C/gaia_dust$( [ $LEVEL = 7 ] && echo 7).csv" "$W/gaia_dust$( [ $LEVEL = 7 ] && echo 7).csv"
${CC:-cc} -std=gnu11 -O2 -Wall -Wextra -Werror -DHPX_LEVEL=$LEVEL -o "$W/ck_sky_gen" native/kernel/tools/ck_sky_gen.c -lm
set -- --cache "$W" --out "$OUT" --report "$REPORT" --inputs "$W/inputs"
[ -n "${SKY_RA:-}" ] && set -- "$@" --ra "$SKY_RA"
[ -n "${SKY_DEC:-}" ] && set -- "$@" --dec "$SKY_DEC"
[ -n "${SKY_FOV:-}" ] && set -- "$@" --fov "$SKY_FOV"
[ -n "${SKY_ROLL:-}" ] && set -- "$@" --roll "$SKY_ROLL"
[ -n "${SKY_MAG:-}" ] && set -- "$@" --mag "$SKY_MAG"
[ -n "${SKY_GRID:-}" ] && set -- "$@" --grid "$SKY_GRID"
[ -n "${SKY_EXPOSURE:-}" ] && set -- "$@" --exposure "$SKY_EXPOSURE"
[ -n "${SKY_GAIN:-}" ] && set -- "$@" --gain "$SKY_GAIN"
[ -n "${SKY_SKYGLOW:-}" ] && set -- "$@" --skyglow "$SKY_SKYGLOW"
"$W/ck_sky_gen" "$@"
echo "ck_sky_gen: wrote $OUT and $REPORT"
