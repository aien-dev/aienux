# Slow flight through the real nearby stars: plan (no code yet)

Owner direction (2026-10-19, 18:55 CDT): the sky is either "the sky above me" or "somewhere
new". The later "somewhere new" option is a slow flight through real nearby stars in 3D,
built from Gaia parallaxes. This note is the plan. Nothing here is built or committed as code.

## What the data are

Gaia DR3, table `gaiadr3.gaia_source`, ESA/Gaia/DPAC, CC BY-NC 3.0 IGO (credit ESA/Gaia/DPAC).
Columns used: `ra`, `dec` (degrees, epoch 2016.0), `parallax` (milliarcseconds),
`parallax_over_error`, `pmra`, `pmdec` (mas/yr), `phot_g_mean_mag`, `bp_rp`.

Counts, measured on 2026-10-19 with synchronous TAP `COUNT(*)` queries on the ESA server:

| selection | stars |
|---|---|
| parallax > 10 mas (within 100 pc) and parallax_over_error > 10 | 372,226 |
| parallax > 20 mas (within 50 pc) and parallax_over_error > 10 | 48,809 |

The second is the first version's target: 48,809 stars is a size the kernel can hold. (Those
counts are the whole sky. The view a flight sees is a fraction of them.)

## How a star becomes a point in space

1. Distance = 1000 / parallax (parsecs). With parallax_over_error > 10 the parallax error is
   under 10 percent, so inverting it is safe. Stars with a poorer parallax are left out.
2. Apply the Gaia parallax zero point before inverting (Lindegren et al. 2021, A&A 649, A4;
   about -0.017 mas across the sky). Its effect at 50 pc is small but it is applied, not ignored.
3. Position at the time of the flight: move the star along its proper motion from epoch 2016.0
   (`pmra` is already multiplied by cos dec; mas/yr). For a star moving 10 arcseconds a year (the Barnard's
   Star class) the shift over ten years is 100 arcseconds, about 0.03 degree, so it matters and is applied.
4. Unit vector times distance gives x, y, z in parsecs, in the J2000 equatorial frame. The
   kernel can then use the same integer projection as the sky now (sky_real.c).

## The flight

- The camera moves slowly through the cloud along a straight or gently curved path (the path,
  the speed and the time of one flight are NOT yet decided; they need Drake's choice).
- Each frame: the stars within a set distance of the camera are projected with a perspective
  divide (nearer stars larger and brighter, with the inverse-square law for flux).
- Brightness: from G magnitude, through the same star model as the sky (the magnitude law in
  integers). Colour: from BP-RP through the black-body chain already in the generator. The map from
  BP-RP to B-V is an approximation and must be stated where used.
- Compute: flat arrays (x, y, z, flux, colour), as the direction doc asks, so the Radeon
  compute path (ledger L142 to L144) can read them. The CPU path is the same code on the integer
  side, so the test can compare both.

## Data as a table

- The generator is plain C, like `native/kernel/tools/ck_sky_gen.c`. No Python is committed.
- The table holds one record per star, in the flight's fixed frame. Its size is set by the
  record: this is a design figure, not a measurement: about 16 bytes per star, so about 0.8 MB for
  48,809 stars. It must be measured (`ls -l` of the generated file, and the IMAC kernel size)
  before it is accepted.
- Fetch by the same pattern as `scripts/ck_sky_gen.sh`: TAP query text in the script, sha256 pinned,
  refuse any other file. Credit and licence in NOTICE and docs/ALLEN_SKY_SCIENCE.md.

## Steps and what proves each one

1. Generator and query (C, pinned sha256). Proof: a count in the report matches the counts above.
2. Host test: projection of known stars against their catalogue positions (as the sky test does
   for BSC5 stars), at the chosen epoch. Proof: worst error in pixels, printed.
3. Frames at three camera positions, looked at by a person. Proof: pictures in evidence/.
4. Time per frame at 3840 x 2160 on one core, and the same on the GPU path once it exists.
   Proof: the numbers, with the method. No estimate is accepted as a timing.
5. Allen command: "fly through the nearby stars" (or similar), plus "stop" and "the sky above me"
   to go back. Proof: the reply test in ck_allen_ui_test.sh.

## Not decided (for Drake)

- Speed and length of one flight, direction of travel, and whether the path loops.
- Whether the flight starts from the sky above you (the Sun's neighbourhood is the same for
  everyone) or from a random place each boot.
- Whether to include the 100 pc set (372,226 stars) or only the 50 pc set in the first version.
