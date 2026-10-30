#!/usr/bin/env bash
# L332 rung 2 window 0, part 1 (before the reboot). Run ONLY inside the coordinator-scheduled window, after the GPU users
# on the coordinator's list are stopped. Records the box, checks the module, writes a temporary "do not load NVIDIA"
# file (removed by part 2 as its first action) and reboots. Usage: window0-pre.sh [--go]  (without --go: checks only).
set -u
export PATH=$PATH:/usr/sbin:/sbin   # modinfo, lsmod and friends live in sbin; a normal user shell may not list it
here=$(cd "$(dirname "$0")" && pwd); repo=$(cd "$here/../../.." && pwd)
log=$here/window0-$(date -u +%Y%m%dT%H%M%SZ).log; ln -sfn "$(basename "$log")" "$here/window0-current.log"
ts() { echo "$(date -u +%FT%T.%NZ) $*" | tee -a "$log"; }
fail() { ts "PRE FAIL: $*"; exit 1; }
KO=$repo/native/linux/aien-gpu/aien_gpu.ko
ts "pre start, commit $(git -C "$repo" rev-parse HEAD), kernel $(uname -r)"
{ lsmod | grep -E '^nvidia'; nvidia-smi -L; cat /proc/cmdline; sha256sum "$KO"; } >> "$log" 2>&1
[ -f "$KO" ] || fail "no $KO (make -C native/linux/aien-gpu)"
[ "$(modinfo -F vermagic "$KO" | cut -d' ' -f1)" = "$(uname -r)" ] || fail "aien_gpu.ko vermagic does not match $(uname -r)"
[ "$(mokutil --sb-state 2>/dev/null)" = "SecureBoot disabled" ] || fail "Secure Boot state changed: unsigned module would be refused"
initrd=$(lsinitramfs "/boot/initrd.img-$(uname -r)") && [ -n "$initrd" ] || fail "cannot list the initramfs: the nvidia-in-initramfs check would pass silently"
grep -qE '/nvidia(_uvm|_modeset|_drm)?\.ko' <<< "$initrd" && fail "nvidia modules in initramfs: the modprobe.d file would not hold them back"
holders=$(sudo fuser /dev/nvidia* 2>/dev/null | tr -s ' ' '\n' | grep -E '^[0-9]+$' | sort -u)
for p in $holders; do ts "holder pid $p $(ps -o user=,comm= -p "$p")"; done
other=$(for p in $holders; do [ "$(ps -o comm= -p "$p")" = nvidia-persiste ] || echo "$p"; done)
[ -z "$other" ] || { [ "${1:-}" = --go ] && fail "GPU still held by: $other (coordinator stops them first)"; ts "WARN still held by: $other"; }
[ "${1:-}" = --go ] || { ts "checks only (no --go): PASS"; exit 0; }
# Self-reverting hold-back (window-boot.sh): one-shot GRUB entry, /run/modprobe.d file, hardware watchdog, panic reboot,
# 25-min lease. Every boot after the window boot is the default entry with NVIDIA's driver; nothing to delete by hand.
"$here/window-boot.sh" check >> "$log" 2>&1 || fail "window-boot.sh check failed (see log); was 'window-boot.sh install --yes' run in prep?"
"$here/window-boot.sh" arm --yes >> "$log" 2>&1 || fail "window-boot.sh arm failed: not rebooting"
sync; ts "one-shot window entry armed; rebooting (part 2: window0-post.sh after ssh is back)"
sudo systemctl reboot
