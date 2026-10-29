# Bootable recovery media for Machine 1

> **Operator doing TRUST-1:** follow
> [TRUST-1-OPERATOR-STEPS.md](TRUST-1-OPERATOR-STEPS.md) (Step 4 rebuilds the
> stick, Step 6 boots it and proves the unlock). This page is the technical
> record of what the stick contains and how it was first proven.
>
> **Two places, two sets of tools.** Commands in "Build and verify" run in a
> normal Ubuntu terminal on the Spark. Commands after "Boot the USB once" run
> at the recovery prompt on the stick, which only has the tools listed under
> "What ships". `aien-proof` (the shared test-board tool) is **not** on the
> stick: use it only from Ubuntu.

This procedure builds and exercises the bootable recovery USB for
[issue #17](https://github.com/aien-dev/aienux/issues/17) and TRUST-1 Gate 1.
The physical run on 2026-09-25 booted the USB once, mounted the internal root
and ESP read-only, checked the fallback loader and recovery tools, and
returned to Ubuntu. It made no filesystem or firmware changes. The evidence
is in [recovery_boot_machine1.md](../evidence/recovery_boot_machine1.md).

That historical stick loaded an external, unauthenticated initrd. Its
read-only boot receipt remains valid for that observation, not for
authenticated recovery userspace or for safely presenting offline secrets.
The signed replacement described below is implemented in the builder but
has not been staged, booted or qualified on Machine 1.

**Issue #17 done state:** Machine 1 has booted the USB recovery environment;
the environment mounted the NVMe root and ESP read-only, confirmed repair and
boot-entry tools are present, and the boot was recorded. Actual filesystem
repair and boot-entry mutation are intentionally outside this read-only
exercise.

## What the new builder ships (not yet hardware-qualified)

- `scripts/build_recovery_media.sh /dev/sdX1` accepts an existing, unmounted
  FAT partition only. It refuses the internal NVMe and ATLAS_RECOV, mounts
  the target with `nodev,nosuid,noexec`, and writes through a private mount
  point. An ext4/other Unix filesystem is refused before any mount or write;
  preexisting filesystem symlinks must never be followed as root.
- The new build carries signed shim plus a single **owner-signed standalone GRUB
  EFI image**. The GRUB image contains its menu, the signed production
  kernel and the complete initrd in its embedded memdisk. The menu loads
  `(memdisk)/boot/vmlinuz` and `(memdisk)/boot/initrd.img`; there is no external
  `grub.cfg`, kernel or initrd for a USB editor to replace independently.
  Firmware/shim acceptance of this owner signature requires the exact
  signing certificate to be enrolled and not revoked; `sbverify` on the build
  host alone does not establish that enrollment or a successful physical boot.
- `scripts/build_standalone_recovery_initrd.sh` creates the unsigned RAM
  rescue payload for embedding; its standalone output is **not** an
  authenticated boot artifact and must never be loaded directly from USB.
  It contains these programs (the `BINARIES` list in that
  script, plus gocryptfs):
  - shell and basics: BusyBox (with its built-in commands), `sh`, `bash`,
    `mount`, `umount`, `mkdir`, `cat`, `grep`, `sed`, `sha256sum`;
  - disks: `lsblk`, `blkid`, `findmnt`, `cryptsetup`;
  - repair and boot entries: `fsck.vfat`, `mkfs.vfat`, `fsck.ext4`, `chroot`,
    `efibootmgr`;
  - TRUST-1 Gate 1: `tpm2_pcrread` (startup measurements), `sbverify` (loader
    signatures), `age` (opens the offline spare), `gocryptfs` and
    `fusermount3` (open and close the private storage read-only).

  It also packages the Machine 1 xHCI/HID/USB-storage/NVMe/dm-crypt modules.
  It does **not** contain `aien-proof`, `tpm2_eventlog`, `mokutil`, a
  repository checkout, or any key. Internal storage remains unmounted until
  the operator mounts it.
- The build copies the attended collector into the image at
  `/usr/local/sbin/collect_recovery_boot_evidence`; it does not depend on a
  repository checkout being present in the rescue shell.
- `scripts/verify_recovery_tools.sh` extracts a locally built initrd and
  checks its tools and collector. This proves contents, not boot integrity.
- `scripts/test_recovery_media_security.sh` checks target-type refusal and
  signed-image tamper refusal using throwaway keys and scratch files.
  `AIENOS_RECOVERY_QEMU_EMBED=1` also boots a signed standalone GRUB directly
  under AAVMF Secure Boot with a published snakeoil test key, checks hostile
  external `grub.cfg` files do not override its embedded config, and observes
  firmware refusal after an embedded initrd byte changes. It does **not**
  exercise Ubuntu shim, owner MOK enrollment or physical hardware.
- `scripts/qemu_verify_recovery_media.sh` verifies the RAM payload in a
  zero-disk QEMU kernel boot. It does **not** exercise shim or signed GRUB.

The builder keeps temporary initrd, unsigned EFI and signing intermediates in
a private build directory, removes them on exit, and never puts those files
on the removable medium.

## Build and verify

The owner key must remain on a separate offline signing machine, never on
Machine 1's everyday Linux. On Machine 1, export **public boot inputs only**:
the selected `/boot/vmlinuz-<kernel-version>`, the known Ubuntu shim
`/boot/efi/EFI/boot/BOOTAA64.EFI`, optional MOK manager
`/boot/efi/EFI/ubuntu/mmaa64.efi`, and the exact matching
`/lib/modules/<kernel-version>/` tree with `modules.dep*` metadata. Copy
those files from Machine 1 to a separate **data-only transfer medium** and
then to the offline signer; do not transfer the owner signing key in this
direction or run the signing operation on Machine 1. Record each input's
SHA-256 digest on Machine 1 and compare every transferred copy on the
offline signer before building; a mismatch or unverified source is a STOP.
Install the matching module tree at `/lib/modules/<kernel-version>/` on
the offline signer so `modprobe -S <kernel-version> --show-depends` finds it.
The signer must also have the recovery `BINARIES`, their shared
libraries, `gocryptfs`, `libtss2-tcti-device`, `grub-mkstandalone`, `sbsign`,
`sbverify`, `zstd`, `xz` and a FAT filesystem toolchain. The private signing
key is never transferred back with the public inputs or copied into the
recovery image. The signer needs the prepared USB partition attached when
the script runs.

New temporary foreign build dependencies on this path, measured on the
development host: `grub-mkstandalone` (GRUB `2.12-1ubuntu7.3`) packages
kernel, initrd and menu into an ARM64 PE with embedded memdisk;
`sbsign`/`sbverify` (`0.9.4`) sign and check its Authenticode certificate.
Record the offline signer's exact versions with the build receipt rather
than assuming they match. The forward replacement is an Omega-authored
PE/memdisk builder and signer with independent verification; it is not
implemented in this change.

The preexisting unattended USB and 2026-09-25 receipt predate this
authenticated-image builder. They do not establish that the replacement
boots or that the owner certificate is enrolled. The replacement requires
an attended Secure Boot ON boot and refusal after initrd-only tampering
before TRUST-1 Gate 1 may rely on it. The offline AAVMF test uses a different
trust anchor and does not close that operator/hardware requirement.

```bash
# On the prepared offline signer, after checking transferred input digests:
lsblk -f
bash scripts/test_recovery_media_security.sh
AIENOS_RECOVERY_QEMU_EMBED=1 bash scripts/test_recovery_media_security.sh
bash scripts/verify_recovery_tools.sh
bash scripts/qemu_verify_recovery_media.sh
export AIENOS_RECOVERY_KERNEL=/verified/public/vmlinuz-<kernel-version>
export AIENOS_RECOVERY_SHIM=/verified/public/BOOTAA64.EFI
export AIENOS_RECOVERY_MOKMANAGER=/verified/public/mmaa64.efi
export AIENOS_RECOVERY_SIGNING_KEY=/secure/offline/boot-signer.key
export AIENOS_RECOVERY_SIGNING_CERT=/secure/offline/boot-signer.crt
sudo -v
sudo --preserve-env=AIENOS_RECOVERY_KERNEL,AIENOS_RECOVERY_SHIM,AIENOS_RECOVERY_MOKMANAGER,AIENOS_RECOVERY_SIGNING_KEY,AIENOS_RECOVERY_SIGNING_CERT \
    bash scripts/build_recovery_media.sh /dev/sdX1
```

The builder refuses missing signer inputs and refuses to install an
unsigned GRUB. Its local `sbverify --cert` checks the built image signature,
not actual firmware/shim admission. Confirm the selected partition's
identity and FAT type before invoking it; a mounted or non-FAT target is
rejected. Do not format it as a side effect of this build.

## Boot the USB once

Use the firmware boot menu or set a one-time BootNext. Do not change the
default boot order. The attended run observed the USB as `BootCurrent: 0004`
and `BootOrder: 0001,0003,0004`.

```bash
sudo efibootmgr --bootnext 0004
sudo reboot
```

Expect the AIENOS GRUB menu and the rescue banner `AIENOS Standalone Hardware
Recovery Core` / `Root filesystem: 100% RAM disk`. The shell reports Secure
Boot, PCRs, devices, and firmware entries. The NVMe is initially unmounted.

## Inspect internal storage read-only

The Machine 1 root is ext4 UUID
`d27bfd26-ff30-400e-9eca-9cdf73de9406` on `/dev/nvme0n1p2`. Check and mount it
read-only:

```sh
blkid /dev/nvme0n1p2
fsck.ext4 -n /dev/nvme0n1p2
mkdir -p /mnt/root
mount -o ro /dev/nvme0n1p2 /mnt/root
```

The ESP is vfat UUID `9DA2-3597` on `/dev/nvme0n1p1`. Check and mount it
read-only at `/mnt/esp`:

```sh
blkid /dev/nvme0n1p1
fsck.vfat -n /dev/nvme0n1p1
mkdir -p /mnt/esp
mount -o ro /dev/nvme0n1p1 /mnt/esp
ls -l /mnt/esp/EFI/BOOT/BOOTAA64.EFI
```

`fsck.vfat`, `mkfs.vfat`, `fsck.ext4`, and `chroot` are present for a
separately authorized repair workflow. Do not use repair or format options
during this evidence exercise. `efibootmgr` can list firmware entries; this
exercise only reads entries and does not change efivars or BootOrder.

## Capture the recovery evidence

Run the collector packaged on the USB, not a copy assumed to exist in the
repository checkout. Use the exact mount locations above. Type it as shown,
with no `aien-proof hold` in front: that tool is not on the stick, and an
earlier version of this page wrongly said to use it here.

```sh
env AIENOS_ESP_MNT=/mnt/esp \
    /usr/local/sbin/collect_recovery_boot_evidence /mnt/root
```

The collector is read-only. A PASS checks removable-media boot, Secure Boot
byte readability, internal root mount, ESP mount and fallback loader,
efivarfs presence, and readable boot entries. Preserve its complete output.

For the TRUST-1 Gate 1 round trip (opening the private storage read-only with
the offline spare), use the longer form with the `AIENOS_UNLOCK_*` settings in
[TRUST-1-OPERATOR-STEPS.md, Step 6](TRUST-1-OPERATOR-STEPS.md#step-6-boot-the-recovery-stick-with-secure-boot-on-and-prove-the-unlock-gate-1-attended).

For the 2026-09-25 historical boot, the Machine 1 `aien-proof hold`
around the old stick build was the last ledger event before reboot; that run
had no separate boot ledger event. For the new offline-signed build, do not
claim such a Machine 1 build hold exists. Record the offline signer's
source-input digests, signed EFI digest, signer certificate fingerprint,
host and AAVMF test outputs, and the distinct attended boot/return evidence.

## Return to Linux and complete the record

Exit any chroot, unmount `/mnt/esp` and `/mnt/root`, then reboot or power off.
Confirm Linux returns and record its root/ESP mounts. Record the recovery
`BootCurrent` and `BootOrder`, the return-to-Linux proof, tool results and
Secure Boot observations before and after the boot. Record only test-board
events actually taken from Ubuntu; the old build hold belongs solely to
the 2026-09-25 run. If an abbreviated collector or receipt omitted the
Secure Boot byte or post-return state, say so explicitly; do not infer it.

For the 2026-09-25 run, the recovery entry was `Boot0004* UEFI: USB USB Hard
Drive, Partition 1`; BootCurrent was `0004` and BootOrder remained
`0001,0003,0004`. The root and ESP were mounted read-only, the fallback
loader was present, the listed repair tools were present, and Ubuntu
returned. Secure Boot was observed enabled immediately before the run. The
abbreviated receipt omitted the EFI byte and the supplied return output did
not capture a second Secure Boot observation. See the evidence record for
full values and limits.

## Issue #17 mapping

| Requirement | Evidence |
| --- | --- |
| USB boots Machine 1 into rescue | Recovery `BootCurrent: 0004`, Linux kernel and command line in the evidence record |
| Mount the NVMe root | Root UUID and read-only `/mnt/root` mount |
| Repair `/boot/efi` capability | `fsck.vfat`, `mkfs.vfat`, and `chroot` are present; no repair was performed |
| Restore boot-entry capability | `efibootmgr` is present and boot entries are readable; no entry was changed |
| Record boot and recovery | Attended output and return-to-Linux observations in the evidence record |

The QEMU zero-disk boot and the earlier unattended self-test are separate
supporting evidence. They do not replace the attended mount and return
evidence recorded for this exercise.
