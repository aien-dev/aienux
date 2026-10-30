# Allen's canvas: evidence

Branch feat/allen-canvas (aienos-mv). Grammar and reasons: `docs/ALLEN_CANVAS.md`.

- `questions.txt`: the ten questions asked of the model.
- `round-1/`: prompt v1 (`prompt.txt`), every raw reply exactly as the model wrote it (`NN-slug.txt`), the
  question (`ask-NN-slug.txt`), timings and token counts (`log.tsv`, `run.log`), and the parse report:
  **10 of 10 clean**, 10 of 10 used the canvas.
- `round-2/`: prompt v2 (the one on the branch): **3 of 4 clean** (one reply had two say lines); stopped
  after four questions when other jobs held the GPU lock (see `run.log`).
- `frames/`: what was painted, as 1440 px JPEG (the test writes full PNGs to
  `target/native-kernel-allen-canvas/`): `samples/` (the test's fixed scenes, the spark arriving, and the
  sky after the things have gone), `round-1/` and `round-2/` (the real replies).
- `frame-times-final.txt`: frame times at 3840x2160 on one host core, base 3f48a88 against the branch, held
  under the quiet lock and the GPU lock, with the load at start and end. Earlier runs during the work:
  `frame-times.txt` (first shade), `frame-times-oval-before-speedup.txt`.

Model: qwen3.8-27b-fp8 served by aien-serve-http at 127.0.0.1:18091 (the brief named Qwen3-30B-A3B; the
server on that port serves this 27B). Commands:

    sh scripts/ck_allen_canvas_test.sh --replies evidence/allen-canvas/round-1
    LIVE_DIR=evidence/allen-canvas/round-3 sh scripts/ck_allen_canvas_test.sh --live
