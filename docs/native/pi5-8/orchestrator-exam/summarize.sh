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

# Score every results/*.jsonl into results/summary.tsv (per split, plus split "all" = train+dev+heldout pooled),
# and write results/tokens.tsv (prompt/output token stats per model).
here=$(cd "$(dirname "$0")" && pwd)
{
  "$here/score.sh" --header
  for f in "$here"/results/*.*.*.jsonl; do
    b=$(basename "$f" .jsonl); mode=${b##*.}; r=${b%.*}; split=${r##*.}; name=${r%.*}
    "$here/score.sh" "$name" "$split" "$mode"
    [ "$split" = train ] && "$here/score.sh" "$name" all "$mode"
  done | sort -t$'\t' -k1,1 -k2,2 -k3,3
} > "$here/results/summary.tsv"
# token cost per model (free mode, dev): prefix = prompt tokens of the shortest message (D079), the prompt a Pi would cache once
{ printf 'model\tprefix_ptok\tmean_ptok\tmean_ctok\tmean_ms_host_cpu\n'
  for f in "$here"/results/*.dev.free.jsonl; do
    n=$(basename "$f" .dev.free.jsonl)
    jq -rs --arg n "$n" '(map(select(.id=="D079").ptok)[0]) as $p | [$n,$p,(map(.ptok)|add/length|round),((map(.ctok)|add/length*10|round)/10),(map(.ms)|add/length|round)]|@tsv' "$f"
  done
} > "$here/results/tokens.tsv"
