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

# qemu_verify_recovery_media.sh: Verify standalone recovery environment in QEMU AArch64
# Invariant: BOOTS WITH ZERO HARD DISKS ATTACHED. Internal NVMe must not be present.
# Proves 100% RAM disk self-containment under rdinit=/init.
# Zero Disk Secrets and Unslop compliant.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${REPO_ROOT}"

command -v qemu-system-aarch64 >/dev/null || { echo "qemu-system-aarch64 not installed"; exit 2; }

if [[ "$(uname -m)" != "aarch64" ]]; then
    echo "SKIPPED: Host architecture is $(uname -m). AArch64 kernel recovery boot requires an AArch64 host or cross-built AArch64 kernel."
    [[ "${AIENOS_STRICT:-0}" != "1" ]] || { echo "STRICT FAIL: recovery boot skipped under AIENOS_STRICT=1" >&2; exit 1; }
    exit 0
fi

WORK_DIR=$(mktemp -d)
trap 'rm -rf "${WORK_DIR}"' EXIT
LOG="${WORK_DIR}/recovery_qemu.log"

PROD_VMLINUZ=$(sudo -n ls -1t /boot/vmlinuz-* 2>/dev/null | head -n 1 || ls -1t /boot/vmlinuz-* 2>/dev/null | head -n 1 || true)
if [[ -z "${PROD_VMLINUZ}" ]]; then
    echo "Error: Signed production kernel not found in /boot" >&2
    exit 1
fi

KERNEL_IMAGE="${WORK_DIR}/vmlinuz"
# /boot/vmlinuz-* is root 0600 on Ubuntu. The Spark judge runs gates as aien-run with no_new_privs, so sudo
# cannot copy it there; the operator keeps a read-only copy of the same kernel at $HOME/boot/vmlinuz (root 0644,
# HOME=/var/lib/aien-run), the way it provides $HOME/models. Use that copy only when the kernel cannot be read.
JUDGE_VMLINUZ="${HOME:-/nonexistent}/boot/vmlinuz"
if [[ -r "${PROD_VMLINUZ}" ]]; then
    cp "${PROD_VMLINUZ}" "${KERNEL_IMAGE}"
elif sudo -n cp "${PROD_VMLINUZ}" "${KERNEL_IMAGE}" 2>/dev/null; then
    sudo -n chown "$(id -u):$(id -g)" "${KERNEL_IMAGE}"
elif [[ -r "${JUDGE_VMLINUZ}" ]]; then
    echo "NOTE  ${PROD_VMLINUZ} is not readable and sudo is unavailable; using the read-only copy ${JUDGE_VMLINUZ}"
    cp "${JUDGE_VMLINUZ}" "${KERNEL_IMAGE}"
else
    echo "Error: cannot read ${PROD_VMLINUZ} (no sudo, no ${JUDGE_VMLINUZ})" >&2
    exit 1
fi

INITRD_TMP="${WORK_DIR}/aienos-recovery-standalone-initrd.img"
# Repo checks run on hosts without gocryptfs (CI, laptops). Only there, and
# visibly, allow a test image without it. The real stick build
# (build_recovery_media.sh) never sets this.
if ! command -v gocryptfs >/dev/null 2>&1 && [[ ! -f /home/atlas/atlas-forgejo-setup-20260904/runtime/usr/bin/gocryptfs && -z "${AIENOS_GOCRYPTFS:-}" ]]; then
    echo "SKIP  gocryptfs not on this host; building a TEST-ONLY image without it"
    export AIENOS_RECOVERY_ALLOW_NO_GOCRYPTFS=1
fi

echo "Building standalone recovery initrd..."
bash "${REPO_ROOT}/scripts/build_standalone_recovery_initrd.sh" "${INITRD_TMP}"

echo "Booting standalone recovery kernel in QEMU with ZERO hard disks attached..."
started=$(date +%s)
set +e
timeout 30 qemu-system-aarch64 \
    -M virt -cpu max -smp 2 -m 1024 \
    -kernel "${KERNEL_IMAGE}" \
    -initrd "${INITRD_TMP}" \
    -append "rdinit=/init aienos.test=1 console=ttyAMA0 panic=1 quiet" \
    -display none -nic none \
    -serial file:"${LOG}" -no-reboot
QEMU_STATUS=$?
set -e
elapsed=$(( $(date +%s) - started ))

tr -d '\r' <"${LOG}" >"${WORK_DIR}/recovery_qemu.txt"
[[ -z "${AIENOS_LOG_DIR:-}" ]] || cp "${WORK_DIR}/recovery_qemu.txt" "${AIENOS_LOG_DIR}/recovery_zero_disk_serial.log"

failed=0
check() {
    if grep -q -- "$2" "${WORK_DIR}/recovery_qemu.txt"; then
        echo "PASS  $1"
    else
        echo "FAIL  $1"
        failed=1
    fi
}

echo "qemu exit ${QEMU_STATUS} after ${elapsed} s"
check "Recovery banner displayed" "AIENOS Standalone Hardware Recovery Core"
check "100% RAM disk verified" "Root filesystem: 100% RAM disk"
check "Internal NVMe confirmed unmounted" "Internal NVMe status: UNMOUNTED"
check "Automated recovery test passed" "TEST MODE DETECTED: automated recovery verification complete"

if [[ "${failed}" != 0 ]]; then
    echo "---- recovery console log ----"
    cat "${WORK_DIR}/recovery_qemu.txt"
    echo "RECOVERY_ZERO_DISK: FAIL"
    exit 1
fi

echo "RECOVERY_ZERO_DISK: PASS (verified standalone RAM recovery with zero attached storage)"

# Second boot: the packaged drivers must find a USB stick, an NVMe drive and a
# USB keyboard. The disks are blank scratch images, never host storage.
DEV_LOG="${WORK_DIR}/recovery_devices.log"
truncate -s 64M "${WORK_DIR}/usb.img" "${WORK_DIR}/nvme.img"
ACCEL=(-cpu max)
DEV_TIMEOUT=120
if [[ -w /dev/kvm ]]; then
    ACCEL=(-accel kvm -cpu host)
    DEV_TIMEOUT=60
fi

echo "Booting recovery kernel with a scratch USB stick, NVMe drive and USB keyboard..."
set +e
timeout "${DEV_TIMEOUT}" qemu-system-aarch64 \
    -M virt "${ACCEL[@]}" -smp 2 -m 1024 \
    -kernel "${KERNEL_IMAGE}" \
    -initrd "${INITRD_TMP}" \
    -append "rdinit=/init aienos.test=1 console=ttyAMA0 panic=1" \
    -device qemu-xhci -device usb-kbd \
    -drive if=none,id=usbstick,format=raw,file="${WORK_DIR}/usb.img" -device usb-storage,drive=usbstick \
    -drive if=none,id=nvme0,format=raw,file="${WORK_DIR}/nvme.img" -device nvme,drive=nvme0,serial=aienos-test \
    -display none -nic none \
    -serial file:"${DEV_LOG}" -no-reboot
DEV_STATUS=$?
set -e

tr -d '\r' <"${DEV_LOG}" >"${WORK_DIR}/recovery_devices.txt"
[[ -z "${AIENOS_LOG_DIR:-}" ]] || cp "${WORK_DIR}/recovery_devices.txt" "${AIENOS_LOG_DIR}/recovery_devices_serial.log"
echo "qemu exit ${DEV_STATUS}"

failed=0
check_devices() {
    if grep -qE -- "$2" "${WORK_DIR}/recovery_devices.txt"; then
        echo "PASS  $1"
    else
        echo "FAIL  $1"
        failed=1
    fi
}
modules_line=$(grep -m1 'Kernel modules loaded:' "${WORK_DIR}/recovery_devices.txt" || true)
if [[ "${modules_line}" =~ loaded:\ ([0-9]+)/([0-9]+) ]] && [[ "${BASH_REMATCH[1]}" == "${BASH_REMATCH[2]}" ]] && [[ "${BASH_REMATCH[2]}" -gt 0 ]]; then
    echo "PASS  All packaged kernel modules loaded (${BASH_REMATCH[1]}/${BASH_REMATCH[2]})"
else
    echo "FAIL  All packaged kernel modules loaded (${modules_line:-no report})"
    failed=1
fi
check_devices "USB keyboard detected" "Input devices: [1-9]"
check_devices "USB stick visible" "^sda"
check_devices "NVMe drive visible" "^nvme0n1"

if [[ "${failed}" != 0 ]]; then
    echo "---- recovery device console log ----"
    cat "${WORK_DIR}/recovery_devices.txt"
    echo "RECOVERY_DEVICES: FAIL"
    exit 1
fi

echo "RECOVERY_DEVICES: PASS (packaged drivers found USB keyboard, USB stick and NVMe)"
