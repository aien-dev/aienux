# L332 rung 2, window 0: give the GPU to our driver for a few minutes, read it, give it back

Purpose: the smallest real step. Prove we can take the GB10 away from NVIDIA's driver and give it back, measure how long
that takes (nvidia.ko has never been reloaded on this box since boot, so nobody knows), and let our module `aien-gpu`
read the chip's identity and set up its memory area. Our module writes nothing to the GPU in window 0.

Scheduled by: the coordinator (ledger line "L332 window 0"). Run only inside that window. Hands-off: nothing needs a
person at the machine unless the box fails to come back from a reboot (see Recovery).

## What goes down, and for how long
Everything that uses the GPU, from part 1 until the restore reboot at the end of part 2. The four NVIDIA modules are
nvidia_drm, nvidia_modeset, nvidia_uvm, nvidia (lsmod). Holders of /dev/nvidia* on 2026-10-21 22:0xZ (`sudo fuser -v`):
- aien-serve-http pid 2587647 (the 35B on 18091, owner L306/L387 server lane),
- qwen36_stroke_g pid 2613825 (motor stroke work, L312 lane),
- nvidia-persistenced pid 1816 (root service, enabled; fails harmlessly while the modules are held back).
Plus anything the judge or a window starts meanwhile. Stop order: the coordinator stops the chip jobs and the 35B server
(owners restart them after the restore reboot); persistenced is left alone (the reboot handles it).
Expected time down: two reboots plus about 2 minutes of steps. Reboot-to-ssh time on this box is not measured (journal
gaps are not clean reboots); part 2 logs boot time and uptime, so this window measures it.

## Why a reboot and not "unload the driver"
NVIDIA's display part has a worker that spins forever on this headless box (L158: nvEvoWaitForCoreNotifier). Unloading
`nvidia_modeset` waits for that worker, so the unload would most likely hang with the GPU half torn down (INFERRED from
~/handoffs/fixer-modeset-kthread.md item 2; not tested). So the GPU is freed by booting once with NVIDIA's modules held
back. Revised 2026-10-26 (NoNvDriver review): the hold-back is self-reverting (`window-boot.sh`). GRUB_DEFAULT=0 stays;
a one-shot custom.cfg entry `aien-gpu-window` (today's command line plus `aien.gpuwindow=1 systemd.watchdog_sec=30
panic=10 oops=panic`) is chosen for exactly one boot by `grub-reboot`. In that boot only, aien-gpu-window-hold.service
writes the hold-back file to /run/modprobe.d (tmpfs) before udev and modules-load, and aien-gpu-window-lease.service
reboots after 25 minutes. Any later boot, planned or not, is today's boot with NVIDIA; no file to delete by hand. The
old /etc/modprobe.d file survived a crash and pinned every later boot to "no NVIDIA". The NVIDIA modules are not in the
initramfs (checked, fail-closed, by part 1 and by `window-boot.sh check`). Alternative not chosen: kexec with
`modprobe.blacklist=` on the command line (no file, faster) because the GPU state after a kexec without a firmware reset
is UNKNOWN on GB10.

## Facts checked on 2026-10-21 (read-only)
- Secure Boot disabled, lockdown none, module signatures not enforced: the unsigned aien_gpu.ko loads (taints).
- aien_gpu.ko vermagic 7.0.0-1019-nvidia = running kernel.
- kernel.panic = 0: a kernel panic would freeze the box until power-cycled. Part 2 sets kernel.panic=10 and
  panic_on_oops=1 (runtime only, gone at the next boot) before our module touches the chip, so a bad register read
  reboots the box on its own.
- GB10 = 000f:01:00.0, 10de:2e12, BAR0 64 MiB at 0x24000000.

## Steps
Scripts in this directory, each line timed into window0-<UTC>.log (window0-current.log points at it).
Prep (changes the boot config, needs Drake's go): `./window-boot.sh check` (read-only, any time), then
`./window-boot.sh install --yes` (adds the inert GRUB entry and the two units; normal boots are unchanged).
0. Any time before the window: `./window0-pre.sh` (no --go) checks the module, Secure Boot, initramfs and lists holders.
1. Window opens; coordinator stops the GPU users; `./window0-pre.sh --go`: records lsmod, nvidia-smi, cmdline, module
   hash, refuses if anything but persistenced still holds the GPU, runs `window-boot.sh check` and `arm --yes` and
   reboots into the one-shot window entry.
2. After ssh is back: `./window0-post.sh`: refuses unless this is the window boot; logs boot time; checks no nvidia
   module and no driver on the GPU; sets panic auto-reboot; insmod aien_gpu allow_bind=1; aien_gpu_check (identity,
   firmware size, arena mmap pattern); rmmod; optional FLR recovery test (`AIEN_W0_FLR=1`, Drake decides); removes the
   /run hold-back file; timed `modprobe nvidia nvidia_uvm` + first nvidia-smi answer (NVIDIA's load-to-ready baseline);
   Omega gpu_launch_cost_test --reps 3 (first open after load, timed); prints WINDOW0 PASS or FAIL; normal reboot.
3. After the restore reboot: `lsmod | grep -c ^nvidia` = 4, `nvidia-smi -L` lists the GPU, no aien.gpuwindow=1 on the
   command line, `./window-boot.sh status` shows no next_entry; owners restart their servers; commit the log; window
   closes. `./window-boot.sh uninstall --yes` when no more windows are planned.

## Recovery
- Anything odd during part 2: `sudo rmmod aien_gpu` if loaded, then `sudo reboot` (the next boot is the default entry).
- Box does not come back within 10 minutes after part 1's reboot: the 30 s hardware watchdog (SBSA, armed by systemd
  from the command line) resets a hung boot, and the default entry with NVIDIA comes up. Whether an SBSA reset also
  resets GSP/SEC2 is NOT_RUN; if ssh never answers, a power cycle at the wall is the only step left (Drake, physical).
- Window overruns: the lease reboots the box 25 minutes into the window boot.
- Kernel panic or oops while our module reads the chip: auto-reboot after 10 s (command line, so it holds from the first
  instruction, not only after part 2 sets it), back to today's boot.

## Gate (window 0 PASS when all hold)
- aien-gpu: PMC_BOOT_0 read (not 0, not all-ones), firmware 75,012,080 bytes, arena mmap pattern PASS, rmmod clean.
- PMC_BOOT_42 logged. window1-pre.sh refuses unless CHIP_ID = (BOOT_42 >> 20) & 0x3ff is 0x1AB (GB10B), because the
  window-1 package carries GB10B images only; the Spark reports sm_121, which in 580.173.02 points at GB20B (0x1BB)
  rather than GB10B (Thor, T264). Until window 0 reads it, the chip identity is unmeasured.
- After the reload: nvidia-smi lists the GPU; Omega launch-cost test exit 0.
- After the restore reboot: four modules back, not the window boot, no next_entry.
- Log committed under evidence/own-gpu-driver/rung2/, pass or fail.

## Window 1: start NVIDIA's GSP firmware with our own code
Only after window 0 PASS (part 1 refuses otherwise). Same take-away and give-back as window 0 (reboot with NVIDIA held
back, restore reboot), so the same people stop the same GPU users.

What our code does to the chip, all of it from the source lines quoted in native/kernel/gpu/gsp_mmio.h and
gpu/contract/gb10_gsp.devc: builds the start-up package in memory the GPU can read (L192 builder), writes one 868-byte
signed packet into SEC2's mailbox memory, rings SEC2 once, waits for SEC2 to hand control to GSP, writes the GSP-RM
version into one register, then only reads: GSP's status messages until "INIT_DONE" or 30 s.

Why this cannot harm the chip: the GPU checks NVIDIA's signature on the firmware itself (the chain of trust lives in
SEC2 and the boot ROM; we hand over NVIDIA's signed images byte for byte, pinned by SHA-256). Our module lets user space
reach only the 12 start-up registers (6 of them writable), only with allow_boot=1, and rings the SEC2 doorbell once per
load. What can go wrong is that GSP is left stopped or half started until the GPU is reset.

UNKNOWN until this window (said out loud):
- whether the restore reboot resets the GB10's GSP and SEC2 fully. If it does not, NVIDIA's driver fails after the
  restore reboot (part 3 shows it) and the fix is a full power cycle at the wall: Drake, physical. That would also keep
  the 35B server down until then. This is the one real cost; the coordinator weighs it when scheduling.
- whether SEC2 accepts the packet with Linux's driver absent (inventory 14.5), whether GSP-FMC accepts our WprMeta and
  heap, whether our GspSystemInfo values (rung2/system_info.txt, each tagged with its source) are what GSP-RM needs,
  whether GSP-RM asks for a CPU sequencer run before INIT_DONE (window 1 records it and stops; handling it is window 2).

Steps (each line timed into window1-<UTC>.log, window1-current.log points at it):
0. Before the window: `make -C native/linux/aien-gpu all tools sim-test` (sim test PASS), the package
   `./window1-package.sh <dir>` (WINDOW1_PACKAGE: PASS), then `./window1-pre.sh <dir>` (checks only).
1. Window opens, GPU users stopped: `./window1-pre.sh <dir> --go` (blacklist file, reboot).
2. After ssh is back: `./window1-post.sh`: deletes the file, aborts if any NVIDIA module is loaded, sets panic
   auto-reboot, insmod aien_gpu allow_bind=1 allow_boot=1 arena_mib=<package size + 9> heap_mib=512, runs
   aien_gsp_boot (register trace with times, plan, every status message, GSP log buffers, verdict) into
   window1-<UTC>-run/, prints WINDOW1 PASS or STOPPED, reboots WITHOUT rmmod.
3. After the restore reboot: `./window1-check.sh` (four NVIDIA modules, nvidia-smi answers, file gone, aien_gpu not
   loaded): WINDOW1 RESTORE PASS. Owners restart their servers. Commit the log and the run directory, pass or fail.

Recovery: as window 0. If part 3 fails (nvidia-smi does not list the GPU after the restore reboot), reboot once more;
if it still fails, the GPU needs a power cycle (Drake, physical), and nothing else is tried on the chip until then.

Gate (window 1 PASS): aien_gsp_boot verdict INIT-DONE, register trace inside the allowlist, WINDOW1 RESTORE PASS.
Any other verdict is still a result: it names the step that stopped (SEC2, mask, lockdown, FMC code, core, queue).

## Later windows (each needs its own prepared step and its own request)
1. (window 1 above.)
2. The RPC subset from docs/numeric/DRIVER_FLOOR.md and one channel (gpu/contract/gb10_chan.devc).
3. Omega on aien-gpu through L202's typed handles: golden tokens identical to nvidia.ko, per-word timing and start time side by side.
