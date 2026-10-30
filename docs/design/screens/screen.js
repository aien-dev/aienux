// screen.js -- shared behaviour for the first-run mockups: fit the 1920 x 1080 board to the window, draw the
// words in Allen's stroke font, stagger entrances, and the keyboard path (arrows move the lit choice, Enter
// or Space picks it, Escape is "Not now", any letter goes to the bar). Links between screens make the flow
// clickable. ?clean=1 hides the screen's name tag for screenshots.
"use strict";
(function () {
  const board = document.querySelector(".board");
  const params = new URLSearchParams(location.search);
  if (params.get("clean") === "1") document.body.classList.add("clean");
  function fit() {
    const k = Math.min(innerWidth / 1920, innerHeight / 1080);
    board.style.transform = `translate(-50%, -50%) scale(${k})`;
    board.style.transformOrigin = "50% 50%";
  }
  addEventListener("resize", fit);
  fit();
  afRender(board);

  // entrances: each .rise waits its turn (data-at ms, else 90 ms after the one before)
  let at = 0;
  for (const el of board.querySelectorAll(".rise")) {
    at = el.dataset.at ? Number(el.dataset.at) : at + 90;
    el.style.animationDelay = `${at}ms`;
  }

  // the lit choice
  const groups = [...board.querySelectorAll(".choices, .buttons")];
  for (const g of groups) {
    const items = [...g.querySelectorAll(".choice, .button")];
    items.forEach((it) => { it.tabIndex = 0; it.setAttribute("role", "button");
      it.addEventListener("mouseenter", () => light(g, it)); it.addEventListener("focus", () => light(g, it));
      it.addEventListener("click", () => go(it)); });
  }
  function light(g, it) { for (const o of g.querySelectorAll(".lit")) o.classList.remove("lit"); it.classList.add("lit"); }
  function go(it) { if (it && it.dataset.href) location.href = it.dataset.href + (params.get("clean") ? "?clean=1" : ""); }
  const notNow = board.querySelector(".not-now");
  if (notNow) { notNow.tabIndex = 0; notNow.setAttribute("role", "button"); notNow.addEventListener("click", () => go(notNow)); }
  addEventListener("keydown", (e) => {
    const g = groups[0];
    const items = g ? [...g.querySelectorAll(".choice, .button")] : [];
    const i = items.findIndex((x) => x.classList.contains("lit"));
    if (["ArrowDown", "ArrowRight"].includes(e.key) && items.length) { light(g, items[(i + 1) % items.length]); e.preventDefault(); }
    else if (["ArrowUp", "ArrowLeft"].includes(e.key) && items.length) { light(g, items[(i - 1 + items.length) % items.length]); e.preventDefault(); }
    else if ((e.key === "Enter" || e.key === " ") && items[i]) { go(items[i]); e.preventDefault(); }
    else if (e.key === "Escape" && notNow) go(notNow);
    else if (e.key.length === 1 && board.querySelector(".bar")) board.querySelector(".bar").classList.add("focus");
  });
  window.__screen_ready = true;
})();
