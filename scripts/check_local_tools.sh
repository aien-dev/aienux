#!/usr/bin/env bash
# Compile the pinned native Omega units and compare with signed embedded code.
set -euo pipefail
cd "$(dirname "$0")/.."
om=${OMEGA_SOURCE:?set OMEGA_SOURCE to the clean pinned Omega checkout}
[[ $(git -C "$om" rev-parse HEAD) = bf54b19fa590e8ae925c0e80815516dbf41e7326 ]]
[[ $(sha256sum "$om/seed/out/omc" | cut -d' ' -f1) = 62775d2a391cfef7cde64e387655caa9809a267093112910ec76bfa568f3712b ]]
[[ $(sha256sum "$om/seed/omc/omc.om" | cut -d' ' -f1) = 96d51b63c8f451661ac715395078e04f39195e50c9e8fe37746cda121e4d7a1e ]]
out=${LOCAL_TOOLS_OUT:-$PWD/target/local-tools-host}
mkdir -p "$out"
for name in arithmetic text system; do
    "$om/seed/out/omc" < "native/kernel/x86/tools/$name.om" > "$out/$name.raw"
done
cc -std=c11 -O2 -Wall -Wextra -Werror -Inative/sig -Inative/argus \
    native/kernel/x86/tests/local_admission_test.c native/kernel/artifact/format.c \
    native/argus/sha256.c native/sig/ed25519.c native/sig/sha512.c native/sig/ct.c -o "$out/admission"
"$out/admission" "$out/arithmetic.raw" "$out/text.raw" "$out/system.raw"
cargo test -p allen-courier --bin allen-courier
