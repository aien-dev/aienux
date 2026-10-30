# Frame time per stage

Two different things were timed. Neither is the iMac.

## 1. The sketch (`night.html`), in a browser with no graphics card

OBSERVED, 2026-10-20, on the Spark. Command (run through `quiet-build`):

```sh
node docs/design/tools/shoot.mjs "file://$PWD/docs/design/boot/night.html?measure=1&w=1920&h=1080" \
  --size 1920x1080 --wait "window.__night_done" --timeout-ms 240000 \
  --result "JSON.stringify(window.__night_result)" > frame-times-1920x1080.json     # exit 0
```

and the same at 1280x720 (exit 0). Raw results: `frame-times-1920x1080.json`, `frame-times-1280x720.json`.

The browser drew with SwiftShader, a CPU stand-in for a graphics card (the renderer string is in the JSON).
Headless Chrome on this machine gave no real GPU path. The machine was busy: load average 54.6 on 20 cores
at the end of the run. Each frame is timed from the start of drawing to a one-pixel read-back, which forces
the frame to finish.

| stage (ms of the night) | 1920x1080 mean / p95 / max frame (ms) | 1280x720 mean / p95 / max frame (ms) |
| --- | --- | --- |
| dark (0 to 2600)        | 441 / 668 / 668 (n 4)  | 104 / 262 / 262 (n 18) |
| holes (2600 to 6000)    | 275 / 444 / 444 (n 12) | 74 / 114 / 141 (n 44)  |
| spiral (6000 to 12000)  | 217 / 297 / 369 (n 27) | 93 / 145 / 192 (n 61)  |
| merger (12000 to 15200) | 309 / 486 / 486 (n 12) | 88 / 149 / 163 (n 39)  |
| settled (after 15200)   | 402 / 518 / 518 (n 3)  | 104 / 136 / 136 (n 13) |

One-time bakes before the first frame, 1920x1080: clouds 114 ms, scale-up 31.6, stars 16.9, galaxies 54.3,
appear map 8.5. At 1280x720: 98.7, 33.4, 6.5, 38.0, 9.9.

What this means: on a CPU-only renderer the sketch runs at a few frames a second at 1080p, so it is a
picture-accurate reference, not a smoothness reference. INFERRED: on any real graphics card the shader is a
single full-screen pass and should hold the display rate, but that is NOT MEASURED here.

## 2. The kernel's own night (`native/kernel/ui`), on one CPU core

OBSERVED, 2026-10-20: the kernel UI host test, built with `cc` from L266's sources
(`feat/allen-boot-everywhere@a8c4eaf`), printed:

```
ALLEN_UI_TEST: PASS (61 checks)
3768 frames at 3840x2160 on one core: average 12392 us, slowest 83234 us
```

That is the shipping drawing code, 4K, software only, on one Spark core: about 12.4 ms a frame on average,
inside a 60 Hz budget (16.7 ms) on average, with a slowest frame of 83 ms. The iMac's own number is UNKNOWN
until L266 times it on the device.
