# Real sky: measurements and pictures

Branch `feat/real-sky`. The method, sources, limits and the sound and picture audit are in
[docs/ALLEN_SKY_SCIENCE.md](../../docs/ALLEN_SKY_SCIENCE.md). This folder holds what was measured.
Nothing here was measured on the iMac.

## Pictures (build 176bc51: Gaia DR3 level 7 in V on the physical scale, no anchor, gain 1.0; the Magellanic Clouds' light from Gaia raised to the OpenNGC total; 3840x2160 frame model)

| file | what it shows |
|---|---|
| `sky-whole.png` | the whole chosen view, scaled to 1920x1080: stars, the Milky Way band, dark sky |
| `sky-centre-1to1.png` | 1280x720 at full size, centred on the GW150914 position |
| `sky-galaxy0-1to1.png` | the Large Magellanic Cloud at full size: a soft glow, 1.99 times the surrounding sky at its centre (sky test) |
| `sky-galaxy1-1to1.png` | the Small Magellanic Cloud at full size: 1.81 times the surrounding sky at its centre |
| `sky-brightest-1to1.png` | the brightest star at full size, with its glow |
| `gain-3-whole.png`, `gain-5-whole.png` | history: the older anchored build at gain 3 and 5 (before 9ab1281; the gain is retired) |

Made by `scripts/ck_sky_test.sh`. The gain pictures came from test binaries built from the older tables
(see `gain-measure.txt`).

## Files

- `gain-measure.txt`: history. The retired gain 1.2, tied to the old 678 S10 anchor and measured with
  the inverse projection that was off by two (fixed at 9ab1281). The scale is now Gaia's own, in V.
- `generator-report.txt`: the report of the generator run (inputs with sha256, the sky's peak and 90 %
  region, star counts, photometry, the "cloud light" lines for the LMC, SMC and M31, table sizes).
- `sky-test.log`: the host sky test, 15 checks, 0 failed (two new: each Cloud glows at its place).
- `allen-memory-test.log`: the Allen memory test, 189 checks, 0 failed.
- `allen-ui-test.log`: the Allen window test and its picture checks; the place test (26 checks) is
  run by the same script.
- `chirp-audit.txt`: the sound and picture numbers set against GW150914 (see section 5 of the science document).

## Sizes

| table | bytes |
|---|---|
| stars in view (1035 of 8404 to V 6.5) | 8280 |
| Milky Way luma | 64980 |
| Milky Way colour | 32580 |
| `sky_data.c` as source text (level 7) | 453141 |
| `IMACKERN.EFI` (built at `176bc51`, `make -C native/kernel imac-kernel`, -Werror) | 405504 |

## Timings (Spark, one core, shared machine: not a quiet-lock timing, not the iMac)

- The whole sky at 3840x2160 (`ck_night_init`, best of 3 per run): level 6 135 to 136 ms; level 7 109 to 140 ms.
  The ranges overlap: no difference between the levels can be claimed.
- At 176bc51 (level 7): 114.8 ms and 200.6 ms in two runs of the sky test (shared machine; not a timing claim).
- Generator, level 7 (joins four Gaia chunks, 196608 cells): 72 to 115 s. Level 6: about 2 s.

## What was not done

- The Clouds' inner structure (the LMC bar, 30 Doradus) is below the 1 degree grid; Gaia's crowding thins the bar (not measured).
- M31 has no steady light: Gaia resolves 5.8 % of it.
- The grid is 1 degree; a 0.5 degree grid is not built.
- Nothing was run on the iMac's screen; the on-screen ratios are measured on the host's sRGB path.
- The full `make -C native/kernel test` suite and the QEMU tests were not run here.
