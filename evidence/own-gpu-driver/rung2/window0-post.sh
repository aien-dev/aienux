#!/usr/bin/env bash
# L332 rung 2 window 0, part 2 (after the reboot from part 1). First removes the temporary blacklist, then: our module reads
# the chip and gives it back, NVIDIA's driver is loaded again (timed: today's load-to-ready baseline), Omega proves the chip
# works, and a normal reboot restores the box exactly (display modules included). Usage: window0-post.sh [--no-reboot]
set -u
export PATH=$PATH:/usr/sbin:/sbin   # modinfo, lsmod and friends live in sbin; a normal user shell may not list it
here=$(cd "$(dirname "$0")" && pwd); repo=$(cd "$here/../../.." && pwd)
log=$here/$(readlink "$here/window0-current.log")
ts() { echo "$(date -u +%FT%T.%NZ) $*" | tee -a "$log"; }
BLK=/run/modprobe.d/zz-aien-gpu-window.conf
grep -q 'aien.gpuwindow=1' /proc/cmdline || { ts "WINDOW0 ABORT: this is not the one-shot window boot (no aien.gpuwindow=1); nothing done"; exit 1; }
ts "post start; window boot; hold-back file present: $([ -e "$BLK" ] && echo yes || echo NO) (kept until our module is gone); lease: $(systemctl is-active aien-gpu-window-lease.service)"
ts "boot: $(uptime -s) (uptime $(cut -d' ' -f1 /proc/uptime) s), systemd: $(systemd-analyze 2>/dev/null | head -1)"
ts "lsmod nvidia: [$(lsmod | grep -cE '^nvidia')] lspci: $(lspci -k -s 000f:01:00.0 | grep -c 'in use')"
sudo sysctl -q -w kernel.panic=10 kernel.panic_on_oops=1; ts "panic auto-reboot 10 s set (runtime only)"
rc=0
ts "insmod aien_gpu allow_bind=1"; sudo insmod "$repo/native/linux/aien-gpu/aien_gpu.ko" allow_bind=1 || rc=1
sudo dmesg | grep -i aien-gpu | tee -a "$log"
CHK=$here/aien_gpu_check; [ -x "$CHK" ] || gcc -O2 -o "$CHK" "$repo/native/linux/aien-gpu/aien_gpu_check.c"
sudo "$CHK" 2>&1 | tee -a "$log"; [ "${PIPESTATUS[0]}" = 0 ] || rc=1
ts "rmmod aien_gpu"; sudo rmmod aien_gpu || rc=1
ts "aien-gpu part rc=$rc"
# Recovery proof for later windows (NoNvDriver review). Opt-in, Drake decides: AIEN_W0_FLR=1 window0-post.sh.
# With no driver bound, reset the function by the kernel's own reset_method (measured "flr bus"); NVIDIA's stock driver
# must then bind and answer below. If it does, "FLR + stock rebind" is a tested recovery path for window 1; if not,
# window 1 may only end in a reboot or a power cycle. A failed FLR leaves the lease/watchdog/restore reboot as the way out.
D=/sys/bus/pci/devices/000f:01:00.0
if [ "${AIEN_W0_FLR:-0}" != 1 ]; then ts "FLR test not requested (AIEN_W0_FLR unset): recovery-by-FLR stays NOT_RUN"
elif [ -e "$D/driver" ]; then ts "FLR test skipped: a driver is still bound ($(basename "$(readlink "$D/driver")"))"; rc=1
else ts "reset_method: $(cat "$D/reset_method")"; echo 1 | sudo tee "$D/reset" > /dev/null && ts "FLR via $D/reset: done" || { ts "FLR via $D/reset: FAILED"; rc=1; }; fi
sudo rm -f "$BLK"; ts "hold-back file removed before the stock reload: $([ -e "$BLK" ] && echo NO || echo yes)"
t0=$(date +%s%N); sudo modprobe nvidia && sudo modprobe nvidia_uvm; t1=$(date +%s%N); nvidia-smi -L >> "$log" 2>&1; t2=$(date +%s%N)
ts "nvidia modprobe ms $(( (t1 - t0) / 1000000 )), first nvidia-smi answer ms $(( (t2 - t1) / 1000000 )) (load-to-ready baseline)"
BIN=$HOME/workspace/targets/l332-omega/gpu_launch_cost_test
t0=$(date +%s%N); "$BIN" --reps 3 >> "$log" 2>&1; orc=$?; t1=$(date +%s%N)
ts "omega launch-cost test exit $orc wall ms $(( (t1 - t0) / 1000000 )) (first open after driver load)"
ts "WINDOW0 $( [ $rc = 0 ] && [ $orc = 0 ] && echo PASS || echo FAIL )"
[ "${1:-}" = --no-reboot ] && exit 0
ts "normal reboot to restore nvidia_modeset/nvidia_drm and the services"; sync; sudo systemctl reboot
