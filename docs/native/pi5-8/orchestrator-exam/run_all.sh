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

# Run the exam on every model in models.tsv (downloaded by download_models.sh), JOBS models at a time.
# usage: run_all.sh   env: JOBS (default 3) THREADS (default 6) MODELS_DIR
here=$(cd "$(dirname "$0")" && pwd)
root=${MODELS_DIR:-$HOME/models/pi5-8-orchestrator}
export THREADS=${THREADS:-6}
grep -v '^#' "$here/models.tsv" | while IFS=$'\t' read -r name role repo url file; do
  t=""
  echo "$name|$root/$name/$file|$t"
done | xargs -P "${JOBS:-3}" -I{} bash -c 'IFS="|" read -r n f t <<<"{}"; TEMPLATE=$t '"$here"'/run_exam.sh "$f" "$n" >/dev/null 2>>'"$here"'/results/run_all.err; echo done $n'
