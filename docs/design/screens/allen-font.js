// allen-font.js -- Allen's stroke typeface for the screen mockups.
// Generated from native/kernel/ui/allen_font.c on feat/allen-boot-everywhere@a8c4eaf by docs/design/tools/extract-font.mjs. Do not edit.
// Glyph grid: cap height at y = 0, baseline at y = 20; one design unit = cap / 20 px. Commands: wN advance,
// M move, L line, A cx,cy,rx,ry,a0,a1 arc in degrees (0 right, 90 down), D dot. Round caps and joins.
// Half the stroke width is weight / 1000 design units (kernel ck_af_face_init).
"use strict";
const AF_GLYPHS = [
  "w6",
  "w6 M3,0 L3,14 D3,20",
  "w10 M3,0 L3,5 M7,0 L7,5",
  "w15 M6,2 L4,20 M11,2 L9,20 M2,8 L13,8 M1,14 L12,14",
  "w15 A8,5,5,5,-30,-270 A8,15,5,5,270,510 M8,-2 L8,22",
  "w16 A4,4,2,3,0,360 A12,16,2,3,0,360 M13,1 L3,19",
  "w16 M14,20 L5,9 A7,5,3,4,150,400 L3,14 A7,15,4,5,180,60 L13,11",
  "w6 M3,0 L3,5",
  "w9 A10,10,6,12,240,120",
  "w9 A-1,10,6,12,-60,60",
  "w14 M7,2 L7,10 M3,4 L11,8 M11,4 L3,8",
  "w14 M2,12 L12,12 M7,7 L7,17",
  "w6 M3,19 L2,23",
  "w11 M2,13 L9,13",
  "w6 D3,20",
  "w12 M10,0 L2,20",
  "w14 A7,10,5,10,0,360",
  "w14 M3,4 L8,0 L8,20",
  "w14 A7,6,5,6,200,400 L2,20 L12,20",
  "w14 A7,5,5,5,200,450 A7,15,5,5,270,520",
  "w14 M10,20 L10,0 L1,14 L13,14",
  "w14 M12,0 L4,0 L4,9 A7,14,5,6,235,520",
  "w14 A7,14,5,6,0,360 M10,0 L3,11",
  "w14 M2,0 L12,0 L5,20",
  "w14 A7,5,4,5,0,360 A7,15,5,5,0,360",
  "w14 A7,6,5,6,0,360 M4,20 L11,9",
  "w6 D3,8 D3,20",
  "w6 D3,8 M3,19 L2,23",
  "w14 M12,6 L2,12 L12,18",
  "w14 M2,10 L12,10 M2,15 L12,15",
  "w14 M2,6 L12,12 L2,18",
  "w14 A7,5,5,5,190,450 L7,14 D7,20",
  "w19 A9,12,3,4,0,360 M12,8 L12,14 A14,14,2,2,180,0 A9,11,7,9,0,-300",
  "w16 M2,20 L8,0 L14,20 M4,13 L12,13",
  "w15 M2,0 L2,20 M2,0 L8,0 A8,5,4,5,270,450 L2,10 M2,10 L9,10 A9,15,4,5,270,450 L2,20",
  "w17 A9,10,7,10,45,315",
  "w16 M2,0 L2,20 M2,0 L6,0 A6,10,8,10,270,450 L2,20",
  "w14 M12,0 L2,0 L2,20 L12,20 M2,10 L10,10",
  "w13 M12,0 L2,0 L2,20 M2,10 L10,10",
  "w18 A9,10,7,10,-45,-360 M11,10 L16,10",
  "w16 M2,0 L2,20 M14,0 L14,20 M2,10 L14,10",
  "w6 M3,0 L3,20",
  "w13 M10,0 L10,14 A6,14,4,6,0,180",
  "w15 M2,0 L2,20 M13,0 L2,12 M6,8 L14,20",
  "w13 M2,0 L2,20 L12,20",
  "w20 M2,20 L2,0 L10,14 L18,0 L18,20",
  "w16 M2,20 L2,0 L14,20 L14,0",
  "w20 A10,10,8,10,0,360",
  "w15 M2,20 L2,0 L8,0 A8,5,5,5,270,450 L2,10",
  "w20 A10,10,8,10,0,360 M12,14 L18,21",
  "w15 M2,20 L2,0 L8,0 A8,5,5,5,270,450 L2,10 M8,10 L14,20",
  "w16 A8,5,5,5,-30,-270 A8,15,5,5,270,510",
  "w16 M1,0 L15,0 M8,0 L8,20",
  "w16 M2,0 L2,13 A8,13,6,7,180,0 L14,0",
  "w16 M1,0 L8,20 L15,0",
  "w22 M1,0 L6,20 L11,0 L16,20 L21,0",
  "w16 M2,0 L14,20 M14,0 L2,20",
  "w16 M1,0 L8,10 L15,0 M8,10 L8,20",
  "w16 M2,0 L14,0 L2,20 L14,20",
  "w9 M7,-1 L3,-1 L3,21 L7,21",
  "w12 M2,0 L10,20",
  "w9 M2,-1 L6,-1 L6,21 L2,21",
  "w12 M2,8 L6,1 L10,8",
  "w14 M1,23 L13,23",
  "w7 M2,0 L4,4",
  "w15 A7,14,5,6,0,360 M12,8 L12,20",
  "w15 M2,0 L2,20 A7,14,5,6,0,360",
  "w13 A7,14,5,6,40,320",
  "w15 A7,14,5,6,0,360 M12,0 L12,20",
  "w14 M2,14 L12,14 A7,14,5,6,0,-320",
  "w11 M5,20 L5,4 A9,4,4,4,180,300 M2,8 L10,8",
  "w15 A7,14,5,6,0,360 M12,8 L12,22 A7,22,5,5,0,150",
  "w15 M2,0 L2,20 A7,13,5,5,180,360 L12,20",
  "w6 M3,8 L3,20 D3,3",
  "w7 M4,8 L4,22 A0,22,4,5,0,120 D4,3",
  "w13 M2,0 L2,20 M11,8 L2,15 M5,13 L12,20",
  "w6 M3,0 L3,20",
  "w21 M2,8 L2,20 A6,12,4,4,180,360 L10,20 A14,12,4,4,180,360 L18,20",
  "w15 M2,8 L2,20 A7,13,5,5,180,360 L12,20",
  "w14 A7,14,5,6,0,360",
  "w15 M2,8 L2,27 A7,14,5,6,0,360",
  "w15 A7,14,5,6,0,360 M12,8 L12,27",
  "w10 M2,8 L2,20 A7,13,5,5,180,290",
  "w14 A7,11,4,3,-30,-270 A7,17,4,3,270,510",
  "w11 M5,2 L5,17 A8,17,3,3,180,40 M2,8 L10,8",
  "w15 M2,8 L2,15 A7,15,5,5,180,0 M12,8 L12,20",
  "w14 M2,8 L7,20 L12,8",
  "w20 M2,8 L6,20 L10,8 L14,20 L18,8",
  "w14 M2,8 L12,20 M12,8 L2,20",
  "w14 M2,8 L7,20 M12,8 L4,27",
  "w14 M2,8 L12,8 L2,20 L12,20",
  "w10 M8,-1 L6,0 L6,8 L4,10 L6,12 L6,20 L8,21",
  "w6 M3,-1 L3,23",
  "w10 M2,-1 L4,0 L4,8 L6,10 L4,12 L4,20 L2,21",
  "w14 M2,13 L5,11 L9,14 L12,12"
];

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
    const P = (x, y) => `${(pen + x * u).toFixed(2)} ${(y0 + y * u).toFixed(2)}`;
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
  for (const para of text.split("\n")) {
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
    const face = AF_FACES[el.dataset.af] || AF_FACES.body, text = el.textContent.replace(/\s+/g, " ").trim();
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
    svg.setAttribute("viewBox", `0 0 ${w.toFixed(1)} ${h.toFixed(1)}`); svg.setAttribute("aria-hidden", "true");
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
