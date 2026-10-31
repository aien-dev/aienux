#!/bin/sh
# The firmware-hosted Allen with the native look, as one Omega unit for `target x86efi`.
# usage: build-fw.sh OUT.EFI      (OMEGA_COMPILER selects the compiler; its digest must be the pin)
# Order matters: globals and texts precede their use; every module lists what it needs.
set -eu
[ "$#" = 1 ] || { echo 'usage: build-fw.sh OUT.EFI' >&2; exit 64; }
root=$(CDPATH= cd -- "$(dirname "$0")/../../.." && pwd)
A=$root/native/omega/allen
compiler=${OMEGA_COMPILER:-"$root/build/allen-toolchain/seed/out/omc"}
[ -f "$compiler" ] && [ -x "$compiler" ] || { echo 'pinned compiler missing; follow native/omega/allen/README.md' >&2; exit 69; }
# Pin: omega-mv feat/x86efi-file-word 81ab1900 (draft #981 on #980 on #977): register slots, live-slot calls,
# the efi 11 file word, const/bitcast and the 65536-entry site tables the firmware unit needs.
[ "$(sha256sum "$compiler" | cut -d' ' -f1)" = 9995f8534e0e54470194d57f163421add779cf1994062f8d9a417d0a39f2b075 ] || {
 echo 'compiler digest differs from the qualified pin' >&2
 exit 65
}
out=$1
[ ! -e "$out" ] || { echo 'fresh output required' >&2; exit 73; }
unit="$out.om"
{
 echo 'target x86efi'
 for f in platform.om base.om vendor/sha256-core.om font-data.om font.om look-paint.om look-skydata.om look-sky.om \
  look-night.om look-mind.om look-home.om surface.om github.om http.om net.om tls-crypto.om vendor/cert-sha384.om \
  vendor/cert-p256.om tls-p384.om vendor/cert-root.om tls-cert.om tls.om session.om repository-screen.om runtime.om \
  code.om courier.om scheduler.om memory.om intent.om reply.om context.om application.om look-chat.om look-install.om \
  look-stat.om look-show.om efi-main.om demo.om; do cat "$A/$f"; done
} > "$unit"
# Names are one namespace across the unit: a duplicate fn/global/const/text/struct is refused here, not by omc's line.
dup=$(grep -E '^(fn|global|const|text|struct) ' "$unit" | awk '{print $2}' | sort | uniq -d)
[ -z "$dup" ] || { echo "duplicate names in the unit: $dup" >&2; exit 65; }
if ! "$compiler" < "$unit" > "$out.tmp" 2> "$out.err"; then
 rc=$?
 L=$(grep -o 'line [0-9]*' "$out.err" | cut -d' ' -f2 || true)
 echo "compile rc=$rc $(cat "$out.err")" >&2
 [ -n "$L" ] && { sed -n "${L}p" "$unit"; awk -v L="$L" 'NR<=L && /^fn /{f=$2} END{print "in fn " f}' "$unit"; } >&2
 rm -f "$out.tmp"
 exit "$rc"
fi
[ -s "$out.tmp" ]
# PE32+ for x64: MZ at 0 and the PE signature where e_lfanew points.
[ "$(head -c 2 "$out.tmp")" = MZ ] || { echo 'not a PE image' >&2; rm -f "$out.tmp"; exit 65; }
mv "$out.tmp" "$out"
echo "ok $(sha256sum "$out" | cut -c1-16) $(wc -c < "$out") bytes, $(wc -l < "$unit") lines, $(grep -c '^fn ' "$unit") fns, compiler $(sha256sum "$compiler" | cut -c1-8)"
