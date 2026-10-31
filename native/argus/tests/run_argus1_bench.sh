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

set -euo pipefail

bench_bin=$1
mode=$2
integration_bin=$3
lane_out=$4
quiet_file="${HOME}/workspace/.spark-quiet"
lock_file="${HOME}/workspace/.argus-bench.lock"

if [[ -e "$quiet_file" ]]; then
    echo "ARGUS-1 benchmark blocked: .spark-quiet is present" >&2
    exit 2
fi
if ! command -v taskset >/dev/null || ! command -v flock >/dev/null; then
    echo "ARGUS-1 benchmark needs taskset and flock on the Spark host" >&2
    exit 2
fi

flock -n "$lock_file" bash -c '
    set -euo pipefail
    taskset -c 7 "$1"
    if [[ "$2" == full ]]; then
        taskset -c 7 make -f lane_b.mk LANE_B_OUT="$4" bench-ring
        taskset -c 7 "$3" --bench
    fi
' bash "$bench_bin" "$mode" "$integration_bin" "$lane_out"
