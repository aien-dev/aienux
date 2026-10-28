# Spark storage-free boot and input: attended procedure

Status: IMPLEMENTED host candidate composition; physical boot, platform xHCI DMA/HID, RTL8127 DMA/link/TX and hardware handoff are NOT_RUN. Nothing here authorizes a write, firmware change, model stop or reboot. Main obtains Drake's explicit current authorization at each consequential step. Receipts live at `/home/drakestapleton/handoffs/spark-five-20261028/QualBootInput/`; `commands.jsonl` records executed commands and exits, not physical qualification.

## Artifact and scope

Base: aienux `fix/dma-unconfine-retain-20261028` at `832bcbd98fc59a6f7695aefa68223caa7e08df05`, plus RTL8127/UDP PR #450 and the unchanged seeded Omega GPT writer from #461. Build the dedicated `full CK_SPARK_INPUT=1` variant. It includes PCI discovery and post-exit bus-master sweep, SMMU-confined xHCI/USB boot HID keyboard and bounded command shell, then RTL8127/UDP snapshot and release. It excludes NVMe driver/probe, Store, security, Allen and sustained console-session stages. `CK_CONSOLE_SESSION=1` remains QEMU-only; do not weaken that gate to obtain hardware input.

Hardware-staging build (only existing provisioned public identity files; never generate substitute identities):

```
nice -n 19 ionice -c3 make -j2 -C native/kernel full CROSS= \
  OUT=/home/drakestapleton/handoffs/spark-five-20261028/QualBootInput/candidate \
  CK_SPARK_INPUT=1 CK_HARDWARE_STAGING=1 \
  CK_OWNER_PUBKEYS="$OWNER_PUBLIC_FILE" CK_MACHINE_ID="$MACHINE_ID_FILE" \
  CK_FB_HOLD_S=120 AIENOS_COMMIT="$(git rev-parse HEAD)"
```

The existing provisioning refusal remains mandatory. Missing owner material means hardware-staging artifact NOT_RUN; a non-staging host/QEMU artifact is not an authorized substitute. No private owner key is needed. Use `tools/spark-image/spark-image.om` exactly as its README specifies: compiler Omega `c3fb7969727131a0df9332bfd7dedc5ee8b357d2`, compiler SHA-256 `c99eda2465036c0637a444689a65dd7fc736e4fa8a8b4cf3456b0ce1b57f8996`, compile success before execution, dedicated receipts working directory with `BOOTAA64.EFI`. The writer creates only `spark-boot.img` there. Retain ELF, linked source/object list, symbols, disassembly, complete image and hashes; reader checks must extract an identical EFI and find a valid GPT/FAT. No MODEL.MAP is included. Kernel and GCC/binutils remain existing legacy C/assembly dependencies; packaging is Omega, not a kernel migration.

## Media and rollback prerequisites

Keep the existing recovery USB **read-only and untouched**: Generic Flash Disk serial `5D0344B4`, LABEL `AIENOSRECOV`, GPT disk GUID `308d8c7d-103e-402b-be02-619a8669fe7f`, partition GUID `f227357c-1e80-4fd0-a55e-2efbef1dd25c`. It is Boot0002 today, not the historical Boot0004. Parent's read-only inspection is `/home/drakestapleton/handoffs/spark-five-20261028/parent/recovery-stick-readonly-20261028.md`. Its on-stick attended receipt records `RECOVERY_BOOT_GATE: PASS` on this Spark on 2026-09-25, not a fresh boot today. Partition SHA-256 `b595376e5acb75c725321c950b4f89ef5b2cb19607a5e6609db8e35255007c31` is the parent-recorded measurement.

Drake must supply a **separate blank candidate medium**. It has not yet been identified, so no device path is authorized. Main first records its serial, size, partition identities and current mount state (`lsblk -o NAME,PATH,MODEL,SERIAL,SIZE,FSTYPE,LABEL,PARTUUID,MOUNTPOINTS`, `/dev/disk/by-id`), shows Drake that exact whole-device target and candidate hash, and obtains explicit overwrite authorization. Refuse the recovery serial and every internal NVMe disk. Only after authorization, unmount the candidate's own partitions and write through that verified whole-device `/dev/disk/by-id/...` path, not an assumed `/dev/sdX`. Example command after binding `$CANDIDATE_BY_ID` to the freshly approved serial (not a command executed by this work):

```
sudo dd if="$RECEIPTS/spark-boot.img" of="$CANDIDATE_BY_ID" bs=4M conv=fsync status=progress
sudo cmp -n "$(stat -c %s "$RECEIPTS/spark-boot.img")" "$RECEIPTS/spark-boot.img" "$CANDIDATE_BY_ID"
```

Record write and readback exits. A changed target or hash requires fresh approval. No internal ESP writes, no partition edits to the recovery disk. Boot0000 ALLEN.EFI is a write probe and prohibited; Boot0005 CORE.EFI and `/boot/efi` remain untouched. Capture current `efibootmgr -v` and BootOrder before the window. Do not delete or reorder existing entries.

## Receiver: iMac or MacBook

Use the Realtek 10GbE RJ45 LAN on the Spark and a receiver Ethernet interface on the same isolated layer-2 network (switch, or a direct link whose physical link is verified). No IP assignment is required by the sender: it broadcasts IPv4/UDP port 47031. Choose an already permitted LAN and firewall policy; changing host permissions/firewall is a separate authorization. Wi-Fi/VLAN separation can discard broadcasts. A missing UDP report is a failed/NOT_RUN observation, never a reason to bypass SMMU.

On the receiver, from this source revision:

```
cc -std=gnu11 -O2 -Wall -Wextra -Werror \
  native/kernel/tools/ck_imac_report_listen.c -o ck_imac_report_listen
mkdir -p spark-input-receive
./ck_imac_report_listen 47031 spark-input-receive 300
```

The listener was built on the MacBook (Darwin `/usr/bin/cc`) at `/Users/drakestapleton/handoffs/spark-five-20261028/QualBootInput/ck_imac_report_listen`, SHA-256 `78408ce963d44b21b8da0c7b7b629530aab3aceda63da6a3f99f87f2b17e536e` (`macbook-listener-build.txt` in local receipts). No receiver socket/service was started. Run that binary with port47031, an output directory and deadline300 at the attended window. Expect `ready port=47031`, explicit UNAUTHENTICATED warnings, and then `complete run=<16 lowercase hex> auth=none bytes=... parts=... file=.../spark-<run>.unauthenticated.txt` in `listen.log`. Finished part repeats do not create another file. Reports are readable/forgeable by LAN peers, not signed receipts, commands or authority. Keep raw listener logs plus continuous phone video of the Spark screen. The report snapshots the console **after keyboard release but before NIC initialization**; it does not contain its own NIC success/release or the final report. Those require display evidence.

## Model stop and one-shot boot (Main/operator only)

Checkpoint every lane first. Follow `B7B-PHYSICAL-BOOT-PREP.md` section 7 and capture a fresh port-18091 model identity, health, PID, exact executable hash/arguments, environment and actual supervisor. Do not kill a remembered PID or an unexamined supervisor. Explicit authorization must cover stopping the current resident model, the reboot, this exact EFI/image digest, confined USB/RTL DMA and firmware one-shot selection. No such authorization has been exercised by this work.

The current read-only snapshot from QualSmmu is `qwen3.6-35b-a3b-fp8`, port18091, context49152, slots8, stroke1, executable `bin-19173f9e`, GPU lock `/tmp/aien-serve-18091.lock`. This is a restore recipe, **not permission to run it now**:

```
Q36_BATCH_BIN=bin-19173f9e Q36_BATCH_SLOTS=8 Q36_MAX_CONTEXT=49152 \
AIEN_Q36_STROKE=1 AIEN_SERVE_GPU_LOCK=/tmp/aien-serve-18091.lock \
/home/drakestapleton/.local/state/aien-q36/start-35b-batch.sh
```

Revalidate the snapshot at the window; launcher defaults differ and must not replace it. Main uses the actual supervisor's stop procedure, verifies the listener/process is stopped and lock ownership released without taking someone else's GPU lock, then proceeds. Never start a second model beside an incumbent.

Prefer vendor Esc/Del -> Save & Exit -> Boot Override -> the newly verified candidate USB. This is one boot and preserves BootOrder. If Main instead chooses **BootNext**, first identify a firmware entry that points to the candidate medium's exact GPT partition and `\\EFI\\BOOT\\BOOTAA64.EFI`, compare it with `efibootmgr -v`, and obtain explicit authorization for that exact entry. Do not infer a Boot#### from this document. Then `sudo efibootmgr -n "$CANDIDATE_BOOTNUM"`, read back BootNext and unchanged BootOrder, and only then use the explicitly authorized reboot. If no verified candidate entry exists, use Boot Override; creating one is a separate firmware-change approval. On cancellation before reboot, clear only the BootNext just set with `sudo efibootmgr -N` after confirming its target; never delete another lane's one-shot request.

## What Drake types and what to capture

Connect the monitor before power-on, a simple wired USB boot-protocol keyboard, the candidate medium and receiver Ethernet. Keep the proven recovery medium available, not overwritten. Start the receiver and phone recording before selecting the candidate.

1. Confirm `kernel: alive`, `kernel_el: EL1h`, `mmu_switch: ... switched=yes`, GIC/timer/SMP results and the storage-free `devices:` banner. On Spark the host ACPI prediction has six platform controllers USB0..USB5, streams0..5 on SMMUv3 `0x13800000`. This is static routing, not hardware proof. Actual SMMU bring-up/confinement and HID enumeration must succeed. Require a confined xHCI grant before `keyboard: ready`; deny/timeout means no input qualification.
2. During `recovery_access: waiting 5000 ms ...`, type **nothing**. Wait for `recovery_access: choice=normal reason=timeout` and `keyboard: shell ready ...` / `aienos>`. Do not type `r`: that intentionally selects a recovery halt, not this test.
3. Type each line and Enter, promptly within the displayed total/idle bounds: `abc`, `help`, `el`, `mem`, `report`, `uptime`, `exit`. No secrets, credentials or arbitrary commands. Require `keyboard_echo: abc`, `keyboard_line: abc`, `unknown command: abc`; `commands: help mem el report uptime exit`; `EL1`; `conventional_memory_kb: <value>`; `report_kind: shell` with the exact built `aienos_commit`; `uptime_ms: <value>`; and `keyboard: done (exit)`.
4. Require xHCI halt/revoke and `smmu: xhci ... returned to abort (rc=0)` before `devices: xhci=fenced (rc=0)`. Any `LIVE`, `abort FAILED`, bus-master revoke failure, panic, fault or unconfined bypass fails the attempt.
5. Then require actual RTL8127 identity, `dma_gate: rtl8127 granted (Confined)`, `rtl8127: init OK`, link up, report transmission and `devices: rtl8127=report-sent (rc=0)`. Capture the exact init/error/link/TX lines; do not substitute the host chip model. Require `dma_gate: rtl8127 bus master revoked` and `smmu: rtl8127 stream ... returned to abort (rc=0)` on the screen. Receiver file must contain the matching build/keyboard transcript. No report or a denied SMMU grant is a truthful failed observation, not permission to change gates.
6. Capture `report_kind: final` and `screen: holding 120 s before reset` if GOP is usable; the framebuffer hold is not a watchdog. Record normal-OS return and that BootNext was consumed/absent and BootOrder unchanged. A core final report alone does not prove input/network success; both witnesses are required for this path.

## Abort and restore

A bounded keyboard timeout should release the grant and continue, not be called input PASS. Stop the window for an unexpected DMA state, panic/fault, absent display, recovery halt or hang. If hung, use the documented chassis power button **until off**, wait ten seconds, remove the candidate medium, then power on. No vendor-qualified numeric hold duration is asserted. Boot the unchanged normal OS; Esc/Del Boot Override can select it. If normal OS does not boot, select the protected recovery USB by its freshly verified identity (today Boot0002), follow its proven read-only recovery procedure, and do not repair internal disks blindly. Do not select ALLEN.EFI.

After normal OS returns Main verifies OS/disk health, fresh NVIDIA state and no surviving unexpected lock owner, restores the exact saved model configuration through its actual supervisor (recipe above only if still current), checks `/health` and `/v1/models` on `127.0.0.1:18091`, and confirms the original served model before resuming lanes. Record each stop/restore observation, not merely command launch. Keep any failed boot receipt unchanged. Physical result remains NOT_RUN until this attended procedure is explicitly authorized and performed.
