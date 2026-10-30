# The night, as maths

The boot reference (`night.html`, this folder) and the kernel draw the same night. This page writes every step
as an equation, with the kernel line it comes from, so a port to any framebuffer needs no guessing.

Source: `native/kernel/ui/night.c`, `chirp.c`, `paint.c`, `allen_ui.c` on `feat/allen-boot-everywhere` (L266
milestone 1, byte-identical to the iMac's `aienos_commit 0930240`). The kernel is integer-only; the sketch uses
floating point. Where they differ it is said below.

## 1. Space and time

- Design grid 1920 × 1080. Scale `k = min(W·256/1920, H·256/1080)`; a design length `v` is `S(v) = ⌊(v·k + 128)/256⌋` px.
- The kernel works in 16ths of a pixel; pixel `i`'s centre is `16i + 8`. The sketch uses `i + 0.5`.
- Centre of the merger `C = (⌊W/2⌋, ⌊0.40·H⌋)` (`allen_ui.c` 853).
- `t` is ms on the night's clock from the first frame.

| Constant | ms | Kernel |
|---|---|---|
| holes appear | 2600 | `CK_NIGHT_HOLES_MS` |
| spiral begins | 6000 | `CK_NIGHT_SPIRAL_MS` |
| fade-in ends | 6800 | `CK_NIGHT_FADE_END_MS` |
| merger | 12000 | `CK_NIGHT_MERGE_MS` |
| ripple run | 3600 | `CK_NIGHT_RIPPLE_MS` |
| last hole gone | 15200 | `CK_NIGHT_GONE_MS` |
| wide orbit, one turn | 5200 | `ORBIT_MS` (night.c 371) |

## 2. Noise (paint.c)

- `hash(x, y, s)`: `h = x·0x8da6b343 ⊕ y·0xd8163841 ⊕ s·0xcb1ab31f` (32-bit wrap), then `h ⊕= h≫13; h ·= 0x5bd1e995;
  h ⊕= h≫15; h ·= 0x27d4eb2d; h ⊕= h≫16`.
- `smooth(t) = 3t² − 2t³` on 0..1 (kernel `ck_smooth`, t and result in 256ths).
- `noise(x, y, s)`: value noise on a lattice of 256 units: the four corner values `hash(·)≫16` blended with
  `smooth` of the fractions. Range 0..65535.
- `fbm(x, y, s, n) = Σₒ noise(x·2ᵒ, y·2ᵒ, s + 7919·o) · 2ⁿ⁻¹⁻ᵒ / Σ 2ⁿ⁻¹⁻ᵒ`.
- `gauss(v, w) = exp(−(v/w)²)`.

The sketch ports these bit for bit (JavaScript `Math.imul`), so every star sits where the kernel puts it.

## 3. The sky, made once

### 3.1 The Milky Way band (night.c 72)

A line from `(−W/20, 0.90·H)` along `(1.10·W, −0.92·H)`, half-width `h₀ = 0.16·H`. For a point:
`across = ((x−x₀)·dy − (y−y₀)·dx)/len`, `along = ((x−x₀)·dx + (y−y₀)·dy)/len`.

### 3.2 The soft layer at a quarter resolution (night.c 97)

On the grid `x = 4i, y = 4j`, with `n₁ = fbm(768x/H, 768y/H, 101, 5)`, `n₂ = fbm(1792x/H, 1792y/H, 202, 4)`:

- core `= gauss(across, h₀)`, wide `= gauss(across, 2h₀)`
- dust `= gauss(across − 0.02H, 0.05H) · smooth((n₂ − 22000)/26000)`
- bright `= gauss(along − 0.40·len, 0.38·len)`
- mw `= core · (0.5 + n₁/131072) · (1 − 0.8·dust) · (0.45 + 0.55·bright)`
- halo `= wide · (0.6 + 0.4·n₁/65536)`
- nebulae: `vi = smooth((n₄ − 36000)/29535)²`, `bl = smooth((n₅ − 35000)/30535)²`, `ro = smooth((n₆ − 33000)/32535) · wide`
  with `n₄, n₅, n₆ = fbm(·, 404|505|606, 4)` at 512/H, 666/H and 1024/H.
- top `= 256 − 256y/H`; in 16ths of a level:
  - R `= 26 + top/32 + 704·mw + 96·halo + 352·vi + 80·bl + 352·ro`
  - G `= 34 + top/32 + 640·mw + 128·halo + 128·vi + 192·bl + 112·ro`
  - B `= 92 + top/16 + 736·mw + 256·halo + 448·vi + 480·bl + 240·ro`

Scaled to full size bilinearly with a 4 × 4 Bayer dither added before dropping to 8 bits (night.c 140), so the
faint gradients show no bands.

### 3.3 Stars (night.c 170)

Every star is a glow: added light `a · (1 − d²/r²)³` inside radius `r`, colour by `hash % 100`:
<10 `(170,192,255)`, <55 `(218,228,255)`, <85 `(255,250,242)`, <97 `(255,234,196)`, else `(255,204,160)`.

| Population | Count | Position | Kept when | Level `lv` / radius |
|---|---|---|---|---|
| field | `W·H/800` | `hash(i,1|2,77)` in 16ths | `hash(i,3,77)&65535 ≤ 36000 + 29535·gauss(across, h₀)` | `14 + 241·u⁴/2³²`, u = `hash(i,4,77)&255`; r = `(20 + 16·lv/255)·k/256` 16ths |
| dust | `W·H/170` | `hash(i,6|7,78)` | `hash(i,8,78)&65535 ≤ gauss(across, 0.8h₀)²` | `10 + hash(i,9,78)%44`, r = `20k/256` 16ths, colour `(226,230,255)` |
| bright | `clamp(W·H/50000, 24, 256)` | `hash(i,1|2,91)` | always | `190 + hash%66`, r = `34k/256`; plus a halo of 5r at level 22; they twinkle (§4.7) |

### 3.4 Galaxies (night.c 221)

Six, from `gdef` (position in thousandths, radius in design px, tilt, turn, kind, brightness). Per pixel, rotate by
`turn`, unflatten by `cos(tilt)` (at least 600/16384), radius `ρ` in 256ths of the galaxy's own:

- spiral arms: phase `φ = 2θ + 30000·(log₂ρ − 8)` (turns in 65536ths), `arm = ((1 + cos φ)/2)²`, faded in outside the core
  by `smooth((ρ − 34)/90)`; clumps `0.5 + 0.336·nz + 0.164·fine`; disc light `exp(−ρ·12/5 / 256) · (0.125 + 0.875·arm·clump)`.
- bulge `0.9·gauss(ρ_b, 58)`; round galaxies `0.8·gauss(ρ_b, 30)` and disc `0.6·exp(−4.2ρ_b/256)`.
- edge-on: a dust lane `gauss(v, 0.05r_g)` takes up to 85% of the disc and 61% of the bulge inside ρ < 240.
- soft edge `1 − smooth((ρ − 250)/80)`. Colours: bulge `(255,222,178)`, disc `(150,170,255)`, knots `(255,140,190)` at half.

### 3.5 When each part of the sky comes out of the dark (night.c 296)

On a 4 × 4 cell grid: `appear = (6·fbm(640x/H, 640y/H, 909, 4)/256 + 4·255·|p − C|/far)/10`, far = the farthest corner.

## 4. Each frame

### 4.1 Coming out of the dark, 0 to 6.8 s (night.c 314)

For a pixel of brightness `l` (its largest channel, 0..255): starts at `t₀ = 300 + 12·appear + 8·(255 − l)` ms, then
`smooth((t − t₀)/1365)`. Bright stars come first, faint sky last, in patches spreading from the centre.

### 4.2 The orbit (night.c 373)

Wide separation `a₀ = S(270)` px. Angle in turns:

- 2.6 s to 6.0 s: `θ = (t − 2600)/5200`, separation `a₀`.
- 6.0 s to 12.0 s, with time left `L = (12000 − t)/6000`: separation `a₀·L^¼`, angle
  `θ = 3400/5200 + (8/5)·(6000/5200)·(1 − L^⅝)` (the inspiral keeps the wide orbit's angular speed where it joins).
- Hole positions `C ± (a/2·cos 2πθ, a/2·sin 2πθ / 2)`: the orbit leans away, half as tall as wide.

Integer form: `L^⅛` by three integer square roots of a Q30 number (`root8`); `L^¼ = r²`, `L^⅝ = r⁵`.

### 4.3 Each hole grows in (night.c 400)

Einstein radius `E = S(60)·smooth((t − 2600)/2600)`; shadow radius `r_s = S(20)·smooth((t − 4000)/2000)`.

### 4.4 Bent light (night.c 480)

A point lens moves the image at angle `θ` from a hole to where the star really is: `β = θ − E²/θ`. Per pixel `p`,
for each hole at `h`: `d = p − h`; if `|d|² < (4.4E)²`, the sky is sampled at `p − d·f` with `f = E²/|d|²` (|d|² at least
1/4 px²), eased to zero between 2.2E and 4.4E by `((19.36E² − |d|²)/(14.52E²))²`. Both holes' pulls add. Stars behind
a hole stretch into arcs and a second image appears on the far side.

The kernel works this out exactly within `max(E, 1.5r_s) + 8` px of a hole and on an 8-pixel grid, bilinearly
interpolated, elsewhere in the bent region (night.c 568). The sketch works it out exactly everywhere; the arcs
are a little sharper (receipts, §6).

### 4.5 Shadow and photon ring (night.c 523)

Inside `1.5·r_s`: darken to black with a half-pixel soft edge at `r_s`; add `(255,228,196)` at `(22 − 16·|r − 1.04r_s|)·6/256`
(a ring about 1.4 px wide just outside the shadow) plus a glow `46/256 · (1.5r_s − r)/(0.5r_s)` between `r_s` and `1.5r_s`.

### 4.6 Merger, ring-down and the ripple (night.c 412, 436)

- 12.0 s to 15.2 s, one hole at C: `E = 1.414·S(60)`; `r_s` grows from `1.262·S(20)` to `2·S(20)` over 180 ms, wobbling
  `±7%·sin(2πt/95 ms)·exp(−t/260 ms)` (it rings); from 12.9 s both fade by `keep = 1 − smooth((t − 12900)/2300)`,
  `E·keep`, `r_s·keep²`.
- The ripple, 12.0 s to 15.6 s: crest radius `R = (far + 2.2w)·(t − 12000)/3600`, width `w = clamp(S(60), 4, 200)`,
  envelope `smooth(t/220) · (1 − smooth((t − 2160)/1440))`, amplitude `S(6)·(1 − 0.586·R/far)·env`, wavelength `S(110)`.
  At offset `o = |p − C| − R` within `±2.2w`: radial shift `amp·sin(2πo/λ)·gauss(o, w)·(0.35 + 0.65·cos 2(φ − ψ))`, ψ the
  direction the pair last lay on (it stretches one way and squeezes across, as a gravitational wave does), and a
  faint light `(86,108,176)·gauss(o, 0.6w)·env·46/256`.

### 4.7 Twinkling (night.c 629)

Each bright star adds a glow of radius `2·34k/256` 16ths at `((1 + sin 2π(t + phase)/period)/2)² · 120/256`, period
2.6 to 6.8 s, scaled by its fade-in; skipped where the lens region covers it.

### 4.8 The merger's flash (allen_ui.c 437)

12.0 s to 13.6 s: radius `S(40) + S(220)·easeOut(t/1200)` with `easeOut(x) = 1 − (1 − x)³`, strength
`smooth(t/120)·exp(−max(0, t − 120)/450)·0.9`. Two glows at C: `(200,210,255)` at strength/3 and `(255,240,224)` at a
third of the radius. From 100 ms the remaining hole's shadow takes its black back over 300 ms.

## 5. The chirp (chirp.c)

48 kHz, one channel copied to both. `T = 600 ms` before the merger, `L` the share of it left.

- 11.36 s to 11.40 s: 200 Hz, fading in by `smooth` over 40 ms to the level at `L = 1`.
- 11.40 s to 12.00 s: pitch `200·L^(−3/8)` Hz, capped at 1000 Hz (reached at `L = 0.2^(8/3) ≈ 0.0137`, 8.2 ms before the
  merger); level `0.70·(L_cap/L)^¼`, which is pitch to the ⅔ power, peaking at 70% of full scale; phase is the integral
  `200·T·(8/5)·(1 − L^⅝)` turns.
- 12.00 s to 12.30 s: 1000 Hz held, level `0.70·exp(−t/30 ms)`.

The kernel builds a 4096-entry sine table by rotation and works in Q30 (`chirp.c`); the sketch uses `Math.sin`.

## 6. Receipts

Commands run from the repository root on the Spark, 2026-10-20. Kernel frames come from the design skill's
`render_night.c` built against the L266 sources (`git archive origin/feat/allen-boot-everywhere native/kernel/ui`).

### 6.1 The sketch matches the kernel

`ffmpeg -i <sketch>.png -i <kernel>.ppm -lavfi psnr` (8-bit RGB, 1920 × 1080; higher is closer; 48 dB is about one
level of 255 on average):

| t (ms) | whole frame (dB) | centre 480 × 360 around the holes (dB) |
|---|---|---|
| 1500 | 85.4 | not measured |
| 4000 | 51.4 | 44.9 |
| 9000 | 56.6 | 49.9 |
| 11800 | 57.3 | 49.9 |
| 14000 | 51.0 | 49.1 |
| 16000 | 59.8 | not measured |

OBSERVED. The flash (12.0 to 13.6 s) is drawn by `allen_ui.c`, not `night.c`, so these frames avoid it; it is
checked by eye only.

### 6.2 Frame time per stage

See `receipts/frame-times.md`.

## 7. Not in this sketch

- Allen's mind gathering from the merger's light (12.3 s to 15.3 s, `mind.c`) and his words. The kernel draws them;
  the screen mockups show the formed mind.
- The galaxies' light sweep while Allen speaks (`ck_night_lights` shimmer). Not part of the boot.

## 8. PROPOSAL, not shipped: closer to the physics

The aien-design-language skill asks for real astrophysics. The night as shipped is a faithful look, not a
simulation. If Drake wants it closer, these are the changes, for L266 to weigh (none is in the sketch or the kernel):

1. Shadow size from the lens: for a non-spinning hole the shadow radius is √27/2 Rs ≈ 2.6 Rs. Today `r_s = S(20)`
   and `E = S(60)` are set independently; tying `r_s` to the mass that sets `E` would keep their ratio physical.
2. The ring-down frequency and damping from the merged mass (today 95 ms period and 260 ms decay, chosen by eye).
3. Real stars: positions, magnitudes and B−V colours from a public catalogue (Hipparcos or Tycho-2) instead of
   hashed stars, as the brand-art renderer already plans.
4. The chirp's time stretch stated as a factor in the science doc (a real 30-solar-mass merger sweeps through
   this band in well under a second; the 600 ms here is chosen).
