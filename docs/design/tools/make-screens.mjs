// make-screens.mjs -- writes the plain first-run mockups (every screen except 04 and 05b) into the current directory.
//   cd docs/design/screens && node ../tools/make-screens.mjs
import { writeFileSync } from "node:fs";
const page = (file, title, tag, cls, body) => writeFileSync(file, `<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>${title}</title>
<link rel="stylesheet" href="screens.css">
</head>
<body>
<div class="board ${cls}">
  <div class="tag">${tag}</div>
${body.trim().split("\n").map((l) => "  " + l).join("\n")}
</div>
<script src="allen-font.js"></script>
<script src="screen.js"></script>
</body>
</html>
`);
const says = (text, top, at, extra = "") => `<p class="says center rise" style="top: ${top}px"${at !== undefined ? ` data-at="${at}"` : ""} data-af="voice"${extra}>${text}</p>`;
const btns = (top, items) => `<div class="buttons" style="top: ${top}px">\n${items.map(([t, href, lit]) => `  <div class="button rise${lit ? " lit" : ""}" data-href="${href}"><span data-af="body">${t}</span></div>`).join("\n")}\n</div>`;
const choices = (top, items) => `<div class="choices" style="top: ${top}px" role="listbox">\n${items.map(([t, w, href, lit]) => `  <div class="choice rise${lit ? " lit" : ""}" data-href="${href}"><span data-af="body">${t}</span><span class="what" data-af="small">${w}</span><span class="key" data-af="small">Enter</span></div>`).join("\n")}\n</div>`;
const found = (top, rows) => `<div class="found" style="top: ${top}px">\n${rows.map(([l, v, cls = ""]) => `  <span class="label rise" data-af="body">${l}</span><span class="value rise ${cls}" data-af="body">${v}</span>`).join("\n")}\n</div>`;
const bar = (text, focus = false) => `<div class="bar rise${focus ? " focus" : ""}">${text}</div>`;

page("01-power-on.html", "Screen 1: power on", "1 · power on (the maker's logo natively; a black window in aienos-run; we draw nothing)", "", `<span hidden data-href="02-night.html"></span>`);

page("02-night.html", "Screen 2: the night", "2 · the night (still at 11.8 s; live reference: ../boot/night.html)", "", `
<div class="board-still" style="position:absolute;inset:0;background:url('assets/night-11800.jpg') center/cover"></div>
<div class="buttons" style="top: 980px"><div class="button rise" data-href="../boot/night.html"><span data-af="small">Play the night with its sound</span></div><div class="button lit rise" data-href="03-allen-arrives.html"><span data-af="small">Next screen</span></div></div>`);

page("03-allen-arrives.html", "Screen 3: Allen arrives", "3 · Allen arrives", "sky", `
${says("Hello. I'm Allen.", 640, 0)}
${says("I'm going to look after this computer with you.", 690, 1400)}
${bar('<span data-af="body">What would you like this computer to do best?</span>').replace('class="bar rise"', 'class="bar rise" data-at="3600"')}
<span class="not-now" hidden data-href="04-intent.html"></span>
<div class="buttons" style="top: 1100px"><div class="button lit" data-href="04-intent.html"><span data-af="small">next</span></div></div>`);

page("03b-still-starting.html", "Screen 3, slow: still starting", "3 · slow step: what Allen says when a step passes its budget (here 5 s)", "sky", `
${says('Still starting. The disk is slow to answer.', 640, 0)}
<div class="center rise" style="position:absolute;left:0;right:0;top:700px"><span class="dot"></span></div>`);

page("04b-something-else.html", "Screen 4: something else", "4 · something else: Allen says what he understood", "sky", `
${says("You want a quiet computer for writing. I'll keep things simple and save power.", 640, 0, ' data-wrap="900"')}
${btns(760, [["That's right", "05-getting-to-know.html", true], ["Not quite", "04-intent.html"]])}
${bar('<span class="typed" data-af="body">I mostly write, and I like it quiet</span><span class="caret"></span>', true)}`);

page("05-getting-to-know.html", "Screen 5: getting to know this computer", "5 · getting to know this computer (example values; each line eases in as soon as it is known)", "sky", `
${says("Getting to know this computer.", 590, 0)}
${found(652, [["Screen", "2560 by 1440, 144 times a second"], ["Graphics", "NVIDIA GeForce RTX 4070, 12 GB of its own memory"], ["Processor", "AMD Ryzen 7 7800X3D, 8 cores"], ["Memory", "32 GB"], ["Storage", "1 TB fast drive, 2 TB drive"], ["Network", "wired, connected"], ["Sound", "speakers and a microphone"], ["Keyboard and mouse", "working"]])}
<div class="rise" style="position:absolute;left:880px;top:928px"><span class="dot"></span></div>
<span class="not-now" hidden data-href="05b-what-allen-found.html"></span>
<div class="buttons" style="top: 1100px"><div class="button lit" data-href="05b-what-allen-found.html"><span data-af="small">next</span></div></div>`);

page("06-consent.html", "Screen 6: the one consent", "6 · the one consent (the list holds only what changes the machine)", "sky", `
${says("Here's what I'd like to set up:", 560, 0)}
<div class="panel rise" style="left: 640px; top: 616px; width: 640px">
  <p data-af="body" style="margin: 4px 0 14px">Use my own drivers for the graphics, network and storage</p>
  <p data-af="body" style="margin: 0 0 14px">Set your screen to 144 times a second</p>
  <p data-af="body" style="margin: 0 0 14px">Run the AI on your graphics card</p>
  <p data-af="body" style="margin: 0 0 18px">Build drivers for the 2 parts I can't drive yet</p>
  <p data-af="small" style="color: var(--allen-colors-hint); margin: 0">I won't touch your other drives, and you can undo any of this.</p>
</div>
${btns(880, [["Set it up", "07a-games.html", true], ["Show me each one", "06b-show-each.html"], ["Not now", "09-ready.html"]])}`);

page("06b-show-each.html", "Screen 6: one at a time", "6 · show me each one (1 of 4)", "sky", `
<p class="center rise" style="position:absolute;left:0;right:0;top:600px;color:var(--allen-colors-hint)" data-af="small">1 of 4</p>
${says("Use my own drivers for the graphics, network and storage?", 640, 120, ' data-wrap="900"')}
${btns(740, [["Yes", "06b-show-each.html", true], ["Skip", "06b-show-each.html"]])}`);

page("07a-games.html", "Screen 7a: making games smoother", "7a · play games smoothly (example values; every number measured on this machine)", "sky", `
${says("Making games smoother.", 590, 0)}
${found(652, [["Screen", "set to 144 times a second", ""], ["Graphics", "using my driver", ""], ["Processor", "keeping the fast cores free for the game", ""], ["Measured here", "118 frames a second in my test scene (before: 97)", ""]])}
<p class="center rise" style="position:absolute;left:0;right:0;top:820px;color:var(--allen-colors-hint)" data-af="body">I can't tune this graphics card's clock speed yet. I'm building a driver that can.</p>
${btns(900, [["Next", "07b-ai-fit.html", true]])}`);

page("07b-ai-fit.html", "Screen 7b: finding the best way to run an AI", "7b · run an AI model here (fit from the model's measured size, not a table)", "sky", `
${says("Finding the best way to run an AI here.", 590, 0)}
<p class="center rise" style="position:absolute;left:460px;width:1000px;top:660px;color:var(--allen-colors-primary)" data-af="body" data-wrap="1000">Your graphics card has 12 GB of its own memory. Models up to about 14 billion parameters fit on it. The 30 billion one fits if I share it with main memory, but it runs slower.</p>
${btns(800, [["Next", "07c-one-found.html", true]])}`);

page("07c-one-found.html", "Screen 7c: one model found", "7c · which AI: exactly one found (lit choice is taken after 10 s, and Allen says so)", "sky", `
${says("I found Qwen 30B on this computer. I'll use it.", 640, 0)}
${btns(720, [["Use it", "09-ready.html", true], ["Choose another", "07c-several.html"]])}
<p class="center rise" style="position:absolute;left:0;right:0;top:796px;color:var(--allen-colors-faint)" data-af="small">I'll go with this in 10 seconds</p>`);

page("07c-several.html", "Screen 7c: several found", "7c · which AI: several found (best fit first and lit)", "sky", `
${says("I found three. The first one fits this computer best.", 600, 0)}
${choices(660, [["Qwen 30B", "Runs on your graphics card", "09-ready.html", true], ["Llama 8B", "Runs on your graphics card, quicker, shorter answers", "09-ready.html"], ["Qwen 27B on the Spark", "On the Spark next door, fast", "09-ready.html"], ["Connect my own", "Claude, ChatGPT or another service", "07c-connect.html"]])}`);

page("07c-none.html", "Screen 7c: none found", "7c · which AI: none found", "sky", `
${says("I didn't find an AI model on this computer.", 620, 0)}
${choices(690, [["Get one for me", "The 30B model, an 18 GB download", "09-ready.html", true], ["Connect my own", "Claude, ChatGPT or another service", "07c-connect.html"]])}
<p class="not-now center rise" style="top: 820px" data-af="small" data-href="09-ready.html">Not now</p>`);

page("07c-connect.html", "Screen 7c: connect my own", "7c · connect my own: which service", "sky", `
${says("Which service?", 620, 0)}
${btns(690, [["Claude", "07c-key.html", true], ["ChatGPT", "07c-key.html"], ["Another service", "07c-key.html"]])}`);

page("07c-key.html", "Screen 7c: paste a key", "7c · paste your key (checking it with one short request)", "sky", `
${says("Paste your key for Claude. I'll keep it sealed on this computer and won't show it again.", 620, 0, ' data-wrap="900"')}
<p class="center rise" style="position:absolute;left:0;right:0;top:860px;color:var(--allen-colors-hint)" data-af="small">Checking the key <span class="dot" style="margin-left:10px"></span></p>
${bar('<span class="typed" data-af="body">********************************</span><span class="caret"></span>', true)}`);

page("07c-key-failed.html", "Screen 7c: the key did not work", "7c · the key did not work", "sky", `
${says("That key didn't work. Claude said the key isn't valid. Try again?", 620, 0, ' data-wrap="900"')}
${btns(720, [["Try again", "07c-key.html", true], ["Use a different service", "07c-connect.html"]])}`);

page("08-hardware.html", "Screen 8: setting up your hardware", "8 · setting up your hardware (six state words, nothing else)", "sky", `
${says("Setting up your hardware.", 590, 0)}
${found(652, [["Graphics (AMD Radeon Pro 580)", "ready, my driver", "state"], ["Network (Broadcom)", "ready, my driver", "state"], ["Wi-Fi (Broadcom BCM4360)", "building a driver for it", "state"], ["Webcam", "ready, the maker's driver", "state"]])}
<p class="center rise" style="position:absolute;left:0;right:0;top:820px;color:var(--allen-colors-hint)" data-af="small">Building a driver can take a while. You can keep using the computer.</p>
${bar('<span data-af="body">Speak to Allen</span>')}`);

page("08b-turn-on.html", "Screen 8: the second consent", "8 · the second consent (only when a built driver is ready)", "sky", `
${says("I built a driver for your Wi-Fi and tested it without touching the real one. Turn it on?", 620, 0, ' data-wrap="900"')}
${btns(740, [["Turn it on", "09-ready.html", true], ["Not now", "09-ready.html"]])}`);

page("08c-put-back.html", "Screen 8: it failed, the old one is back", "8 · a built driver failed on the real device: Allen undoes it by himself", "sky", `
${says("That didn't work on the real Wi-Fi. I've put the old driver back, and I'll keep working on it.", 640, 0, ' data-wrap="900"')}
${bar('<span data-af="body">Speak to Allen</span>')}`);

page("09-ready.html", "Screen 9: Allen is ready", "9 · Allen is ready", "sky", `
${says("Allen is ready.", 600, 0)}
<p class="center rise" style="position:absolute;left:460px;width:1000px;top:664px;color:var(--allen-colors-hint)" data-af="body" data-wrap="1000">Games will use your screen at 144 times a second. Your AI is Qwen 30B, on your graphics card. I'm still building a driver for your Wi-Fi and I'll tell you when it's ready.</p>
${says("Ask me anything, or tell me what you'd like to do.", 800, 900)}
${bar('<span data-af="body">Speak to Allen</span><span class="caret"></span>', true)}`);
