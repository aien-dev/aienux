# Allen's first-run screens

Mockups of every screen in `docs/FIRST_RUN_UX.md` (L266, `origin/feat/allen-boot-everywhere@b1c24cb2`), on
the kernel's 1920 x 1080 design grid, using only the generated tokens (`../tokens/allen-tokens.css`) and the
kernel's own stroke font (`allen-font.js`, extracted from `native/kernel/ui/allen_font.c`). This is the build
target for L267 (the wizard) and for the runner windows (L261, L262, L265).

- Start at `index.html`. Each page works with the keyboard alone: arrows move the lit choice, Enter or Space
  picks it, Escape is "Not now", typing goes to the bar. Links follow the flow.
- `?clean=1` hides the screen's name tag. `shots/` holds every screen at 960 x 540 (shot at 1920 x 1080) and
  `shots/all-2560x1440.jpg`, all 22 at 2560 x 1440, to show the layout holds when scaled.
- Pages are generated: `cd docs/design/screens && node ../tools/make-screens.mjs && node ../tools/make-found.mjs`
  (04-intent is written by hand). Shots: `node ../tools/shoot.mjs "file://$PWD/<page>?clean=1" --size 1920x1080
  --wait "window.__screen_ready && performance.now() > 5000" --out <png>`; all 44 exit 0 on 2026-10-20.
- `05b-what-allen-found.html` is new: the found parts as a constellation round Allen's light, one star each,
  threads drawn in one by one. It sits between screen 5 and screen 6.
- Backgrounds are real kernel frames (`assets/`, rendered from L266's `allen_ui.c`), so the sky matches.

## Wording changes against the spec (for L266 to take or leave)

Every line was read aloud and cut for plainness. Where the mockup differs from `FIRST_RUN_UX.md`:

| Screen | Spec says | Mockup says | Why |
| --- | --- | --- | --- |
| 4 | Faster frames, a steady picture, the most from your screen and graphics. | Faster, steadier frames from your graphics and screen | one idea, fits the row |
| 4 | Use the graphics and memory in this computer to run an AI that stays on it. | An AI that runs on this computer and stays on it | says what it is, not how |
| 4 | (bar line) What would you like this computer to do best? | question above the choices; bar says "Or just tell me" | the question and the bar no longer repeat each other |
| 6 | Nothing on your other drives is changed. You can undo any of this. | I won't touch your other drives, and you can undo any of this. | Allen speaks as himself |
| 7b | ...fits if I share it with main memory, slower. | ...fits if I share it with main memory, but it runs slower. | the clipped ending read as a typo |
| 7c | It stays sealed on this computer and is never shown again. | I'll keep it sealed on this computer and won't show it again. | Allen speaks as himself |
| 7c | That key didn't work. The service said: key not valid. Try again? | That key didn't work. Claude said the key isn't valid. Try again? | names who said it, no colon quote |
| 8 | I built a driver for your Wi-Fi and tested it safely. Turn it on? | I built a driver for your Wi-Fi and tested it without touching the real one. Turn it on? | "safely" is a claim; this says what was done |
| 8 | I've put the old one back. I'll keep working on it. | I've put the old driver back, and I'll keep working on it. | "the old one" was unclear |
