# Read-only AArch64 core boot medium

`spark-image.om` writes a deterministic GPT disk with a FAT32 ESP and the removable-media entry `EFI/BOOT/BOOTAA64.EFI`. It reads `BOOTAA64.EFI` and creates/truncates `spark-boot.img` in its current directory. Run only in a dedicated artifact directory, never on a block device. The writer derives from PR #455 at `4d082319`; no host partitioning/FAT library builds the image.

## Payload boundary

The attended Spark diagnostic uses the existing **core-only** `native/kernel` target, not `full`, Allen, Store, Pi or B7b variants. Source pin: `659ddd2d7e3e140cb9bf942c3171cbd9cf3d092e`.

Build with `make -j2 -C native/kernel CROSS= CK_HARDWARE_STAGING=1 CK_OWNER_PUBKEYS=<existing-owner-public-file> CK_MACHINE_ID=<existing-machine-identity-file> CK_FB_HOLD_S=120 AIENOS_COMMIT=<source-pin>` under `nice -n 19 ionice -c3`. Do not create keys or bypass provisioning refusals. Owner file paths are operator-local. The core target does not link device/storage stages; their weak references remain unbound. Its EFI stub reads an optional model map but never writes a disk or EFI variable. The USB medium deliberately contains no model map.

The selected existing kernel is C/assembly, built by GCC/binutils, not an Omega kernel. Those are explicit legacy dependencies; replacing the freestanding core in seeded Omega remains the replacement path. The **medium writer** is seeded Omega. Do not describe packaging as kernel migration.

Expected scope: firmware load, own EL1 address space, guard-page check, GIC, physical timer, secondary-core check-in, GOP text console, memory summary, ACPI-derived CPU/GIC facts, final report and reset. Complete memory-descriptor/ACPI-table inventories, PCI inventory, NVMe, network, AienOS userland and Allen are **NOT_RUN** in this core-only qualification. QEMU is EXERCISED, never physical qualification.

## Build and readers

Use omc from Omega `c3fb79697`, expected SHA-256 `c99eda2465036c0637a444689a65dd7fc736e4fa8a8b4cf3456b0ce1b57f8996`. Verify the compiler digest before compiling this source. Compile stdout into a temporary output; execute it only after compiler exit zero. Place the selected EFI alongside the resulting executable and run it in the artifact directory. Record both digests.

Independent readers: `sgdisk -p spark-boot.img`, `sgdisk -v spark-boot.img`, `mdir -i spark-boot.img@@1M ::/EFI/BOOT`. Extract the ESP with `dd if=spark-boot.img of=esp.part bs=512 skip=2048 count=194527`, then `fsck.vfat -n esp.part`. Extract the EFI using mcopy and require byte equality. Missing input must return 2. Repeated runs with the same EFI must produce the same digest.

The geometry is inherited unchanged: 196608 sectors; ESP 2048..196574; 512-byte sectors, 32 reserved sectors, two 1497-sector FATs, 191501 data clusters. BPB and root-directory labels both read AIENUX.

## Emulator acceptance and physical limits

Use fresh AAVMF variables and `qemu-system-aarch64 -M virt,virtualization=on,gic-version=3,iommu=smmuv3,acpi=on -accel tcg,thread=single -cpu max -smp 4 -m 2048`. Attach the complete GPT image with `readonly=on` and a separate NVMe backing file with `readonly=on`; trace `pci_nvme_io_cmd`, `pci_nvme_write`, and `nvme_write_zeroes`. Keep command, firmware digests, serial, monitor screendump, traces and exit status. Require zero NVMe write events, final report and reset exit zero. Check the unstripped ELF and disassembly for absent defined NVMe/block-write functions and absent device/Store stages. Symbol absence alone is not a whole-program proof: retain the linked source list and review EFI indirect calls too.

`virt` is not GB10: `max` is not the heterogeneous Cortex-X925/A725 system, and QEMU PL011/virtual PCI/GOP do not qualify Spark UART, PCIe, display or SMMU. Firmware providers and emulators are verification dependencies, not first-party runtime components.

## Attended window

No tool here writes a USB, internal ESP, BootNext or BootOrder, or reboots a host. Main schedules the window and owns live-service stop/restore. All lanes/evidence must be checkpointed outside temporary storage first. A verified recovery medium, monitor connected before power-on, wired keyboard and an attended operator are prerequisites.

Prefer the external USB and the vendor-documented **Esc or Del → Save & Exit → Boot Override → USB**. This selects one boot without editing BootOrder. Do not assume a historical Boot#### still identifies the same device. A BootNext alternative requires a freshly verified entry targeting this exact artifact, explicit staging authorization and an unchanged default order. The old Allen entry is not this core artifact.

Record the display continuously with a phone: look for `report_kind: kernel`, `kernel: alive`, `kernel_el: EL1h`, `mmu_switch: ... switched=yes`, `gic: v3`, timer/SMP results, `report_kind: final` and `screen: holding 120 s before reset`. The hold applies when the framebuffer console is usable; it is not a watchdog. A panic is a failed qualification even if it returns to the normal OS. Serial capture additionally requires a physically identified compatible connector; do not guess UART wiring or claim a serial receipt from a photo.

If hung, the repository recovery procedure says to hold the chassis power button **until off**, wait ten seconds, then press it again; no numeric hold duration is established by the vendor manual. Remove the candidate USB after power-off and boot the unchanged normal OS entry. Esc/Del and Boot Override select the normal OS if automatic fallback fails. Unchanged boot order reduces boot-loop risk; it is not a guarantee against hardware/firmware failure. Main restores the exact saved serving binary/configuration, checks health, then resumes lanes. Physical result fields remain empty until the attended run.
