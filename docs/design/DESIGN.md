---
version: alpha
name: Allen
description: A night sky with one calm light in the middle. Allen's look on every screen AienOS draws.
colors:
  primary: "#ECF0FC"
  night: "#060812"
  hint: "#96A0C4"
  faint: "#828CB0"
  glow: "#96AAFF"
  mist: "#C8D2FF"
  ink: "#4056C4"
  warm: "#FFD496"
  warm-soft: "#FFDEB4"
  heart: "#FFD2AA"
  heart-core: "#FFECD6"
  halo: "#3C4AD2"
  halo-violet: "#705CFF"
  sky-glow: "#6E82FF"
  panel: "#0C1026"
  lit: "#3B446F"
typography:
  voice:
    fontFamily: Quicksand
    fontSize: 27px
    fontWeight: 400
    lineHeight: 40px
    letterSpacing: "0.02em"
  body:
    fontFamily: Quicksand
    fontSize: 21px
    fontWeight: 500
    lineHeight: 30px
    letterSpacing: "0.01em"
  small:
    fontFamily: Quicksand
    fontSize: 16px
    fontWeight: 500
    lineHeight: 22px
    letterSpacing: "0.02em"
  title:
    fontFamily: Quicksand
    fontSize: 40px
    fontWeight: 300
    lineHeight: 52px
    letterSpacing: "0.04em"
rounded:
  sm: 8px
  md: 14px
  panel: 22px
  pill: 999px
spacing:
  xs: 4px
  sm: 8px
  md: 16px
  lg: 24px
  xl: 40px
  xxl: 72px
  bar-width: 720px
  bar-height: 54px
  bar-bottom: 96px
  text-width: 900px
  words-below-centre: 250px
components:
  page:
    backgroundColor: "{colors.night}"
    textColor: "{colors.primary}"
    typography: "{typography.body}"
  allen-says:
    backgroundColor: "{colors.night}"
    textColor: "{colors.primary}"
    typography: "{typography.voice}"
  allen-title:
    backgroundColor: "{colors.night}"
    textColor: "{colors.primary}"
    typography: "{typography.title}"
  hint-text:
    backgroundColor: "{colors.night}"
    textColor: "{colors.hint}"
    typography: "{typography.small}"
  fine-print:
    backgroundColor: "{colors.night}"
    textColor: "{colors.faint}"
    typography: "{typography.small}"
  input-bar:
    backgroundColor: "{colors.night}"
    textColor: "{colors.primary}"
    typography: "{typography.body}"
    rounded: "{rounded.pill}"
    height: 54px
    width: 720px
  input-bar-caret:
    backgroundColor: "{colors.night}"
    textColor: "{colors.glow}"
  choice:
    backgroundColor: "{colors.panel}"
    textColor: "{colors.mist}"
    typography: "{typography.body}"
    rounded: "{rounded.md}"
    padding: 16px
  choice-lit:
    backgroundColor: "{colors.lit}"
    textColor: "{colors.primary}"
    typography: "{typography.body}"
    rounded: "{rounded.md}"
    padding: 16px
  button-primary:
    backgroundColor: "{colors.lit}"
    textColor: "{colors.primary}"
    typography: "{typography.body}"
    rounded: "{rounded.pill}"
    padding: 14px
  button-primary-hover:
    backgroundColor: "{colors.lit}"
    textColor: "{colors.warm-soft}"
  button-secondary:
    backgroundColor: "{colors.panel}"
    textColor: "{colors.mist}"
    rounded: "{rounded.pill}"
    padding: 14px
  panel:
    backgroundColor: "{colors.panel}"
    textColor: "{colors.primary}"
    rounded: "{rounded.panel}"
    padding: 24px
  found-line:
    backgroundColor: "{colors.night}"
    textColor: "{colors.primary}"
    typography: "{typography.body}"
  found-line-label:
    backgroundColor: "{colors.night}"
    textColor: "{colors.hint}"
    typography: "{typography.body}"
  state-word:
    backgroundColor: "{colors.night}"
    textColor: "{colors.glow}"
    typography: "{typography.small}"
  state-word-needs-you:
    backgroundColor: "{colors.night}"
    textColor: "{colors.warm}"
    typography: "{typography.small}"
  focus-ring:
    backgroundColor: "{colors.night}"
    textColor: "{colors.mist}"
  orb-halo:
    backgroundColor: "{colors.halo}"
  orb-halo-violet:
    backgroundColor: "{colors.halo-violet}"
  orb-heart:
    backgroundColor: "{colors.heart}"
  orb-heart-core:
    backgroundColor: "{colors.heart-core}"
  water:
    backgroundColor: "{colors.ink}"
  sky-wash:
    backgroundColor: "{colors.sky-glow}"
---

## Overview

Allen's look is a night sky with one calm light in the middle. The sky is the stage and Allen is the only actor.
Everything a person reads sits in the centre or in one bar at the bottom. There are no windows, menus, tabs,
sidebars or progress bars. Every change eases in and nothing cuts.

This file is the one place the look is written down. The native kernel (boot and setup), the `aienos-run`
windows on Windows, macOS and Linux, the Try page on aienos.com and the docs read their values from the three
files generated from it (`docs/design/tokens/allen_tokens.h`, `allen_tokens.rs`, `allen-tokens.css`). Change a
value here, run `cargo run -p allen-design-tokens -- generate`, and every surface follows. A test fails if the
generated files fall behind this one.

Where the values come from: the colours, sizes, clocks and curves are read from the iMac kernel's drawing code
(`native/kernel/ui/` on `feat/allen-boot-everywhere`, the same bytes the iMac runs, commit 0930240), not from
screenshots. Each token below names its source line. The web font, glass panels and entrance motion come from the
Allen design-language skill.

## Colors

Light on dark, cool blues for everything calm, one warm colour for what is lit or alive.

- **Night (#060812):** the sky behind everything. `allen_ui.c` 254, `CK_RGB(6, 8, 18)`.
- **Primary (#ECF0FC):** the words Allen says and the words a person types. `allen_ui.c` 279.
- **Hint (#96A0C4):** labels, placeholders, the left half of a found line ("Graphics:"). `allen_ui.c` 271.
- **Faint (#828CB0):** fine print, footers. `allen_ui.c` 300.
- **Glow (#96AAFF):** edges, focus halos, calm state words ("ready"). `allen_ui.c` 293.
- **Mist (#C8D2FF):** secondary buttons, unlit choices, links. `allen_ui.c` 509.
- **Ink (#4056C4):** deep accents, the cool water. Design language.
- **Warm (#FFD496):** a fresh letter, anything that needs the person ("waiting for you"). `allen_ui.c` 230.
- **Warm-soft (#FFDEB4):** hover glow on a primary action. `allen_ui.c` 431.
- **Heart (#FFD2AA) and Heart-core (#FFECD6):** the warm light at the centre of Allen's mind. `mind.c` 263, 264.
- **Halo (#3C4AD2) and Halo-violet (#705CFF):** the faint light round the mind. `mind.c` 228, 229.
- **Sky-glow (#6E82FF):** the faint wash near the top of the sky. `allen_ui.c` 260.
- **Panel (#0C1026):** glass behind a group of choices, drawn at 58% to 84% over the sky. Design language.
- **Lit (#3B446F):** the solid colour of a lit choice or primary button: Glow at 34% over Panel. Web surfaces
  draw it as a gradient from `rgb(150 170 255 / .34)` to `rgb(84 104 224 / .30)`.

There is no red and no green. Nothing on a first-run screen is an error; a problem is a sentence in Primary with
its state word in Warm.

## Typography

Allen's own typeface is drawn in pen strokes by the kernel (`allen_font.c`); there is no font file. Web and runner
surfaces use Quicksand (OFL), the nearest match. Weight and size carry the hierarchy; one family only.

| Role | Kernel cap height at 1080p | Kernel stroke weight | Web size / weight |
|---|---|---|---|
| voice (what Allen says) | 19 px | 1000 | 27 px / 400 |
| body (choices, found lines) | 15 px | 1150 | 21 px / 500 |
| small (hints, state words) | 11 px | 1250 | 16 px / 500 |
| title ("Allen is ready.") | web only | n/a | 40 px / 300 |

```allen-tokens
native-type:
  voice-cap: 19px
  body-cap: 15px
  small-cap: 11px
  voice-weight: "1000"
  body-weight: "1150"
  small-weight: "1250"
  line: 40px
  wrap: 900px
```

Native surfaces (the kernel and anything that draws through it) use the `native-type` tokens with Allen's stroke
typeface. Mockups in `docs/design/screens/` draw their words with the same strokes, exported from `allen_font.c`, so
what a builder sees in a mockup is what the kernel can draw.

Web sizes are the kernel cap height divided by 0.70, Quicksand's cap height as a share of its size (INFERRED from
the font's published metrics; not measured on a rendered glyph yet). Allen's line height is 40 px
(`allen_ui.c` 186) and his text wraps at 900 px (`allen_ui.c` 191).

## Layout

Every Allen surface lays out on a 1920 by 1080 design grid scaled by `k = min(width / 1920, height / 1080)`
(`allen_ui.c` `scale_for`), so a phone, a laptop and a 5K iMac show the same picture.

- Allen's centre is at 50% across and 40% down (`allen_ui.c` 853).
- His words start 250 px below the centre (`words_top`) and wrap at 900 px.
- The input bar is 720 by 54 px, centred, its bottom edge 96 px above the screen's bottom (`allen_ui.c` 871).
- The spacing scale is xs 4, sm 8, md 16, lg 24, xl 40, xxl 72. Groups of choices stack with md between rows and
  xl between a question and its choices.
- Found lines (screen 5) line up on one column: label right-aligned in Hint, value left-aligned in Primary.

## Elevation & Depth

Depth comes from light, not shadows. The sky is the floor. Glass panels sit just above it with a 1 px edge in Glow
at 14% to 16% and an 18 px backdrop blur, and only where a group of choices needs holding together. Allen's mind
is the brightest thing on screen. Nothing else glows unless it is lit or needs the person.

## Shapes

Round and soft. Fields and buttons are pills (`rounded.pill`). Choice rows use `rounded.md` (14 px). Panels use
`rounded.panel` (22 px). There are no sharp corners, no icons in rounded squares and no dividing rules.

## Components

- `allen-says` is how Allen speaks: Primary on the night in the voice size, letter by letter (35 ms each), each
  fresh letter starting Warm and cooling to Primary over 900 ms.
- `input-bar` is the only text field. It is always at the bottom, a pill on the night at 55%. Its caret is a Glow bar that breathes (1.1 s) instead of blinking and holds still for 0.5 s after each key (`allen_ui.c` 284).
  On focus its edge turns Mist at 55% with a 28 px Sky-glow halo at 22%.
- `choice` and `choice-lit` are the answers to a question. One is always lit (the first, unless the person moved).
  The lit one is `Lit` with Primary words and a Glow edge; the others are Mist words on Panel.
- `button-primary` is light, not paint: the Lit gradient with a Mist edge; on hover the edge and words turn
  Warm-soft. `button-secondary` is a Mist outline on Panel. "Not now" is always a secondary button.
- `found-line` and `found-line-label` are one line of what Allen found on screen 5.
- `state-word` is a calm word in Glow ("ready", "building a driver for it"); `state-word-needs-you` is Warm
  ("waiting for you").
- `orb-*` are the lights that make Allen's mind (halo, violet halo, heart). Web surfaces that cannot draw the
  mind use a crop of the kernel's own render (`allen-1-formed.png`), never a drawn or generated orb.

## Motion

Motion explains a change and then stops. The tokens are in the block below; the generator reads it.

```allen-tokens
motion:
  ease-smooth: "cubic-bezier(0.3333, 0, 0.6667, 1)"
  ease-out: "cubic-bezier(0.3333, 1, 0.6667, 1)"
  ease-enter: "cubic-bezier(0.22, 0.61, 0.36, 1)"
  frame: 16ms
  letter: 35ms
  letter-cool: 900ms
  word-rise: 600ms
  words-fade: 450ms
  read-base: 6000ms
  read-per-letter: 70ms
  state-blend: 700ms
  choice-lit: 100ms
  dawn: 900ms
  dusk: 4000ms
  dusk-fallback: 1400ms
  bar-rest: 9000ms
  bar-rise: 900ms
  caret-breath: 1100ms
  caret-hold: 500ms
  mind-form: 3000ms
  enter-rise: 10px
  enter-blur: 3px
heartbeat:
  breath-period: 5500ms
  breath-depth: "0.030"
  think-pulse-period: 900ms
  heart-idle: "0.195"
  heart-listen: "0.273"
  heart-think: "0.469"
  heart-speak: "0.313"
  heart-ease-speak: 80ms
  heart-ease: 400ms
```

- `ease-smooth` is the kernel's `ck_smooth` (3t² − 2t³) and `ease-out` is `ck_ease_out` (1 − (1 − t)³); both
  Bezier forms are exact, not fitted. `ease-enter` is the design language's curve for web entrances.
- Entrances rise: from opacity 0, 10 px lower and 3 px blurred, to rest, over `word-rise`.
- A lit choice must light within 100 ms of the key or click (`choice-lit`), so the person knows they were heard.
- Dusk (going to sleep) gathers inward over 4 s and is longer than the dawn; dawn is 0.9 s and calm.
- The heartbeat: at rest Allen breathes, his size swelling ±3.0% over 5.5 s (`mind.c` 173). While thinking his
  heart pulses every 0.9 s (`mind.c` 144). Heart brightness per mood is the kernel's 50, 70, 120 and 80 out of 256.

## Boot sequence

The night is one clock, identical on every machine. Values from `night.h` and `chirp.h`.

```allen-tokens
boot:
  holes-appear: 2600ms
  fade-end: 6800ms
  spiral: 6000ms
  merge: 12000ms
  ripple-run: 3600ms
  gone: 15200ms
  allen-born: 12300ms
  allen-formed: 15300ms
  centre-x: "0.50"
  centre-y: "0.40"
sound:
  chirp-rate: 48000Hz
  chirp-from: 200Hz
  chirp-to: 1000Hz
  chirp-peak: "0.70"
  chirp-in: 40ms
  chirp-rise: 600ms
  chirp-start: 11400ms
  chirp-end: 12300ms
```

- 0 to 6.8 s the eyes adjust: brightest stars first, then faint ones, then the Milky Way, in patches.
- 2.6 s two black holes start to bend the starlight, then show their shadows, circling.
- 6.0 s they spiral in, faster and closer (separation as τ^¼, angle as τ^⅝, τ the time left).
- 11.4 s the sound comes in; 12.0 s they merge and a ripple runs out through the stars; the sound ends by 12.3 s.
- 12.3 s Allen starts to gather from the merger's light; he is formed by 15.3 s; the last black hole is gone by 15.2 s.
- The maths, frame by frame, is in `docs/design/boot/MATHS.md`.

## Sound

There is one sound: the chirp of two black holes merging, raised into hearing (200 Hz rising to 1000 Hz over
0.6 s, loudness following pitch to the ⅔ power, peak 70% of full scale at the merger, a 0.3 s ring). It is made
from the formula at boot; there is no sound file.

- First run plays it once with the night. Nothing else in first run makes a sound. Typing, choosing and finishing
  are silent; Allen's light shows them.
- No sound device: the night plays silent and nobody is told.
- A person who turns sound off is never asked again.

## Voice

Every word a person reads goes through the humanizer skill. Allen is a person speaking to one other person.

- Short sentences, everyday words. Say what Allen is doing, then stop.
- Name hardware by what it does, then its maker ("Graphics: NVIDIA GeForce RTX 4070").
- Never on screen: our own part names (Aienux, Interplane, Store, kernel, contract, driver ABI), error codes,
  file paths, port numbers.
- No marketing words (seamless, powerful, blazing, magic, effortless, stunning), no exclamation marks, no
  "Great!", no "Oops", no emoji.
- Numbers only when measured on this machine. "I couldn't measure this yet." is a full answer.
- Questions are about intent and consent only, each asked once, each with "Not now".
- Allen says "I" and "you". He does not apologise twice or reassure ("Don't worry").

## Accessibility

Contrast is measured by the generator (`cargo run -p allen-design-tokens -- check`), WCAG 2.2 relative luminance,
on the solid colours; a pair below its floor fails the build.

```allen-tokens
contrast:
  primary-on-night: "7.0"
  hint-on-night: "4.5"
  faint-on-night: "4.5"
  mist-on-panel: "4.5"
  primary-on-lit: "4.5"
  warm-on-night: "4.5"
  glow-on-night: "4.5"
  primary-on-panel: "7.0"
  warm-soft-on-lit: "4.5"
drawn-alpha:
  hint: "1.0"
  faint: "1.0"
kernel-today-alpha:
  hint: "0.586"
  faint: "0.430"
reduced-motion:
  night: still
  letter: 0ms
  word-rise: 0ms
  state-blend: 150ms
  dusk: 400ms
  breath-depth: "0"
  think-pulse-period: 0ms
keys:
  next: ArrowDown ArrowRight Tab
  previous: ArrowUp ArrowLeft Shift+Tab
  choose: Enter Space
  not-now: Escape
  speak: any letter
```

- Contrast is measured as drawn: a colour drawn at part opacity over the night is blended first (`drawn-alpha`),
  then measured. Hint and Faint must be drawn opaque. The kernel today draws the bar's "Speak to Allen" hint at
  150/256 and the footer at 110/256 (`allen_ui.c` 271, 300); `kernel-today-alpha` keeps those for the report,
  which shows them below the floor. That is a finding for the kernel's owner, not a waiver.
- Measured ratios (the generator's output, `cargo run -p allen-design-tokens -- check`) are in
  `docs/design/tokens/contrast.txt`.
- Reduced motion (the host's or the person's setting): the night shows its last frame (the merger's light, Allen
  formed) with no orbit and no ripple; text appears whole; blends shorten to 150 ms; Allen does not breathe or
  pulse, his heart brightness alone shows his mood. The chirp still plays unless sound is off.
- Reduced motion on the very first boot (when the runner or firmware says so): the 15.2 s night is replaced by a
  0.9 s dawn onto its last frame, then Allen arrives. The night is never forced on someone who asked for less motion.
- Keyboard only: every screen works with the keys above and nothing else. Focus is always visible as a Mist edge
  with a Sky-glow halo. Typing letters anywhere goes to the input bar.
- Touch: every target is at least 44 by 44 px. Every choice has words; nothing is colour alone.

## Do's and Don'ts

- **Do** read a value from the generated files. **Don't** type a colour or a duration into a surface.
- **Do** keep one lit thing per screen. **Don't** light two buttons.
- **Do** say what is happening in one line when a step is slower than its budget. **Don't** show a bar that jumps
  or a spinner with no words.
- **Do** use the kernel's own renders for the sky and Allen on the web. **Don't** use a stock starfield, a
  generated orb or a bright screenshot.
- **Don't** add red, green, a second font, icons in rounded squares, or a second sound.
