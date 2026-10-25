# Omega boot-image builder

`imac-image.om` writes the whole Allen boot medium with the seeded compiler: protective MBR, a GPT
with one FAT32 EFI System Partition, and `\EFI\BOOT\BOOTX64.EFI` plus `STARTUP.NSH` inside it. No
host partitioning tool, FAT tool or filesystem library is used — every byte, including both GPT
headers with their CRCs, the FAT32 BPB, FSInfo, both FAT copies and the directory entries, is
written by this program.

An unpartitioned FAT "superfloppy" boots on OVMF but is not what Apple firmware enumerates:
it looks for an `\EFI\BOOT\BOOTX64.EFI` on a partition. This builder exists so the attended iMac
trial stages a partitioned disk.

## Build

Two steps, both from this directory:

```sh
# 1. the EFI application, composed in the order that compiles
OMEGA_COMPILER=<seed>/seed/out/omc ALLEN_SRC=<checkout>/native/omega/allen \
  sh build-efi.sh BOOTX64.EFI ff17a936719049af35c793860d246003170ade526bc8da4a421bdaf9cf0dc33b

# 2. the boot medium that carries it
"$OMEGA_COMPILER" < imac-image.om > imac-image
chmod +x imac-image
./imac-image                      # writes imac-boot.img next to BOOTX64.EFI
```

`build-efi.sh` writes `target x86efi`, concatenates the twenty sources in the verified order, pipes
them to the pinned compiler, prints the compiler and output digests, and exits 65 when the digest
does not match the one you passed. The twenty sources are **not on main**: they are on aienux
`feat/allen-imac-competition` (`d4ffde3c`), so point `ALLEN_SRC` at a checkout of that branch. On the
`d4ffde3c` sources with compiler `c99eda24…` it reproduces `ff17a936…` exactly.

`imac-image` exit status: 0 written, 2 input open, 3 input empty, 4 output open, 5 truncate, 9 close
input, 6 sync, 7 close output, 8 a short write. The geometry is fixed (disk 196608 sectors, ESP
2048..196574, FAT32 with 512-byte sectors, 1 sector per cluster, 32 reserved, 2 FATs of 1497 sectors,
191501 clusters, constant volume serial and constant directory timestamps), so the same EFI bytes
always give the same image digest.

## Receipt (first run, 2026-10-25)

| Item | Value |
|---|---|
| builder source | `imac-image.om` sha256 `d76d3852daf026238d22b0d88faedba15338e372e5d423a8e3c5b677fdde5f14` |
| compiler | omega `feat/allen-imac-competition` `c3fb79697`, `seed/out/omc` sha256 `c99eda2465036c0637a444689a65dd7fc736e4fa8a8b4cf3456b0ce1b57f8996` |
| input EFI | aienux `feat/allen-imac-competition` `d4ffde3c`, `target x86efi`, sha256 `ff17a936719049af35c793860d246003170ade526bc8da4a421bdaf9cf0dc33b` (1528832 bytes) |
| output image | `imac-boot.img` sha256 `f0811738c2a30bc93076f18884fb9cdc2c081ae83775d147565352b40a4c85f0`, 100663296 bytes |

Reproducibility is the useful property here, and it is checkable from the repositories rather than
asserted: `build-efi.sh` re-derives `ff17a936…` from the `d4ffde3c` sources with that compiler, and
two runs of `imac-image` on the same EFI produce the same image digest. This deliberately does not
chase byte-identity with the `sgdisk`/`mkfs.vfat`/`mtools` image that preceded it: `mkfs.vfat` takes
the volume serial from the clock and mtools stamps directory times, so that reference is not
reproducible by construction. The acceptance is same inputs, same digest, firmware boots it, and the
payload round-trips.

## Verification performed

Reading tools are used only as independent checkers, never as part of the build:

- `sgdisk -p imac-boot.img` — partition 1, sectors 2048..196574, code `EF00`, name `ALLEN`.
- `sgdisk -v imac-boot.img` — `No problems found` (both CRCs, primary and backup headers agree).
- `fsck.vfat -n <partition>` — clean: 5 files, 2990/191501 clusters, no FSInfo or `..` complaints.
- `mdir -i imac-boot.img@@1M ::/EFI/BOOT` — `BOOTX64.EFI`, 1528832 bytes.
- `build-efi.sh` with `ALLEN_SRC` on `d4ffde3c` and compiler `c99eda24…` — exits 0 against the
  expected digest `ff17a936…` (run 2026-10-25 23:14Z; the digest is an argument, so a drift exits 65).
- `mcopy` round trip on the final image `f0811738…` — the extracted EFI hashes to `ff17a936…` and
  `cmp` is silent; `STARTUP.NSH` reads `fs0:\EFI\BOOT\BOOTX64.EFI\r\n`.
- OVMF with a fresh NVRAM (no boot entry) — `BdsDxe` creates its own entry from the ESP, starts the
  image, and Allen's window renders with `Network is up. Type a request.` The capture holds only the
  firmware's own ICMPv6 (3 packets, no DHCP/DNS/TCP), so this proves the layout, a cold boot and the
  SNP probe; it is not evidence about the Omega transport path, which is still blocked on aienux#454.

Emulator evidence, not hardware qualification: no physical iMac was touched, and Apple firmware has
not been exercised.
