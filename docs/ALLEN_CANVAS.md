# Allen's canvas: the model paints the screen

Drake's words: "It all will be on a screen that isn't being processed. It is literally being painted on by
the model." Until this branch, the language model behind Allen could only hand back one plain line of text,
and the kernel decided everything else. Allen's canvas is the first step past that: the model now decides
*what is shown*, in a small language of its own, and the kernel paints it in Allen's style. This document is
the grammar and the reasons for it.

Code: `native/kernel/svc/allen_scene.{h,c}` (the parser), `native/kernel/ui/canvas.{h,c}` (the painter),
small hooks in `native/kernel/ui/allen_ui.c` (`ck_allen_ui_scene`). The prompt the model is given:
`crates/allen-courier/scene_prompt.txt`. Tests: `scripts/ck_allen_canvas_test.sh` (add `--live` to ask the
real model). Evidence: `evidence/allen-canvas/`.

## 1. Why a language, and why this one

The model must not draw pixels. Three reasons, each measured or physical:

1. **Safety.** Whatever the model writes, the kernel has to survive it. A small line grammar with fixed limits
   can be parsed in a few hundred lines of integer C with every byte bounds-checked and fuzzed (300,000
   random and mutated replies per test run, under AddressSanitizer and UndefinedBehaviorSanitizer). Pixel
   commands or a general layout language (HTML, SVG, a display list) cannot be checked that cheaply, and would
   let one reply clutter or break the screen.
2. **Calm.** The design rule is one thing in the centre, the rest at the edge, every change eased in
   (`docs/ALLEN_PRESENCE_RESEARCH.md` section 3). If the model chose coordinates and colours it could break
   that rule on any answer. In this language it can only say *what* (a number, a list, a person) and *roughly
   where* (left, right, above); the look, the motion and the palette stay the kernel's.
3. **Reliability of a mid-size model.** A model writes keyword lines far more reliably than nested JSON
   (the inference-server notes record Qwen3 dropping a closing bracket and running on past the end of an
   array). A line it gets wrong is dropped on its own; the rest of the scene still shows.

The vocabulary is the smallest set that covers what people ask a computer for most of the day: an answer
(a number, a note), steps or choices (a list), time (a timer), people (a person and a message), something to
look at (a picture, a video, a quote), and the feeling of the room (a mood). The same lines will later carry a
friend's message arriving (`person from`) or a video playing in its place (`video`) without a single window:
the thing the line names is what appears, in the sky beside Allen.

## 2. The grammar

A scene is plain ASCII, at most **12 lines** that count and at most **2048 bytes**; anything after is not read.

```
scene   = { line "\n" }
line    = mood | say | thing | (blank, a code fence, or the model's <think> block: skipped)
mood    = "mood" { tone | water }
tone    = "night" | "dawn" | "deep" | "clear"
water   = "still" | "slow" | "lively"
say     = "say" [":"] text                    (exactly one; Allen speaks it; at most 159 characters)
thing   = kind { word } ":" field { "|" field } (at most 4 things on screen at once)
kind    = "note" | "list" | "number" | "quote" | "person" | "timer" | "picture" | "video"
word    = "left" | "right" | "above"           (where it sits; otherwise the kernel chooses)
        | "warm" | "quiet"                     (lit warm to stand out, or held back)
        | "bloom" | "rise" | "fade"            (how it arrives; bloom is the default)
        | "to" | "from"                        (a person's message: going to them or come from them)
```

The fields of each kind, with the room each one has (longer text is cut at a word and ends in "..."):

| kind    | fields                         | room (characters)           | needs |
|---------|--------------------------------|-----------------------------|-------|
| note    | title, text (or just text)     | 48, 240                     | 1     |
| list    | title, item, item, ...         | 48, then 64 each (5 items)  | 1     |
| number  | value, caption                 | 16, 64 (value has a digit)  | 1     |
| quote   | text, who                      | 220, 48                     | 1     |
| person  | name, message                  | 32, 200                     | 2     |
| timer   | when, label                    | 24, 64                      | 1     |
| picture | what it shows, where from      | 120, 64                     | 1     |
| video   | what it shows, where from      | 120, 64                     | 1     |

A timer's `when` is a length (`25m`, `1h 30m`, `90 seconds`, `in 10 minutes`, up to 24 hours) and then the
ring of light empties as the time runs out; anything else (`6:00 pm`) is shown as written, in a full ring.

Example (a real reply shape):

```
mood calm
say Seventeen percent of 240 is 40.8.
number right: 40.8 | 17% of 240
note left quiet: How | 240 x 0.17 = 40.8
```

### What the parser forgives, and what it counts

The kernel never shows a line it does not understand, and never complains on screen. Every reply gets a report
(`ck_scene_stats`): lines read, kept, **rejected** (an unknown first word, or a thing with nothing in it),
**tidied** (understood after taking off a list mark, numbering, bold stars or a code fence the grammar does
not use), **cut** (a field too long, or bytes outside printable ASCII left out), **over** (past 12 lines, past
4 things, a second say or mood) and **truncated** (past 2 KiB). A reply is **clean** when nothing was
rejected, tidied, over or truncated and it has its say line; that is the number reported for the real model.
A reply with no scene lines at all is still spoken: its first line becomes the say.

On the courier's one-line wire a scene travels with its newlines written as `\n`;
`ck_scene_unwire()` turns it back into lines before `ck_allen_ui_scene()`.

## 3. How it is painted (ui/canvas.c)

Everything is drawn into the same night as Allen, in the palette of `ui/allen_ui.c` and nothing else: night
rgb(6 8 18), text rgb(236 240 252), hint rgb(150 160 196), glow rgb(150 170 255), mist rgb(200 210 255), ink
rgb(64 86 196), warm rgb(255 212 150) and rgb(255 222 180). The type is Allen's own stroke face.

- **Nothing is a window.** No box has an edge. Words rest in a soft pool of shade that fades out over about
  80 px, so stars never show through text and no rectangle is ever drawn. Text things have one thread of
  light down their side (warm for a quotation). A number is large, thin strokes over a faint glow. A timer is
  a ring of light that empties, its moving end a warm point. A person is a ring round their initial. A
  picture or video not yet here is a soft field of deep blue light with a brighter light drifting slowly
  across it, and "not here yet" under it; a video adds a play mark drawn in light.
- **Allen stays alone in the centre.** Things sit in two columns beside him (each at most two things, kept
  above his spoken words) or in one place above him; at most four things at once. The kernel chooses when the
  model does not say, and moves a thing to where there is room when the asked-for place is full.
- **Every change eases in.** A thing blooms: a spark leaves Allen's heart, flies on a curve to its place,
  touches the water there (a drop of ink in the fluid), and the thing opens line by line (each line eases up
  10 px and in over 0.7 s, 70 ms after the one before). Things come one after another, 260 ms apart, as Allen
  starts to speak. A new line from the person sends them away over 0.7 s; a new scene replaces them the same
  way; left alone they go after 90 s (a running timer stays). The mood's tone blends over 0.7 s: `dawn` warms
  the low sky, `clear` lifts the high sky with mist, `deep` darkens the night; `lively` water gets a gentle
  current pushed round Allen now and then.
- **The kernel's rules hold.** Integer only (1/256 and 16ths of a pixel), no floating point (the test
  compiles both files `-ffreestanding -mgeneral-regs-only` and checks they call nothing outside the kernel but
  memset/memcpy/memmove), no allocation (one static 384 KiB pool for the large digits, rasterized at start so
  drawing never writes to it), and drawing only reads, so the other cores can share a frame. Only the parts
  of the screen that change are redrawn; the test checks that drawing those parts gives the same frame as
  drawing everything.

## 4. The prompt

`crates/allen-courier/scene_prompt.txt` is the system prompt. It names the lines, gives the rules (plain
ASCII, at most three things, be honest about what Allen cannot do yet: no weather, clock, internet, messages or
media of his own) and three short examples that are not the test questions. `allen-scene`
(`crates/allen-courier/src/bin/allen-scene.rs`) builds it in with `include_str!`, so the relay can use the
same file the same way. The live test asks the model one question at a time and waits out the server's 503
(busy) answers, waiting as its Retry-After asks (1 to 15 s), for up to thirty minutes per question.

## 5. Measured

All on the Spark (GB10 host CPU, one core, 3840 x 2160), evidence in `evidence/allen-canvas/`.

- **Real model replies** (Qwen3.8-27B FP8 on our own server, 127.0.0.1:18091, temperature 0.3, ten questions
  in `questions.txt`, raw replies kept as written): round 1, prompt v1: **10 of 10 parsed cleanly** and 10 of
  10 used the canvas (`round-1/parse-report.txt`). Round 2, prompt v2 (one more rule: watch or play means a
  video line, see means a picture line): **3 of 4 clean**; the fourth wrote two say lines (the second is
  dropped, the first is spoken), and the round was stopped after four questions when other jobs held the GPU
  lock for good (`round-2/run.log`). Together: **13 of 14 real replies clean**, every one of them understood
  and painted. The server is shared and answers one request at a time; a reply took 1.5 to 14 minutes of
  waiting (Retry-After honoured, never two requests at once).
- **Fuzz**: 300,000 replies per run (random bytes, mutated scenes, word soup), each in a buffer of exactly its
  length, under AddressSanitizer and UndefinedBehaviorSanitizer: no overrun, no undefined behaviour, no scene
  outside its limits. It found one real bug outside the parser: `ck_atan2` in `ui/paint.c` shifted a negative
  number left (undefined in C); fixed by multiplying.
- **Frame cost** (`frame-times-final.txt`, held under the quiet lock): the existing window test averages
  19.8 and 19.1 ms per frame on the base commit and 20.9 and 18.3 ms with the canvas hooked in (two pairs, the
  machine's load fell from 43 to 9 during the run): no cost that rises above the noise. In the canvas test, frames
  while plain answers are spoken average 19.2 ms and frames while scenes arrive and rest average 24.1 ms (worst
  45 ms, during an arrival, when the spark and a whole thing are redrawn).
- **Image size**: IMACKERN.EFI 307,200 bytes (274,432 before); the zeroed memory grows by the 384 KiB pool.

## 6. What is not done

- The courier does not carry scenes to the iMac yet: the relay (`crates/allen-courier/src/main.rs`) and the
  kernel's answer path belong to the "Allen through the model" branch; the hook they need is
  `ck_allen_ui_scene()` plus `ck_scene_unwire()`.
- Pictures and videos are places only; nothing fetches or decodes them.
- None of this has run on the real iMac.
