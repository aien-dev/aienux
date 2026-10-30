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

# pi5_fact_req.sh -- write a fact request file for the PI5-5 Cortex qualification image (make pi5-fact).
# QUALIFICATION ONLY. Layout: native/kernel/core/fact_req.h (512 bytes: magic AIENFCT1, version 1, flags 0,
# tag QUALIF-FACT-DATA, length 1..48, reserved 0, printable ASCII text, zero padding).
#
#   pi5_fact_req.sh <out file> "<fact text, 1..48 printable ASCII characters>"
#
# Refuses an existing output file. A fact is not a secret; do not put one in it anyway.
set -euo pipefail
out="${1:?output file}"
txt="${2:?fact text}"
[[ ! -e "${out}" ]] || { echo "refusing: ${out} exists" >&2; exit 1; }
n=${#txt}
(( n >= 1 && n <= 48 )) || { echo "fact must be 1..48 characters (got ${n})" >&2; exit 1; }
LC_ALL=C
[[ "${txt}" =~ ^[\ -~]+$ ]] || { echo "fact must be printable ASCII" >&2; exit 1; }
{
    printf 'AIENFCT1\x01\x00\x00\x00\x00\x00\x00\x00QUALIF-FACT-DATA'
    printf "\\x$(printf '%02x' "${n}")\\x00\\x00\\x00\\x00\\x00\\x00\\x00"
    printf '%s' "${txt}"
    head -c $((48 - n)) /dev/zero
    head -c 424 /dev/zero
} > "${out}"
[[ "$(stat -c %s "${out}")" == 512 ]] || { rm -f "${out}"; echo "internal error: size" >&2; exit 1; }
echo "wrote ${out} (512 bytes, fact of ${n} characters)"
