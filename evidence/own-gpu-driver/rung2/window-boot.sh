#!/usr/bin/env bash
# window-boot.sh: the self-reverting boot path for L332 GPU windows (NoNvDriver review, 2026-10-26).
#
# Replaces "write /etc/modprobe.d/zz-aien-gpu-window*.conf and reboot". That file persists: if part 2 never runs, every
# later boot (watchdog reset, power cycle) comes up WITHOUT NVIDIA's driver until a person deletes it. Here instead:
#   - GRUB default stays entry 0 (NVIDIA's driver). GRUB_DEFAULT is not touched; grub.cfg is not regenerated.
#   - /boot/grub/custom.cfg (sourced by /etc/grub.d/41_custom) adds ONE extra entry 'aien-gpu-window'. 'arm' selects it
#     for the next boot only, via grub-reboot (grubenv next_entry; 00_header honours it with GRUB_DEFAULT=0 and clears
#     it on use).
#   - That entry adds: aien.gpuwindow=1 (activates the two inert units), systemd.watchdog_sec=30 (PID 1 arms the SBSA
#     hardware watchdog for this boot: a hard hang resets the box into the default entry), panic=10 oops=panic
#     (a kernel panic or oops reboots after 10 s into the default entry).
#   - aien-gpu-window-hold.service writes the "do not load NVIDIA" file to /run/modprobe.d (tmpfs: this boot only).
#   - aien-gpu-window-lease.service reboots 25 min after boot (L405 lease 20 min + 5 min restore margin).
# Any reboot after the window boot, wanted or not, lands on NVIDIA's driver with nothing to clean up.
#
# Usage:
#   window-boot.sh check            (default) read-only: builds the entry and units into a temp dir, runs grub-script-check
#                                   and systemd-analyze verify, shows what install would change. Writes nothing outside /tmp.
#   window-boot.sh install --yes    copies custom.cfg and the two units (inert on normal boots). Coordinator + Drake approval.
#   window-boot.sh arm --yes        next boot only = 'aien-gpu-window' (grub-reboot). Inside the window only.
#   window-boot.sh status           what is installed / armed now.
#   window-boot.sh uninstall --yes  removes custom.cfg and the units.
set -euo pipefail
export PATH=$PATH:/usr/sbin:/sbin
HERE=$(cd "$(dirname "$0")" && pwd)
ENTRY=aien-gpu-window
UNITS="aien-gpu-window-hold.service aien-gpu-window-lease.service"
KVER=$(uname -r)
CUSTOM=/boot/grub/custom.cfg
die() { echo "WINDOW_BOOT: FAIL ($*)" >&2; exit 1; }

# The entry repeats the running kernel's command line (minus GRUB's BOOT_IMAGE) and adds the window flags.
build_entry() {
  local uuid cmd
  uuid=$(sed -n 's/.*root=UUID=\([0-9a-fA-F-]\{36\}\).*/\1/p' /proc/cmdline)
  [[ -n $uuid ]] || die "no root=UUID= in /proc/cmdline"
  [[ $(findmnt -no UUID -T /boot) == "$uuid" ]] || die "/boot is not on the root filesystem $uuid; entry paths would be wrong"
  [[ -f /boot/vmlinuz-$KVER && -f /boot/initrd.img-$KVER ]] || die "kernel or initrd for $KVER missing in /boot"
  cmd=$(sed 's/^BOOT_IMAGE=[^ ]* //' /proc/cmdline)
  case " $cmd " in *" aien.gpuwindow="*|*" module_blacklist="*|*" modprobe.blacklist="*) die "running cmdline already carries window flags";; esac
  cat <<EOF
# AIEN GPU window entry (L332 / NoNvDriver). Never the default: selected for one boot by 'window-boot.sh arm'.
menuentry '$ENTRY' --id $ENTRY {
	insmod gzio
	insmod part_gpt
	insmod ext2
	search --no-floppy --fs-uuid --set=root $uuid
	linux /boot/vmlinuz-$KVER $cmd aien.gpuwindow=1 systemd.watchdog_sec=30 panic=10 oops=panic
	initrd /boot/initrd.img-$KVER
}
EOF
}

check() {
  local t; t=$(mktemp -d /tmp/window-boot.XXXXXX)
  build_entry > "$t/custom.cfg"
  grub-script-check "$t/custom.cfg" || die "grub-script-check refused $t/custom.cfg"
  echo "entry: grub-script-check PASS ($t/custom.cfg)"
  grep -q 'custom.cfg' /etc/grub.d/41_custom || die "/etc/grub.d/41_custom does not source custom.cfg"
  grep -q 'next_entry' /etc/grub.d/00_header || die "/etc/grub.d/00_header has no next_entry (grub-reboot would not work)"
  echo "grub: 41_custom sources custom.cfg; 00_header honours next_entry; GRUB_DEFAULT=$(sed -n 's/^GRUB_DEFAULT=//p' /etc/default/grub) unchanged"
  [[ -e /sys/class/watchdog/watchdog0 ]] && echo "watchdog: $(cat /sys/class/watchdog/watchdog0/identity) present ($(cat /sys/class/watchdog/watchdog0/state))" \
    || echo "watchdog: NONE (systemd.watchdog_sec has no device; hangs need a power cycle)"
  for u in $UNITS; do cp "$HERE/window-boot/$u" "$t/$u"; done
  (cd "$t" && systemd-analyze verify --man=no $UNITS) || die "systemd-analyze verify refused the units"
  echo "units: systemd-analyze verify PASS"
  local initrd; initrd=$(lsinitramfs "/boot/initrd.img-$KVER") && [[ -n $initrd ]] || die "cannot list /boot/initrd.img-$KVER: initramfs check would pass silently"
  grep -qE '/nvidia(_uvm|_modeset|_drm)?\.ko' <<< "$initrd" && die "nvidia modules in initramfs: /run/modprobe.d would come too late"
  echo "initramfs: no nvidia modules"
  if [[ -e $CUSTOM ]]; then diff -u "$CUSTOM" "$t/custom.cfg" && echo "custom.cfg: installed copy identical" || echo "custom.cfg: install would REPLACE the file above"; else echo "custom.cfg: not installed (install would create it)"; fi
  echo "WINDOW_BOOT CHECK: PASS (nothing written outside $t)"
}

need_yes() { [[ ${1:-} == --yes ]] || die "$2 needs --yes"; }

case ${1:-check} in
  check) check ;;
  install) need_yes "${2:-}" install; check
    t=$(mktemp -d /tmp/window-boot.XXXXXX); build_entry > "$t/custom.cfg"
    [[ ! -e $CUSTOM ]] || cmp -s "$CUSTOM" "$t/custom.cfg" || die "$CUSTOM exists with other content; refusing to overwrite"
    sudo install -m 0644 "$t/custom.cfg" "$CUSTOM"
    for u in $UNITS; do sudo install -m 0644 "$HERE/window-boot/$u" "/etc/systemd/system/$u"; done
    sudo systemctl daemon-reload; sudo systemctl enable $UNITS
    echo "WINDOW_BOOT INSTALL: PASS (inert until 'arm')" ;;
  arm) need_yes "${2:-}" arm
    [[ -f $CUSTOM ]] && grep -qF -- "--id $ENTRY" "$CUSTOM" || die "entry not installed"
    for u in $UNITS; do systemctl is-enabled -q "$u" || die "$u not enabled"; done
    sudo grub-reboot "$ENTRY"
    [[ $(grub-editenv /boot/grub/grubenv list | sed -n 's/^next_entry=//p') == "$ENTRY" ]] || die "grubenv next_entry not set"
    echo "WINDOW_BOOT ARM: PASS (next boot only: $ENTRY; every later boot: default entry with NVIDIA)" ;;
  status)
    echo "default: GRUB_DEFAULT=$(sed -n 's/^GRUB_DEFAULT=//p' /etc/default/grub); grubenv: $(grub-editenv /boot/grub/grubenv list | tr '\n' ' ')"
    [[ -f $CUSTOM ]] && echo "custom.cfg: installed" || echo "custom.cfg: absent"
    for u in $UNITS; do echo "$u: $(systemctl is-enabled "$u" 2>/dev/null || true) $(systemctl is-active "$u" 2>/dev/null || true)"; done
    grep -q 'aien.gpuwindow=1' /proc/cmdline && echo "this boot: WINDOW BOOT" || echo "this boot: normal" ;;
  uninstall) need_yes "${2:-}" uninstall
    sudo systemctl disable $UNITS || true
    for u in $UNITS; do sudo rm -f "/etc/systemd/system/$u"; done
    sudo rm -f "$CUSTOM"; sudo systemctl daemon-reload
    echo "WINDOW_BOOT UNINSTALL: PASS" ;;
  *) die "unknown command ${1}" ;;
esac
