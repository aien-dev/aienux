#!/usr/bin/env bash
# L332 rung 2 window 1, part 2 (after the reboot from part 1). First removes the temporary blacklist. Then our module takes
# the GPU with both switches on, and aien_gsp_boot hands NVIDIA's signed GSP firmware to the chip through SEC2 (one
# attempt: the module refuses a second doorbell) and waits for GSP-RM's INIT_DONE. The module is NOT unloaded afterwards
# (GSP may still use its memory); a normal reboot gives the GPU back to NVIDIA's driver, and part 3 checks that.
# Usage: window1-post.sh [--no-reboot]
set -u
export PATH=$PATH:/usr/sbin:/sbin   # modinfo, lsmod and friends live in sbin; a normal user shell may not list it
here=$(cd "$(dirname "$0")" && pwd); repo=$(cd "$here/../../.." && pwd)
log=$here/$(readlink "$here/window1-current.log")
ts() { echo "$(date -u +%FT%T.%NZ) $*" | tee -a "$log"; }
# The /run hold-back file stays for the whole window boot (tmpfs: gone at the restore reboot), so nothing can load
# NVIDIA's driver beside ours.
grep -q 'aien.gpuwindow=1' /proc/cmdline || { ts "WINDOW1 ABORT: this is not the one-shot window boot (no aien.gpuwindow=1); nothing done"; exit 1; }
ts "post start; window boot; hold-back file present: $([ -e /run/modprobe.d/zz-aien-gpu-window.conf ] && echo yes || echo NO); lease: $(systemctl is-active aien-gpu-window-lease.service)"
ts "boot: $(uptime -s) (uptime $(cut -d' ' -f1 /proc/uptime) s)"
ts "lsmod nvidia: [$(lsmod | grep -cE '^nvidia')] driver in use on the GPU: $(lspci -k -s 000f:01:00.0 | grep -c 'in use')"
[ "$(lsmod | grep -cE '^nvidia')" = 0 ] || { ts "WINDOW1 ABORT: an NVIDIA module is loaded; two drivers on one GPU must never happen"; exit 1; }
PKG=$(cat "$here/window1-package.path"); SHIM=$repo/native/linux/aien-gpu
# arena: everything the builder places, plus 8 MiB for the logs, queues and page tables (the tool refuses if short)
bytes=$(cat "$PKG"/rm_elf.bin "$PKG"/rm_sig.bin "$PKG"/bootloader.bin "$PKG"/fmc.bin | wc -c)
arena_mib=$(( bytes / 1048576 + 8 + 1 ))
sudo sysctl -q -w kernel.panic=10 kernel.panic_on_oops=1; ts "panic auto-reboot 10 s set (runtime only)"
ts "insmod aien_gpu allow_bind=1 allow_boot=1 arena_mib=$arena_mib heap_mib=512"
sudo insmod "$SHIM/aien_gpu.ko" allow_bind=1 allow_boot=1 arena_mib="$arena_mib" heap_mib=512 || { ts "WINDOW1 FAIL: insmod"; sudo dmesg | grep -i aien-gpu | tee -a "$log"; exit 1; }
sudo dmesg | grep -i aien-gpu | tee -a "$log"
out=$here/window1-$(date -u +%Y%m%dT%H%M%SZ)-run; app=$(cat "$PKG/app_version.txt")
ts "aien_gsp_boot --pkg $PKG --app-version $app --out $out"
t0=$(date +%s%N)
sudo "$SHIM/build/aien_gsp_boot" --pkg "$PKG" --app-version "$app" --out "$out" --polls 50000 --pause-us 100 --wait-ms 30000 2>&1 | tee -a "$log"
brc=${PIPESTATUS[0]}; t1=$(date +%s%N)
sudo chown -R "$(id -un)" "$out"
ts "aien_gsp_boot exit $brc wall ms $(( (t1 - t0) / 1000000 ))"
cat "$out/result.txt" >> "$log" 2>/dev/null
sudo dmesg | tail -40 > "$out/dmesg-tail.txt"
ts "WINDOW1 $( [ "$brc" = 0 ] && echo 'PASS (GSP-RM INIT_DONE on our driver)' || echo "STOPPED (see $out/result.txt)" )"
[ "${1:-}" = --no-reboot ] && exit 0
ts "normal reboot: gives the GPU back to NVIDIA's driver (aien_gpu is not unloaded; GSP may still use its memory)"
sync; sudo systemctl reboot
