#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Host/disposable-QEMU verification only; no device path argument exists.
set -eu
cd "$(dirname "$0")/../../.."
: "${AIEN_OMC:?set AIEN_OMC to the existing pinned seeded compiler}"
want=bcfa582892afca8f65e551fe56867d918d86e84540c7381be8fa9ef18d0cf0f6
actual=$(sha256sum "$AIEN_OMC")
[ "${actual%% *}" = "$want" ] || { echo 'compiler pin mismatch' >&2; exit 2; }
out=${OMEGA_NVME_EVIDENCE:-$(mktemp -d /tmp/omega-nvme-evidence-XXXXXX)}
mkdir -p "$out"
case "$out" in /*) ;; *) echo 'evidence directory must be absolute' >&2; exit 2;; esac
base=native/disk
compose() { cat "$base/omega_nvme.om" "$base/omega_nvme_transport.om" "$1"; }
compile() { nice -n 15 "$AIEN_OMC" < "$1" > "$2"; chmod +x "$2"; }
compose "$base/omega_nvme_main.om" > "$out/read.om"
compile "$out/read.om" "$out/read"
compose "$base/tests/omega_nvme_probe.om" > "$out/probe.om"
compile "$out/probe.om" "$out/probe"
compose "$base/tests/omega_nvme_adminwrap.om" > "$out/adminwrap.om"
compile "$out/adminwrap.om" "$out/adminwrap"
compose "$base/tests/omega_nvme_failstop.om" > "$out/failstop.om"
compile "$out/failstop.om" "$out/failstop"
${CC:-cc} -std=c11 -O2 -Wall -Wextra -Werror -Wno-unused-function \
 "$base/tests/omega_nvme_runner.c" "$base/disk_nvme.c" "$base/disk.c" -o "$out/runner"
: > "$out/host.log"
for c in healthy singlepage lba4k dstrd serial firmware approval nsid extent range before prp zero metadata format nsze sqes cqes cap stride readytimeout readycfs status; do
 nice -n 15 "$out/runner" "$out/read" "$c" >> "$out/host.log"
done
nice -n 15 "$out/runner" "$out/adminwrap" adminwrap >> "$out/host.log"
for c in cid sqid sqhead phase timeout cfs; do
 nice -n 15 "$out/runner" "$out/failstop" "$c" >> "$out/host.log"
done
for c in opcode0 opcode1 opcode8 opcode9 opcode128 opcode132 adminformat; do
 nice -n 15 "$out/runner" "$out/probe" "$c" >> "$out/host.log"
done
# Remove exactly the named three-line refusal block in disposable sources.
# These are verification mutants, never source edits or candidate artifacts.
mutate() {
 awk -v target="$1" 'BEGIN {skip=0;hits=0} $0==target {skip=3;hits++} skip>0 {skip--;next} {print} END {if(hits!=1)exit 2}' "$2" > "$3"
}
: > "$out/mutants.log"
for spec in 'cid|if ne and dw3 65535 cid|cid|read' \
 'sqid|if ne shr dw2 16 q|sqid|read' \
 'status|if ne and shr dw3 17 2047 0|status|read' \
 'sqhead|if ne and dw2 65535 get tail q|sqhead|read' \
 'reopen|if gt get st 0 0|timeout|failstop' \
 'opcode|if not ok|opcode1|probe'; do
 name=${spec%%|*}; rest=${spec#*|}; guard=${rest%%|*}; rest=${rest#*|}; case_name=${rest%%|*}; source=${rest#*|}
 mutate "$guard" "$out/$source.om" "$out/mutant-$name.om"
 compile "$out/mutant-$name.om" "$out/mutant-$name"
 if nice -n 15 "$out/runner" "$out/mutant-$name" "$case_name" > "$out/mutant-$name.log" 2>&1; then
  echo "FAIL mutant survived $name" >&2; exit 1
 fi
 echo "PASS killed guard-removal mutant $name (baseline case passed)" >> "$out/mutants.log"
done
# Retention negative control: delete only the in-flight ownership marker.
awk '$0=="put st 1 1" {hits++;next} {print} END {if(hits!=1)exit 2}' "$out/read.om" > "$out/mutant-retention.om"
compile "$out/mutant-retention.om" "$out/mutant-retention"
if nice -n 15 "$out/runner" "$out/mutant-retention" timeout > "$out/mutant-retention.log" 2>&1; then
 echo 'FAIL retention mutant survived' >&2; exit 1
fi
echo 'PASS killed retention guard-removal mutant' >> "$out/mutants.log"
: > "$out/qemu.log"
# Firmware identity is intentionally pinned; a different QEMU version refuses.
qemu-system-aarch64 --version > "$out/qemu-version.txt"
for c in healthy serial firmware approval nsid extent range before prp zero; do
 nice -n 15 "$out/runner" "$out/read" "$c" qemu >> "$out/qemu.log"
done
sha256sum "$AIEN_OMC" "$base/omega_nvme.om" "$base/omega_nvme_transport.om" \
 "$base/omega_nvme_main.om" "$base/tests/omega_nvme_runner.c" "$out/read" > "$out/sha256.txt"
printf '%s\n' 'PASS Omega NVMe host + QEMU + guard mutants; physical NOT_RUN' | tee "$out/result.txt"
printf 'Evidence: %s\n' "$out"
