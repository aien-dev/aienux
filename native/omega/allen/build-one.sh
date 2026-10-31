#!/bin/sh
# External development orchestration. Every compiler invocation verifies its pin.
set -eu
[ "$#" = 2 ] || { echo 'usage: build-one.sh SOURCE OUTPUT' >&2; exit 64; }
root=$(CDPATH= cd -- "$(dirname "$0")/../../.." && pwd)
compiler=${OMEGA_COMPILER:-"$root/build/allen-toolchain/seed/out/omc"}
[ -f "$compiler" ] && [ -x "$compiler" ] || {
 echo 'pinned compiler missing; follow native/omega/allen/README.md' >&2
 exit 69
}
# Pin: omega-mv feat/x86efi-file-word 81ab1900 (draft #981 on #980 on #977): register slots, live-slot calls, the efi 11 file word, const/bitcast and the site tables.
[ "$(sha256sum "$compiler" | cut -d' ' -f1)" = 9995f8534e0e54470194d57f163421add779cf1994062f8d9a417d0a39f2b075 ] || {
 echo 'compiler digest differs from the qualified pin' >&2
 exit 65
}
[ ! -e "$2" ] && [ ! -L "$2" ] || { echo 'fresh output required' >&2; exit 73; }
temporary=$(mktemp "${2}.build.XXXXXX")
trap 'rm -f -- "$temporary" "$temporary.header" "$temporary.dynamic"' EXIT
trap 'exit 74' HUP INT TERM
"$compiler" < "$1" > "$temporary"
[ -s "$temporary" ]
readelf -h "$temporary" > "$temporary.header"
grep -q 'Machine:.*AArch64' "$temporary.header"
grep -q 'Type:.*EXEC' "$temporary.header"
readelf -l -d "$temporary" > "$temporary.dynamic"
if grep -Eq 'INTERP|NEEDED' "$temporary.dynamic"; then exit 65; fi
chmod 700 "$temporary"
# Atomic no-replace publication also refuses a link created during compilation.
ln -T -- "$temporary" "$2"
