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

```sh
OMC=<pin>/seed/out/omc            # compiler that supports the host words
sha256sum "$OMC"                  # record it; it is part of the receipt
"$OMC" < imac-image.om > imac-image
chmod +x imac-image
cp <the .efi you built> BOOTX64.EFI     # same directory
./imac-image                      # writes imac-boot.img
```

Exit status: 0 written, 2 input open, 3 input empty, 4 output open, 5 truncate, 9 close input,
6 sync, 7 close output, 8 a short write. The geometry is fixed (disk 196608 sectors, ESP
2048..196574, FAT32 with 512-byte sectors, 1 sector per cluster, 32 reserved, 2 FATs of 1497
sectors, 191501 clusters), so the same EFI bytes always give the same image digest.

## Receipt (first run, 2026-10-25)

| Item | Value |
|---|---|
| builder source | `imac-image.om` sha256 `d76d3852daf026238d22b0d88faedba15338e372e5d423a8e3c5b677fdde5f14` |
| compiler | omega `feat/allen-imac-competition` `c3fb79697`, `seed/out/omc` sha256 `c99eda2465036c0637a444689a65dd7fc736e4fa8a8b4cf3456b0ce1b57f8996` |
| input EFI | aienux `feat/allen-imac-competition` `d4ffde3c`, `target x86efi`, sha256 `ff17a936719049af35c793860d246003170ade526bc8da4a421bdaf9cf0dc33b` (1528832 bytes) |
| output image | `imac-boot.img` sha256 `f0811738c2a30bc93076f18884fb9cdc2c081ae83775d147565352b40a4c85f0`, 100663296 bytes |

Reproducibility is the useful property here: recompiling the same twenty `.om` files with that
compiler produced the same EFI digest as the image built earlier the same day, and building that EFI
into this image twice produced the same image digest.

## Verification performed

Reading tools are used only as independent checkers, never as part of the build:

- `sgdisk -p imac-boot.img` — partition 1, sectors 2048..196574, code `EF00`, name `ALLEN`.
- `sgdisk -v imac-boot.img` — `No problems found` (both CRCs, primary and backup headers agree).
- `fsck.vfat -n <partition>` — clean: 5 files, 2990/191501 clusters, no FSInfo or `..` complaints.
- `mdir -i imac-boot.img@@1M ::/EFI/BOOT` — `BOOTX64.EFI`, 1528832 bytes.
- `mcopy` round trip — the extracted EFI hashes equal to the input EFI.
- OVMF with a fresh NVRAM (no boot entry) — `BdsDxe` creates its own entry from the ESP, starts the
  image, and Allen's window renders with `Network is up. Type a request.`

Emulator evidence, not hardware qualification: no physical iMac was touched, and Apple firmware has
not been exercised.
