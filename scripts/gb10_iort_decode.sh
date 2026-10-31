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

# gb10_iort_decode.sh: dump the firmware IORT table (read-only) and resolve the
# GB10 (PCI segment 15, requester id 0x0100) to its SMMUv3 node and stream id.
# Host only. Needs root to read /sys/firmware/acpi/tables/IORT. Changes nothing.
# aienos#286 cut B5; see docs/GB10_IORT_DECODE.md.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${1:-/tmp/aienos-iort}
seg=${2:-15}
rid=${3:-0x0100}
mkdir -p "$out"
sudo cat /sys/firmware/acpi/tables/IORT > "$out/IORT.bin"
rustc --edition 2021 -O -o "$out/iort_decode" "$here/iort_decode.rs"
sha256sum "$out/IORT.bin"
"$out/iort_decode" "$out/IORT.bin" "$seg" "$rid"
