#!/bin/sh
# Compose the firmware-hosted Allen sources in the order that compiles, add the `target x86efi`
# selector, and build the EFI application the boot image carries.
#
# The twenty sources are not on main: they live on aienux `feat/allen-imac-competition`
# (`d4ffde3c`). Build them from a checkout of that branch, or point ALLEN_SRC at one.
#
#   OMEGA_COMPILER=<seed>/seed/out/omc sh build-efi.sh BOOTX64.EFI
#   OMEGA_COMPILER=<seed>/seed/out/omc sh build-efi.sh BOOTX64.EFI ff17a936...
#
# Second argument, if given, is the expected sha256; a difference exits 65 instead of passing.
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
allen=${ALLEN_SRC:-"$root/native/omega/allen"}
compiler=${OMEGA_COMPILER:-}
files="platform.om base.om vendor/sha256-core.om font-data.om font.om surface.om github.om
http.om net.om tls-crypto.om vendor/cert-sha384.om vendor/cert-p256.om tls-p384.om
vendor/cert-root.om tls-cert.om tls.om session.om repository-screen.om efi-main.om demo.om"

[ -n "$compiler" ] && [ -x "$compiler" ] || {
 echo 'OMEGA_COMPILER must name the seed compiler (seed/out/omc)' >&2
 exit 69
}

missing=0
for f in $files; do
 if [ ! -f "$allen/$f" ]; then
  echo "missing $allen/$f" >&2
  missing=1
 fi
done
[ "$missing" = 0 ] || {
 echo 'the Allen sources are on aienux feat/allen-imac-competition (d4ffde3c), not on main' >&2
 exit 66
}

out=${1:-BOOTX64.EFI}
[ ! -e "$out" ] && [ ! -L "$out" ] || { echo 'fresh output required' >&2; exit 73; }
unit="${out}.om"
trap 'rm -f -- "$unit"' EXIT HUP INT TERM

{
 echo 'target x86efi'
 for f in $files; do
  cat "$allen/$f"
 done
} > "$unit"

"$compiler" < "$unit" > "$out"
got=$(sha256sum "$out" | cut -d' ' -f1)
printf '%s  %s\n' "$got" "$out"
printf 'compiler %s\n' "$(sha256sum "$compiler" | cut -d' ' -f1)"

want=${2:-}
if [ -n "$want" ] && [ "$got" != "$want" ]; then
 echo "expected $want; the sources or compiler differ" >&2
 exit 65
fi
