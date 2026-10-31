# B7B-PHYSICAL-BOOT-PREP: the attended B7b hardware boot, on paper only

Status: PREPARATION DOCUMENT (updated 2026-10-28). **Physical NOT_RUN. Nothing here authorizes staging or booting.**
Host/QEMU qualification and candidate-only builds do not qualify Spark hardware. The fixture-provisioned full
candidate is NOT boot-admissible; no approved Spark owner material is available. This lane performs no ESP or media writes.

Owner issue: aien-dev/aienux#286 (native GB10, cut B7b). Authority: Drake's ruling of 2026-10-10 (aien-architecture#190,
item 5) authorizes B7b *preparation*, not a boot; the boot needs "a first operator-confirmed, two-hour attended window
after the recovery media, rollback, trust and preflight checks pass" (`docs/GB10_IORT_DECODE.md` section 10).
Related, read first: `docs/native/PREFLIGHT-AND-RECOVERY.md`, `docs/NATIVE_BOOT_ONE_TIME.md`,
`docs/NATIVE_BOOT_ROLLBACK_CONTRACT.md`, `docs/RECOVERY_MEDIA_MACHINE1.md`.

## 1. What B7b is, and what is already built

Goal: boot an explicitly admitted probe once, enable SMMUv3 B at the IORT-selected address (expected 0x13000000),
confine segment 15 requester 0x100 to one allocated page **plus that stream's accepted IORT RMR identity ranges**,
read IDR0/IDR1, release the stream back to abort, and collect SMMU events before the final report. The probe never
hands its page to the GPU and starts no GPU DMA. A full image also runs its ordinary device/security/Store/input
stages; "no GPU DMA" does not mean "no other device I/O". A core-only probe excludes those stages.

Built and tested without hardware (PR #302, squash `863bef83`, issue #286 comment):

- Probe image: `make [full] CK_B7B_PROBE=1` (output directory `target/native-kernel-b7b-probe`, allowed with
  `CK_HARDWARE_STAGING`); code in `native/kernel/core/smmu_svc.c` (`ck_b7b_probe`). Default images are unchanged and
  the link refuses a default image that carries the probe.
- QEMU gate: `scripts/qemu_ck_b7b_probe_test.sh`. Default mode: the Spark-target image refuses fail-closed on QEMU
  (route none, `rc=-3 nostream`, nothing confined). `--positive`: TEST-ONLY segment-0 image confines one page, reads
  IDR0/IDR1, unconfines. `--events` uses a disposable QEMU NVMe negative-DMA oracle and all-SID decoded readout.
  `--self-test` includes missing/wrong-SID/wrong-address event, overflow and pending negative controls plus flag refusals.
- Expected Spark output: `docs/GB10_IORT_DECODE.md` section 10, labelled *expected, not observed*.
- New opt-in build: `make [full] CK_B7B_PROBE=1 CK_B7B_EVENT_READOUT=1`, separate
  `target/native-kernel-b7b-events` output. `ck_b7b_event_readout` runs before the final report. It enumerates all
  instance slots and consumes at most one queue capacity per live instance; no unbounded wait on a producer.
  The host observer test covers all SIDs, full/wrapped queues, overflow acknowledgement, later-arriving events,
  and raw-word retention; the host DROP_EVENT mutant must fail that same test.

Built 2026-10-12 (native v1 package P02): the probe variant of the one-time staging script,
`scripts/stage_one_time_boot_ck_b7b.sh` (`--dry-run` / `--apply`, same shape as `_full.sh`). It builds the probe
hardware staging image twice and refuses on a digest mismatch, checks the image carries the segment-15 probe and no
TEST marker, and stages it as `\EFI\AIENOS\aienos-ck-b7b.efi` with a `STAGED-CK-B7B.TXT` record. Its dry run was
executed on the Spark on 2026-10-12 (read-only; it stopped on four preflight items, among them the recovery stick not
being inserted), and two builds of the image at commit `51c05da` with the labelled TEST-FIXTURE owner files (a build
check only, never a boot) gave one digest. This is historical preparation, not today's G7 or staging authority.

## 2. People and things required (all physical, all Drake's)

| need | detail |
|---|---|
| Drake present for the whole window | at the machine, display and keyboard attached; a fresh go-ahead for this boot, given in this window (a past approval does not carry over) |
| Window | two hours, written down in advance with start time; nothing else uses the machine in it |
| Recovery stick | existing `AIENOSRECOV`, current firmware **Boot0002**, PARTUUID `f227357c-1e80-4fd0-a55e-2efbef1dd25c`; serial `5D0344B4`; read-only for this lane; parent alone owns any newly authorized backup/reuse/restore |
| Camera and witnesses | camera on screen for whole boot; primary native xHCI/USB keyboard plus independent RTL8127 UDP boot-report witness on iMac/MacBook (neither may be silently dropped) |
| Witness | a second person (or Drake plus a time-stamped recording) who reads the screen aloud and keeps the clock |
| Notes | paper or a laptop for times and the exact words on screen |
| Prerequisite 0: owner provisioning | completed Drake-provided `CK_OWNER_PUBKEYS` + `CK_MACHINE_ID` ceremony before full staging; currently NOT DONE; never generate a substitute or reuse another machine's key |
| Candidate medium | separate blank medium preferred; Drake subsequently approved parent-owned backup then reuse of the recovery stick, conditional on verified backup and an executable recovery restore path; this lane never writes it |

The October 28 parent inspection found the existing recovery stick in the Spark, with shim/GRUB, recovery
kernel/initrd, `capture-attended.sh`, and `aienos-evidence/attended-recovery-machine1.txt`. The parent read that file:
2026-09-25T00:26:25Z, BootCurrent was this stick (then Boot0004), NVMe root/ESP mounted read-only,
`RECOVERY_BOOT_GATE: PASS`. This is historical physical recovery proof, not a recovery boot today. Recheck medium
identity before GO. Current Boot0002 and historical Boot0004 are the same PARTUUID; never rewrite it to match a number.

Later October 28 authorization correction, relayed by the parent: Drake approved backing up this stick and then
reusing it. **The parent owns every `/dev/sda` action**, including backup, verification, writing and restoration;
this lane remains read-only. Before reuse/GO, retain a byte-exact backup with digest, partition metadata and the
on-stick recovery evidence, and prove a restoration route on another available machine if Spark fails to boot.
A backup stored only on an unbootable Spark is not a usable recovery route. While the stick contains a candidate,
Boot0002 does not magically remain a recovery environment: restore the verified recovery bytes before using it as
recovery. Recheck its restored identity/hash and historical evidence before another boot window.

The full-image build parser (`native/kernel/tools/ck_owner_gen.c`, matching the existing native/m5 owner boundary)
accepts text lines `name hex`, blank lines and `#` comments. Public-key file: exactly
`owner_root_ed25519 <64 hex digits>`. Identity file: exactly `machine_id <64 hex digits>` and
`store_uuid <32 hex digits>`. Duplicate/unknown names, malformed lengths, all-equal placeholders and known TEST
values are refused. These are public identity inputs, not K_vol or private signing keys. Labelled
`native/kernel/tests/fixtures/owner/` inputs prove build mechanics only and MUST NOT be staged.

## 3. Recovery preflight: read-only checklist

Run on the Spark in Linux, as the normal user, before the window opens. **Every command below only reads or builds
into `target/`.** None needs sudo, none writes a boot variable, stages, mounts read-write or reboots. Stop at the first
failure.

| step | command | PASS looks like |
|---|---|---|
| P1 | `git -C <checkout> status --porcelain` | prints nothing (clean tree) |
| P2 | `git -C <checkout> rev-parse HEAD` | the commit the probe image is built from; write it down |
| P3 | `quietlock check` | clear (no other session holds the machine quiet) |
| P4 | `efibootmgr` (no arguments) | `Boot0002 AIENOSRECOV` matches the recovery PARTUUID; `BootCurrent: 0001`; `BootOrder` and no `BootNext` match the captured current Linux baseline (do not assume an old BootOrder) |
| P5 | `lsblk -f` and parent-approved read-only identification | exact recovery identity above and internal root/ESP match; if parent reuses recovery medium, verified backup and independent-machine restore route are recorded before any write |
| P6 | `bash scripts/verify_recovery_tools.sh` | `PASS` (static check of the recovery initrd; `NOT_RUN` is not a pass) |
| P7 | `bash scripts/qemu_verify_recovery_media.sh` | the recovery environment boots in QEMU with zero hard disks |
| P8 | `bash scripts/qemu_ck_rollback_test.sh` and `bash scripts/qemu_native_rollback_test.sh` | the rollback gates pass for this commit |
| P9 | `bash scripts/qemu_ck_b7b_probe_test.sh --self-test`, no flag, `--positive`, then `--events` | all corresponding verdicts PASS, exact real negative SID/address decoded, final queue readout before report |
| P10 | `bash scripts/ck_repro_build.sh full` (builds twice, compares); the probe staging image is built twice by the stage script itself (P12 shows the commands, `--apply` runs them and STOPs on a digest mismatch; `ck_repro_build.sh` does not build staging images, verified 2026-10-12 by reading it) | two equal digests for the default full image; the probe image digest is written down from the stage script output |
| P11 | `bash scripts/native_boot_preflight.sh --image <probe image .efi> --expect-sha256 <digest from P10>` | last line `NATIVE_PREFLIGHT: READY`; the `ASK` lines are Drake's and are answered in person |
| P12 | `AIENOS_OWNER_PUBKEYS=<file> AIENOS_MACHINE_ID=<file> bash scripts/stage_one_time_boot_ck_b7b.sh --dry-run` | prints every command, touches nothing, ends "Pre-flight clean"; Drake reads it |
| P13 | reconcile the stick's `aienos-evidence/attended-recovery-machine1.txt` with its exact identity and an earlier attended physical recovery boot | Drake confirms physical recovery proof; mere file presence, QEMU boot, or firmware entry is insufficient |

Secure Boot is recorded, never changed. As of 2026-10-01 it is OFF for development by owner decision
(`docs/native/PREFLIGHT-AND-RECOVERY.md` item 3); `native_boot_preflight.sh` records the state and stops if it is ON.
Whether the physical-boot block described in `docs/NATIVE_BOOT_ONE_TIME.md` section 1 (trust chain) still applies is
UNVERIFIED here and is part of Drake's go/no-go.

## 4. Go / no-go

**GO requires all of these, decided by Drake at the start of the window:**

- G1 P1 to P12 passed within the last 24 hours on the commit being staged, outputs saved.
- G2 P13: a recovery-stick evidence record exists and Drake saw the stick boot.
- G3 Drake, camera and witness are present; the window is open; no other job needs the machine.
- G4 The image digest printed by the stage script equals the digest from P10 (and later the digest the kernel prints
  on screen).
- G5 `BootNext` unset, `BootOrder` unchanged (P4), Linux is the running system.
- G6 Everyone can state the abort rule (section 7) from memory.
- G7 The media procedure `docs/campaigns/SPARK-BOOT-INPUT-20261028.md` is integrated and reviewed with Drake.
  Either a separate identified medium is supplied, or the parent has completed the specifically authorized
  recovery-backup/reuse prerequisite above. Only the parent acts on that medium. The legacy ESP staging script's
  `--apply` still requires separate explicit ESP authorization and is not the current route.
  Incomplete owner ceremony or composed keyboard/network witness image keeps this campaign NO-GO.
- G8 Drake says "go" out loud in the window, and the witness writes the time.
- G9 The exact admitted candidate includes both native confined xHCI/USB keyboard and RTL8127 UDP report witness.
  The standalone core-only SMMU artifact does not include either and cannot satisfy this combined physical campaign.

**NO-GO (any one):** any preflight STOP line; a dirty tree; the stick missing; an unexpected `BootNext`; a digest
mismatch; anyone unsure; less than the full window left; an open question about the SMMU B side effect (section 8)
that Drake wants answered first.

## 5. Boot steps (only after GO; Drake or the witness reads each step aloud)

1. Start camera, witness receiver and notes; write exact source revision, candidate digest and medium identity.
   Follow section 7's separately authorized incumbent stop procedure. Do not proceed while 18091 remains live or
   without a verified restoration recipe.
2. Capture the before state with `sudo scripts/verify_native_rollback.sh --capture-pre evidence/pre_boot_capture.json`
   only with the window's explicit authorization; preserve existing BootOrder/BootNext/Secure Boot and ESP.
3. Use the separately reviewed medium-identification/image-writing procedure in
   `docs/campaigns/SPARK-BOOT-INPUT-20261028.md`. Parent alone may write the exact authorized candidate medium,
   including recovery-stick reuse only after the verified backup/independent restore prerequisite. No internal ESP write.
   Verify the exact candidate hash after packaging. Full TEST-FIXTURE image is never eligible.
4. Drake selects that candidate through the firmware's attended one-time boot menu and starts the clock.
   Do not use ALLEN.EFI or Boot0000. No automatic reboot or persistent boot-order edit is part of this route.
5. Expected SMMU lines (expected, not observed): `kernel: alive`; `b7b_probe: start segment=15 rid=0x100 ...`;
   IORT route instance/base/SID; every `b7b_rmr:` interval; one-page confinement; IDR0/IDR1; successful unconfine;
   `AIENOS_B7B_PROBE: CONFINED_AND_RELEASED`. Photograph **all** SIDs in every `b7b_event:` line, then each live
   queue's `events`, `overflow`, `pending`, and all `not-live`/`failed` slots between final begin/end.
   GB10 and HDA0 records must be attributed by the actual IORT stream map, not guessed from neighboring IDs.
6. Confirm readout precedes `report_kind: final`; record native keyboard evidence and independent UDP witness.
   A queue with overflow/pending or a missing witness is incomplete, never a physical PASS. The console hold
   (`CK_FB_HOLD_S=120`) then reset is expected, not physically verified; no further event observation is promised
   during the hold. If the firmware does not return to Linux, use section 7 recovery.
7. Back in Linux, capture post state and run `verify_native_rollback.sh --pre ... --post ... --verify` on the exact
   saved pair. Restore and verify the incumbent 18091 answer service by section 7. Preserve the candidate/recovery
   media and all evidence; no second attempt without fresh authority.

## 6. Observation points

| when | what to read | meaning |
|---|---|---|
| after the reboot, before any AIENOS text | firmware messages | `Secure Boot Violation` means the image was refused: a STOP condition, do not bypass; the machine falls back to Linux |
| `kernel: alive` | banner, memory map | the probe image started; the probe runs only after this line |
| `route ...` line | instance, base, stream | must read instance 1, base 0x13000000, stream 0x100. Anything else is a finding about the IORT decode (`GB10_IORT_DECODE.md` section 3), not a reason to retry |
| IDR0/IDR1 line | two register values | record verbatim; SIDSIZE must cover 0x100 |
| summary line | `CONFINED_AND_RELEASED`, `REFUSED` or `FAIL` | any other summary is a finding: `REFUSED` names a reason (`nostream`, `failed`), `FAIL` means unconfine failed or no live instance matched |
| after the summary | any extra line, panic, freeze, blank screen | write the time and the exact words |

## 7. Abort and rollback

- **Abort rule.** If the screen shows a panic, shows nothing new for 60 seconds, or anyone says "stop": touch nothing
  until the 60 seconds are over, then hold the chassis power button until the machine is off, wait ten seconds, press
  it again. The one-time menu choice must not change captured BootOrder; normal fallback is Linux (`0001`).
- **If Linux does not come up:** use the firmware one-time menu for `AIENOSRECOV` (current historical entry
  **Boot0002**, verify recovery PARTUUID), but only if its recovery contents are present. If the parent reused it,
  the parent first restores its verified recovery backup using the pre-arranged independent-machine route.
  Then mount internal root/ESP **read-only** and run the collector in `docs/RECOVERY_MEDIA_MACHINE1.md`.
- **Repair** (removing `\EFI\AIENOS`, touching `BootOrder`) needs a new, separate go-ahead from Drake at that moment.
- **Never**: change Secure Boot, change firmware settings, run `--apply` a second time, retry a failed probe in the
  same window without a fresh go, or "fix" anything on the machine during the window.
- Rehearsed so far only in QEMU (`scripts/qemu_ck_rollback_test.sh`, gate M0_ROLLBACK); not on the Spark.

### Resident model stop and exact restore (operator-only, separate approval)

Stopping the service is NOT authorized by this document. Before opening a physical boot window the parent must
obtain Drake's explicit approval to stop/restore the live 18091 model and capture its *current* command, executable
hash and non-secret runtime settings. On October 28 the read-only snapshot was PID 16911,
`bin-19173f9e/aien-serve-http`, 8 slots, context 49152, stroke 1. PIDs are not stable; never signal that recorded PID
without identifying the current listener and executable again. Do not dump the entire process environment into
receipts: it may contain credentials.

1. Save `curl -fsS http://127.0.0.1:18091/v1/models`, `ss -ltnp 'sport = :18091'`,
   `readlink /proc/<confirmed-pid>/exe`, and `sha256sum /proc/<confirmed-pid>/exe` in the private window evidence.
   Confirm with the coordinator that no other process owns the GPU window. Record any supervising unit and its
   restart policy; if supervised, use that unit's approved stop/restore instead of fighting an automatic restart.
2. After the exact stop authorization and quietlock/spark-window approval: `kill -TERM <confirmed-pid>`.
   Wait for process exit; verify the listener is gone and no model GPU process remains using read-only telemetry.
   If shutdown does not finish, STOP: no unapproved SIGKILL, reset, driver unload or VFIO operation.
3. Proceed with the approved one-time boot. If staging/preflight fails before reboot, restore immediately by the
   same recipe; do not leave the incumbent stopped while investigating.
4. After Linux returns and rollback checks pass, the coordinator restores the captured incumbent. For the recorded
   October 28 baseline, the exact non-default settings are:

   ```sh
   Q36_BATCH_BIN=bin-19173f9e Q36_BATCH_SLOTS=8 Q36_MAX_CONTEXT=49152 \
     AIEN_Q36_STROKE=1 AIEN_SERVE_GPU_LOCK=/tmp/aien-serve-18091.lock \
     /home/drakestapleton/.local/state/aien-q36/start-35b-batch.sh
   ```

   Re-capture before the window: if the incumbent changed, use its newly recorded recipe, not this historical one.
   The launcher's default binary/stroke differ, so invoking it without these settings is not an exact restore.
5. Verify the restored executable hash, listener, `/v1/models`, and one real `/v1/chat/completions` answer with the
   captured model id. Record observed status/output without inventing expected wording. A model-list response alone
   does not prove answer service restoration. Close the operator window only after restoration or an explicit
   operator-directed recovery handoff.

Undo: restore only the captured incumbent settings and approved staging backup. For authorized recovery-stick
reuse, the parent alone restores and verifies its byte-exact recovery backup; this lane never writes the stick.
Never delete CORE.EFI, boot ALLEN.EFI/Boot0000, or change BootOrder/Secure Boot. Removing a newly created boot
entry or staged file is a distinct write requiring Drake's approval; preserve before/after receipts.

## 8. Known unknowns (carried from #302)

- UNKNOWN whether GB10 or HDA0 attempts DMA while no OS drives it. `CK_B7B_EVENT_READOUT=1` prints every event
  consumed by `ck_dma_faults` during stages and a final bounded drain of **every already-live** SMMU instance,
  regardless of SID. It does not enable unused SMMUs just to read them. Each record includes instance/base,
  SID, architectural type/name, word-2 address and all four raw words. Unknown types remain UNKNOWN with raw data.
  Each final queue summary reports overflow and whether records remain pending; an inactive instance says
  `not-live`, not "zero faults". Overflow/pending means incomplete observations. No-event output is not evidence
  that a device tried DMA, that a GPU path is confined, or that cache visibility is correct.
- This readout consumes/acknowledges events and is probe-only. It runs before the final report, **not continuously
  during the framebuffer hold**. Events arriving later can be missed. The architectural abort policy can suppress
  event reporting for some traffic; absence of a record must never be promoted to absence of traffic.
- Every accepted RMR range is printed separately when installed. The permitted set is the explicit window UNION
  those ranges while the stream translates; after release its STE aborts. An RMR address is not a valid
  "out-of-window must fault" negative target. An address in word 2 is interpreted as an input address only for
  address-bearing translation/access/permission classes; retain raw words for other classes.
- UNVERIFIED that the Spark's real IORT routes segment 15 rid 0x100 to instance 1; this is the expectation from
  decoded tables (`GB10_IORT_DECODE.md` sections 3 and 9), and the first thing the window tests.
- UNVERIFIED hold time and reset behavior of the probe image on the real console.

## 9. Evidence to collect

Camera recording of the whole boot; the witness's time-stamped notes; photographs of the summary screen; the stage
script output; `pre_boot_capture.json`, `post_boot_capture.json` and the verifier verdict; the preflight outputs P1 to
P12 saved with the commit and digests; `efibootmgr` output before and after; and a short result note that quotes the
summary line verbatim and lists every deviation from section 6. The result is posted on aienos#286 as observed facts
with limits; a physical PASS is claimed only for what was seen.

## 10. Statement of non-execution

This page is preparation, not authorization. October 28 receipts under
`/home/drakestapleton/handoffs/spark-five-20261028/QualSmmu/` record host/QEMU runs and candidate-only builds.
The full fixture-provisioned artifact is NOT boot-admissible. Physical boot, actual GB10/HDA0 fault observation,
GPU traffic/visibility and combined USB/network input witnesses are NOT_RUN. No ESP, USB or boot-entry writes,
model stop, firmware operation or GPU effect is authorized or performed by this lane.

## 11. First GPU-traffic confinement experiment (DESIGNED, not implemented or authorized)

The no-DMA B7B probe is not this experiment. QEMU's NVMe DMA oracle proves SMMU event plumbing only, not GB10
stream routing, GPU execution, GPU GMMU behavior, cache visibility, or fault recovery.

Required native capability from path 2: a composed **native Aienux** GPU submission endpoint, bound to kernel-owned
typed DMA-window/MMIO/IRQ capabilities, that (a) maps a supplied IOVA into GPU VA, (b) issues a bounded GPU-initiated
store, (c) performs the target-correct engine/system visibility operation and writes a GPU-uncached completion,
and (d) proves global device quiescence before bus-master revoke/SMMU release. GPU fault/status collection must
distinguish a GPU GMMU rejection from an SMMU rejection. There is no such endpoint in `omega-gpu-driver`
`b1c8e6ea76da81dc47b57670304bf256b0ba9818`: `build.sh` emits Linux/VFIO `w0`/`fwprep`, not a native kernel
artifact; `gsp/dispatch.om` refuses hardware sequencer effects; `chan/dma.om` owns VFIO mappings, not native SMMU
authority. Path 2's draft [omega-gpu-driver#6](https://github.com/aien-dev/omega-gpu-driver/pull/6),
`95383da4789556c9ca09befb9c6ab788e4e25d79`, `GB20B.md` ("Composed launcher and native effect seam"),
explicitly leaves native submit/fence/completion/quiesce absent. Its GB20B SEC2/BOOT/RPC host implementation alone
does not supply this capability. A native adapter and composed submit artifact are missing prerequisites, not
permission to substitute a CPU store or Linux/VFIO.

Proposed sequence, only after that native endpoint is implemented and independently exercised:

1. With the exact chip profile, firmware provenance, cold/warm state, operator window and recovery admitted,
   allocate one native DMA arena W containing all GPU-accessible queue descriptors, GPU page tables, command bytes,
   payload and a GPU-uncached completion. Record the complete interval and every accepted IORT RMR exception.
   Confine the IORT-routed GPU SID on its actual SMMU to W UNION RMR; all other streams stay abort. Do not claim
   "one page" if the submitted command's own dependencies require more. No global/bypass mapping.
2. Allocate a CPU-visible canary page C outside W and outside **every** accepted RMR; prove interval disjointness
   with checked interval operations and log actual addresses. Initialize input/output/completion/canary on CPU,
   perform the native publication boundary, and drain all older events as a separately labelled baseline.
3. Positive: submit a GPU command storing a unique recorded value inside W. After the specified system visibility
   boundary, observe the uncached completion and exact CPU-visible payload; collect all SMMU/GPU faults. Require
   the expected bytes and no new target-SID fault. CPU writes and simulator stores cannot satisfy this condition.
4. Negative: map a GPU VA to the **ungranted IOVA C** in the GPU's own GMMU (GPU page tables stay inside W), then
   request a GPU-initiated write there. If native GPU mapping authority refuses that mapping, record its refusal:
   that is NOT an exercised SMMU fault. Otherwise require an SMMU F_TRANSLATION for the actual GPU SID and C,
   intact CPU canary, and the documented device fault/recovery state. An unrelated SID/event is not acceptance.
   Never require completion from an operation whose DMA was deliberately aborted; use bounded fault observation.
5. If fault containment/global idle is uncertain, quarantine all backing and STOP; do not free/reuse pages or
   retry. After verified device quiescence, revoke bus mastering, install abort and invalidate, checking the return
   code. On failed release retain the confinement slot/backing (base fix `832bcbd9`), never pretend release succeeded.
6. Recovery inside W is a distinct accepted submission after the documented target-specific reset/recovery path,
   requiring fresh data, completion and event observations. Reset/recovery itself needs separate exact approval;
   this document does not authorize one. Report transfer, visibility, confinement and recovery separately.

Visibility authority: NVIDIA open 580.173.02 `nvos.h`, `NVOS32_ATTR2_GPU_CACHEABLE_YES`, says system-memory GPU
caching "will not be coherent with direct CPU mappings". Unified physical RAM does not remove the engine/cache
fence requirement. DOCS SILENT here on an admitted native GB20B engine fence/quiesce endpoint; path 2 must implement
and qualify it before step 3. Current physical GPU traffic, completion visibility and SMMU negative control: NOT_RUN.
