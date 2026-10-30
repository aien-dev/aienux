# Allen's sky and sound: the science audit

Owner's rule (Drake, 2026-10-19): the whole idea of this system is grounded in
real astrophysics. The merger sound is there because it is real. The sky behind
Allen must be the real sky, not an invented one.

This note says, for every picture and sound in Allen's startup, what is real,
what is a simplification, and what is only an illustration. Every number comes
from a source that is named, or from a measurement that is in `evidence/real-sky/`.
Where a source says nothing, that is said too. The code is in
`native/kernel/ui/` (`night.c`, `sky_real.c`, `sky_data.c`, `chirp.c`).

Words: "design pixel" is one pixel of a 1920 wide screen; the picture scales
from there. V is the visual magnitude of a star (smaller is brighter, 6.5 is
about the faintest star a person sees on a dark night). B-V is a star's colour
number (smaller is bluer).

## 1. The verdict, one line each

| Part | Verdict |
|---|---|
| Stars (9096 in the catalogue, 8404 to V 6.5) | Real: position, brightness, colour. Limits in section 3. |
| Milky Way | Real: Gaia DR3 G, BP and RP flux per HEALPix level-7 cell, moved to V with each cell's own colour, on the physical scale with no anchor or gain. Brightest band V 20.17 mag/arcsec2 against the published 20.7 (Leinert et al. 1998), section 3. |
| Dark sky glow | Real: V 22.0 mag/arcsec2, the published dark-site sky (Leinert et al. 1998). Not boosted. |
| Large and Small Magellanic Clouds | Real: true place, size and tilt (OpenNGC); their light is Gaia DR3's resolved stars raised to the catalogue total V (LMC 0.29, SMC 2.30), section 3. Visible: centre 2.0 (LMC) and 1.8 (SMC) times the surrounding sky. |
| The view | Real: a pinhole-camera projection centred on GW150914's most probable sky position. |
| Starlight bent by the black holes | Real physics (point-mass lens), with one invented edge and one invented scale (section 4). |
| Black hole shadows and the thin ring | Real size rule (shadow radius sqrt(27)/2 Rs), drawn as a simple disc and ring. |
| Orbit and spiral | Real law (separation as t^(1/4), angle as t^(5/8)), with time compressed and the number of turns cut (section 4). |
| Merger and the new, larger hole | Right to within 5 percent (section 4). |
| The ripple across the sky | Illustration only. A real wave cannot be seen. |
| Ringing of the final hole | Illustration, wrong timescale (section 4). |
| The startup sound | The law for the pitch is real. The sweep, the cycle count and the ring are not the real signal (section 5). |
| Galaxies' sweeping light, twinkle, Allen's mind, the water | Art, not physics (section 6). |

## 2. Sources

| Input | What it gives | Licence and credit |
|---|---|---|
| Yale Bright Star Catalogue, 5th revised ed. (Hoffleit and Warren 1991), CDS V/50 | 9096 stars with J2000 position, proper motion, V, B-V | The CDS ReadMe carries no licence or copyright line. Credit: Hoffleit and Warren 1991, via CDS Strasbourg. |
| Gaia DR3 `gaia_source`, query 1 below (ESA/Gaia/DPAC) | Source count and summed G, BP, RP mean flux of every source with G > 7, per HEALPix level-7 cell (nside 128, 196608 cells) | CC BY-NC 3.0 IGO. Credit: ESA/Gaia/DPAC. Pinned sha256 in `scripts/ck_sky_gen.sh`. |
| Gaia DR3 `astrophysical_parameters`, query 2 below | Mean G-band extinction `ag_gspphot` per the same level-7 cells (the dust) | CC BY-NC 3.0 IGO. Credit: ESA/Gaia/DPAC. |
| Leinert et al. 1998 (A&AS 127, 1), as quoted in arXiv:2401.15518 (Bochum V 20.7 at the bright Milky Way) and arXiv:2604.09427 (dark-sky reference V 22) | The dark sky (V 22, the one brightness the display is tied to) and the bright Milky Way (V 20.7, now an independent check of the Gaia scale) | Cited only: published numbers, no data copied. |
| Gaia DR3 documentation (ESA/Gaia/DPAC), chapter 5: section 5.4.1 Table 5.4 (zero points G 25.6874, BP 25.3385, RP 24.7479), https://gea.esac.esa.int/archive/documentation/GDR3/Data_processing/chap_cu5pho/cu5pho_sec_photProc/cu5pho_ssec_photCal.html ; Table 5.9 (G - V from BP - RP), https://gea.esac.esa.int/archive/documentation/GDR3/Data_processing/chap_cu5pho/cu5pho_sec_photSystem/cu5pho_ssec_photRelations.html | Gaia flux to V surface brightness, per cell | Cited only: published constants. |
| OpenNGC (Mattia Verga), pinned to commit be3cd6c | Galaxy positions, sizes, tilt angles, and the integrated V that sets the Magellanic Clouds' total light | CC BY-SA 4.0. Credit: Mattia Verga. Only two galaxies (their positions, sizes and magnitudes, which are facts) reach the tables. The ShareAlike term against this repository's licence (AGPL-3.0-or-later) is for the desk to clear. |
| GWTC-1 sky map of GW150914, LIGO-P1800381-v7 | Where on the sky the signal came from (HEALPix probability map) | Released by LIGO and Virgo through the LIGO Document Control Center. Credit: LIGO Scientific Collaboration and Virgo Collaboration. |
| Abbott et al., PRL 116, 061102 (2016) | The published numbers the audit measures against | Cited only. |

Why Gaia DR3 for the Milky Way: it is the only measured, whole-sky, optical
source-count and flux data at hand, and the owner's direction (2026-10-19) is
to compute the Milky Way from measured data. CC BY-NC 3.0 IGO allows
non-commercial use with credit (ESA/Gaia/DPAC); this project is non-commercial
(AGPL-3.0-or-later). Tables are compiled in, so a commercial use would need
ESA's agreement, noted for the desk.

**The two Gaia queries (run on 2026-10-19 against https://gea.esac.esa.int/tap-server/tap/async, ADQL).**
Level 7 is run in four HEALPix ranges of 49152 cells each (the TAP service returns at most about
50000 rows per job), the four results joined with one header. The script does this when it is
asked for level 7 (`SKY_HPX_LEVEL`, default 7) and fetches any chunk that is missing.

Query 1 (flux per cell), chunk c of 4 covers cells (c-1) x 49152 to c x 49152 - 1, output
`gaia_hpx7_flux_c.csv`; the joined table `gaia_hpx7.csv` has sha256
`4d1412d3df02f6ee34120b8fd9b96bde6766fcaf50c569a94b45b72d4ec151cd` (196608 rows):

```sql
SELECT GAIA_HEALPIX_INDEX(7, source_id) AS hpx7, COUNT(*) AS nsrc, SUM(phot_g_mean_flux) AS g_flux,
       SUM(phot_bp_mean_flux) AS bp_flux, SUM(phot_rp_mean_flux) AS rp_flux
FROM gaiadr3.gaia_source
WHERE phot_g_mean_flux IS NOT NULL AND phot_g_mean_mag > 7.0
  AND GAIA_HEALPIX_INDEX(7, source_id) BETWEEN LO AND HI
GROUP BY hpx7 ORDER BY hpx7
```

Query 2 (dust per cell), the same four chunks, output `gaia_dust7.csv`, sha256
`ce46146f06fa33791d81cde3f02f19a7ac259e5555b13c03cdb8092e75174d89`:

```sql
SELECT GAIA_HEALPIX_INDEX(7, source_id) AS hpx7, AVG(ag_gspphot) AS mean_ag, COUNT(*) AS n
FROM gaiadr3.astrophysical_parameters
WHERE ag_gspphot IS NOT NULL
  AND GAIA_HEALPIX_INDEX(7, source_id) BETWEEN LO AND HI
GROUP BY hpx7 ORDER BY hpx7
```

Level 6 (49152 cells, the earlier build) is still in the script (`SKY_HPX_LEVEL=6`, sha256
`334bde2b...` and `105f8f51...`). Each sha256 is of the CSV the TAP service returned (the joined
file for level 7); the script refuses any other file.

Each input is downloaded once by `scripts/ck_sky_gen.sh`, checked against a
sha256 written in the script (a file that does not match is refused), and
turned into tables by `native/kernel/tools/ck_sky_gen.c` (plain C, no Python).
The kernel never touches the network: the tables are compiled in.
`native/kernel/ui/sky_data.txt` is the generator's report from the last run.

## 3. The sky

**The view.** A gnomonic projection (what a pinhole camera sees), 80 degrees of
sky across the screen's width, north up, centred where the GW150914 sky map is
most probable. That point is the highest-probability pixel of the map (HEALPix
nside 512): right ascension 113.697, declination -72.757 degrees, in the
constellation Volans near Carina. In galactic coordinates it is l 284.54, b
-22.70. The map's 90 percent region is 181.6 square degrees, all of it on a
3840 x 2160 screen. The centre is the map's peak, not a point source: the real
signal could have come from anywhere in that region. The Large Magellanic Cloud
is inside it.

**Stars.** Position is the catalogue's J2000 position moved along its proper
motion to epoch 2026.8. Brightness: light from a star is proportional to
10^(-0.4 V), the definition of the magnitude scale. Colour: B-V goes to an
effective temperature by Ballesteros (2012, EPL 97, 34008), that temperature to
the colour of a black body through the CIE 1931 observer (Wyman, Sloan and
Shirley 2013, fit), then to sRGB. Checks the generator prints: B-V -0.30 is
16602 K and sRGB (177, 198, 255); B-V 0.65 is 5778 K (the Sun's) and (255, 241,
234); B-V 1.50 is 3794 K and (255, 207, 155). 310 catalogue stars have no B-V
and are drawn at 0.6, close to the Sun's. On a 3840 x 2160 screen the view holds
71 stars to V 4, 179 to V 5, 560 to V 6, 904 to V 6.5.

Limits, stated plainly:
- No star fainter than V 6.5 is drawn one by one. The faint ones are inside the
  Milky Way light (below).
- Stars occupy only the neighbouring native pixel centres of a compact point:
  at most two pixels across, independent of UI scale. Static catalogue stars
  and their animated twinkles use the same bounded footprint, without soft
  lobes. A real star is smaller than a screen can resolve; this display profile
  is a rendering choice, not a measurement.
- Catalogue directions, magnitude lookup and colour inputs are retained.
  Linear-light point deposition still quantizes and clips. The compact profile
  does not conserve the retired broad profile's integrated display light, and
  clipping can change a bright star's apparent colour. Historical generator
  peak/energy descriptions are not new per-pixel calibration measurements.
- No atmosphere: no extinction near the horizon, no airglow colour, no moon.
  (The faint sky glow of an extremely dark site is added, section below.)

**Milky Way.** The light is Gaia DR3's own: for each of the 196608 HEALPix level-7 cells
(about 0.46 degree across), the summed G-band flux of the stars in it (stars with G > 7).
That is light, not a photograph. The cells are splatted onto a full-sphere 360 x 180 grid
(1 degree, north up) within 0.75 degrees of each level-7 cell's centre and
averaged (`SPLAT_DEG` in `native/kernel/tools/ck_sky_gen.c`; level 6 used
1.5 degrees).

The runtime now evaluates this source map independently at every native output
pixel. It no longer makes a quarter-resolution screen image and interpolates
that image back up, nor adds a runtime blur pass. This removes screen-space
reconstruction loss; it cannot manufacture angular detail absent from the
source grid. The stored map includes a longitude wrap column; its lower-rate
chroma plane is likewise finite-resolution. The diffuse band is therefore not
claimed to contain native-4K photographic detail. Crisp catalogue-star points
are a separate layer.

Colour and luminance are kept apart. A cell's light is split between the red, green and blue
channels by a tint from its BP-RP colour (reddened by the mean G-band extinction,
`ag_gspphot`, E(B-V) = A_G / 2.285, Cardelli et al. 1989), with the channel weights chosen so
that their luminance (0.2126 R + 0.7152 G + 0.0722 B) is exactly the cell's light. The first build
put almost all of the light into the red channel, so the band was about a third of its
true luminance. That is corrected.

Scale: physical, with no anchor (since 9ab1281 / 176bc51). The number is Gaia's own measured light:

- A cell's summed G flux F (electrons per second) over its area A (arcsec2) is a surface brightness,
  G = 25.6874 - 2.5 log10(F / A) mag/arcsec2 (the DR3 G zero point; Gaia DR3 documentation, section
  5.4.1, Table 5.4). 1 S10 (the light of a V 10 star per square degree) is V 27.78 mag/arcsec2.
- G is not V. Each cell is moved to Johnson V with its own integrated colour, BP - RP from its summed
  BP and RP fluxes and the DR3 zero points (BP 25.3385, RP 24.7479, same table), through
  G - V = -0.02704 + 0.01424 c - 0.2156 c^2 + 0.01426 c^3, c = BP - RP, valid -0.5 to 5, scatter
  0.03 mag (Gaia DR3 documentation, chapter 5, relationships with other photometric systems,
  Table 5.9). For the reddened bulge (c about 2) G - V is -0.75: taking G as V made the brightest band
  0.6 magnitude too bright (V 19.69 in G against 20.31 in V, both at the old 1.5 degree splat).
- Result (`native/kernel/ui/sky_data.txt`): the brightest band is **V 20.17 mag/arcsec2 (1111 S10)**.
  The published Bochum value is V 20.7 (Leinert et al. 1998; arXiv:2401.15518, Table 3). The two are
  independent: Leinert's number is now a check, not the scale. Gaia is 0.5 magnitude brighter here
  (UNVERIFIED cause; Bochum's beam and the bulge's patchy dust differ from a 0.75 degree average).
- The dark sky's own glow is V 22.0 mag/arcsec2 (205 S10; Leinert et al. 1998, also the reference
  V 22 of arXiv:2604.09427). It is added everywhere, with no gain. Brightest band over the dark sky:
  5.42 (luminance).
- The gain (`SKY_GAIN` / `--gain`) is 1.0 and stays 1.0: it exists only for a deliberate test. The
  earlier gain 1.2 (tied to a 678 S10 anchor and measured with the old inverse projection, which was
  off by a factor of two, see below) is retired; its table is in `evidence/real-sky/gain-measure.txt`
  as history.
- Each grid point (1 degree) averages the level-7 cells within 0.75 degree (about 8 cells). It was 1.5
  degree, sized for level 6; that smeared the Small Magellanic Cloud over a 3 degree disk.

**The screen-to-sky step (fixed at 9ab1281).** `sky_real.c` turns each screen point back into a sky
direction to read the grid. The forward projection puts a tangent-plane offset u at u / tan(fov/2)
half-widths; the inverse used half that, so the grid was read from points twice as far from the view
centre as the pixel. The Milky Way and the Clouds were drawn in the wrong places. The sky test now
catches it (below): with the old step both Clouds read 0.95 to 0.99 times their surround.

**Galaxies.** Brighter than V 6.5 and in the view, OpenNGC has two: the Large
Magellanic Cloud (ESO056-115, V 0.29, 646 x 550 arcminutes, position angle 170)
and the Small Magellanic Cloud (NGC0292, V 2.30, 300 x 180 arcminutes, position
angle 45). They are placed at their true centre, size and tilt. On a 3840 wide
screen the LMC's half axes are 219 x 191 px, the SMC's 112 x 74. Their outlines only carry the
gravitational-wave shimmer; their steady light is in the Milky Way grid:

**The Magellanic Clouds' light (real, from two catalogues).** Gaia sees each Cloud's brightest stars
(the LMC is about 50 kpc away, so Gaia's G 20.7 limit reaches absolute G about +2) but not the faint,
crowded rest. The generator (`gaia_clouds` in `ck_sky_gen.c`), for each catalogue galaxy at least 2
degrees long:

1. takes the Gaia cells inside its OpenNGC ellipse (the D25 isophote);
2. takes the local sky as the median cell between 1.2 and 1.6 ellipse radii (Milky Way foreground);
3. sums the light above that sky: the resolved part;
4. compares it with the catalogue's integrated V and adds the missing part in the pattern of the
   resolved stars (each cell's excess times total / resolved), so the total inside the ellipse is the
   catalogue's and the shape is Gaia's.

| galaxy | Gaia cells | local sky (S10) | Gaia resolves | catalogue V | share | centre before, after (V mag/arcsec2) |
|---|---|---|---|---|---|---|
| LMC (ESO056-115) | 372 | 48.7 | V 0.83 | 0.29 | 61% | 21.58, 21.11 |
| SMC (NGC0292) | 55 | 42.2 | V 2.78 | 2.30 | 64% | 21.64, 21.22 |
| M31 (NGC0224) | | | V 6.53 | 3.44 | 5.8% | not added (under 25%: too few stars to carry the shape) |

"Centre" is the inner fifth of the ellipse, the light added to the dark sky's V 22.0. A physical check
from the catalogue alone: V 0.29 over the LMC's ellipse (1.0e9 arcsec2) is a mean of V 22.8
mag/arcsec2; an exponential disk is several times brighter at its centre than its mean, which puts
the centre near V 21, as found.

On screen (`scripts/ck_sky_test.sh`, check "galaxy N glows at its place": median linear luminance
inside 0.3 of the short half axis over the median of a ring at 1.3 to 1.6 long half axes; the stars
do not move a median): **LMC 1.99, SMC 1.81** times the surround (bar 1.5). With the old screen-to-sky
step: 0.99 and 0.95. Limits: Gaia's crowding limit thins the LMC bar's stars, so the true centre is
somewhat more concentrated than drawn (UNVERIFIED how much); the grid is 1 degree, so the bar and
30 Doradus are not resolved.

**The sky above you (wired).** `sky_real.c` holds Greenwich mean sidereal time (IAU 1982),
the zenith for a latitude and longitude, and the 1976 IAU precession back to J2000
(`ui/tests/sky_test.c` checks them against published values). Allen takes a place in two ways:

- a town name from a table of 20 (`memphis`, `chicago`, ...), as before;
- coordinates typed as: `35 north 90 west`, `35.1 n, 90.0 w`, `35.1, -90.0`, `lat 35.1 lon -90`,
  `-33.87 151.21`, `north 35 west 90`. The parser reads the raw line (the folded one has lost the
  points and minus signs), in integer arithmetic, in millionths of a degree. A bare pair of numbers,
  arithmetic, a name, or a number with an operator is not a place. Tested in
  `native/kernel/svc/tests/allen_place_test.c` (26 checks, run by `scripts/ck_allen_ui_test.sh`).

"sky above me" then shows the sky over that place, turning at Earth's rate (the screen
refreshes every 10 seconds, `allen_ui.c`). Coordinates set the place only; the mode is unchanged.

## 4. The black holes and the picture of the merger

Measured by `ui/tests/chirp_audit.c`, output in `evidence/real-sky/chirp-audit.txt`.
Published values used: source-frame masses 36 and 29 solar masses, final mass 62,
final spin 0.67, redshift 0.09, luminosity distance 410 Mpc (Abbott et al. 2016,
PRL 116, 061102, table I; abstract).

**Bent light (real physics).** A point mass moves the image of a star seen at
angle theta from it, to where the star really is, beta = theta - E^2/theta. The
code does exactly that (`warp()` in `night.c`), with the pull of both holes
added. Two weak points, both ours:
- The pull is eased to zero between 2.2 E and 4.4 E so the bent region has no
  edge. The real pull has no edge: it falls as 1/theta and never stops. What the
  picture leaves out is 27.3 design px at 2.2 E and 13.6 px at 4.4 E (E is 60 px).
  Far from the holes the sky is not bent here and would be, slightly.
- The scale. A hole's shadow has radius sqrt(27)/2 = 2.598 Rs (Rs is the
  Schwarzschild radius, 2GM/c^2). The Einstein radius for a far-away source is
  sqrt(2 Rs / D) in angle against the shadow's 2.598 Rs / D, for a viewer at
  distance D. The picture's ratio of Einstein radius to shadow radius is 3.00
  (60 over 20 px). That is exactly what a viewer 30.4 Rs from the hole sees:
  3229 km from a 36 solar mass hole. It is a consistent picture, but of a viewer
  who could not survive there. It is a chosen viewpoint, close enough to make
  the arcs big.
- Superposing two single-hole pulls is accurate only when the pull is weak. Near
  a hole the picture draws the shadow and a thin bright ring at 1.04 of its
  radius, as a simple disc and a ring. Real strong lensing has a photon ring and
  an infinite stack of thinner copies of the sky. Not drawn.

**Orbit and spiral (real law, compressed).** Separation as t^(1/4), angle as
t^(5/8), t the time left (leading-order inspiral, Peters 1964; Abbott et al.
2016). The picture's wide orbit is 270 design px, which with 1 Rs = 7.70 px is
35.1 Rs of a single hole, or 35.1 GM/c^2 of the pair. Physics there: orbital
period 0.46 s, gravitational-wave pitch 4.4 Hz, 42 s to the merger, about 150
turns. The picture: one orbit per 5.2 s (11.4 times slower at the start), 6 s
to the merger (7 times faster in total), 1.85 turns. So the law is real, the
clock is not. The orbit is drawn leaning 60 degrees away from face on (its
height is half its width).

**Merger.** Equal masses are drawn; the real ones are 36 and 29 (ratio 0.8, close
enough for a picture, but a real unequal pair is lopsided). The final hole is
drawn with a shadow 2.00 times each initial one; the published masses give
62 over 32.5 = 1.91, so the picture is 4.8 percent big. Its Einstein radius grows
1.414 times (square root of 2) where the square root of 1.91 is 1.381, the same
gap. The real remnant spins at 0.67, which squashes and shifts its shadow;
the picture draws a round, unspun one.

**The ripple across the sky (illustration).** The wave from GW150914 stretches
space by about 10^-21 (peak strain 1.0e-21), a shift that no eye or telescope
could see, and it does not move the stars across the sky. The picture draws a
ring of stretch and squeeze with a cos 2 theta pattern, which is the true
shape of a gravitational wave's effect on a ring of free particles, at a size
about 20 orders of magnitude too large, running out from the merger over 3.6 s
as if the wave were visible light crossing the screen. It stays because it makes
the idea visible. It should be called an illustration wherever it is shown.

**Ringing (illustration, wrong timescale).** The real final hole rings in its
least damped mode at 250 Hz (Berti, Cardoso and Will 2006 fit with mass 62
(detector frame 67.6), spin 0.67; matches the published 250 Hz top of the sweep)
with a quality factor of 3.17, which is a damping time of 4.0 ms: gone in about
two cycles. The picture's shadow wobbles with a 95 ms period and a 260 ms decay time: 2.7
cycles per decay time where the real one has 1. A real ringdown is too quick to
see at all; the picture slows the period 24 times and the decay 65 times. Shown as a wobble for the same
reason as the ripple.

## 5. The startup sound

`ui/chirp.c`, a "blirp" under a second, built from a formula and no recording.

Real: the pitch of an inspiral rises as (time left)^(-3/8) and the strength of
the wave as pitch^(2/3). The code uses exactly those, and the peak is at the
merger.

Measured against the real signal (`evidence/real-sky/chirp-audit.txt`):

| | Published and physics | The kernel's sound |
|---|---|---|
| Pitch at the start | 35 Hz | 200 Hz (5.7 times higher) |
| Pitch at the top | about 250 Hz | 1000 Hz (4.0 times higher) |
| Sweep length | about 0.2 s (0.16 s by the leading-order law) | 0.59 s (3 times longer) |
| Cycles | about 8 from 35 to 150 Hz (8.4 by the law) | 179 |
| Ring after the merger | 250 Hz, gone in 4.0 ms | 1000 Hz held, 30.0 ms time constant |

The sound is therefore the right kind of curve with invented numbers: higher
(raised into hearing, but by 5.7 at the start and 4.0 at the top, not one
factor), longer, with about twenty times the cycles, and a ring that is held at
the peak pitch for far longer than the real one lasts. A single factor
would be a real thing: a gravitational-wave signal scales exactly with
the total mass, so a lighter pair would give higher pitch and a shorter
chirp. The kernel's sound is not any one pair's.

What a real version would be: the signal as it is, 35 up to 250 Hz over about
0.2 s with its eight cycles, then a 250 Hz ring dying in 4 ms. It is audible
(it is what LIGO's own audio of the event plays), but it is low and short,
and small speakers do not reproduce 35 Hz. That choice, real and quiet against
louder and invented, is Drake's. Work for it is a separate branch, `feat/real-chirp`.

## 6. Everything else on the screen

- **Twinkle.** The brightest stars (V 2.0 and brighter) twinkle. Twinkling
  (scintillation) is the atmosphere, so this is correct for an observer on the
  ground and wrong for one in space. It is the observer the sky already implies.
- **The sweeping band of light on the galaxies** while Allen answers: art.
  The galaxies themselves do not change.
- **Allen's mind** (the sphere of points and threads), **the water**, **the
  orb**: art. They make no claim about the real world and say nothing about it.
- **The fade in** (the eyes adjusting to the dark, bright stars first): real in
  kind (the eye does adapt over minutes, rods take about half an hour), shown in
  seven seconds.

## 7. For the black-hole dusk (goodbye)

Notes for the branch `feat/goodbye-event-horizon`, so its picture can follow the
same rules (the physics, all standard results for a non-spinning hole):
- Shadow radius sqrt(27)/2 Rs = 2.598 Rs. The photon ring is the shadow's edge,
  at the light's critical orbit of 1.5 Rs.
- The innermost stable circular orbit is 3 Rs (6 GM/c^2), the inner edge of a
  thin disc.
- The disc's temperature falls with radius as r^(-3/4) (Shakura and Sunyaev
  1973), times (1 - sqrt(r_in/r))^(1/4) near the inner edge. A hot inner disc is
  blue-white, the outer redder. That temperature maps to colour with the same
  black body chain used for stars here (`blackbody_rgb`).
- Doppler beaming: the side of the disc turning towards the viewer is brighter.
  Intensity scales as delta^3 at one frequency and delta^4 over all, delta being
  the Doppler factor (Rybicki and Lightman).
- Light from the far side of the disc is bent over the top of the hole and
  seen above the shadow. Real pictures show this.
Only the first two are in the picture here; the rest are for the dusk.

## 8. What this change measured

Sizes, timings and pictures are in `evidence/real-sky/README.md`. On one core of the Spark (not the iMac):

- The generated table `sky_data.c` is 452276 bytes at level 7 and at level 6 (the grid is the same
  1-degree grid; only the values differ). The tables in the image are 64980 bytes of luma and 32580
  of colour, as before.
- The whole sky at 3840 x 2160 is made in 122 to 140 ms (`ck_night_init`, best of 3 per run; four
  runs at level 6 gave 135 to 136 ms, four at level 7 gave 109 to 140 ms). The ranges overlap: no
  timing difference between the levels can be claimed. The machine was shared with other jobs.
- Level 6 against level 7: same table size and the same timing within noise, so the level is chosen
  by the input. Level 7 uses all 196608 Gaia cells (the level-6 build used 49152). Its table differs
  from level 6 in 945 of its 5610 lines. The Galactic-centre luminance ratio is the same to two
  decimals (3.23 at gain 1.2, the retired anchored build).
- `IMACKERN.EFI` is 405504 bytes, built by `make -C native/kernel imac-kernel` at `176bc51` (and at `4221f12`, the earlier code
  commit; the docs commit after it changes no code). The earlier real-sky figure was 348160.
- Host checks (at 176bc51): the sky test 15 of 15 pass (`ck_sky_test.sh`, two new checks: each Cloud glows at its place); the Allen window test and its picture
  checks, the Allen place test (26 checks) and the Allen memory test are in
  `evidence/real-sky/README.md`. Not run: the full `make -C native/kernel test` suite and the QEMU
  tests; those were not run in this change.
- Nothing here was measured on the iMac.

## 9. Open

- The Clouds' centres are drawn in the pattern of the stars Gaia resolves; crowding thins the LMC bar, so
  the true centre is more concentrated (not measured). M31 has no steady light (Gaia resolves 5.8%).
- The Milky Way's brightest band from Gaia (V 20.17) is 0.5 magnitude brighter than Bochum's 20.7; the
  cause is not established.
- The level-7 grid is still 1 degree. A 0.5-degree grid would use the cells in full; it doubles the table
  (about 260 KB more) and has not been measured.
- The on-screen ratios (Clouds over surround) are measured on the host's sRGB path (8 bits, gamma). The
  iMac's panel may differ; they are measured again on the iMac when a frame can be read there.
- The flight through nearby stars is a plan (`docs/SKY_FLIGHT_PLAN.md`), not code.
- Gaia DR3 is CC BY-NC 3.0 IGO; the repository is AGPL-3.0-or-later. The desk to confirm the use.
- The real chirp (section 5): a decision for Drake.
- The OpenNGC ShareAlike term against the repository licence.
- The ripple and the ringing should carry an "illustration" label wherever shown to the public.
- The sky was checked on a host, not on the iMac's real screen.
