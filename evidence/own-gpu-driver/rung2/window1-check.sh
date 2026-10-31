#!/usr/bin/env bash
# L332 rung 2 window 1, part 3 (after the restore reboot): the box is back as before. Read-only.
set -u
export PATH=$PATH:/usr/sbin:/sbin   # modinfo, lsmod and friends live in sbin; a normal user shell may not list it
here=$(cd "$(dirname "$0")" && pwd)
log=$here/$(readlink "$here/window1-current.log")
ts() { echo "$(date -u +%FT%T.%NZ) $*" | tee -a "$log"; }
ok=1
ts "check start; boot $(uptime -s)"
grep -q 'aien.gpuwindow=1' /proc/cmdline && { ts "still the window boot (aien.gpuwindow=1): the restore reboot did not happen; reboot"; ok=0; }
[ -e /run/modprobe.d/zz-aien-gpu-window.conf ] && { ts "hold-back file still in /run: unexpected outside the window boot"; ok=0; }
next=$(grub-editenv /boot/grub/grubenv list | sed -n 's/^next_entry=//p'); [ -z "$next" ] || { ts "grubenv next_entry still set ($next): clear it with 'sudo grub-editenv /boot/grub/grubenv unset next_entry'"; ok=0; }
n=$(lsmod | grep -cE '^nvidia'); ts "nvidia modules loaded: $n (want 4)"; [ "$n" = 4 ] || ok=0
lsmod | grep -q '^aien_gpu' && { ts "aien_gpu loaded after reboot: unexpected"; ok=0; }
nvidia-smi -L >> "$log" 2>&1 || { ts "nvidia-smi failed: GPU not back on NVIDIA's driver"; ok=0; }
ts "WINDOW1 RESTORE $([ $ok = 1 ] && echo PASS || echo FAIL)"
