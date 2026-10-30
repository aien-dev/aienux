#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Drake Stapleton (AIEN / aien-dev)
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# build_recovery_media.sh: Build a signed, self-contained UEFI recovery image
# on a dedicated FAT USB partition. No separately loaded kernel, menu or
# initrd may be present on the writable medium.
# Guard: Multi-factor validation rejects ATLAS_RECOV and the internal NVMe.
# Invariant: NO PLAINTEXT SECRETS IN REPOSITORY OR BUILD ARTIFACTS.

set -euo pipefail

umask 077

require_vfat() {
    local target_type
    target_type=$(blkid -p -o value -s TYPE "$1" 2>/dev/null || true)
    if [[ "${target_type}" != vfat ]]; then
        echo "FATAL: Recovery target must be an existing FAT (vfat) partition; found ${target_type:-unknown}." >&2
        return 1
    fi
}

canonical_target() {
    local resolved
    resolved=$(readlink -e -- "$1") || {
        echo "FATAL: Recovery target path does not resolve: $1" >&2
        return 1
    }
    if [[ "${resolved##*/}" == nvme* ]]; then
        echo "FATAL: Recovery target resolves to internal NVMe: ${resolved}" >&2
        return 1
    fi
    printf '%s\n' "${resolved}"
}

# Read-only probes for offline tests. Neither validates a block device nor
# authorizes an image build.
if [[ "${1:-}" == --check-fstype ]]; then
    [[ $# == 2 ]] || { echo "Usage: $0 --check-fstype <scratch-image>" >&2; exit 1; }
    require_vfat "$2"
    echo "TYPE_ONLY: vfat (no device authorization or media build)"
    exit 0
elif [[ "${1:-}" == --check-target-path ]]; then
    [[ $# == 2 ]] || { echo "Usage: $0 --check-target-path <scratch-path>" >&2; exit 1; }
    canonical_target "$2"
    echo "PATH_ONLY: no device authorization or media build"
    exit 0
fi

TARGET_DEV="${1:-}"

if [[ -z "${TARGET_DEV}" ]]; then
    echo "Usage: $0 <target-partition> (e.g. /dev/sdb1)" >&2
    echo "Error: Target block device must be explicitly specified." >&2
    exit 1
fi

# Resolve aliases before the block check, all identity probes and the mount.
# A /dev/disk/by-id/ alias must not hide an internal /dev/nvme* partition.
TARGET_DEV="$(canonical_target "${TARGET_DEV}")"

if [[ ! -b "${TARGET_DEV}" ]]; then
    echo "Error: Target ${TARGET_DEV} is not a valid block device." >&2
    exit 1
fi

# Multi-factor identity inspection
TARGET_LABEL=$(lsblk -no LABEL "${TARGET_DEV}" 2>/dev/null || blkid -s LABEL -o value "${TARGET_DEV}" 2>/dev/null || true)
TARGET_UUID=$(lsblk -no UUID "${TARGET_DEV}" 2>/dev/null || blkid -s UUID -o value "${TARGET_DEV}" 2>/dev/null || true)
TARGET_PARTUUID=$(lsblk -no PARTUUID "${TARGET_DEV}" 2>/dev/null || blkid -s PARTUUID -o value "${TARGET_DEV}" 2>/dev/null || true)
TARGET_MOUNTS=$(lsblk -no MOUNTPOINTS "${TARGET_DEV}" 2>/dev/null || true)

# 1. Reject ATLAS_RECOV by label, filesystem UUID, and partition UUID
KNOWN_ATLAS_RECOV_UUID="669D-4D0E"
KNOWN_ATLAS_RECOV_PARTUUID="335d7260-01"

if [[ "${TARGET_LABEL}" == "ATLAS_RECOV" || "${TARGET_UUID}" == "${KNOWN_ATLAS_RECOV_UUID}" || "${TARGET_PARTUUID}" == "${KNOWN_ATLAS_RECOV_PARTUUID}" ]]; then
    echo "FATAL: Target matches ATLAS_RECOV identity (Label: , UUID: , PARTUUID: )." >&2
    echo "ATLAS_RECOV contains encrypted offline recovery keys and must NEVER be overwritten." >&2
    exit 1
fi

# 2. Reject internal NVMe storage and system partitions
KNOWN_HOST_ROOT_UUID="d27bfd26-ff30-400e-9eca-9cdf73de9406"
KNOWN_HOST_EFI_UUID="9DA2-3597"

if [[ "${TARGET_UUID}" == "${KNOWN_HOST_ROOT_UUID}" || "${TARGET_UUID}" == "${KNOWN_HOST_EFI_UUID}" ]]; then
    echo "FATAL: Target ${TARGET_DEV} is an internal host system disk or EFI system partition." >&2
    exit 1
fi

# 3. Reject active host mounts
if [[ -n "${TARGET_MOUNTS}" ]]; then
    echo "FATAL: Target ${TARGET_DEV} has active mountpoints: ${TARGET_MOUNTS}. Unmount before provisioning." >&2
    exit 1
fi

# 4. If target is a disk containing partitions, ensure none of its child partitions match ATLAS_RECOV
CHILD_IDS=$(lsblk -no LABEL,UUID,PARTUUID "${TARGET_DEV}" 2>/dev/null || true)
if echo "${CHILD_IDS}" | grep -qE "ATLAS_RECOV|${KNOWN_ATLAS_RECOV_UUID}|${KNOWN_ATLAS_RECOV_PARTUUID}"; then
    echo "FATAL: Device ${TARGET_DEV} contains partition matching ATLAS_RECOV identity." >&2
    exit 1
fi

# FAT cannot store symlinks. Reject a Unix filesystem before any privileged
# mount or write, and check the actual mounted type after mounting.
require_vfat "${TARGET_DEV}"
if [[ "${AIENOS_RECOVERY_ALLOW_NO_GOCRYPTFS:-0}" == 1 ]]; then
    echo "FATAL: Test-only no-gocryptfs image is not eligible for recovery media." >&2
    exit 1
fi

for tool in grub-mkstandalone sbsign sbverify findmnt; do
    command -v "${tool}" >/dev/null || { echo "Error: required tool ${tool} not installed." >&2; exit 1; }
done
SIGNING_KEY="${AIENOS_RECOVERY_SIGNING_KEY:-}"
SIGNING_CERT="${AIENOS_RECOVERY_SIGNING_CERT:-}"
if [[ -z "${SIGNING_KEY}" || -z "${SIGNING_CERT}" || ! -r "${SIGNING_KEY}" || ! -r "${SIGNING_CERT}" ]]; then
    echo "FATAL: Supply readable AIENOS_RECOVERY_SIGNING_KEY and AIENOS_RECOVERY_SIGNING_CERT (owner-enrolled shim trust). No unsigned recovery image is installed." >&2
    exit 1
fi

WORK_DIR="$(mktemp -d /tmp/aienos-recovery-build.XXXXXX)"
MOUNT_POINT=""
mounted=0
cleanup() {
    if [[ "${mounted}" == 1 ]]; then
        umount "${MOUNT_POINT}" || echo "WARNING: Could not unmount ${MOUNT_POINT}; inspect before removing media." >&2
    fi
    if [[ -n "${MOUNT_POINT}" ]]; then
        rmdir "${MOUNT_POINT}" 2>/dev/null || true
    fi
    rm -rf "${WORK_DIR}"
}
trap cleanup EXIT

# The signed PE contains GRUB's own menu, the production kernel and the
# complete initrd. GRUB reads only its embedded memdisk, never USB paths.
PROD_VMLINUZ="${AIENOS_RECOVERY_KERNEL:-$(ls -1t /boot/vmlinuz-* | head -n 1)}"
SHIM_EFI="${AIENOS_RECOVERY_SHIM:-/boot/efi/EFI/boot/BOOTAA64.EFI}"
MOK_EFI="${AIENOS_RECOVERY_MOKMANAGER:-/boot/efi/EFI/ubuntu/mmaa64.efi}"
if [[ ! -r "${PROD_VMLINUZ}" || ! -r "${SHIM_EFI}" || "${PROD_VMLINUZ##*/}" != vmlinuz-* ]]; then
    echo "FATAL: Require a readable vmlinuz-* kernel and signed shim input." >&2
    exit 1
fi
if [[ -n "${AIENOS_RECOVERY_MOKMANAGER:-}" && ! -r "${MOK_EFI}" ]]; then
    echo "FATAL: Explicit MOK manager input is unreadable." >&2
    exit 1
fi
echo "Building signed recovery image with kernel ${PROD_VMLINUZ}..."
AIENOS_RECOVERY_KVER="${PROD_VMLINUZ##*/vmlinuz-}" \
    bash "$(dirname "$0")/build_standalone_recovery_initrd.sh" "${WORK_DIR}/initrd.img"
cat > "${WORK_DIR}/grub.cfg" <<'GRUB_EOF'
set timeout=5
set default=0
menuentry "AIENOS Standalone Recovery System (RAM Maintenance Core)" {
    linux (memdisk)/boot/vmlinuz rdinit=/init console=ttyAMA0,115200n8 console=tty0 quiet
    initrd (memdisk)/boot/initrd.img
}
menuentry "Reboot to Incumbent Linux" {
    exit
}
GRUB_EOF
cat > "${WORK_DIR}/sbat.csv" <<'SBAT_EOF'
sbat,1,SBAT Version,sbat,1,https://github.com/rhboot/shim/blob/main/SBAT.md
grub,2,AIENOS Recovery GRUB,aien-dev,1,https://github.com/aien-dev/aienux
SBAT_EOF
grub-mkstandalone -O arm64-efi --fonts='' --locales='' --themes='' \
    --modules='linux normal' --compress=no --sbat="${WORK_DIR}/sbat.csv" \
    -o "${WORK_DIR}/recovery-unsigned.efi" \
    "/boot/grub/grub.cfg=${WORK_DIR}/grub.cfg" \
    "/boot/vmlinuz=${PROD_VMLINUZ}" \
    "/boot/initrd.img=${WORK_DIR}/initrd.img"
sbsign --key "${SIGNING_KEY}" --cert "${SIGNING_CERT}" \
    --output "${WORK_DIR}/recovery.efi" "${WORK_DIR}/recovery-unsigned.efi"
sbverify --cert "${SIGNING_CERT}" "${WORK_DIR}/recovery.efi" >/dev/null

MOUNT_POINT="$(mktemp -d /mnt/aienos-recovery.XXXXXX)"
mount -t vfat -o nodev,nosuid,noexec "${TARGET_DEV}" "${MOUNT_POINT}"
mounted=1
if [[ "$(findmnt -n -o FSTYPE --target "${MOUNT_POINT}")" != vfat ]]; then
    echo "FATAL: Mounted recovery target is not FAT; refusing to write." >&2
    exit 1
fi

echo "Populating signed EFI boot structure on ${TARGET_DEV}..."
mkdir -p "${MOUNT_POINT}/EFI/BOOT" "${MOUNT_POINT}/EFI/ubuntu"
# Remove unsigned leftovers from older recovery builds. The standalone EFI
# never reads these; leaving them risks confusing an operator or a future
# fallback loader into treating unauthenticated USB bytes as recovery code.
rm -f "${MOUNT_POINT}/EFI/BOOT/grub.cfg" "${MOUNT_POINT}/EFI/ubuntu/grub.cfg" \
    "${MOUNT_POINT}/EFI/BOOT/mmaa64.efi" \
    "${MOUNT_POINT}/aienos-recovery/vmlinuz" "${MOUNT_POINT}/aienos-recovery/initrd.img"
cp "${SHIM_EFI}" "${MOUNT_POINT}/EFI/BOOT/BOOTAA64.EFI"
if [[ -r "${MOK_EFI}" ]]; then
    cp "${MOK_EFI}" "${MOUNT_POINT}/EFI/BOOT/mmaa64.efi"
fi
# Both possible Ubuntu shim lookup paths receive the same owner-signed PE.
cp "${WORK_DIR}/recovery.efi" "${MOUNT_POINT}/EFI/BOOT/grubaa64.efi"
cp "${WORK_DIR}/recovery.efi" "${MOUNT_POINT}/EFI/ubuntu/grubaa64.efi"
sync
echo "AIENOS signed standalone recovery media build complete on ${TARGET_DEV}."
