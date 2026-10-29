#!/bin/bash
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

# Deterministic split of the frozen pool (cases-pool.psv, 102 cases written before the prompt freeze) into
# cases-train.psv and cases-dev.psv. Within each category, in file order, every 4th case (positions 4, 8, ...) goes to dev, the rest to train.
# The held-out set (cases-heldout.psv) was written after the freeze and is never part of this split.
here=$(cd "$(dirname "$0")" && pwd)
hdr='# id|category|expected_action|expected_text|turns (user ~~ assistant ~~ user ...; the last turn is the one scored)'
echo "$hdr" > "$here/cases-train.psv"; echo "$hdr" > "$here/cases-dev.psv"
grep -v '^#' "$here/cases-pool.psv" | awk -F'|' -v d="$here/cases-dev.psv" -v t="$here/cases-train.psv" '{c[$2]++; if (c[$2]%4==0) print >> d; else print >> t}'
