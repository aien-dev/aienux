# Allen through the model (2026-10-19)

Goal (Drake, 2026-10-19): every line typed to Allen on the real iMac is answered by our own model server on the
Spark (aien-serve-http, 127.0.0.1:18091, qwen3.8-27b-fp8), in Allen's voice, with his memory in the prompt, and every
exchange is kept as his learning record. Allen's own canned lines stay only for goodbye, the naming exchange (the very
first meeting), and "I lost my thread" when the model cannot be reached.

Plain summary: it works end to end in the emulator (QEMU) with the real 27B answering. It has NOT run on the real iMac.
The part that carries the words over the iMac's network chip (receive side of the Broadcom driver) has never run on that
hardware, so it is the most likely thing to need a fix on the first boot.

## Disk state (worktree and branch)
- Worktree `~/workspace/aienos-allen-model`, branch `feat/allen-through-the-model`, stacked on feat/allen-presence 3f48a88
  (which is itself not in main yet). Pushed to origin with an explicit refspec. Head: see the last line of this file's
  "Commits" list. Not tested on the real iMac.
- Siblings untouched: no edits under native/kernel/ui/*; in kmain_x86.c only the courier wiring (small hunks).

## Commits (newest last; every one pushed)
1. 8e74e25 checkpoint of the earlier agent's unfinished work (it died at the account limit; none of it had been built as a whole)
2. 364b5dc courier fixes + host test over UDP + model-first greeting
3. 7c82421 keep the kernel waiting while the model is busy (WAIT every 2 s), failed answer is Allen's own line, relay launcher
4. 61016a6 UDP host test can wait longer for a busy model
5. (last) crumbs recompiled with the pinned tool d0f0b95, evidence folder, no code change

## What was built
- Kernel courier over Ethernet: `native/kernel/x86/courier_udp_x86.c` + `courier_udp.h`. HELLO/HI, job in parts of 1000 bytes
  with ACK and resend every 2 s, answer lines with GOT (resent by the relay every 300 ms until heard), 30 s of silence gives
  up with Allen's own line. Broadcast frames, source 0.0.0.0, UDP port 47033 (the same shape as the lab report, which is
  known to reach the Spark). Lab image only (`make imac-kernel CK_X86_REPORT=1`); a lab address can be set with
  `CK_COURIER_PEER=192.168.1.108`. The serial courier stays for QEMU.
- Chip receive: `ck_x86_tg3_recv` in tg3_x86.c (new, lab image only). NEVER RUN ON REAL HARDWARE.
- Relay: `crates/allen-courier` (std only). Serial wire for QEMU, UDP wire for the iMac. System prompt = Allen's rules (from
  docs/ALLEN_PRESENCE_RESEARCH.md) + the instruction the kernel wrote + memory the kernel sends (his name, the person's,
  facts, the newest 6 notes). One request at a time; a busy server (503) is asked again after 0.25, 0.5, then 1 s (honouring
  Retry-After) for up to 900 s (`--busy-wait`); while no word has come yet it sends WAIT every 2 s so the kernel keeps
  waiting. Answers stream back in parts so the ink settles into words. Every exchange is appended to
  `~/.local/state/aienos/allen/conversations/<date>.jsonl` (time, wire, run, ask, full system prompt, memory, reply, parts,
  status, waits, ms first word, ms whole). THIS IS THE DATA PIPELINE, NOT WEIGHT TRAINING.
- Kernel policy: model first. `allen_mind.far` is set when a courier has said hello; then only goodbye and the very first
  meeting's naming/greeting are answered locally, everything else (including a later "hello allen") goes to the model; what
  the person teaches ("remember ...") is still stored quietly. Offline fallback (the old canned table) is used only when no
  courier has said HI. The window shows the thinking ink while a job is out.
- Learning on the iMac: after each answered exchange Allen writes a note ("asked: ... / said: ...", bounded) to his memory
  on disk, prints `allen_memory: note N (the exchange) saved|kept until power off in X ms`, and the next job carries the
  newest 6 notes. An exchange that got no answer is not noted.
- Relay launcher: `scripts/allen_courier_up.sh`. A plain nohup process, NOT a systemd unit, on purpose: it opens a UDP port on
  the LAN that anyone there can talk to (no password yet, and it spends the shared model), so it should run only while Allen
  is being tried; stop with `pkill -f 'allen-courier.*--udp 47033'`.

## Bugs found and fixed by actually running it (the earlier agent had never built it)
1. Relay did not compile (a closure held the wire while it said WAIT).
2. Kernel courier read the message kind at offset 33 instead of 39, so it would never have heard HI, ACK or LINE.
3. Kernel courier used `clock | 1` as a "not zero" flag, which can put a time one tick ahead and wrap `now - then`.
4. Relay resent answer lines with an empty run id; the kernel ignores those.
5. A shared model server held the request for minutes with no word back, so the kernel gave the job up after 30 s: the relay
   now sends WAIT meanwhile.
6. Makefile defined CK_COURIER_PEER_STR twice.
7. A failed answer used to be saved as a note ("said: the far side had nothing to say"); now it is Allen's own line and no note.

## Tests and numbers (all real runs; evidence in `evidence/allen_through_the_model_2026-10-19/`)
Head 61016a6 for code (later commits add only crumbs and evidence). Spark, rustc 1.98.1, gcc 13.3.0.
- `cargo fmt --check`, `cargo clippy --all-targets -D warnings`, `cargo test -p allen-courier`: clean, 14 tests pass.
- `scripts/ck_allen_memory_test.sh`: PASS (189 checks), unchanged. `scripts/ck_allen_ui_test.sh`: PASS (91 checks), unchanged.
- `scripts/ck_courier_udp_test.sh` (new): the kernel's own courier code against the real relay over UDP on the host, with a
  small stand-in model so it is exact. PASS in five runs: clean wire (first part 251 ms, whole 797 ms), losing about 1 frame in
  3 each way (first 629 ms, whole 1461 ms, 3 lost each way), losing about 1 in 2 (the job itself was lost and sent again,
  whole 3180 ms), model answering busy twice (relay waited, whole 2796 ms), model silent for 4 s (relay sent 2 WAITs, whole
  4792 ms). Output: host-tests-and-builds-at-61016a6.txt.
- `make imac-kernel` (default image) builds, IMACKERN.EFI 278528 bytes (sha256 baeac8cc...), and the build's own check that the
  default image has no lab-only symbol passes. `make imac-kernel CK_X86_REPORT=1` builds, IMACKERN-LAB.EFI 294912 bytes
  (sha256 3bd79116...), commit stamp 61016a6. The courier over Ethernet is in the LAB image only.
- QEMU end to end, `scripts/qemu_ck_allen_courier_test.sh`: PASS, 22 of 22 checks, with the REAL 27B at 127.0.0.1:18091
  through the relay (serial wire; QEMU has no Broadcom chip, so the Ethernet wire cannot run in QEMU). Typed: "hello allen"
  (first meeting: Allen answers himself, "Hello Humanity."), "write a haiku about rain" (job 1), "what did i just ask you"
  (job 2), "goodbye". Answers: "Soft rain taps the glass. Grey clouds drift across the sky. The world breathes slow." and
  "You asked me to write a haiku about rain." (the second used the note of the first exchange). Both exchanges are in the
  learning log with the first note inside job 2's memory. Screenshot `qemu-allen-window.png` shows the answer and "2 notes, 1 link".
  Round-trip, kernel clock: first word after 246.4 s and whole answer after 248.4 s (job 1); 112.9 s and 113.2 s (job 2).
  HONEST READING: almost all of that is waiting. The shared server answered 503 or held the request while other agents' jobs
  held the GPU (relay waits 96 and 85, server log "lock wait 147485 ms" and "lock wait 24147 ms"). The model's own work was
  prefill 1538 ms + decode 3227 ms (21 tokens, 6.5 tok/s) and prefill 2066 ms + decode 1854 ms (11 tokens, 5.9 tok/s) on that
  busy GPU, about 4.8 s and 3.9 s. Two earlier runs failed for the same reason before the relay learned to wait (one at the
  relay's old 120 s limit, one at the kernel's 30 s silence rule); they are fixed, not hidden.
- Memory write cost in QEMU: `allen_memory: note 1 (the exchange) kept until power off in 0 ms`, note 2 in 4 ms. These did NOT touch a
  disk (the QEMU image has no AIENOS partition), so they say nothing about disk cost. On the iMac the line will say `saved in N ms`.
- Real model over the UDP courier on the host: NOT RUN. I tried (`ALLEN_MODEL=127.0.0.1:18091 scripts/ck_courier_udp_test.sh`, output
  `host-udp-test-real-model-attempt.txt`): the five stand-in runs passed again, and the real run failed at once because the model server
  on 18091 was no longer there. Its log shows it went down and could not load again at 14:41 ("needs 41305 MiB plus 8192 MiB kept
  free, MemAvailable 37521 MiB (another model may be loaded); refusing"); two other 27B servers (ports 18191 and 18291, started by other
  jobs) hold the memory, and the keepalive script keeps retrying. I did not touch it (the rule is never to restart that server).
  The real 27B did answer through this relay earlier (the QEMU run above, 14:23 to 14:30), so the relay and the model work together; what is
  missing is only the UDP framing against the real model, which is the same relay code path with a different wire.

## NOT tested
- The real iMac. Nothing on this branch has run there.
- The Broadcom receive code (ck_x86_tg3_recv) and the whole Ethernet courier on hardware.
- A disk write of the exchange notes (QEMU has no AIENOS partition; the code path is the existing note writer).
- The relay's UDP path with the real kernel binary (QEMU cannot do it); only the kernel's own courier code on the host.
- Whether the Spark's firewall passes broadcast UDP 47033 in (the lab report's 47031 does, so it should).

## Drake's instruction (one line, then what to look for)
Shipping is ONE bundled image with the sibling branches (one boot per bundle), so nothing was pushed to the netboot spool.
When the bundle is built from the lab build (`make imac-kernel CK_X86_REPORT=1`):
1. On the Spark: `bash ~/workspace/aienos-allen-model/scripts/allen_courier_up.sh` (starts the relay; stop it later with
   `pkill -f 'allen-courier.*--udp 47033'`).
2. Stage the bundle with `scripts/ck_imac_netboot_push.sh <path>/IMACKERN-LAB.EFI` (back up next.job first), press the iMac's power button once, type
   a line to Allen (anything but goodbye).
Report lines to look for (in `~/.local/state/aienos/imac-netboot/reports/`): `courier: Ethernet to the far side on UDP port 47033 ...`,
`courier: ready, the far side said hello`, `screen: Allen asked the far side (N characters in, M in the job)`,
`screen: the far side began job 1 after N ms`, `screen: the far side answered job 1 (N characters out, P parts, T ms)`,
`allen_memory: note N (the exchange) saved in N ms`. On the Spark: `~/.local/state/aienos/allen/relay.log`
("courier: udp: hello from a kernel, run ...") and today's file in `~/.local/state/aienos/allen/conversations/`.
If "ready, the far side said hello" never appears, the likely cause is the chip receive path; the same boot's `net_tg3` and `courier:` report lines say which side was silent.
