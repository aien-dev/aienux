#!/usr/bin/env bash
# L332 rung 2 window 1, part 1 (before the reboot). Run ONLY inside the coordinator-scheduled window 1, after window 0
# passed and the GPU users on the coordinator's list are stopped. Checks everything part 2 needs, records the box, writes
# a temporary "do not load NVIDIA" file (removed by part 2 as its first action) and reboots.
# Usage: window1-pre.sh PKG_DIR [--go]   (without --go: checks only; PKG_DIR from window1-package.sh)
set -u
export PATH=$PATH:/usr/sbin:/sbin   # modinfo, lsmod and friends live in sbin; a normal user shell may not list it
here=$(cd "$(dirname "$0")" && pwd); repo=$(cd "$here/../../.." && pwd)
PKG=${1:?usage: window1-pre.sh PKG_DIR [--go]}; PKG=$(cd "$PKG" && pwd)
log=$here/window1-$(date -u +%Y%m%dT%H%M%SZ).log; ln -sfn "$(basename "$log")" "$here/window1-current.log"
ts() { echo "$(date -u +%FT%T.%NZ) $*" | tee -a "$log"; }
fail() { ts "PRE FAIL: $*"; exit 1; }
SHIM=$repo/native/linux/aien-gpu
KO=$SHIM/aien_gpu.ko BOOT=$SHIM/build/aien_gsp_boot
ts "pre start, commit $(git -C "$repo" rev-parse HEAD), kernel $(uname -r), package $PKG"
{ lsmod | grep -E '^nvidia'; nvidia-smi -L; cat /proc/cmdline; sha256sum "$KO" "$BOOT"; cat "$PKG/MANIFEST.txt"; } >> "$log" 2>&1
ls "$here"/window0-*.log 2>/dev/null | xargs -r grep -l 'WINDOW0 PASS' | grep -q . || fail "no window-0 log with WINDOW0 PASS: window 0 comes first"
# Chip identity gate (NoNvDriver review 2026-10-26). The package carries the GB10B images (GspRmBoot_GB10B,
# GspRmFmcGfwProdSigned_GB10B, .fwsignature_gb10y). NVIDIA's 580.173.02 source also has GB20B (arch 0x1B, impl 0xB,
# family gb20y) with DIFFERENT images, and the Spark's sm_121 points at the GB2XX line. Window 0 logs PMC_BOOT_42;
# CHIP_ID = bits 29:20 (nv_ref.h:171; ARCHITECTURE 29:24 = 0x1A GB100 / 0x1B GB200, nv_ref.h:182-183;
# IMPLEMENTATION 23:20 = 0xB). Hand SEC2 the GB10B image only when the chip measured 0x1AB.
w0=$(ls "$here"/window0-*.log 2>/dev/null | xargs -r grep -l 'WINDOW0 PASS' | tail -n 1)
b42=$(grep -ho 'PMC_BOOT_42 [0-9a-fA-F]\{8\}' "$w0" | tail -n 1 | cut -d' ' -f2)
[ -n "$b42" ] || fail "window-0 log $w0 has no PMC_BOOT_42 value: chip identity unmeasured"
chip=$(printf '%03x' $(( (0x$b42 >> 20) & 0x3ff )))
ts "chip identity from $(basename "$w0"): PMC_BOOT_42 0x$b42 CHIP_ID 0x$chip"
[ "$chip" = 1ab ] || fail "chip is 0x$chip, not GB10B (0x1ab); this package holds GB10B images (GB20B is 0x1bb and needs the GB20B archives + gb20y signature): refusing"
[ -f "$KO" ] && [ -x "$BOOT" ] || fail "build first: make -C native/linux/aien-gpu all tools"
[ "$(modinfo -F vermagic "$KO" | cut -d' ' -f1)" = "$(uname -r)" ] || fail "aien_gpu.ko vermagic does not match $(uname -r)"
modinfo -F parm "$KO" | grep -q '^allow_boot:' || fail "aien_gpu.ko has no allow_boot (old build)"
for f in rm_elf rm_sig bootloader fmc fmc_hash fmc_pkey fmc_sig system_info; do [ -s "$PKG/$f.bin" ] || fail "package lacks $f.bin"; done
[ -s "$PKG/app_version.txt" ] || fail "package lacks app_version.txt"
[ "$(mokutil --sb-state 2>/dev/null)" = "SecureBoot disabled" ] || fail "Secure Boot state changed: unsigned module would be refused"
initrd=$(lsinitramfs "/boot/initrd.img-$(uname -r)") && [ -n "$initrd" ] || fail "cannot list the initramfs: the nvidia-in-initramfs check would pass silently"
grep -qE '/nvidia(_uvm|_modeset|_drm)?\.ko' <<< "$initrd" && fail "nvidia modules in initramfs: the modprobe.d file would not hold them back"
holders=$(sudo fuser /dev/nvidia* 2>/dev/null | tr -s ' ' '\n' | grep -E '^[0-9]+$' | sort -u)
for p in $holders; do ts "holder pid $p $(ps -o user=,comm= -p "$p")"; done
other=$(for p in $holders; do [ "$(ps -o comm= -p "$p")" = nvidia-persiste ] || echo "$p"; done)
[ -z "$other" ] || { [ "${2:-}" = --go ] && fail "GPU still held by: $other (coordinator stops them first)"; ts "WARN still held by: $other"; }
[ "${2:-}" = --go ] || { ts "checks only (no --go): PASS"; exit 0; }
echo "$PKG" > "$here/window1-package.path"
# Self-reverting hold-back (window-boot.sh): see window0-pre.sh.
"$here/window-boot.sh" check >> "$log" 2>&1 || fail "window-boot.sh check failed (see log); was 'window-boot.sh install --yes' run in prep?"
"$here/window-boot.sh" arm --yes >> "$log" 2>&1 || fail "window-boot.sh arm failed: not rebooting"
sync; ts "one-shot window entry armed; rebooting (part 2: window1-post.sh after ssh is back)"
sudo systemctl reboot
