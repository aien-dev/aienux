#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Drake Stapleton (AIEN / aien-dev)
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# efi-commit.sh -- print the source commit stamp compiled into an AIENOS kernel EFI.
#
#   efi-commit.sh <EFI file>
#
# The kernel prints its build stamp (native/kernel/Makefile AIENOS_COMMIT) as
# "aienos_commit: <stamp>" in every report. The stamp is a plain 40-digit hex
# string in the image; a Pi build (CK_PI5=1) made from a tree whose tracked files
# differ from HEAD carries "<40 hex>-dirty" instead. This script greps the image
# for it (grep -a) so the stamp can be read without booting the image.
#
# Prints the stamp on stdout.  Exit 0: exactly one stamp.  1: none found (for
# example a build without git, stamp "unknown").  2: more than one distinct stamp.
# 3: the file cannot be read.
# Used by pi5-run.sh (logged in run.log) and scripts/ck_pi5_build_check.sh.
set -euo pipefail
efi="${1:?EFI file}"
[[ -r "${efi}" ]] || { echo "efi-commit: cannot read ${efi}" >&2; exit 3; }
# Runs of 40 or more hex digits (optionally followed by -dirty), then keep only
# exactly 40 digits: a longer run (a 64-digit SHA-256 in the image) is not a stamp.
stamps=$(LC_ALL=C grep -a -o -E '[0-9a-f]{40,}(-dirty)?' "${efi}" \
    | LC_ALL=C awk '/^[0-9a-f]{40}(-dirty)?$/' | LC_ALL=C sort -u || true)
n=$(printf '%s' "${stamps}" | grep -c . || true)
case "${n}" in
    1) printf '%s\n' "${stamps}" ;;
    0) echo "none"; exit 1 ;;
    *) echo "ambiguous: $(printf '%s' "${stamps}" | tr '\n' ' ')"; exit 2 ;;
esac
