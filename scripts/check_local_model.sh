#!/usr/bin/env bash
# Qualification only: compare native Omega model output to the pinned independent
# C decoder. Builds native/kernel/x86/model/check_local_model.c with the host C
# compiler and runs it. Usage: check_local_model.sh EVIDENCE_DIR
# EVIDENCE_DIR holds omega-model-probe, reference-probe, llama.raw and checkpoints.
set -euo pipefail
cd "$(dirname "$0")/.."
[[ $# -eq 1 ]] || { echo "usage: check_local_model.sh EVIDENCE_DIR" >&2; exit 2; }
tmp=$(mktemp -d "${TMPDIR:-/tmp}/check_local_model.XXXXXX")
trap 'rm -rf "$tmp"' EXIT
"${HOSTCC:-cc}" -std=c11 -D_DEFAULT_SOURCE -O2 -Wall -Wextra -Werror -Inative/argus \
    -o "$tmp/check_local_model" native/kernel/x86/model/check_local_model.c native/argus/sha256.c -lm
"$tmp/check_local_model" "$1"
