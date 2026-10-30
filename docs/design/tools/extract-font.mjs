// extract-font.mjs -- copies Allen's stroke typeface out of the kernel (native/kernel/ui/allen_font.c) into
// docs/design/screens/allen-font.js, so the screen mockups draw their words with the same strokes the kernel
// draws, at the kernel's cap heights and stroke weights. Dev tool only.
//
//   node docs/design/tools/extract-font.mjs path/to/allen_font.c "source label" > docs/design/screens/allen-font.js

import { readFileSync } from "node:fs";

const [src, label = "native/kernel/ui/allen_font.c"] = process.argv.slice(2);
if (!src) {
  console.error("usage: extract-font.mjs allen_font.c [label]");
  process.exit(2);
}
const c = readFileSync(src, "utf8");
const start = c.indexOf("glyphs[CK_AF_COUNT] = {");
const end = c.indexOf("};", start);
if (start < 0 || end < 0) {
  console.error("extract-font: glyph table not found");
  process.exit(1);
}
const glyphs = [...c.slice(start, end).matchAll(/^\s*\/\*.*?\*\/\s*"([^"]*)"/gm)].map((m) => m[1]);
if (glyphs.length !== 95) {
  console.error(`extract-font: expected 95 glyphs (ASCII 32..126), found ${glyphs.length}`);
  process.exit(1);
}

process.stdout.write(`// allen-font.js -- Allen's stroke typeface for the screen mockups.
// Generated from ${label} by docs/design/tools/extract-font.mjs. Do not edit.
// Glyph grid: cap height at y = 0, baseline at y = 20; one design unit = cap / 20 px. Commands: wN advance,
// M move, L line, A cx,cy,rx,ry,a0,a1 arc in degrees (0 right, 90 down), D dot. Round caps and joins.
// Half the stroke width is weight / 1000 design units (kernel ck_af_face_init).
"use strict";
const AF_GLYPHS = ${JSON.stringify(glyphs, null, 0).replace(/","/g, '",\n  "').replace("[", "[\n  ").replace(/]$/, "\n]")};

// The faces Allen's window uses (allen_ui.c 823: cap heights at 1080p; 860: weights).
const AF_FACES = { voice: { cap: 19, weight: 1000, line: 40 }, body: { cap: 15, weight: 1150, line: 30 },
                   small: { cap: 11, weight: 1250, line: 22 } };

function afGlyph(ch) {
  const code = ch.charCodeAt(0);
  return AF_GLYPHS[code >= 32 && code <= 126 ? code - 32 : "?".charCodeAt(0) - 32];
}

function afAdvance(ch) { return parseInt(afGlyph(ch).slice(1), 10); }

// SVG path data for one line of text, in px, pen starting at x0 with the cap top at y0.
function afPath(text, cap, x0 = 0, y0 = 0) {
  const u = cap / 20, out = [];
  let pen = x0;
  for (const ch of text) {
    const g = afGlyph(ch), toks = g.split(" ").slice(1);
    const P = (x, y) => \`\${(pen + x * u).toFixed(2)} \${(y0 + y * u).toFixed(2)}\`;
    for (const t of toks) {
      const n = t.slice(1).split(",").map(Number);
      if (t[0] === "M") out.push("M" + P(n[0], n[1]));
      else if (t[0] === "L") out.push("L" + P(n[0], n[1]));
      else if (t[0] === "D") out.push("M" + P(n[0], n[1]) + "l0.01 0");
      else if (t[0] === "A") {
        const [cx, cy, rx, ry, a0, a1] = n, steps = Math.max(2, Math.ceil(Math.abs(a1 - a0) / 8));
        for (let i = 0; i <= steps; i++) {
          const a = ((a0 + (a1 - a0) * i / steps) * Math.PI) / 180;
          out.push((i ? "L" : "M") + P(cx + rx * Math.cos(a), cy + ry * Math.sin(a)));
        }
      }
    }
    pen += parseInt(g.slice(1), 10) * u;
  }
  return { d: out.join(""), width: pen - x0 };
}

function afMeasure(text, cap) { let w = 0; for (const ch of text) w += afAdvance(ch); return (w * cap) / 20; }

// Word wrap to a width in px (the kernel wraps greedily, allen_ui.c 150), then balanced: the narrowest width
// that keeps the same number of lines, so no line ends with one stranded word.
function afWrapAt(text, cap, width) {
  const lines = [];
  for (const para of text.split("\\n")) {
    let line = "";
    for (const word of para.split(" ")) {
      const next = line ? line + " " + word : word;
      if (line && afMeasure(next, cap) > width) { lines.push(line); line = word; } else line = next;
    }
    lines.push(line);
  }
  return lines;
}
function afWrap(text, cap, width) {
  const greedy = afWrapAt(text, cap, width);
  if (greedy.length < 2) return greedy;
  let lo = 0, hi = width;
  while (hi - lo > 2) { const mid = (lo + hi) / 2; if (afWrapAt(text, cap, mid).length <= greedy.length) hi = mid; else lo = mid; }
  return afWrapAt(text, cap, hi);
}

// Draws every [data-af] element's text with the stroke font. The real text stays in the page for screen
// readers and search; the drawing is aria-hidden. data-af = voice | body | small; <p> wraps to its width (or data-wrap px), anything else stays on one line.
function afRender(root = document) {
  for (const el of root.querySelectorAll("[data-af]")) {
    if (el.dataset.afDone) continue;
    const face = AF_FACES[el.dataset.af] || AF_FACES.body, text = el.textContent.replace(/\\s+/g, " ").trim();
    const wrap = Number(el.dataset.wrap || 0) || (el.tagName === "P" ? el.clientWidth || 900 : 1e9);  // only paragraphs wrap
    const lines = afWrap(text, face.cap, wrap), sw = (2 * face.weight / 1000) * (face.cap / 20);
    const w = Math.max(...lines.map((l) => afMeasure(l, face.cap))) + sw * 2;
    const h = (lines.length - 1) * face.line + face.cap * 27 / 20 + sw * 2;
    const align = getComputedStyle(el).textAlign;
    const d = lines.map((l, i) => {
      const lw = afMeasure(l, face.cap), x = align === "center" ? (w - lw) / 2 : align === "right" ? w - lw - sw : sw;
      return afPath(l, face.cap, x, sw + i * face.line).d;
    }).join("");
    const ns = "http://www.w3.org/2000/svg", svg = document.createElementNS(ns, "svg");
    svg.setAttribute("width", w.toFixed(1)); svg.setAttribute("height", h.toFixed(1));
    svg.setAttribute("viewBox", \`0 0 \${w.toFixed(1)} \${h.toFixed(1)}\`); svg.setAttribute("aria-hidden", "true");
    svg.classList.add("af-svg");
    const path = document.createElementNS(ns, "path");
    path.setAttribute("d", d); path.setAttribute("fill", "none"); path.setAttribute("stroke", "currentColor");
    path.setAttribute("stroke-width", sw.toFixed(2)); path.setAttribute("stroke-linecap", "round");
    path.setAttribute("stroke-linejoin", "round");
    svg.appendChild(path);
    const span = document.createElement("span");
    span.className = "af-text"; span.textContent = text;
    el.textContent = ""; el.append(span, svg); el.dataset.afDone = "1";
  }
}
`);
