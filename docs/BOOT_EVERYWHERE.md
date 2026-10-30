# The boot sequence everywhere (L266)

What a person sees and hears when AienOS starts, on every machine: the night sky, two black holes circling and
merging, a short rising chirp at the merger, then Allen. One module, the same timing, the same sound. This page
says where it runs today, how fast, and what is still missing. Every number carries its command and exit code;
claims are tagged OBSERVED (seen in a run), INFERRED (follows from code or a run, not seen directly) or UNKNOWN.

## 1. One module

| Part | File | Source |
|---|---|---|
| The night (stars, Milky Way, galaxies, the two black holes, the ripple) | `native/kernel/ui/night.c` | byte-identical to the iMac build `0930240` |
| Allen's home on the screen (draws the night, then Allen) | `native/kernel/ui/allen_ui.c`, `mind.c`, `paint.c`, `allen_font.c` | same |
| The chirp | `native/kernel/ui/chirp.c` | same |
| Frame receipt and the night's clock for the sound | `native/kernel/ui/boot_show.c` | new (L266) |
| aarch64 runner (Spark, Pi 5, the aarch64 VM) | `native/kernel/core/boot_screen.c` | new |
| x86 runner (iMac, PC, the x86 VM) | `native/kernel/x86/boot_x86.c` | new |
| VM screen (virtio-gpu 2D) | `native/kernel/dev/virtio_gpu.c` | new |
| VM speakers (virtio-sound) | `native/kernel/dev/virtio_snd.c` | new, shared by both kernels |
| Request core and the aarch64 PCI binding | `native/kernel/dev/virtio_q.c`, `virtio_bind.c` | new |

Timing lives in two headers and nowhere else: `night.h` (`CK_NIGHT_*`: stars out by 6.8 s, holes at 2.6 s,
spiral from 6.0 s, merger at 12.0 s, gone at 15.2 s) and `chirp.h` (`CK_CHIRP_*`: 600 ms sweep from 200 Hz to
1000 Hz ending at the merger, 300 ms ring).

**The sound file and its licence.** There is no sound file. The chirp is computed at boot from the shape of a
real gravitational-wave chirp (pitch rising as t^(-3/8), loudness as pitch^(2/3)), integer math, in
`ui/chirp.c`. No recorded audio and no third-party material: its licence is this repository's. The same
formula runs on every machine, so every machine plays the same samples (checked below: the sound a VM played
matches the formula sample for sample).

**How every image builds it in.** The ui files are compiled into every full aarch64 image and, with
`CK_BOOT_SHOW=1`, into the x86 image. The night plays when the image is built with `CK_BOOT_SHOW=1`:
`make -C native/kernel full CK_BOOT_SHOW=1` (aarch64, `$(OUT)/full-boot-show/BOOTAA64.EFI`) and
`make -C native/kernel imac-kernel CK_BOOT_SHOW=1` (x86, `$(OUT)/IMACKERN-BOOT-SHOW.EFI`). The default stays
0 so the 32 emulator gates that use a screen keep their timing. The PC images (L260) and the aienos-run images
(L261, L262, L265) should build with `CK_BOOT_SHOW=1` (ledger notes sent).

**What the runner (aienos-run) must give the VM** so its window shows the real boot screen, not a web
imitation:

| | aarch64 VM | x86_64 VM |
|---|---|---|
| Screen | `-device virtio-gpu-pci,disable-legacy=on,iommu_platform=on` | `-device virtio-vga` |
| Speakers | `-device virtio-sound-pci,audiodev=...,disable-legacy=on,iommu_platform=on` | `-device virtio-sound-pci,audiodev=...` |
| IOMMU | `-M virt,iommu=smmuv3` (the kernel refuses device DMA it cannot confine) | none (that kernel has no IOMMU support yet; it says so) |
| Window | QEMU's own display (`-display cocoa`/`gtk`/`sdl`) or VNC of that screen | same |

A runner with its own virtual machine monitor (L262 KVM, L265 Virtualization.framework) needs a virtio-gpu and
a virtio-sound device model with the same commands (section 3 lists exactly which).

## 2. Measured

How to read a receipt: the night lasts 15.2 s, which is 912 beats of a 60 Hz screen. A "dropped" beat is one
where no new picture was ready. fps is frames drawn divided by 15.2 s.

**Inside the VM (the aienos-run window).** `quiet-build bash scripts/qemu_ck_boot_show_test.sh <arch> <accel>`;
kept files (serial log, mid-night screendump, captured WAV) in `target/boot-show/<arch>-<accel>/`.

Three runs, each exit 0 and `AIENOS_CK_BOOT_SHOW: PASS` (OBSERVED, 2026-10-20, Spark, QEMU 8.2.2, 1280x800):

| Image | Machine | Screen path | Frames in 15.2 s | Dropped of 912 | fps | Draw average | Slowest |
|---|---|---|---|---|---|---|---|
| aarch64 | virt, KVM (host speed) | virtio-gpu | 940 | 9 | 61.8 | 6.6 ms | 33.0 ms |
| aarch64 | virt, TCG (every instruction emulated) | virtio-gpu | 419 | 501 | 27.5 | 33.2 ms | 257.2 ms |
| x86_64 | q35 + OVMF, TCG | GOP framebuffer | 189 | 723 | 12.4 | 77.9 ms | 312.6 ms |

Sound, same runs: the chirp starts at night 11.25 s (aarch64) and 11.31 s (x86_64); the virtio-sound device
plays 11 to 12 periods of 100 ms with 0 starved and 0 refused, then stops cleanly. QEMU wrote what the guest
played to a WAV file; 44936 of 44936 chirp frames are present, correlation with the host-made chirp 1.0000,
largest sample difference 0 (all three runs). A mid-night screendump shows the sky drawn (0.30% of pixels lit,
brightest 255).

Reading it: with KVM the aarch64 image holds 60 Hz in the VM window. TCG emulates the CPU one instruction at a
time, so its numbers say how slow emulation is, not how slow the code is; the x86_64 image can only run under TCG
on this aarch64 host (no x86 KVM here), so x86_64 at 60 Hz in a VM is UNKNOWN until a run on an x86 host with
WHPX, Hyper-V or KVM. The sound and picture are bit-identical in every run; only the pace differs. QEMU prints
`audio: Could not create a backend for voice virtio-sound.in` in every run: the microphone side of the device has
no backend in the gate, the playback side is the one measured, and nothing in the gate depends on capture.

**The same drawing code on the Spark, no VM** (one core, real clock, memory instead of a screen):
`BOOT_SHOW_SIZES="1280x800 1920x1080 3840x2160 5120x2880" quiet-build sh scripts/ck_boot_show_test.sh`,
exit 0 (OBSERVED):

| Screen | Frames | Dropped of 912 | fps | Draw average | Slowest |
|---|---|---|---|---|---|
| 1280x800 | 948 | 2 | 62.3 | 4.5 ms | 22.3 ms |
| 1920x1080 | 947 | 3 | 62.3 | 5.8 ms | 15.6 ms |
| 3840x2160 | 652 | 273 | 42.8 | 21.6 ms | 157.2 ms |
| 5120x2880 | 422 | 490 | 27.7 | 35.1 ms | 97.0 ms |

One Spark core keeps 60 Hz up to 1920x1080 and falls behind at 4K and 5K (OBSERVED). The VM runners cap the
screen at 1920x1080 for that reason.

**The iMac itself** (27-inch, 5K panel driven at 3840x2160, build `0930240`, the netboot report
`imac-2d27001cbf587fae`, OBSERVED, not a run of this branch): "1126 frames by 20 s after Allen woke, average
12.6 ms, slowest 46.8 ms (drawn by a secondary core)"; "the sky made in 476 ms (66 MiB drawing memory)"; the
night on screen 15260 ms after power on. An average of 12.6 ms is inside one 60 Hz beat (16.7 ms), so on
average the iMac holds 60 Hz at 3840x2160; the slowest frame (46.8 ms) missed about two beats (INFERRED from
the report; that report does not count dropped beats, the receipt line on this branch does). The iMac
splits each frame across its cores, the Spark fixture above uses one.

UNKNOWN: the frame rate on the Spark's own screen, on a Pi 5 and on a PC; none has run this build yet.

## 3. What the drivers use

virtio-gpu (virtio 1.x 5.7): GET_DISPLAY_INFO, RESOURCE_CREATE_2D (B8G8R8X8), RESOURCE_ATTACH_BACKING (one
entry), SET_SCANOUT, then per frame TRANSFER_TO_HOST_2D and RESOURCE_FLUSH of the whole picture. virtio-sound
(5.14): PCM_INFO, PCM_SET_PARAMS (stereo, S16, 48 kHz, 100 ms periods, 400 ms buffer), PCM_PREPARE,
PCM_START 150 ms before the chirp comes in, 100 ms transfers fed from the night's clock, PCM_STOP and
PCM_RELEASE after 100 ms of quiet. Every command layout is checked against the virtio spec's Linux UAPI copy
(`scripts/ck_virtio_av_test.sh`).

## 4. The GPU path

Today the night is drawn by the CPU: every pixel worked out from its position and the time, integer math,
in strips (the iMac kernel splits a frame across its cores). What drawing it on a graphics chip would need:

1. **A way to run work on the chip.** On the iMac's AMD Radeon Pro 580 the branch `feat/imac-gpu-shader`
   (5b56323) has a GCN instruction encoder, PM4 command packets and four compute kernels (fill, blit, star
   splat, and `bh_bend.c`, the black holes' light bending written to match `night.c` exactly), checked on a
   host GCN interpreter only; nothing has run on the chip yet (INFERRED from its commit message and tests).
   Missing: the ring buffer and doorbell bring-up, memory the chip can see, firmware loading for the compute
   engine. On the Spark's GB10 the same work is the L198 to L207 driver lanes plus the GB10 launcher.
2. **The split.** The stars, galaxies and Milky Way are the base layer and change slowly; the costly part per
   frame is the light bending around the two holes (each pixel near a hole is moved by the deflection
   formula) and the ripple ring. Those two map one pixel to one GPU thread with no communication between
   pixels: the shape a GPU is built for. The CPU keeps the clock, the sound and the text.
3. **The VM.** Inside aienos-run the guest has no real GPU. A virtio-gpu with 3D (virgl or Venus) would let
   the guest hand shaders to the host's GPU; that is a separate driver (virtio-gpu 3D contexts) and is not
   started.
4. **Is it needed?** On the iMac the CPU path holds 60 Hz on average at 3840x2160 (section 2, INFERRED from a 12.6 ms average frame). A GPU path
   is worth it for 5K panels, for doing more each frame, and for battery; it is not needed for the boot as it
   is today.

## 5. Open findings

- **Dim hint text (from L282, OBSERVED there).** `ui/allen_ui.c` draws the hint line at alpha 150/256 and the faint
  line at 110/256 over the night; measured contrast 3.26:1 and 1.98:1, under the 4.5:1 floor
  (`docs/design/tokens/contrast.txt` on `feat/allen-design-system`). Drawing both at full alpha gives 7.73:1 and
  6.02:1. Not changed here: `ui/` is kept byte-identical to the iMac build (`0930240`) so the iMac stack merges
  cleanly; the one-line fix goes in once that stack has landed, or on the iMac lane if it moves first.
