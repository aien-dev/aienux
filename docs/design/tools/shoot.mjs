// shoot.mjs -- drives a headless Chrome over its DevTools port, with no packages: loads a page at a size,
// optionally waits until a page expression is true, prints that expression's value, and saves a screenshot.
// Used for the boot reference's frame-time receipts and the screen mockups' screenshots. Dev tool only:
// nothing in AienOS runs it.
//
//   node docs/design/tools/shoot.mjs URL [--size 1920x1080] [--out shot.png] [--wait EXPR] [--result EXPR]
//        [--timeout-ms 60000] [--chrome PATH] [--gpu]
//
// Exit 0 on success, 1 if the page never met --wait, 2 on a usage or launch error.

import { spawn } from "node:child_process";
import { mkdtempSync, writeFileSync, existsSync, readdirSync } from "node:fs";
import { tmpdir, homedir } from "node:os";
import { join } from "node:path";

const args = process.argv.slice(2);
const opt = (name, dflt) => {
  const i = args.indexOf(name);
  return i >= 0 ? args[i + 1] : dflt;
};
const url = args[0];
if (!url || url.startsWith("--")) {
  console.error("usage: shoot.mjs URL [--size WxH] [--out FILE] [--wait EXPR] [--result EXPR] [--timeout-ms N]");
  process.exit(2);
}
const [width, height] = opt("--size", "1920x1080").split("x").map(Number);
const out = opt("--out", null);
const waitExpr = opt("--wait", null);
const resultExpr = opt("--result", null);
const timeoutMs = Number(opt("--timeout-ms", "60000"));

function findChrome() {
  const given = opt("--chrome", null);
  if (given) return given;
  const shell = join(homedir(), ".cache/ms-playwright");
  if (existsSync(shell)) {
    for (const d of readdirSync(shell).filter((d) => d.startsWith("chromium_headless_shell"))) {
      for (const sub of readdirSync(join(shell, d))) {
        const p = join(shell, d, sub, "chrome-headless-shell");
        if (existsSync(p)) return p;
      }
    }
  }
  return "google-chrome";
}

const profile = mkdtempSync(join(tmpdir(), "shoot-"));
const flags = [
  "--no-sandbox",
  "--no-first-run",
  "--hide-scrollbars",
  "--autoplay-policy=no-user-gesture-required",
  `--user-data-dir=${profile}`,
  "--remote-debugging-port=0",
  `--window-size=${width},${height}`,
];
if (args.includes("--gpu")) flags.push("--enable-gpu", "--use-angle=gl");
// extra Chrome flags, space separated, e.g. SHOOT_FLAGS="--use-angle=vulkan"
flags.push(...(process.env.SHOOT_FLAGS || "").split(" ").filter(Boolean));
const chrome = spawn(findChrome(), [...flags, "about:blank"], { stdio: ["ignore", "ignore", "pipe"] });

const port = await new Promise((resolve, reject) => {
  let buf = "";
  const timer = setTimeout(() => reject(new Error("chrome did not open its DevTools port")), 20000);
  chrome.stderr.on("data", (d) => {
    buf += d;
    const m = buf.match(/DevTools listening on ws:\/\/[^:]+:(\d+)\//);
    if (m) {
      clearTimeout(timer);
      resolve(Number(m[1]));
    }
  });
  chrome.on("exit", () => reject(new Error("chrome exited before opening its DevTools port")));
}).catch((e) => {
  console.error(`shoot: ${e.message}`);
  process.exit(2);
});

const targets = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
const page = targets.find((t) => t.type === "page");
const ws = new WebSocket(page.webSocketDebuggerUrl);
await new Promise((r) => ws.addEventListener("open", r, { once: true }));
let nextId = 1;
const pending = new Map();
ws.addEventListener("message", (ev) => {
  const msg = JSON.parse(ev.data);
  if (msg.id && pending.has(msg.id)) {
    pending.get(msg.id)(msg);
    pending.delete(msg.id);
  }
});
const send = (method, params = {}) =>
  new Promise((resolve, reject) => {
    const id = nextId++;
    pending.set(id, (m) => (m.error ? reject(new Error(`${method}: ${m.error.message}`)) : resolve(m.result)));
    ws.send(JSON.stringify({ id, method, params }));
  });
const evaluate = async (expr) =>
  (await send("Runtime.evaluate", { expression: expr, returnByValue: true, awaitPromise: true })).result.value;

let code = 0;
try {
  await send("Emulation.setDeviceMetricsOverride", { width, height, deviceScaleFactor: 1, mobile: width < 600 });
  await send("Page.enable");
  await send("Page.navigate", { url });
  const start = Date.now();
  // wait for the document, then for the page's own condition
  while ((await evaluate("document.readyState")) !== "complete") await new Promise((r) => setTimeout(r, 50));
  if (waitExpr) {
    while (!(await evaluate(`Boolean(${waitExpr})`))) {
      if (Date.now() - start > timeoutMs) {
        console.error(`shoot: ${waitExpr} still false after ${timeoutMs} ms`);
        code = 1;
        break;
      }
      await new Promise((r) => setTimeout(r, 100));
    }
  } else {
    await new Promise((r) => setTimeout(r, 400)); // let fonts and the first frame settle
  }
  if (resultExpr) {
    const v = await evaluate(resultExpr);
    console.log(typeof v === "string" ? v : JSON.stringify(v, null, 2));
  }
  if (out) {
    const shot = await send("Page.captureScreenshot", { format: "png" });
    writeFileSync(out, Buffer.from(shot.data, "base64"));
    console.error(`shoot: wrote ${out} (${width}x${height})`);
  }
} catch (e) {
  console.error(`shoot: ${e.message}`);
  code = 2;
}
ws.close();
chrome.kill();
process.exit(code);
