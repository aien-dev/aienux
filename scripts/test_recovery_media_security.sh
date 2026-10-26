#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Offline SB-02/SB-05 regression: scratch images and throwaway signing keys only.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
for tool in mkfs.ext4 mkfs.vfat blkid grub-mkstandalone openssl sbsign sbverify strings; do
    command -v "$tool" >/dev/null || { echo "RECOVERY_SECURITY: NOT_RUN ($tool unavailable)"; exit 3; }
done
t="$(mktemp -d)"
trap 'rm -rf "$t"' EXIT
fail() { echo "RECOVERY_SECURITY: FAIL ($1)" >&2; exit 1; }

# The original root write followed a symlink on a Unix-formatted USB.
mkdir -p "$t/media/EFI/BOOT"
printf 'host sentinel\n' > "$t/host"
ln -s "$t/host" "$t/media/EFI/BOOT/grub.cfg"
printf 'injected menu\n' | tee "$t/media/EFI/BOOT/grub.cfg" >/dev/null
[[ "$(cat "$t/host")" == 'injected menu' ]] || fail 'symlink overwrite primitive did not reproduce'
echo 'PASS  pre-fix symlink overwrite reproduced in scratch files'

truncate -s 8M "$t/ext4.img" "$t/fat.img"
mkfs.ext4 -F -q "$t/ext4.img"
mkfs.vfat -n AIENOSRECOV "$t/fat.img" >/dev/null
if bash "$root/scripts/build_recovery_media.sh" --check-fstype "$t/ext4.img" >"$t/ext4.log" 2>&1; then
    fail 'Unix filesystem admitted'
fi
if ! bash "$root/scripts/build_recovery_media.sh" --check-fstype "$t/fat.img" >"$t/fat.log" 2>&1; then
    fail 'FAT filesystem refused'
fi
[[ "$(cat "$t/host")" == 'injected menu' ]] || fail 'filesystem probe wrote to host sentinel'
echo 'PASS  ext4 refused and FAT accepted before any mount or write (type-only probe)'

# An alias must be evaluated against the canonical device name, not the
# apparently removable-looking link supplied by the caller.
: > "$t/nvme0n1p2"
ln -s "$t/nvme0n1p2" "$t/innocent-usb"
if bash "$root/scripts/build_recovery_media.sh" --check-target-path "$t/innocent-usb" >"$t/alias.log" 2>&1; then
    fail 'alias to internal NVMe name admitted'
fi
ln -s "$t/fat.img" "$t/valid-usb"
if ! bash "$root/scripts/build_recovery_media.sh" --check-target-path "$t/valid-usb" >"$t/valid-alias.log" 2>&1; then
    fail 'alias to FAT scratch path refused'
fi
echo 'PASS  symlink alias to NVMe name refused; benign alias resolved'

# Model the exact mkstandalone/sbsign/sbverify sequence with tiny, throwaway
# payloads. A single changed initrd byte must invalidate the signed PE.
printf 'TEST_INITRD_MARKER\n' > "$t/initrd.img"
printf 'TEST_KERNEL_MARKER\n' > "$t/vmlinuz"
printf 'linux (memdisk)/boot/vmlinuz rdinit=/init\ninitrd (memdisk)/boot/initrd.img\n' > "$t/grub.cfg"
printf 'sbat,1,SBAT Version,sbat,1,https://github.com/rhboot/shim/blob/main/SBAT.md\ngrub,2,AIENOS Recovery GRUB,aien-dev,1,https://github.com/aien-dev/aienux\n' > "$t/sbat.csv"
grub-mkstandalone -O arm64-efi --fonts='' --locales='' --themes='' \
    --modules='linux normal' --compress=no --sbat="$t/sbat.csv" -o "$t/unsigned.efi" \
    "/boot/grub/grub.cfg=$t/grub.cfg" "/boot/vmlinuz=$t/vmlinuz" \
    "/boot/initrd.img=$t/initrd.img" >/dev/null
openssl req -x509 -newkey rsa:2048 -nodes -keyout "$t/key.pem" \
    -out "$t/cert.pem" -subj '/CN=throwaway recovery test/' -days 1 >/dev/null 2>&1
sbsign --key "$t/key.pem" --cert "$t/cert.pem" --output "$t/signed.efi" "$t/unsigned.efi" >/dev/null 2>&1
sbverify --cert "$t/cert.pem" "$t/signed.efi" >/dev/null 2>&1 || fail 'signed PE refused'
if sbverify --cert "$t/cert.pem" "$t/unsigned.efi" >/dev/null 2>&1; then
    fail 'unsigned PE accepted'
fi
marker_offset=''
while read -r offset text; do
    if [[ "$text" == TEST_INITRD_MARKER ]]; then marker_offset="$offset"; break; fi
done < <(strings -t d "$t/signed.efi")
[[ "$marker_offset" =~ ^[0-9]+$ ]] || fail 'initrd marker not embedded in signed PE'
cp "$t/signed.efi" "$t/tampered.efi"
printf X | dd of="$t/tampered.efi" bs=1 seek="$marker_offset" conv=notrunc status=none
if sbverify --cert "$t/cert.pem" "$t/tampered.efi" >/dev/null 2>&1; then
    fail 'changed embedded initrd accepted'
fi
# The production builder must use the same memdisk paths and must not write
# an independent menu or initrd to the FAT volume.
if ! [[ "$(cat "$root/scripts/build_recovery_media.sh")" == *'initrd (memdisk)/boot/initrd.img'* ]] ||
   [[ "$(cat "$root/scripts/build_recovery_media.sh")" == *'initrd /aienos-recovery/initrd.img'* ]]; then
    fail 'builder no longer binds the initrd inside signed PE'
fi
echo 'PASS  signed PE accepted, unsigned and changed-initrd PE refused'

# Optional firmware-level control: AAVMF's published snakeoil TEST db accepts
# a signed standalone GRUB with its embedded config, then rejects that exact
# EFI after a one-byte mutation in its embedded initrd. Ubuntu shim/MOK is
# not exercised here; it requires separate attended qualification.
if [[ "${AIENOS_RECOVERY_QEMU_EMBED:-0}" == 1 ]]; then
    code="${AAVMF_SB_CODE:-/usr/share/AAVMF/AAVMF_CODE.snakeoil.fd}"
    vars="${AAVMF_SB_VARS:-/usr/share/AAVMF/AAVMF_VARS.snakeoil.fd}"
    key="${SNAKEOIL_KEY:-/usr/share/qemu-efi-aarch64/PkKek-1-snakeoil.key}"
    cert="${SNAKEOIL_CERT:-/usr/share/qemu-efi-aarch64/PkKek-1-snakeoil.pem}"
    command -v qemu-system-aarch64 >/dev/null &&
        [[ -r "$code" && -r "$vars" && -r "$key" && -r "$cert" ]] ||
        { echo 'RECOVERY_SECURITY_QEMU: NOT_RUN (AAVMF snakeoil firmware/key unavailable)'; exit 3; }
    openssl pkey -in "$key" -passin pass:snakeoil -out "$t/snakeoil.key" 2>/dev/null
    printf 'serial --unit=0 --speed=115200\nterminal_output serial\necho EMBEDDED_ONLY\nsleep 2\nreboot\n' > "$t/grub.cfg"
    grub-mkstandalone -O arm64-efi --fonts='' --locales='' --themes='' \
        --modules='linux normal serial' --compress=no --sbat="$t/sbat.csv" \
        -o "$t/qemu-unsigned.efi" "/boot/grub/grub.cfg=$t/grub.cfg" \
        "/boot/vmlinuz=$t/vmlinuz" "/boot/initrd.img=$t/initrd.img" >/dev/null
    sbsign --key "$t/snakeoil.key" --cert "$cert" --output "$t/qemu-signed.efi" \
        "$t/qemu-unsigned.efi" >/dev/null 2>&1
    sbverify --cert "$cert" "$t/qemu-signed.efi" >/dev/null 2>&1 ||
        fail 'AAVMF-test image signature invalid'
    qemu_offset=''
    while read -r offset text; do
        if [[ "$text" == TEST_INITRD_MARKER ]]; then qemu_offset="$offset"; break; fi
    done < <(strings -t d "$t/qemu-signed.efi")
    [[ "$qemu_offset" =~ ^[0-9]+$ ]] || fail 'AAVMF-test initrd marker not embedded'
    cp "$t/qemu-signed.efi" "$t/qemu-tampered.efi"
    printf X | dd of="$t/qemu-tampered.efi" bs=1 seek="$qemu_offset" conv=notrunc status=none
    mkdir -p "$t/esp/EFI/BOOT" "$t/esp/EFI/ubuntu" "$t/esp/aienos-recovery"
    printf 'serial --unit=0\nterminal_output serial\necho EXTERNAL_OVERRIDE\nhalt\n' > "$t/esp/EFI/BOOT/grub.cfg"
    cp "$t/esp/EFI/BOOT/grub.cfg" "$t/esp/EFI/ubuntu/grub.cfg"
    printf 'ATTACKER_EXTERNAL_INITRD\n' > "$t/esp/aienos-recovery/initrd.img"
    boot_qemu() {
        local variant="$1" image="$2" result
        cp "$image" "$t/esp/EFI/BOOT/BOOTAA64.EFI"
        cp "$vars" "$t/vars-$variant.fd"
        timeout 60 qemu-system-aarch64 -M virt,virtualization=on,gic-version=3 \
            -accel tcg,thread=single -cpu max -smp 2 -m 1024 \
            -drive if=pflash,format=raw,readonly=on,file="$code" \
            -drive if=pflash,format=raw,file="$t/vars-$variant.fd" \
            -drive if=none,id=esp,format=raw,file=fat:rw:"$t/esp" \
            -device virtio-blk-pci,drive=esp -display none -nic none \
            -serial file:"$t/$variant.log" -no-reboot >/dev/null 2>&1 || true
        result="$(cat "$t/$variant.log")"
        if [[ "$variant" == signed ]]; then
            [[ "$result" == *EMBEDDED_ONLY* && "$result" != *EXTERNAL_OVERRIDE* ]] ||
                fail 'firmware signed control did not execute embedded-only config'
        else
            [[ "$result" == *'Access Denied'* && "$result" != *EMBEDDED_ONLY* ]] ||
                fail 'firmware did not refuse changed-initrd PE with Access Denied'
        fi
    }
    boot_qemu signed "$t/qemu-signed.efi"
    boot_qemu tampered "$t/qemu-tampered.efi"
    echo 'RECOVERY_SECURITY_QEMU: PASS (direct AAVMF Secure Boot, external config ignored; shim/MOK NOT_RUN)'
fi
echo 'RECOVERY_SECURITY: PASS (scratch host checks; optional direct AAVMF; owner shim/MOK physical boot NOT_RUN)'
