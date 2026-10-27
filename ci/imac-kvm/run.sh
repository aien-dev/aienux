#!/bin/sh
# usage: run.sh <image.gz> <expected sha256> <outdir>
# Boots the image as USB mass storage under OVMF with KVM and a fresh vars file, no
# virtio-rng, e1000 with the iPXE EFI option ROM; types the repository request and
# screendumps every 15 s. Emulator evidence only (EXERCISED), never hardware.
set -eu
gz=$1 want=$2 out=$3
mkdir -p "$out"
gunzip -c "$gz" > "$out/disk.img"
got=$(sha256sum "$out/disk.img" | cut -d' ' -f1)
[ "$got" = "$want" ] || { echo "image sha $got != $want"; exit 1; }
cp /usr/share/OVMF/OVMF_VARS_4M.fd "$out/vars.fd"
qemu-system-x86_64 -enable-kvm -machine q35 -cpu host -m 2048 -smp 2 -display none \
 -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
 -drive if=pflash,format=raw,file="$out/vars.fd" \
 -device qemu-xhci,id=xhci -drive if=none,id=stick,format=raw,file="$out/disk.img" \
 -device usb-storage,bus=xhci.0,drive=stick \
 -device e1000,netdev=n0,romfile=/usr/lib/ipxe/qemu/efi-e1000.rom \
 -netdev user,id=n0 -object filter-dump,id=f1,netdev=n0,file="$out/run.pcap" \
 -serial file:"$out/serial.log" -no-reboot -monitor unix:"$out/mon",server,nowait >"$out/qemu.log" 2>&1 &
qp=$!
say() { printf '%s\n' "$1" | socat - unix-connect:"$out/mon" >/dev/null; }
sleep 20
say "screendump $out/t00.ppm"
for k in o p e n spc a l l e n spc g i t h u b spc r e p o s i t o r y; do say "sendkey $k"; sleep 0.12; done
say "sendkey ret 400"
date -u +%H:%M:%S > "$out/typed.txt"
i=1
while [ "$i" -le 8 ]; do
 sleep 15
 say "screendump $out/t$(printf %02d "$i").ppm"
 i=$((i + 1))
done
say quit || kill "$qp"
wait "$qp" || true
rm -f "$out/disk.img"
for p in "$out"/*.ppm; do pnmtopng "$p" > "${p%.ppm}.png" && rm "$p"; done
tcpdump -nr "$out/run.pcap" > "$out/pcap.txt" 2>&1 || true
