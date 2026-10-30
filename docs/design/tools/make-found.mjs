// make-found.mjs -- writes 05b-what-allen-found.html, the constellation of found parts, into the current directory.
//   cd docs/design/screens && node ../tools/make-found.mjs
import { writeFileSync } from "node:fs";
// the parts, placed round Allen (960, 432) like the kernel's notes constellation; example values
const C = [960, 432];
const parts = [
  ["Screen", "2560 by 1440, 144 a second", 600, 170],
  ["Graphics", "RTX 4070, 12 GB", 960, 96],
  ["Processor", "Ryzen 7 7800X3D, 8 cores", 1320, 170],
  ["Memory", "32 GB", 1620, 430],
  ["Storage", "1 TB fast, 2 TB", 1480, 668],
  ["Network", "wired, connected", 1160, 760],
  ["Sound", "speakers and a microphone", 760, 760],
  ["Keyboard and mouse", "working", 470, 640],
  ["Nearby", "2 AienOS computers, a printer", 300, 430],
];
const threads = parts.map(([, , x, y0]) => {
  const y = y0 < C[1] - 100 ? y0 + 66 : y0;                                          // parts above Allen: meet the thread under their words
  const dx = x - C[0], dy = y - C[1], d = Math.hypot(dx, dy), r0 = 150;            // start outside Allen's light
  return `<line x1="${(C[0] + dx / d * r0).toFixed(0)}" y1="${(C[1] + dy / d * r0).toFixed(0)}" x2="${(x - dx / d * 14).toFixed(0)}" y2="${(y - dy / d * 14).toFixed(0)}"/>`;
}).join("\n      ");
const stars = parts.map(([l, v, x, y], i) => `<div class="part rise" data-at="${300 + i * 110}" style="left:${x - 200}px;top:${y - 8}px">
    <span class="star"></span>
    <span class="plabel" data-af="small">${l}</span>
    <span class="pvalue" data-af="body">${v}</span>
  </div>`).join("\n  ");
writeFileSync("05b-what-allen-found.html", `<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Screen 5: what Allen found</title>
<link rel="stylesheet" href="screens.css">
<style>
  /* the computer as a constellation round Allen: one star per part, a thread of light to him (mind.c threads) */
  .threads { position: absolute; inset: 0; }
  .threads line { stroke: rgb(var(--allen-colors-glow-rgb) / .22); stroke-width: 1.2; stroke-linecap: round;
                  stroke-dasharray: 600; stroke-dashoffset: 600;
                  animation: draw 900ms var(--allen-motion-ease-out) forwards; }
  .threads line:nth-child(n) { animation-delay: calc(var(--i, 0) * 110ms + 200ms); }
  @keyframes draw { to { stroke-dashoffset: 0; } }
  .part { position: absolute; width: 400px; display: grid; justify-items: center; row-gap: 8px; }
  .star { width: 9px; height: 9px; border-radius: 50%; background: var(--allen-colors-heart-core);
          box-shadow: 0 0 10px var(--allen-colors-warm), 0 0 26px rgb(var(--allen-colors-glow-rgb) / .5); }
  .plabel { color: var(--allen-colors-hint); }
  .pvalue { color: var(--allen-colors-primary); }
  @media (prefers-reduced-motion: reduce) { .threads line { animation: none; stroke-dashoffset: 0; } }
</style>
</head>
<body>
<div class="board sky">
  <div class="tag">5 · what Allen found (example values; drawn as a constellation round Allen)</div>
  <svg class="threads" width="1920" height="1080" viewBox="0 0 1920 1080" aria-hidden="true">
      ${threads.split("\n      ").map((l, i) => l.replace("<line", `<line style="--i:${i}"`)).join("\n      ")}
  </svg>
  ${stars}
  <p class="says center rise" style="top: 848px" data-at="1400" data-af="voice">This is your computer as I see it.</p>
  <div class="buttons" style="top: 918px">
    <div class="button lit rise" data-at="1700" data-href="06-consent.html"><span data-af="body">Next</span></div>
  </div>
</div>
<script src="allen-font.js"></script>
<script src="screen.js"></script>
</body>
</html>
`);
