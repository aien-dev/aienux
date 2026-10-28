# native/kernel: AIENOS C kernel core (Lane 18)

The first AIENOS kernel written in C for AArch64. It boots in QEMU through
UEFI (AAVMF) the way the Rust kernel does and prints the same observables.
QEMU qualifies nothing physical: a PASS here says nothing about real hardware.

## Layout

| Path | What |
| --- | --- |
| `include/ck.h` | Frozen contract for stage code (devices, security, store). Lane 18 core added only `ck_irq_register`, `ck_irq_enable`, `ck_irq_cpu_enable`. |
| `arch/` | Vectors, EL2 -> EL1h entry (ADR 0009), recoverable probe, exception and IRQ dispatch, GICv3 or GICv2 (picked from the MADT; GICv2 is the Raspberry Pi 5 GIC-400 shape, QEMU gate `AIENOS_QEMU_GIC=2 scripts/qemu_ck_boot_test.sh`), EL1 physical timer. |
| `mm/` | Frame allocator (UEFI map), 4 KiB-granule page-table builder, heap, the kernel address space (`mmu.c`). |
| `core/` | Console, printf, ACPI, reports/panic/PSCI reset, M3 (threads, scheduler, EL0, capabilities, IPC), `kmain.c`. |
| `tests/` | Host unit tests. |
| `third_party/omega/` | The canonical Omega Cortex store, byte for byte from the omega commit in `omega.lock` (checked by `scripts/omega_lock_check.sh`); built freestanding by `svc/cortex_store.c` over the sealed Store through `svc/cortex_journal.c` (native v1 plan P06). |
| `x86/` | x86_64 port, first cut (AIENOS-KERNEL-0, 2017 iMac): UEFI stub, descriptor tables and interrupt stubs, page tables, local APIC timer, ACPI power off, console, secondary-core start, PCI walk, read-only AHCI and NVMe disks, BCM57766 Ethernet link detection; a lab-only, opt-in network report (`CK_X86_REPORT=1`). |
| `../boot/` | Minimal UEFI entry stub (one image with the kernel). |

## Build and test

    make -C native/kernel            # target/native-kernel/BOOTAA64.EFI
    make -C native/kernel test       # host tests, ends CK_CORE_HOST: PASS
    make -C native/kernel sanitize   # same under ASan + UBSan
    make -C native/kernel full       # + stages: target/native-kernel/full/BOOTAA64.EFI
    make -C native/kernel stage-test # stage host tests (also stage-sanitize, stage-free)
    scripts/qemu_ck_boot_test.sh     # QEMU boot of the core-only image, ends AIENOS_CK_M1: PASS
    scripts/qemu_ck_store_test.sh    # QEMU NVMe + Store gate (full image, QEMU-only
                                     # CK_QEMU_UNSAFE_DMA=1 build), ends AIENOS_CK_M4_NVME,
                                     # AIENOS_CK_M4_STORE, AIENOS_CK_ARGUS1_REVOKE
    scripts/qemu_ck_disk_layout_test.sh # QEMU GPT / AIENOS partition gate, ends AIENOS_CK_DISK_LAYOUT

Toolchain: gcc, binutils (ld, objcopy, nm, readelf), make, shell. On a
non-aarch64 host the Makefile uses the `aarch64-linux-gnu-` prefix.
Output lands in `<repo>/target/native-kernel` (ignored by git).

## Owner provisioning (hardware staging image)

The default and QEMU images use the labelled TEST Store keys
(`ck_store_test_keys`, public label, `M5_ID_TEST`), the TEST store uuid and
the TEST ARGUS machine id (0xA1; the default image prints `argus: TEST machine id 0xA1 ...` on the serial console). The hardware staging image never embeds any
of them:

    make -C native/kernel full CK_HARDWARE_STAGING=1 \
        CK_OWNER_PUBKEYS=<owner public keys file> CK_MACHINE_ID=<machine id file>
    # -> target/native-kernel/full-hardware-staging/BOOTAA64.EFI

- Without both files the Makefile stops with `CK_HARDWARE_STAGING refuses the
  TEST Store keys and TEST machine id`. `svc/store_boot.c` and
  `svc/security.c` also `#error` in a `CK_HARDWARE_STAGING` build without the
  generated header, and the linked image is checked (Makefile `owner_check`)
  to carry no TEST Store label, uuid, TEST print line or TEST key symbol.
- The files hold PUBLIC material only, as text lines `<name> <hex>` (`#`
  comments; each name exactly once). Owner file:
  `owner_root_ed25519 <64 hex>` (the Gate 3 Owner Root public key). Machine
  file: `machine_id <64 hex>` (ARGUS machine id, 32 bytes) and
  `store_uuid <32 hex>` (boot Store uuid). `tools/ck_owner_gen.c` checks them
  and writes `ck_owner_prov.h` into the OUT directory (never committed). It
  refuses the RFC 8032 TEST 1/2/3 public keys, the TEST machine id, the TEST
  store uuid, all-equal bytes, wrong lengths, non-hex, duplicates and unknown
  names. Real owner files are never committed.
  The owner file for a YubiKey-held owner key is made by `scripts/pi5-8/owner_pubkey_file.sh` from the exported PIV public key
  (`docs/native/PI5-8-OWNER-CEREMONY.md`, gate `scripts/ck_pi5_owner_check.sh`).
- What the image uses: the machine id becomes the ARGUS machine id, the store
  uuid the boot Store uuid, and the boot log prints a SHA-256 fingerprint of
  the owner root key. Nothing in the kernel verifies with the owner root key
  yet.
- **Production Store keys: TPM Slot 0 is BLOCKED_HARDWARE, Slot 1 recovery
  unlock is implemented (P04 cut 1).** Store keys derive from K_vol (ADR
  0017), which is secret and cannot come from public material.
  `ck_store_production_keys` (the TPM Slot 0 path) still always refuses
  (`CK_SB_E_BLOCKED_OPERATOR`), so a diagnostic hardware staging boot prints
  `store: REFUSED proof=keyed step="production Store key source"` and neither
  reads nor formats the disk. A `CK_SERVICE` image instead keeps the NVMe
  bound and comes up LOCKED; the operator types `unlock <64-hex recovery
  secret>` on the serial console and `ck_store_unlock_boot`
  (`svc/store_boot.c`) runs the ADR 0017 section 2.4 Slot 1 path: on a blank
  disk it draws K_vol, salt and nonce from the entropy service (refused when
  RNDR is), wraps K_vol into a Recovery_RawSecret descriptor (HKDF-SHA256
  KEK) and commits the KeySlotManifest (kind 21) with the first boot record;
  on a non-blank disk it reads the manifest before any key exists, unwraps
  K_vol and opens the Store with PRODUCTION-class keys. A wrong secret fails
  the AES-GCM-SIV tag before the disk is opened; the secret is echoed as `*`
  and never printed. Under `CK_HARDWARE_STAGING` the manifest uuid must be
  the provisioned store uuid. Limits: no TPM path, no key rotation
  (`active_key_epoch` 1), manifest not yet bound into the secman
  `keyslot_manifest_id`. `store_boot_run` refuses TEST-identity keys in a
  hardware staging build before any disk access (host test `stage_test`,
  including `test_unlock_recovery`; QEMU rows 168-170).
- **Production continuity on the service lifecycle (P05 cut 1).** Every
  Store open of a `CK_SERVICE` image (boot with keys, or `unlock`) runs
  `svc/continuity_service.c`: an unprovisioned Store gets the identity
  (AgentRoot, AgentState, Manifest) and the ALLEN subject minted in one
  sealed transaction with RNDR entropy (no entropy: refused, nothing
  written); a provisioned Store has the subject restored read-only, then the
  identity resumed at incarnation + 1. A refusal erases the Store keys and
  keeps the lifecycle LOCKED. The TEST-ONLY mode selection
  (`continuity_boot.c`, `CK_TEST_CONTINUITY`, the `AIENCONT` plan line) and
  the Recovery Core are never in a service image (`cont_check`). Serial lines
  `continuity: PROVISIONED|RESUMED|REFUSED ...` and `allen: GENESIS|RESTORED
  ...`; `status` appends `continuity=...`. Limits: the approved note is not a
  Cortex record yet (P06); no Recovery Core, no TPM; aienos#285 not
  triggered; physical disk qualification NOT_RUN (host test
  `test_continuity_service`; QEMU rows 171-173).
- **Resident inference in the hardware service image (aienos#34, gate N3).**
  The one supported hardware inference build is the service image with the
  model from the verified boot disk:

      cargo build --release --target aarch64-unknown-none --manifest-path crates/aienos-infer-kernel/Cargo.toml
      make -C native/kernel full CK_SERVICE=1 CK_HARDWARE_STAGING=1 \
          CK_OWNER_PUBKEYS=<owner public keys file> CK_MACHINE_ID=<machine id file> \
          CK_INFER_LIB=<path to libaienos_infer_kernel.a> CK_FB_HOLD_S=120
      # -> target/native-kernel/full-service-hardware-staging/BOOTAA64.EFI

  It compiles `core/infer.c` with `CK_INFER_DISK_ONLY`: the fw_cfg model
  reader is not compiled in (Makefile `fwcfg_check` greps the image for
  `fw_cfg`, `opt/aienos/model` and the `fw_dma_read` symbol), the model comes
  only from `MODEL.MAP` read by the UEFI stub (`native/boot/efi_model.c`),
  the kernel recomputes the pinned SHA-256 before decoding, and with no map
  it prints `infer: ingest REFUSED (no boot-disk model ...)` and the
  lifecycle refuses `prompt` ("no inference unit"). The Makefile still
  refuses with hardware staging: `CK_INFER_LIB` without `CK_SERVICE` (the
  one-shot probe image), `CK_SERVICE_TEST_KEYS`, `CK_TEST_FWCFG_ARTIFACTS`,
  every other `CK_TEST_*` flag, `CK_RUST_LIBS`, `CK_QEMU_UNSAFE_DMA`,
  `CK_CONSOLE_SESSION` and missing owner files. The QEMU inference images
  (probe, test-keys service) keep their TEST-ONLY fw_cfg source unchanged.
  Staging: `scripts/stage_one_time_boot_ck_service.sh --dry-run|--apply`
  builds the image twice in clean folders, makes `MODEL.MAP` against the
  pinned model, prints every write with its SHA-256 and (dry run) writes
  nothing; `--target-image IMG --extents ...` stages a disk image instead of
  the machine. Proof: `scripts/qemu_ck_service_disk_test.sh` (GATES.md
  SERVICE_DISK rows 200-210: refusals, dry run byte-identical, two cold boots
  on one emulated disk with generated owner files and secrets, two negative
  controls); `--build-only` runs the `make` refusals alone. Limits: QEMU
  only; Spark NVMe, Spark console input and the owner key ceremony NOT_RUN.
- Proof: `scripts/ck_owner_keys_check.sh` (build only, takes the quiet flag)
  checks the refusals, the generator refusals, and builds the hardware staging
  image from the labelled TEST-fixture files in `tests/fixtures/owner/`
  (public bytes only, distinct from every old TEST constant), then greps the
  ELF and EFI. Ends `CK_OWNER_KEYS_CHECK: PASS`. CI runs it.
- Not covered: the untrusted RFC 8032 TEST 1/2 public key constants
  (`artifact/format.c`, used only by the `CK_SEED0B_TEST_ANCHOR` build and the
  host tools) are still present as data in every image, including hardware
  staging; the ordinary loader trusts no anchor.

## 2017 iMac image (AIENOS-KERNEL-0, x86_64)

    make -C native/kernel imac-kernel        # target/native-kernel/IMACKERN.EFI
    scripts/qemu_ck_imac_kernel_test.sh      # QEMU q35 + OVMF, two CPU models, ends AIENOS_IMAC_KERNEL

The first cut of the C kernel core on x86_64, for the owner's 2017 iMac
(iMac18,3, Core i5-7500; hardware facts in
`evidence/imac_boot0_2026-10-16/`). `x86/efi_x86.c` is the UEFI stub (GOP via
`../boot/efi_gop.c`, RSDP, memory map, ExitBootServices) and hands over the same
CHANDOF3 record, checked by `core/handoff_check.c`; `x86/kmain_x86.c` loads its
own GDT, TSS and IDT (`x86/trap.S`), builds its own identity page tables (2 MiB
pages: RAM write-back, everything else uncached, the framebuffer write-combining,
a 4 KiB guard page under the stack), proves the guard page faults and the fault
is contained, measures the TSC against the HPET, runs the local APIC timer
(xAPIC) for 20 interrupts at 100 Hz, starts every other core the MADT lists
(INIT-SIPI-SIPI into the `x86/smp_x86.S` trampoline below 1 MiB; each core loads
the kernel's tables, checks in and halts), then walks PCI (`x86/pci_x86.c`, MCFG
ECAM window; device registers above the identity map are mapped on demand) and
reads every disk without writing. SATA through `x86/ahci_x86.c` (polled, no
controller or port reset, only IDENTIFY DEVICE and READ DMA EXT exist and the
build refuses any other ATA command); NVMe through the shared
`../disk/disk_nvme.c` (the build refuses any nvme_write or nvme_flush call from
`x86/`). Both disk_dev write hooks always refuse, serial numbers and MAC
addresses are never printed, and each disk's GPT is checked by the shared
`dev/disk_part.c` (both copies, CRCs, the AIENOS partition located if present).
The Broadcom BCM57766 Ethernet chip (14e4:1686) gets a polled driver
(`x86/tg3_x86.c`: core reset, bootcode handshake, rings in one DMA block below
4 GiB, PHY autonegotiation; it never reads received frames and the MAC address
is never printed). In the default image it only detects the link: frame
transmit is not compiled in. The last line before the hold is a one-line
`summary:`. It prints `kernel: alive`, holds the screen 60 s and powers off
through ACPI S5.

Before starting PCI drivers, the kernel inventories the firmware's Intel DMAR
table (`core/dmar.c`, `x86/dmar_x86.c`). It reports remapping units, device
paths and reserved DMA regions without accessing remapping registers. PCI
diagnostics include the segment and requester ID. This is discovery only;
DMA confinement remains **NOT QUALIFIED**. Bounds, refusal behavior and the
optional QEMU DMAR test are documented in
[`docs/IMAC_DMAR_INVENTORY.md`](../../docs/IMAC_DMAR_INVENTORY.md).

**Network report: lab-only, opt-in, unauthenticated.** `make -C native/kernel
imac-kernel CK_X86_REPORT=1` builds a separate lab image,
`IMACKERN-LAB.EFI`, that adds `x86/report_x86.c`, `../net/aienos_net.c` and
tg3 transmit. The default `IMACKERN.EFI`, and any resident or service (ALLEN)
build, contain none of it; the link step fails a default image that has a
report symbol. During the hold the lab image sends, every 2 s while the link
is up, an allowlisted copy of the console (`ck_x86_report_filter`: only
complete, printable lines of at most 240 bytes whose key is on a fixed list of
kernel diagnostic keys; everything else, including `memory:`, is dropped
whole, so chat, keys, tokens and credentials cannot enter it), as UDP
broadcast from 0.0.0.0 to 255.255.255.255 port 47031 (TTL 1), in 1 KiB parts
that each start with
`AIENOS-REPORT v=2 auth=none run=<id> part=<i>/<n> bytes=<total>`.
Anyone on the LAN can read or forge these datagrams; the run id and checksum
are not authenticity, so a report is never a receipt, evidence or a command.
Future service diagnostics belong on the authenticated transport with an
explicitly authorized peer. `tools/ck_imac_report_listen.c` (`make -C
native/kernel imac-report-listen`, run as `ck_imac_report_listen 47031 OUTDIR
SECONDS`) says so when it starts, refuses the retired v=1 raw-console format
and rebuilds each run into `OUTDIR/imac-<run>.unauthenticated.txt`;
`scripts/ck_imac_report_test.sh` checks the default and lab builds' symbols,
the allowlist (sentinel lines never leave), the frames and reassembly on the
host.

**Startup: the night sky.** The moment the clock is measured, before anything
is printed, `ui/allen_ui.c` takes the screen and starts the night
(`ui/night.c`): a dark sky fades in over about seven seconds, the brightest
stars first. The sky is the real one (`ui/sky_real.c`, from tables the generator
`tools/ck_sky_gen.c` makes out of the Yale Bright Star Catalogue, Gaia DR3
integrated stellar flux/extinction, OpenNGC and the GW150914 sky map, by `scripts/ck_sky_gen.sh`): 8404
stars to the naked-eye limit at their true places, brightness and colour, the
Milky Way, and the Large and Small Magellanic Clouds, seen as a pinhole camera
would from the point the first black-hole merger heard (GW150914) most probably
came from. Sources, limits and a science audit of every picture and sound:
`docs/ALLEN_SKY_SCIENCE.md`. Two black holes
appear, bending the starlight round them, circle and spiral in on the
inspiral law (separation as t^(1/4), angle as t^(5/8), t the time left),
merge at 12 s in a flash, and a ripple runs out across the sky. From the light
of the merger Allen gathers (`ui/mind.c`): no face and no body, a mind drawn as
light, about a hundred points on and inside a sphere joined by fine threads,
turning slowly round a warm heart, with sparks running along the threads.
The startup text stays off the screen (`ck_x86_console_fb_hold`) until
Allen's window takes over; a panic, or no window, stops the drawing and
brings the text back. Every pixel is worked out from its position and the
time, so a frame redraws only the parts that change, in strips: once the
secondary cores are up, the first draws the frames and the next two draw
strips of them (`smp: ... state=drawing helpers=2`); before that the boot core
draws a frame between console writes. Kernel lines: `screen: night: the sky
made in ...` and `screen: night: N frames ..., average ... ms, slowest ...
ms` (when Allen wakes, 20 s later, and at goodbye).

The canvas uses the GOP mode's native width and height (including 3840x2160),
with a one-to-one framebuffer copy that respects GOP pitch. The Milky Way is
now inverse-projected at every native pixel: no quarter-resolution screen
cache, enlargement or runtime blur pass remains. Its Gaia-derived source is
still a finite angular map, not a native-4K photograph; native evaluation
cannot invent source detail. Catalogue stars and their dynamic twinkles use
the same point-deposition path with at most two native pixels across,
independent of UI scale. Extended soft lobes and twinkle bloom are removed.
Catalogue direction, magnitude and colour inputs remain, but the compact
profile does not conserve the retired broad profile's integrated display
light; bright-channel clipping can change apparent colour. See
`docs/ALLEN_SKY_SCIENCE.md` for the source and display limits.

Paint coverage, fades and AA weigh linear light through fixed transfer
tables, rather than darkening encoded sRGB bytes. Allen's thread widths
scale with the canvas. His points, spark heads/tails and heart stay small
native-pixel cool points; local coverage mixing, rather than additive
accumulation, prevents warm/white interior hot spots at crossings or on
background stars. Placement, connectivity, cadence, spark routing and voice
response remain. Dusk resolves native destination pixels with mips only for
minification, preserving choreography and the exact final-black endpoint.
The fluid layer paints only its published snapshots and anchors its grid
coordinates independently of dirty clips; all changing star-twinkle footprints
are invalidated. Full-frame and incremental draws must therefore remain
byte-identical, including through sustained thinking and speaking transitions.

`scripts/ck_allen_ui_test.sh` also runs `ck_mind_phases_test.sh` and
`ck_sky_sharpness_test.sh`. The mind fixture checks interior warm/white light,
point footprints and visibility on black and the identical galaxy across
replayed activity histories, as well as the unchanged outside annulus.
Warm/white, enlarged-point, hidden-being and exterior-halo controls must fail.
The sky fixture compares every native pixel against direct celestial-map
samples and measures real catalogue-star/twinkle support, rejecting
quarter-grid, static-bloom, dynamic-bloom and invisible-star controls.
Finite host histories and emulator frames are not physical visual acceptance
or proof of every arbitrary runtime history.

`ui/chirp.c` is the matching sound: a quick "blirp", the last 0.6 s of the
gravitational-wave chirp of the same inspiral, raised into hearing (200 Hz
sweeping to 1000 Hz, loudness as pitch^(2/3)), peaking at the merger and
ringing out within 0.3 s: under a second in all, worked out from the formula. The audit measures it against the real signal: the pitch law is real, the sweep, cycle count and ring are not (`docs/ALLEN_SKY_SCIENCE.md`, section 5). `x86/hda_x86.c` plays
it: on each HD Audio controller it stops any DMA the firmware left running, resets the link with
the settle times Linux uses, talks to the codecs through the command rings
(CORB/RIRB, polled), maps every widget of each codec onto the report
(`sound: codec=N w=0x.. ...`), opens every speaker, line-out and headphone
path back to a converter (power, routing, 0 dB, pin drive, EAPD; on Apple
boards the codec GPIOs), and on the first controller with a path runs one
output stream: the chirp from the moment the night has reached, then 8 s of
silence; the stream is stopped once the chirp is over. Intel controllers get
Linux's PCI settings first (`pci_x86.c`). In QEMU the recording of what the
guest played matches the chirp sample for sample, starting at the night's
moment. A Cirrus CS8409 bridge on an Apple board (the desktop test
computer's codec) gets its own set-up instead: its four speaker amplifiers
sit behind it on its own I2C bus, so the driver sets the bridge's defaults,
resets the amplifiers into shutdown, sets the TDM slots and clocks, configures
and switches on each amplifier and synchronises the two converters, following
the sequence davidjo/snd_hda_macbookpro replays from macOS. That sequence only
plays 44.1 kHz 24-bit, so behind a CS8409 the chirp is resampled to that
(`ck_chirp_fill_441`, host tested against the 48 kHz sound). The amplifiers
run 24 dB under the level macOS uses. **Heard on the test computer
(2026-10-18):** all four amplifiers answered (no I2C transfer unconfirmed)
and the chirp played; at 12 dB under it was very loud and the old ten-second
chirp too long, hence the quieter level and the short blirp. QEMU has no
CS8409, so this path is checked only on that machine.
Tests: `scripts/ck_sky_test.sh` (host: the real sky's projection, sidereal time, zenith, timings; pictures in `target/native-kernel-sky/`), `scripts/ck_allen_ui_test.sh` (host; pictures in
`target/native-kernel-allen-ui/`), `scripts/ck_chirp_test.sh` (the sound as
`target/native-kernel-chirp/chirp.wav`); the QEMU kernel test requires the
night line, the drawing cores, the codec mapped through the command rings,
the chirp stream running and its stop. `AIENOS_IMAC_KERNEL_KEEP_DIR=<dir>`
keeps each run's serial log and a `.wav` recording of its sound.

**Microphone (first cut, 2026-10-18).** On the same controller the first input stream records
1 s from the microphone right after the chirp starts, then `ck_x86_hda_step` stops it and prints
what arrived: `sound: microphone recorded 1000 ms: N samples, P% nonzero (first at i), peak,
rms, dc`. Behind a CS8409 on an Apple board the built-in microphone is a digital microphone
wired into the bridge (iMac boards: pin 0x45 to converter 0x23, DMIC2; MacBook Pro boards: 0x44
to 0x22, DMIC1; source: davidjo/snd_hda_macbookpro `patch_cirrus_real84.h`, which replays what
macOS sends): coefficient 0x09 selects it, 0x82 turns on its clock, the pin gets In Enable
(HDA 7.3.3.13) and the converter the stream tag, 44.1 kHz 24-bit. Any other codec gets its
first analogue input converter fed by a microphone or line-in pin. The QEMU kernel test uses
`hda-duplex` (line-in pin 0x05 to converter 0x04) and requires the path, the stream running
and the summary; QEMU feeds silence, so 0% nonzero there is expected. On the iMac the summary
line in the lab report is the receipt for whether any sound comes through; the mainline Linux
driver never got this microphone working, so a silent first run is a real possibility and the
next step is then the bridge's coefficient dump against the macOS replay.

**Device inventory.** The PCI walk prints one `pci: fn bus:dev.fn id=vendor:device class=...` line
for every function it finds, before the drivers claim theirs. On the real machine these lines are
the ground truth for which drivers are still missing (lab report allowlist: `pci:`).

**Rest.** "goodbye", a tap of the power button, or the idle time without a key rests the
computer; it never restarts (the lab image included). First the dusk (`ui/allen_ui.c`): over four
seconds everything on the screen (the stars, the galaxies, the water's light, Allen, his words)
falls in a spiral into a black hole where he was, an accretion disk glows as the light arrives and
dims again, and the horizon closes to pure black; then nothing moves and nothing is typed. Once it
is black, the display panel is switched off through the existing guarded AMD path
(`x86/panel_x86.c`, below). Only after a successful panel-off does the computer rest
with any-key or power-button wake: the panel comes on before the sky returns, and
the waking key is not typed. If the panel is unsupported or its off sequence fails,
the kernel instead requests full ACPI S5 power-off; a cold power-button start is
then required, not an arbitrary key. It never substitutes black pixels for panel-off.
The normal display-off transition waits for a published, fully presented black
frame, not merely the animation clock reaching its dark state; helper strips
must finish first. The existing bounded watchdog still forces display-off if
the renderer stalls, and logs that the picture did not reach black.
The button is read by polling the PM1 status register named in the ACPI FADT
 (bit 8, PWRBTN_STS; its interrupt enable is cleared so
the firmware never sees the press); the QEMU keyboard test drives it with the monitor's
`system_powerdown`. Without a keyboard there is no window and no rest: the screen holds
CK_X86_HOLD_S seconds, then ACPI S5 (the lab image: restart into its loader).

**Panel off (`x86/panel_x86.c`, `CK_X86_PANEL_OFF=1`, the default of `make imac-kernel`).**
Painting the screen black leaves the backlight lit, so while Allen rests the panel itself is
switched off through the display chip's panel power sequencer, the way its firmware does at
power-off, and switched on again at wake before the picture returns. Only on the chip it was
written for: an AMD VGA controller (vendor 0x1002, class 0x0300) with a Polaris device id
(DCE 11.2; the 2017 iMac's Radeon Pro 570/575/580 is 1002:67df rev c5 at 01:00.0; which of the three is not yet decided), whose VRAM aperture
(BAR0) holds the firmware's framebuffer and whose 256 KiB register aperture (BAR5) answers.
Registers (Linux `dce_11_2_d.h` / `dce_11_2_sh_mask.h`, MIT, index times 4 from BAR5):
`LVTMA_PWRSEQ_CNTL` 0x481b (`LVTMA_PWRSEQ_TARGET_STATE` 0x10, `LVTMA_DIGON` 0x10000,
`LVTMA_BLON` 0x1000000 and their `_OVRD` bits), `LVTMA_PWRSEQ_STATE` 0x481c (`DIGON` 0x2,
`BLON` 0x8, `DONE` 0x10), `BL_PWM_CNTL` 0x4820 (duty 0xffff, `BL_PWM_EN` 0x80000000),
`BL_PWM_GRP1_REG_LOCK` 0x4823. Off: the backlight PWM duty to 0 (locked write, wait for the
update to land), then the sequencer's target state to off, which drops BLON, waits the panel's
power-down delay as the firmware programmed it (`LVTMA_PWRSEQ_DELAY1/2`) and drops DIGON; the
kernel waits for `PWRSEQ_STATE` to say done. On: the reverse, then the PWM duty restored, then
the picture. If the firmware left the override bits set instead, the BLON and DIGON pins are
driven directly (50 ms between backlight off and panel off, 200 ms between panel on and
backlight on). Every write is read back and logged (`screen: panel off: PWRSEQ_CNTL 0x... ->
0x...`); a readback that differs or a wait that runs out (all TSC-bounded, 3 s for the
sequencer) stops the sequence and logs the failure. Unsupported panel hardware,
disabled panel control, or any failed off sequence selects the existing ACPI S5
path after the black frame. No Intel backlight register is guessed: the recorded
iMac18,3 display controller is AMD 1002:67df, and only the existing AMD register
allowlist and framebuffer-ownership guards are used. QEMU has no such display
controller, so its exercised path is S5. Physical backlight-off and reversible
key wake remain NOT_RUN until observed on the actual iMac.

**Keyboard and Allen's window.** `x86/xhci_x86.c` drives a wired USB boot
keyboard (polled xHCI, through USB 2.0 hubs). When one is ready, Allen wakes:
the person is his guest. A quiet glass bar comes up at the bottom for what
they type; on Enter the line rises from the bar into him, he thinks (he draws
in, turns faster, sparks cascade), then answers under him word by word while
the galaxies shimmer with his voice, all in Allen's own stroke typeface
(`ui/allen_font.c`, every printable ASCII character as pen strokes,
rasterized with integer math). Nothing else is on the screen unless it is in
use: the bar fades when the conversation rests (a key brings it back), his
words fade once read, and the notes map (a small constellation) shows only
when a note is saved or handed back. Each Enter gets a local answer from
`svc/allen_reply.c`: "Hello Allen" gives "Hello Humanity."; it also knows small
talk, your name and his own (both kept in his memory, below), facts about the
machine, how long it has been awake and sums said in words ("what is 12 times
4"); anything else goes to the far-off worker when a courier is ready (below)
and otherwise gives "I don't know that yet." Answers print as `allen: <answer>`
on the console. The
window has no countdown to power-off. "goodbye", a tap of the power button, or
10 minutes without a key (`CK_X86_IDLE_S`, default 600) rests the computer: the
screen goes dark, then the guarded AMD panel-off is attempted. If it succeeds,
the three drawing cores rest in MWAIT (the deepest of C6, C3 and C1 that CPUID
lists), and the boot core halts between keyboard polls every 20 ms on a one-shot
LAPIC timer. Any key (not typed) or another tap wakes him. If panel-off is
unavailable or fails, the computer powers off through ACPI S5 instead.
Awake, the hold loop polls devices at most once every 0.5 ms by the TSC, not on
every pass: the earlier device-read storm starved QEMU after keyboard replug.
Holding the button 4 s remains the chipset's hard off. On the iMac the
processor's own energy counter (RAPL) gives
`power: processor X.Y W on average over N s awake|asleep` at each sleep and wake
(never under a hypervisor). Unplugging the keyboard and plugging it
(or another) back in works: the driver watches the keyboard's root port, prints
`keyboard: unplugged`, releases the old device and runs discovery again, then
`keyboard: ready` once more (the QEMU test does exactly this from outside the
guest). A keyboard plugged in for the first time after start-up is not found: the
controller is halted when no keyboard was there at start. The conversation stays on the computer: `keyboard_line:` and `allen:`
are off the lab report's allowlist, which carries only `screen: Allen
answered (N characters in, M out)` and the far side's length lines (below).
Without a keyboard or the memory the text
screen and its 60 s hold stay. Tests: `scripts/ck_allen_ui_test.sh` (host) and
`scripts/qemu_ck_imac_keyboard_test.sh` (QEMU, types "Hello Allen", keeps a
screenshot as `target/native-kernel-imac-keyboard/allen-qemu.png`). What Allen
learns and the notes it keeps are saved in its section of the disk
(`svc/allen_memory.c`, `x86/memory_x86.c`) and come back after a restart.

**The water.** Under the sky, round Allen, there is water: a fluid simulation
(`ui/flow.c`, Jos Stam's stable fluids as in his GDC 2003 paper, in 16.16 fixed
point on a 64 by 36 grid, stepped every 16 ms) carrying ink that is lit where it
moves, cool blue when still and warm white when fast. Idle, it turns slowly and
breathes; while a line is typed a current runs from the bar up to him; while he
thinks a vortex deepens over four seconds and ink wells up in a ring round him;
each word he says pushes a ring out; asleep it stills and clears. Every change
of state is a blend of eased weights over 0.7 s, the dusk takes 4.2 s (see
`docs/ALLEN_PRESENCE_RESEARCH.md`, section 3.3) and the dawn 0.9 s, so nothing on
the screen cuts. Cost, measured by the host window test:
a 3840 by 2160 frame takes 18.1 ms on average on one host core (9.2 ms before
the water, run of 2026-10-18); the kernel draws with three cores in strips.

**His name and yours.** The first time Allen meets someone he says he has no
name yet and asks what they will call him; the next word typed becomes his
name (greeting him as Allen settles it as Allen). "call yourself Nova" or
"your name is Nova" renames him any time; "what is your name" and "who are
you" answer with it; the bar's hint reads "Speak to Nova". "my name is Drake"
tells him the person's name. Both are kept in his memory unit
(`svc/allen_memory.c`, format version 3; a version 2 unit still reads, with no
name of his own) and come back after a restart. "forget everything" keeps his
own name. Host test: `scripts/ck_allen_ui_test.sh` (the "self:" checks) and
`scripts/ck_allen_memory_test.sh`.

**Asking a far-off worker.** Allen is not a language model and does not become
one. When a line is beyond what he knows by heart and a courier is ready, he
writes the job himself (`svc/allen_intent.c`): what is wanted (make, explain,
do or chat, read off the line's first words), who he is, whose computer it is,
and the shape the answer must take (a few short plain sentences in his voice,
no lists, no headings), then keeps thinking until the answer comes back and
says it as his own; after 30 s without one he says no answer came. The courier
is one line each way on a wire (`svc/courier.h`). On this kernel the wire is
the second serial port (`x86/courier_x86.c`: COM2 at 0x2f8, a polled 16550 at
115200 8N1; a `HELLO` call is answered with `HI`, so it does not matter which
side was up first), found at start or not (`courier: a
second serial port answers ...` or `courier: no second serial port; Allen
answers from this computer alone`), then `courier: ready, the far side said
hello`. The far end is `crates/allen-courier`, a Rust relay with no
dependencies beyond the standard library: it carries each job to a chat server
(the OpenAI chat shape, `127.0.0.1:18091` by default), keeps the whole answer
in a work folder and hands Allen at most 150 characters cut at a sentence end,
in plain letters (typographic dashes and quotes become plain ones, accented
letters lose their accents, anything else the typeface cannot draw is dropped;
the kernel drops any stray byte too); it prints lengths and timings, never the
words. The lab image also carries the Ethernet courier
(`x86/courier_udp_x86.c`): numbered UDP messages, acknowledgements and retries.
This laboratory transport does not authenticate its peer. Its boot identifier
and checksums do not establish who sent an answer. Device DMA confinement
(#411), authenticated transport integration and physical round-trip evidence
remain prerequisites for a qualified network service. The lab report sees `screen: Allen asked
the far side (N characters in, M in the job)` and `screen: the far side
answered job N (M characters out)` or `... did not answer job N`: lengths only.
Test: `scripts/qemu_ck_allen_courier_test.sh` (QEMU with COM2 on a Unix socket,
the relay on it and the model server on this machine; NOT_RUN without one;
screenshot `target/native-kernel-allen-courier/allen-far-side.png`). A machine
without a second serial port gets Allen as before, answering alone.

`scripts/ck_courier_udp_test.sh` runs the kernel's Ethernet courier against the
Rust relay on loopback, including two levels of packet loss, two busy replies
and a delayed reply. Its model fixture is compiled with `rustc` from
`x86/tests/courier_model_fixture.rs`; the test needs no interpreter. Set
`ALLEN_MODEL=127.0.0.1:18091` to add a real-model exchange after those cases.
These host tests exercise the protocol, not the physical Broadcom card or
authenticated networking.

The addressless kernel sends to limited broadcast (`255.255.255.255`), not
the LAN's directed broadcast; Linux can reject source-zero directed
broadcast before UDP input. The relay may answer on the LAN's directed
broadcast, which Linux also loops back to its own bound UDP socket. Before
creating or updating a session, the relay accepts only kernel-origin
`HELLO`, `JOB` and `GOT` messages. Its own `HI`, `ACK` and `LINE` packets must
not replace that session's answer destination with the relay's address.
`scripts/ck_courier_udp_frame_test.sh` additionally runs the real kernel
Ethernet producer/parser against the relay in an isolated Linux network
namespace, with source-zero requests and directed-broadcast answers. It
checks actual checksummed LINE/GOT frames, duplicate acknowledgement and
malformed/wrong-run refusal; withholding GOT must provoke retransmission.
This host-frame check does not emulate or qualify the Broadcom hardware.

For conversational recall, the relay sends complete `asked: ... / said: ...`
exchange notes as earlier user and assistant turns, oldest first. These remain
short saved excerpts, not a full transcript; the model is told they may be
incomplete. Other notes stay in the memory prompt. This reconstruction uses
the notes supplied by Allen on each request, so it needs no new relay-side
session store. The live QEMU test checks that the answer to "what did I just
ask you" recalls both the form and topic of the earlier request, separately
from checking that the note was transported.

**The canvas: the model decides what is painted.** Besides the words, the
model writes a few short scene lines (`svc/allen_scene.c` checks them one by
one: at most three things, bounded lengths, plain letters; anything it does
not understand is dropped, never trusted) and `ui/canvas.c` paints each in
Allen's own style round him, with no windows, menus or boxes: a note, a list,
a number, a timer, a person, a quote, a place for a picture or a video, and a
mood for the sky and the water. The system prompt is
`../../crates/allen-courier/scene_prompt.txt`; `allen-scene` asks the model
the questions in `../../evidence/allen-canvas/questions.txt`. The hook into the
window is small (`ck_allen_ui_scene()` in `ui/allen_ui.c`); the courier does
not carry scenes to the machine yet, and none of it has run on the real
computer. Host test: `scripts/ck_allen_canvas_test.sh` (kernel build rules, a
300000-input fuzz under the sanitizers, frames at 3840 x 2160, and
`--replies DIR` paints real model answers; pictures in
`target/native-kernel-allen-canvas/`). Grammar and reasons:
`../../docs/ALLEN_CANVAS.md`.

**The setup wizard.** The first time Allen wakes on a computer he asks one thing in
his own window: what would you like this computer to do (play games smoothly, run
an AI model here, or something else in your own words). He then lists what he found
(PCI functions, USB, processors, disks, screens, `svc/setup_inv.c`) in plain words
and asks one consent, "set it up". Games: the graphics driver state, the screen's
mode and refresh, the processor profile, and what he will and cannot tune yet; a
number is only shown when it was measured (QEMU shows none). AI model: the graphics
memory he found, which models fit, then the model pick (`svc/setup_wizard.c`): on
this computer first (Allen's own 27B + 30B pair if it answers, else an installed or
running model), then peers, then "connect my own" (OpenAI-style or Anthropic
service, key typed once). One found model is offered with "Use it / Choose another"
and starts by itself after 10 s; two or more are never picked for the person. The
key stays in kernel RAM, is tested with one request through the courier relay
(`KEYTEST` in `crates/allen-courier`, http only: the relay has no TLS and we do not
hand-roll it, so https services are refused out loud) and is wiped; only
`llm.conf` (`kind`, `base_url`, `model`, `key_env`, never the key) and a consent
record are kept, with the wizard's place, in fixed records inside Allen's memory
section (`svc/setup_store.c`), so a power cut resumes where it stopped and a
finished wizard is not shown again ("set up again" reopens it). Every screen can be
skipped with Escape. Arrow keys and Tab come from `dev/usb_hid.c`. The window side
is `ck_allen_ui_panel` in `ui/allen_ui.c` (lines easing in, a row of pill choices,
hint, typed-secret dots). The host description (fw_cfg `opt/aienos/host`: inventory
a launcher knows and the guest cannot) is read only by the launcher image
`make imac-kernel CK_X86_HOSTDESC=1` (`IMACKERN-HOST.EFI`); the default image
carries no fw_cfg code and the build checks that. Tests:
`scripts/ck_setup_wizard_test.sh` (host, three machines: a games PC, an AI PC, the
Spark), `scripts/ck_allen_ui_test.sh` (panel pictures `wizard-*.png`) and
`scripts/qemu_ck_allen_wizard_test.sh` (QEMU end to end: power-cut resume, the
three model sources, the key in no log, relay file or disk; screenshots in
`target/native-kernel-allen-wizard/`).
The wizard opens on its own only at Allen's first start with his memory section,
or to resume a setup saved part way (a power cut); a later start with nothing
saved says `wizard: not shown, start N and nothing half done`, and with nowhere to
keep its record it says `wizard: not shown, nowhere to keep it this start`. Either
way Allen's window is left to him, so the wizard never takes it over at every start.

**Code from the Spark.** Any coding request in Allen's chat ("make me a
calculator", "write a snake game in python") is answered at once with "I need to
code that. Let me send it off to the Spark." and goes over the courier as job
`code` (a follow-up such as "now add a square-root key" while code is on the
screen goes as `code-more` with the code before). Allen keeps thinking while he
waits, says plainly at 20 s that longer code takes a while, and gives up out loud
after 30 s without a word from the relay (which says it is still working every
2 s). The relay (`../../crates/allen-courier`, prompt `code_prompt.txt`) asks the
model once, not streamed, waits exactly Retry-After on a busy answer, and sends
the code in numbered pieces of 128 bytes with the whole length and a CRC-32
(`CODE <job> <i>/<n> <bytes> <crc32> <language> <piece>`, then `SAY <job> 1/1
<sentence>`); a missing or spoiled piece makes the kernel ask once more (`AGAIN
<job>`). Limits: 32768 bytes, 800 lines. The code shows beside Allen's words in
fixed-width text with line numbers; long lines wrap; Up, Down, Page Up, Page
Down, Home and End move it and Escape puts it away (`svc/allen_code.c`,
`ck_allen_ui_code` in `ui/allen_ui.c`). Report lines: `allen-code: asked the
Spark (N chars)`, `allen-code: code shown, M lines in S ms`. Tests:
`scripts/ck_allen_code_test.sh` (host: parser, short and long recorded answers,
a lost piece, a spoiled piece, the wait clock), `scripts/qemu_ck_allen_code_test.sh`
(QEMU keystrokes with recorded answers, the code read back over serial, the relay
stopped part way; `ALLEN_CODE_LIVE=1` asks the real model for a Python and a C
calculator and prints the times).

**Starting from its own disk.** When the network loader (`../boot/net_loader.c`)
starts a kernel it hands it a `struct nb_pass` (`../boot/netboot_proto.h`) as
LoadOptions: the processor clock when the loader began, and where the verified
object (signed manifest + image) lies in the loader's memory. The kernel checks
the image's SHA-512 against the manifest again and installs it on the disk that
holds Allen's section, in a 256 MiB EFI System Partition it adds in unused
space (`dev/gpt_add.c` `ck_gpt_add`; never on a disk that already has another
system's), as `\EFI\BOOT\BOOTX64.EFI` on a FAT32 volume it writes itself
(`dev/fat_boot.c`, Microsoft FAT32 specification 1.03). Two slots: the new
kernel goes into the one the firmware does not start, then a single directory
sector switches over, so a power cut leaves the old kernel or the new one,
never a mix; the same kernel again writes nothing. After that the firmware
starts AIENOS from the disk with no stick and no network. Every start prints
`boot_time:` (milliseconds since power on: firmware, loader, kernel, Allen's
window) and `boot_disk:` (what the boot section holds). Tests:
`scripts/ck_fat_boot_test.sh` (host: power cuts at every sampled block,
`fsck.fat` and mtools as outside readers) and
`scripts/qemu_ck_imac_bootdisk_test.sh` (QEMU: install over the network, then
start from the disk alone, then a second kernel into the other slot).

Output: the GOP framebuffer
(`core/fbcon.c`, lines from before ExitBootServices replayed onto it) and COM1
when a 16550 answers there (QEMU; the iMac has none). Shared, unchanged:
`core/fbcon.c`, `fmt.c`, `acpi.c`, `handoff_check.c`, `mm/frames.c`, `heap.c`,
`dev/disk_part.c`, `../disk/disk.c`, `../disk/disk_nvme.c`. The QEMU test runs four
cores and attaches a SATA data disk and an NVMe disk, each with a GPT from
`ck_gpt_image`, and fails unless three cores check in, the kernel finds each
AIENOS partition where the tool wrote it and both disks are byte-identical
afterwards. Not yet: work scheduled on the other cores, x2APIC, NX,
VT-d, receiving on the network, a store. QEMU (no BCM57766 there)
qualifies nothing physical; copy the image to a FAT stick as
`EFI/BOOT/BOOTX64.EFI` and start it from the Option-key boot picker.

## Raspberry Pi 5 image (AIENOS-PI5 step 1)

    make -C native/kernel pi5            # target/native-kernel-pi5/BOOTAA64.EFI (-DCK_PI5=1 -DCK_KLOG=1)
    scripts/ck_pi5_build_check.sh        # build only, ends CK_PI5_BUILD: PASS

The Pi image is the default core image plus board glue (`core/pi5.c`). The
default and QEMU images link none of it: the hooks in `include/ck.h` are weak
and absent, and `scripts/ck_pi5_build_check.sh` checks both images.

**Pi unlock image (PI5-4 cut 1, `make pi5-unlock`, `CK_PI5_UNLOCK=1`).** A
separate image from `pi5-store`: the Store opens only through the ADR 0017
Slot 1 recovery-secret path (`ck_store_pi5_unlock_run` calls
`ck_store_unlock_boot` with the hardware policy). `ck_store_test_keys` and
`ck_store_test_uuid` are not compiled, a TEST-identity key is refused before
any disk access, a Store without a KeySlotManifest (for example a TEST-tier
Store) is refused, a wrong secret fails the K_vol unwrap tag, and every refusal
writes nothing. The secret source is `ck_pi5_unlock_secret` in `core/pi5.c`;
this cut compiles in no source, so the Store stays closed
(`unlock_source: none`). The Makefile refuses the flag with `CK_PI5_STORE`,
`CK_SERVICE`, `CK_HARDWARE_STAGING` and the test flags. Host test:
`make pi5-unlock-test` (`dev/tests/pi5_unlock_test.c`); image checks:
`scripts/ck_pi5_build_check.sh`. Tier label in receipts:
`PI5-UNLOCK-QUALIFICATION`. Not a production image: no TPM, no operator
ceremony.

**Card unlock request (PI5-4 cut 2, `make pi5-unlock-card`, QUALIFICATION ONLY).**
`CK_PI5_UNLOCK_CARD=1` (needs `CK_PI5_UNLOCK=1`, own OUT directory) adds the
one secret source the Pi can have without a TPM or a serial reader: a
THROWAWAY recovery secret in `\EFI\AIENOS\UNLOCK.REQ` on the card (`core/unlock_req.h`,
512 bytes, tier tag `QUALIF-THROWAWAY`). The UEFI stub reads it before
ExitBootServices (`boot/efi_unlock.c`, reads only), the Store stage takes the
secret once and runs the Slot 1 path as if the operator had typed `unlock`
(first-time setup on a blank Store). The boot log says
`unlock_source: card request file (QUALIFICATION ONLY, throwaway secret)`; the
secret is never printed. `pi5-run.sh UNLOCK_REQ=<file>` places the file and
zeroes and deletes it from the card after the run, checking it is gone;
`scripts/pi5_unlock_req.sh` writes a throwaway request (never inside a git
checkout). `CK_PI5_PRODUCTION` is reserved for a future production Pi image: the
Makefile refuses it, so the card source can never be combined with it. A
production image must read its secret from the operator, never from the card.

| Piece | What |
| --- | --- |
| `dev/pi5_wdt.c` | BCM2712 PM watchdog (Linux `bcm2835_wdt.c` registers), armed for `CK_PI5_WDT_S` (15 s) at start, petted at every `ck_set_stage`. A hang resets the board; on the hub the tryboot one-shot flag is already cleared, so it comes back on normal Linux. |
| `core/pi5.c` | Brings up the microSD host with `native/disk/disk_sdhci.c` at a fixed address (the DSDT computes `\_SB.SDC0` `_CRS` in a method the static ACPI scanner does not run; same range as Linux `bcm2712.dtsi` sdio1), clock 200 MHz (`clk_emmc2`), PIO only, so the missing SMMU does not matter. Reads sector 0 and checks the MBR signature. |
| `core/klog.c` | RAM copy of everything printed: the first 192 KiB plus the last 64 KiB. |
| `core/klog_slot.h`, `../boot/efi_klog.c` | The card log slot. Before ExitBootServices the UEFI stub reads `\EFI\AIENOS\KLOG.MAP` (absolute first sector, sector count, nonce) and keeps it only if it checks and exactly one whole disk holds a matching empty slot header there. |
| `ck_pi5_before_reset` | Runs inside `ck_reset` (the normal end and every panic). It re-reads the slot header with the kernel's own SD driver, refuses on any mismatch or if the card size differs from what the firmware saw, writes the log text after the header, flushes, then writes the WRITTEN header last and reads it back. One try only. |

The image cannot boot in QEMU (the SD host and PM addresses are Pi 5 only). It
runs on the hub Pi with the one-shot tryboot runner, which prepares the slot
(one contiguous 2 MiB file, its raw card sector read back and compared before
the map is written) and pulls the log back after the hub returns to Linux.
The runner is `scripts/pi5/pi5-run.sh` (run from the Spark). A Pi build made from a
tree with uncommitted changes to tracked files stamps `aienos_commit: <HEAD>-dirty`
(the default image never does); the runner reads the stamp out of the EFI, logs it in
`run.log`, and says "NOT commit-bound (dirty build)" for a `-dirty` image. Hardware
receipts from earlier runs are in `evidence/pi5_hardware_runs_2026-10-13/`.

## Known hardware risk: disk footprint (CK gate DISK_LAYOUT: NOT_RUN)

**Do not boot the C kernel on a disk holding data until CK gate `DISK_LAYOUT`
is PASS on a forge receipt.** That gate is NOT_RUN (MISSING_IMPLEMENTATION, fix
in progress).

The kernel is partition-unaware. At every boot it writes and restores the last
4 KiB unit of the whole NVMe namespace (`dev/nvme_bind.c:93-127`, called at
`:224`) and uses the first 16 KiB as the anti-rollback anchor
(`dev/disk_layout.h:3,12`; `svc/store_boot.c:164-170`). On a GPT disk those
areas hold the protective MBR, the primary GPT and the backup GPT. Details and
the operator-facing warning: `docs/TRUST-1-OPERATOR-STEPS.md`.

## Operator input and recovery access (C kernel)

Status: **QEMU only.** CK gate `KEYBOARD` (rows 25-32, 30a-30c, 32a and 32b in
`GATES.md`) runs in QEMU; no physical keyboard, xHCI controller or SMMU has
been exercised by this code. Hardware NOT_RUN.

What is implemented (NEXT-PHASE-3 cut 1):

- **Device discovery.** The devices stage finds the USB host controller on
  the PCI ECAM walk by class code 0x0c0330 (xHCI), after ExitBootServices and
  after the bus-master sweep. It gets DMA only through the xHCI DMA fence
  (`dev/xhci_fence.c`): SMMU window first, no SMMU means no DMA. Then
  `dev/usb_kbd.c` resets the controller, finds the first connected port with
  a USB HID boot keyboard, addresses it, configures its interrupt IN
  endpoint and polls it. Polled only: no interrupts, no scheduler, no Store.
- **Key decode** to one fixed US keymap (`dev/usb_hid.c`): every printable
  character a US keyboard types, with and without Shift (letters, digits and
  `!@#$%^&*()`, space, and the symbol keys with their shifted pairs, `?`
  included), plus Enter, Backspace, Escape. Function keys, arrows, Tab and the
  non-US key are ignored. A held key produces one event, not a repeat. The
  console's commands match whole words only, so symbols can only make an
  unknown command; Allen's facts keep letters, digits and spaces.
- **Line input** with echo and a bound of 64 characters. One key past the
  bound makes the whole line fail closed: at Enter it is refused
  (`keyboard_line: overflow (line refused: N keys typed, limit 64)`) and never
  run. The Rust shell drops the extra keys instead; this is the one deliberate
  difference. The line feeds the same six commands as the Rust shell: `help`,
  `mem`, `el`, `report`, `uptime`, `exit`.
- **Recovery-access hook.** Right after the keyboard is ready the kernel
  prints `recovery_access: waiting 5000 ms for an operator key (r = recovery
  console; any other key or no key = normal boot)` and records the first key:
  `r` gives `recovery_access: choice=recovery reason=key-r`, any other key
  gives `choice=normal reason=other-key`, no key gives `choice=normal
  reason=timeout`. Without a usable keyboard it prints `recovery_access:
  unavailable (no operator keyboard: xhci=<state>); choice=normal
  reason=no-keyboard` and does not wait.
- **Recovery console: a stub.** On `r` the xHCI is halted and fenced, then
  the NVMe and virtio-net DMA are released, then the screen shows
  `recovery_console: STUB ...`, `recovery_console: identity
  build_sha256=<64 hex> commit=<commit> (image build identity; not an owner
  or machine identity)` and `recovery_console: halted (no device DMA live;
  power-cycle to leave)`. The digest is SHA-256 over
  `AIENOS-CK-RECOVERY-IDENTITY-V1`, one zero byte, and the commit the image
  was built from, so anyone can recompute it from the commit. The CPU then
  stops for good. It never touches the disk and has no commands.

Bus. The Rust kernel and both QEMU gates use a PCI xHCI controller
(`qemu-xhci`, class 0x0c0330) with a `usb-kbd` attached. The C driver looks
there first, then at the ACPI platform controllers (below).

**The Spark's USB controllers are not on PCI** (MEASURED under Ubuntu on
2026-10-05, read only: no PCI function has class 0x0c03). They are six ACPI
platform devices, bound by Linux `xhci_plat_hcd` through the ACPI id
`PNP0D15` (an xHCI controller). The DSDT describes each one as
`Device (USBn) { Name (_HID, "NVDA8000") Name (_CID, "PNP0D15") ...
Method (_CRS) { Name (RBUF, Buffer () { Memory32Fixed ... }) Return (RBUF) } }`
(`_DSD` names a MediaTek xHCI, `xhci-nvidia-mediatek-host`). The DGX Spark
hardware guide lists four USB Type-C ports on the rear panel and is silent on
the controllers behind them. Which controller serves which rear port is
UNKNOWN.

Platform xHCI discovery (NEXT-PHASE-3 cut 2). The fence now tries the PCI
xHCI first (QEMU, and any machine with one) and, when there is none or it
has no keyboard, the ACPI platform controllers:

- **Walker** (`core/acpi_dev.c`, `core/acpi_platform.c`): a bounded static
  scan of the DSDT (from the FADT) and every SSDT in the XSDT for `Device`
  objects with `_HID` or `_CID` in `NVDA8000`, `NVDA8001`, `PNP0D10`,
  `PNP0D15`, reading the first memory range of a constant `_CRS` template.
  Nothing is executed: `_STA` is not evaluated. A table with a bad
  signature, a bad checksum or a length past what is mapped is refused and
  counted (`acpi_scan: tables=N refused=R first_refusal=E devices=M`).
  Positive control on every boot: the Arm PL011 UART id `ARMH0011`.
- **SMMU stream by ACPI name** (`ck_dma_confine_named`, `core/smmu_svc.c`):
  the IORT named component whose name ends in the device's name gives the
  stream id, as the IORT root-complex mapping does for the NVMe and the PCI
  xHCI. Two components with that name, no single-id mapping, or a stream
  behind an SMMU other than the one this kernel drives (`OtherSmmu`) are
  refused: the controller is left halted and gets no DMA.
- **Per controller** (`dev/xhci_fence.c`): map the range, halt whatever the
  firmware left running, ask for the confined window, run the same keyboard
  phase, then halt, reset (HCRST) and return the stream to abort. A platform
  controller has no bus-master bit; the SMMU stream is the gate.

Spark firmware tables (MEASURED, read only with `sudo cat
/sys/firmware/acpi/tables/<name>`; acpidump and iasl are not installed;
decoded by `tools/ck_acpi_scan.c`, which runs the kernel's own scan; Linux
names and SMMU placement from `/sys`):

| Linux device | ACPI name | `_HID` / `_CID` | main MMIO (`_CRS` first range) | IORT stream | SMMUv3 |
| --- | --- | --- | --- | --- | --- |
| NVDA8000:00 | `\_SB_.USB0` | NVDA8000 / PNP0D15 | 0x1db60000 + 0x7800 | 0x0 | 0x13800000 (first in IORT) |
| NVDA8000:01 | `\_SB_.USB1` | NVDA8000 / PNP0D15 | 0x1db90000 + 0x7800 | 0x1 | 0x13800000 |
| NVDA8000:02 | `\_SB_.USB2` | NVDA8000 / PNP0D15 | 0x1dde0000 + 0x7800 | 0x2 | 0x13800000 |
| NVDA8000:03 | `\_SB_.USB3` | NVDA8000 / PNP0D15 | 0x1de10000 + 0x7800 | 0x3 | 0x13800000 |
| NVDA8001:00 | `\_SB_.USB4` | NVDA8001 / PNP0D15 | 0x1d860000 + 0x7800 | 0x4 | 0x13800000 |
| NVDA8000:04 | `\_SB_.USB5` | NVDA8000 / PNP0D15 | 0x1d870000 + 0x7800 | 0x5 | 0x13800000 |

Each `_CRS` has three more memory ranges (+0x8000 size 0x100, a 0x4000 or
0x5000 block, and a 0x18-byte register window), an interrupt and a GPIO
interrupt; the walker takes only the first, the xHCI register block that
Linux `xhci_plat_hcd` maps. Table digests are in
`tests/fixtures/spark_xhci_acpi_expected.txt`.

**What the Spark will print** (INFERRED from those tables, high confidence;
hardware NOT_RUN): the walker finds all six controllers, and every one is
then **denied** with `dma_gate: xhci USBn denied (SmmuNotReady), controller
left halted`. The reason is not USB: the kernel's SMMU service refuses the
Spark's IORT as a whole (`smmu: IORT malformed, SMMU unusable (fail
closed)`), because the first SMMUv3 (0x13800000, the one behind the USB
controllers) has 15 PCI root-complex mappings (the service keeps at most 8)
with stream ids up to 0xfffff (its linear stream table holds 0x0-0xfff).
The same refusal already applies to the NVMe on the Spark. The USB streams
themselves (0x0-0x5) would fit. So on the Spark this cut still gives
`keyboard: unavailable (no keyboard on any platform xHCI)` and normal boot,
now for a stated reason, and fails closed. Making the SMMU service accept
the Spark IORT (a two-level stream table, more root-complex mappings) is
the next cut. Also UNKNOWN until a hardware run: whether the MediaTek glue
around each controller is powered and clocked when the kernel reads its
registers (Linux handles that in firmware `_STA`/`_PS0` and its own glue
driver; reading a powered-down block could fault), and whether
`ck_mm_mmio_try_map` accepts these ranges. The exact expected lines are in
`tests/fixtures/spark_xhci_acpi_expected.txt` (KEYBOARD row 32b, NOT_RUN).


### Service lifecycle serial input (native v1 N1, aienos#328)

Status: **host-tested 16550, QEMU PL011.** `GATES.md` rows 167c-167f. Hardware NOT_RUN.

- **One console table.** The boot stub picks the console UART from the ACPI SPCR once (`ck_console_set_uart`); output and operator input both use it. PL011/SBSA (types 0x03, 0x0d, 0x0e: QEMU virt) and 16550 (types 0x00, 0x01, 0x12). The DGX Spark's SPCR (read on the machine 2026-10-12) is type 0x01, a 32-bit dword-spaced 16550 at 0x16A00000; before this cut the kernel refused that type, so by the code (not observed on the machine) it had no serial console on the Spark at all.
- **Polled, never interrupt driven.** The lifecycle polls the receiver between its steps and, while the model generates, between generation steps, so `cancel` works. No UART register is written: the line speed is whatever the firmware left (the SPCR says 115200; UNVERIFIED on the machine).
- **Bounded lines.** `core/line_rx.c`: 144 bytes; an overlong line, or one in which the UART reported an overrun, parity, framing or break error, is refused whole and never run.
- **Refusals.** No supported UART: `lifecycle: REFUSED console input unsupported on this platform` and the stop order. Receiver off or not answering: `REFUSED console input not ready`.
- **Operator step.** A serial cable (3.3 V) to that UART and a terminal on the other end. Whether the Spark exposes it on a reachable connector is UNVERIFIED. The local USB keyboard on the service image is a separate cut.

### Console session (Campaign 3 cut C3-1a, QEMU test image only)

Status: **QEMU only.** CK gate `CONSOLE` (rows 146-155 in `GATES.md`). Hardware NOT_RUN. The default image does not contain it.

`make full CK_CONSOLE_SESSION=1` builds a TEST-ONLY image with a final boot stage `console`, entered only after every other boot step. The bounded boot shell above (60 s, idle 15 s) and the recovery window are unchanged. The session then keeps taking input for as long as the operator uses it, with no time limit and no idle exit, until the shell's `exit`:

- **Two sources, one line.** Serial receive (polled, on the UART the console already prints to: PL011/SBSA or 16550 per the ACPI SPCR, `core/uart_rx.h`; the kernel only reads UART registers and never reconfigures the UART) and the USB keyboard both feed the same 64-byte line editor and the same shell. Printable keys only; a 65th key makes the whole line refused.
- **No interleaving inside a line.** The first source that types into a line owns it until Enter. Keys from the other source are dropped and counted in a printed line: `console_input: dropped N key(s) from <source> while <owner> owned the line`.
- **DMA confinement unchanged.** The USB half runs inside a second xHCI fence: SMMU window first, no SMMU means no DMA, and its DMA stays in that one window for the whole session. `exit` halts the controller, turns bus mastering off, returns the stream to abort and prints the same revoke lines. If the keyboard is unavailable, is unplugged (PORTSC connect bit clear) or its endpoint halts, the fence is revoked and serial goes on alone. Only a printable key claims a line, so Backspace on an empty line cannot lock the other source out.
- **No new authority.** The console runs the existing commands only (`help mem el report uptime exit`).

Not yet: any recovery-access change and any hardware run (the Spark's UART and USB are unmeasured by this code; 16550 receive, aienos#328, is host-tested only).

### Physical attended boot: what it will look like

Both prerequisites named in cut 1 now exist: ACPI discovery of the Spark's
platform xHCI controllers (above) and a staging command for the full
hardware image, `scripts/stage_one_time_boot_ck_full.sh` (`--dry-run` prints
every command and touches nothing; `--apply` builds `make full
CK_HARDWARE_STAGING=1` with the operator's own owner files, copies it to
`\EFI\AIENOS\aienos-ck-full.efi`, and sets BootNext once; it never
reboots). The B7b probe boot has its own variant,
`scripts/stage_one_time_boot_ck_b7b.sh` (adds `CK_B7B_PROBE=1`, builds
twice and refuses on a digest mismatch, stages `aienos-ck-b7b.efi`). Do not run it yet: until the SMMU service accepts the Spark IORT,
the run can only show the fail-closed deny lines above, not a keyboard.
The steps below are the procedure that run will follow, so it can be
reviewed now.

Ground rules: nothing here changes firmware settings, Secure Boot keys or the
TPM. The boot is one-time (BootNext): the next restart returns to Ubuntu on
its own. If anything looks different from "What you should see", write down
or photograph the screen and hand it to the orchestrator. Do not retry or
improvise.

1. **Plug in.** At the Spark, with Ubuntu running:
   - the HDMI monitor on the rear HDMI port (the kernel writes to the screen;
     there is no other console you can see);
   - a plain wired USB keyboard in one rear USB Type-C port (use a USB-A to
     USB-C adapter if needed; a wireless or Bluetooth keyboard will not work,
     and a keyboard with a built-in hub may not);
   - the AIENOSRECOV recovery stick, so the firmware's recovery entry
     (Boot0004) is there if anything goes wrong.

   *You should see:* nothing changes on screen yet.

2. **Stage the one-time boot** with `scripts/stage_one_time_boot_ck_full.sh`,
   `--dry-run` first, then `--apply`, with `AIENOS_OWNER_PUBKEYS` and
   `AIENOS_MACHINE_ID` set to the operator's own files (the orchestrator
   names them for that run; the TEST-FIXTURE files are refused). *Changes:* it copies the
   image to the EFI system partition and sets BootNext once; it never
   reboots. *You should see:* the dry run's list of commands, then the apply
   ending without `FAIL`.

3. **Restart** with `sudo reboot`. Do not touch the keyboard until the screen
   shows AIENOS lines (white text on black, starting with the boot report).

4. **Normal boot (do nothing).** Watch for, in order:
   `xhci_acpi: 6 platform controller(s)`, `xhci_plat: USBn try ...`,
   `dma_gate: xhci USBn granted (Confined) smmu=0x13800000 stream=0xn`,
   `keyboard: ready (port P, slot S, endpoint 0x81)`, then
   `recovery_access: waiting 5000 ms ...`. Keep your hands off for those 5
   seconds. *You should see:* `recovery_access: choice=normal reason=timeout`,
   then `keyboard: shell ready` and the prompt `aienos> `.

5. **Type a line.** Type `help` and press Enter. *You should see:* the
   letters appear as you type, then `keyboard_line: help` and `commands: help
   mem el report uptime exit`. Type `exit` and press Enter. *You should see:*
   `keyboard: done (exit)`, `dma_gate: xhci bus master revoked`, and later
   `report_kind: final`. The screen holds the last report, then the Spark
   restarts into Ubuntu by itself.

6. **Recovery boot (second attended boot, staged again as in step 2).** This
   time, as soon as `recovery_access: waiting 5000 ms ...` appears, press
   `r` once. *You should see:* `recovery_access: choice=recovery
   reason=key-r`, `devices: recovery halt nvme=released
   virtio_net=released xhci=released`, `recovery_console: STUB ...`,
   `recovery_console: identity build_sha256=<64 hex> commit=<commit>` and
   `recovery_console: halted (no device DMA live; power-cycle to leave)`. The
   machine then stays on that screen. Photograph it. Then hold the power
   button until the Spark turns off, wait ten seconds, and press it again.
   *You should see:* Ubuntu starts as usual (BootNext was used up).

What success looks like: steps 4 to 6 show exactly those lines, the identity
digest on the photo equals the one the orchestrator recomputes from the
commit, and Ubuntu comes back after each boot with nothing changed on its
disk. Any `report_kind: panic` or `report_kind: fault`, a missing
`bus master revoked` line, or a keyboard that is ready but ignores typing is
a FAIL to report, not to retry.

## Boot sequence

1. UEFI stub: pre-exit report on ConOut, memory map, `ExitBootServices`.
2. `ck_kernel_entry` (firmware EL, firmware MMU): vectors, PSCI conduit
   from the FADT (HVC or SMC, default SMC), own page tables, stack, heap,
   DMA pool.
3. `ck_enter_el1`: EL2 -> EL1h with the MMU already on our tables.
4. EL1: `mmu: enabled`, `mmu_switch: ...` read back from the registers,
   heap check, guard-page self test, GICv3 from the MADT, 200 ms timer window
   (INTID 30, 10 ms period), `kernel: alive`, `kernel_el: EL1h`.
5. Stages, in order, if linked: `ck_stage_devices`, `ck_stage_security`,
   `ck_stage_store`. Each prints `stage <name>: ok`, `stage <name>: FAIL
   rc=<n>` or `stage <name>: not linked`.
6. `report_kind: final`, then PSCI SYSTEM_RESET (WFI loop if refused).

Every reset (final, panic or fault) first calls the stage hook
`ck_stage_quiesce` once, if linked: the devices stage releases a still-live
NVMe controller (NVMe normal shutdown, CC.SHN = 01b then wait for CSTS.SHST
= 10b, bounded 5 s; if it cannot (fatal status, not ready, timeout) it clears
CC.EN and waits up to 1 s for CSTS.RDY = 0, printing `nvme: shutdown
fallback disable ...`; then bus master off; then the SMMU stream back to abort)
and prints `devices: quiesce before reset nvme=none|released-now|already-released`.
The normal store path already released it, printing `nvme: shutdown normal
cc=..->.. csts=.. shst=complete waited_us=N` before `dma_gate: nvme bus master
revoked`. Code: `dev/nvme_shutdown.c` (register sequence, host-tested in
`svc/tests/stage_test.c`), `dev/devices.c`.

A panic prints `report_kind: panic`; an unexpected exception prints
`report_kind: fault` with ESR, FAR and ELR. Both then reset. Before the drop
to EL1, VBAR_EL2 points at a separate fatal-only table (`ck_vectors_el2`):
any EL2 exception reports ESR_EL2/FAR_EL2/ELR_EL2 (`el=2`) from the emergency
stack and resets; it never returns or touches EL1 state. After the stages,
`mm_usage:` prints the stack high-water mark (painted stack) and the heap
low-water free bytes.

## The boot sequence on the screen (L266)

The night the iMac shows at power-on (the stars come out, two black holes circle and merge, a short rising
chirp at the merger, 15.2 s in all) is one shared module, `ui/` (byte-identical to the iMac build `0930240`),
plus `ui/boot_show.c`, which counts every frame against a 60 Hz beat and prints one receipt line:

    boot-show: 1280x800 via virtio-gpu: N frames in 15200 ms, D of 912 slots at 60 Hz dropped, F fps, ...

- `make full CK_BOOT_SHOW=1` (`$(OUT)/full-boot-show/`): the aarch64 image plays it once after the devices
  stage. Screen: the VM's virtio-gpu (`dev/virtio_gpu.c`, the aienos-run window) first, else the GOP
  framebuffer. Sound: the VM's virtio-sound (`dev/virtio_snd.c`), else silent. Both drivers sit on
  `dev/virtio_q.c` and `dev/virtio_bind.c`; their DMA is confined through the SMMU like every other device's,
  and refused without it. Default `CK_BOOT_SHOW=0` keeps the QEMU gates' timing.
- `make imac-kernel CK_BOOT_SHOW=1` (`$(OUT)/IMACKERN-BOOT-SHOW.EFI`): the x86 image plays it after storage on the
  GOP framebuffer (in a VM, virtio-vga), sound on virtio-sound through the same driver (`x86/boot_x86.c`);
  that kernel has no IOMMU yet, so the sound device's DMA is not confined and it says so on the console.
- Shown in the emulator only so far: `scripts/qemu_ck_boot_show_test.sh [aarch64|x86_64] [tcg|kvm]` (screen, receipt,
  screendump, and the captured sound compared sample by sample with the formula). Not yet on the Spark's own
  screen; on real hardware the night uses the GOP framebuffer path, which no gate has exercised yet.
- Host tests: `scripts/ck_boot_show_test.sh`, `scripts/ck_virtio_av_test.sh` (command layouts checked
  against the virtio spec's Linux UAPI copy), `scripts/ck_allen_ui_test.sh`, `scripts/ck_chirp_test.sh`.
- Frame rates, the x86 path and the GPU path: `docs/BOOT_EVERYWHERE.md`.

## Memory map the kernel builds

- RAM (UEFI RAM types with the WB attribute): Normal write-back, RW,
  never executable.
- Kernel image: text read-only + executable, rodata and relocations
  read-only, data and BSS read-write. Nothing is writable and executable.
- Kernel stack 256 KiB (stages need >= 128 KiB) with an unmapped guard page below. An exception taken
  with SP below the stack floor switches to a 16 KiB emergency stack so the
  fault report still prints.
- Heap 64 MiB (`ck_alloc`, zeroed, 16-byte aligned) with an unmapped guard
  page on both sides.
- DMA pool 8 MiB, Normal Non-cacheable (`ck_dma_alloc`, never freed).
- MMIO via `ck_mmio_map`: Device-nGnRE, never executable; panics if the
  range overlaps RAM.
- The frame allocator only hands out EfiConventionalMemory, minus the image
  and the handoff, and minus the RSDP, the XSDT/RSDT and every table the root
  lists (whole pages). Tables, stack, heap and pool come from it and are never
  handed out again.

## Notes for stage code

- `ck_puts` prints the string as given; it does not add a newline.
- `\n` is sent as CR LF on the UART.
- IRQs are masked when a stage starts and are masked again after it returns.
  Use `ck_irq_register` + `ck_irq_enable` + `ck_irq_cpu_enable(1)`.
- Stage code is listed in `stage.mk` and linked by `make full`, compiled with
  `STAGE_CFLAGS` first, then the kernel flags and `-U_FORTIFY_SOURCE`. The
  core defines no `ck_stage_*` defaults; the core-only image is checked to
  contain none and the full image to contain all three.
- NVMe DMA is granted only SMMU-confined: the core SMMUv3 service
  (`core/smmu.c`, `core/smmu_svc.c`; ck.h `ck_dma_confine` /
  `ck_dma_unconfine` / `ck_dma_faults`) finds the SMMU through the ACPI IORT
  and maps only the NVMe DMA region for the NVMe stream. With no SMMU, NVMe
  DMA is refused. `CK_QEMU_UNSAFE_DMA=1` (TEST-ONLY, QEMU only, refused with
  `CK_HARDWARE_STAGING`) builds the unconfined bypass used only when no SMMU
  exists, and prints the `WARNING: UNSAFE NVME DMA BYPASS` lines.
- The IORT stream id of a PCI device is resolved within its PCI segment
  (`ck_dma_confine(segment, rid, ...)`, `ck_iort_stream_id(s, segment, rid,
  ...)`): each root-complex mapping keeps the root complex's
  `pci_segment_number` (IORT root complex node offset 28, ACPICA
  `actbl2.h`), and a mapping of another segment never matches. The parse
  keeps at most `CK_IORT_MAX_MAPS` (32) mappings to the SMMU and refuses the
  whole table beyond that, and refuses two mappings whose input ranges
  overlap in one segment. The DGX Spark's IORT (MEASURED, read under Ubuntu
  2026-10-05) has 15 root complexes, segment n mapping requester ids
  0-0xffff to streams `0x10000*(n+1)` on the first SMMUv3 (0x13800000), and
  segment 15 on the second SMMUv3. Its NVMe (MEASURED: PCI 0004:01:00.0,
  behind that first SMMU) resolves to stream 0x50000 + (bus 1 << 8 | dev 0
  << 3 | fn 0 = 0x100) = 0x50100. The PCI walk itself (`dev/pci.c`) still
  covers segment 0 only, so every `pci_func` carries segment 0 today.
- Stream table format: when the SMMU reports two-level support
  (`SMMU_IDR0.ST_LEVEL == 0b01`), `core/smmu_svc.c` runs a two-level stream
  table (Arm IHI 0070 H.a, `SMMU_STRTAB_BASE_CFG` FMT 0b01, SPLIT 6, L1STD
  section 5.1) of `MIN(SMMU_IDR1.SIDSIZE, 20)` StreamID bits, so stream ids
  up to 0xfffff (the Spark's 0x50100 included) can be granted; otherwise the
  linear table of 4096 entries as before. Every L1 descriptor starts with
  Span 0 (its streams are terminated by the SMMU); the first grant in a
  64-stream span takes one 4 KiB page, fills it with abort STEs, and only
  then publishes the descriptor (Span 7) with a non-leaf `CMD_CFGI_STE`. Boot
  prints `smmu: stream table 2-level log2size=.. split=6 (spans allocated per
  grant)` or `smmu: stream table linear entries=4096`. QEMU 8.2.2
  `iommu=smmuv3` takes the two-level path (log2size=16), so the existing
  QEMU SMMU rows exercise it; the Spark SMMU's ST_LEVEL and SIDSIZE are
  UNKNOWN until a hardware boot reads them (the NVMe working under Linux at
  stream 0x50100 implies SIDSIZE >= 19, INFERRED). `make smmu-l2-mutants`
  (run by `make test`) checks that test_smmu fails against an overflowing
  Span, L1 descriptors valid before any grant, a misaligned L1 table, and an
  L2 page published without abort STEs.

- The kernel writes only inside the AIENOS partition of the boot disk
  (`dev/disk_part.h`, gate `DISK_LAYOUT`). The GPT is parsed read-only:
  protective MBR, primary and backup headers and entry arrays, all CRC32
  checked and required to agree. Exactly one partition must carry the AIENOS
  type GUID `38DAAC89-5EAD-4B40-8B1E-3687A7418061` (on-disk bytes
  `89 AC DA 38 AD 5E 40 4B 8B 1E 36 87 A7 41 80 61`), inside the usable range
  and overlapping no other partition. The Store, torn-slot device and rw probe
  get only that partition's `disk_dev` view, whose every read, write and
  flush goes through `ck_part_xlate` (bounds-checked partition LBA -> disk
  LBA). Otherwise the kernel prints `disk: no AIENOS partition, refusing
  writes` and the Store reports `no boot disk`; there is no whole-disk mode.
  `CK_TEST_DISK_XLATE_BYPASS=1` (TEST-ONLY mutation, refused with
  `CK_HARDWARE_STAGING`) drops the partition offset so the gate can prove it
  catches that. QEMU disk images come from `make gpt-image`
  (`tools/ck_gpt_image.c`); `ck_store_image` also opens only the AIENOS
  partition.

## Differences from the Rust kernel

- `guard_page: ok fault=contained` is an addition: the C kernel proves its
  guard pages fault and that the fault is handled. The Rust kernel has no
  such test.
- M3 (threads, EL0, preemption, placement, IPC) runs in the core before the
  stages (`core/m3.c`, `core/sched.c`, `core/ipc.c`, `arch/m3.S`; host tests
  `tests/test_sched.c`, `tests/test_ipc.c`) and prints the same five lines as
  the Rust kernel, plus an extra `ipc_detail:` line. Differences in how:
  the EL0 return uses SPSR 0x3c5 (Rust 0x5); EL0 entry zeroes all general
  registers and TPIDR_EL0/TPIDRRO_EL0; the EL0 window uses ASID 0 with a
  full TLB flush (Rust ASID 1); context switches save x19-x30 and sp only
  (no FP/SIMD; EL0 FP/SIMD traps). Placement is computed from the MADT; no
  secondary core is started (same as Rust). See GATES.md rows 16-20.
- MAIR adds a Normal Non-cacheable attribute (index 2) for the DMA pool.
