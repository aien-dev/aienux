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

# Tables for ALLEN REFLEX (docs/native/PI5-8-ALLEN-REFLEX.md). Reads only results/allen-reflex.<split>.<mode>.jsonl.
# usage: reflex_tables.sh <split> [<split> ...]   e.g. reflex_tables.sh train dev
# 1. score.sh rows (unchanged scorer), 2. per category: action ok / exact ok / n, 3. per expected label: action ok / exact ok / n, 4. macro exact (mean over expected labels present).
here=$(cd "$(dirname "$0")" && pwd)
modes="reflex nearest perceptron"
"$here/score.sh" --header
for s in "$@"; do for m in $modes; do "$here/score.sh" allen-reflex "$s" "$m"; done; done
norm='def norm: ascii_downcase | gsub("\\s+";" ") | sub("[.!?;,\\s]+$";"") | sub("^\\s+";"") | sub("\\s+$";"");'
for key in cat exp_action; do
  printf '\nper %s: action_ok/exact_ok/n\n' "$key"
  for s in "$@"; do
    for m in $modes; do
      jq -r "$norm"' (.raw|fromjson) as $o
        | [.'"$key"', (if $o.action==.exp_action then 1 else 0 end),
           (if $o.action==.exp_action and ((.exp_text=="") or (($o.args.text // "")|norm)==(.exp_text|norm)) then 1 else 0 end)] | @tsv' \
        "$here/results/allen-reflex.$s.$m.jsonl" |
      awk -F'\t' -v s="$s" -v m="$m" '{a[$1]+=$2; e[$1]+=$3; n[$1]++} END{for(k in n) printf "%s\t%s\t%s\t%d/%d/%d\n", s, m, k, a[k], e[k], n[k]}' | sort -k3,3
    done
  done
done
printf '\nmacro exact over expected labels\n'
for s in "$@"; do
  for m in $modes; do
    jq -r "$norm"' (.raw|fromjson) as $o
      | [.exp_action, (if $o.action==.exp_action and ((.exp_text=="") or (($o.args.text // "")|norm)==(.exp_text|norm)) then 1 else 0 end)] | @tsv' \
      "$here/results/allen-reflex.$s.$m.jsonl" |
    awk -F'\t' -v s="$s" -v m="$m" '{e[$1]+=$2; n[$1]++} END{t=0; k=0; for(x in n){t+=e[x]/n[x]; k++} printf "%s\t%s\t%.1f%% over %d labels\n", s, m, 100*t/k, k}'
  done
done
