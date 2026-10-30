#!/usr/bin/env bash
# L332 rung 1 host gates: devc tests, generated-code drift, simulator twin, differential trace, freestanding, contract mutants, GSP start-up sim + mutants.
set -u
A=$HOME/workspace/aienos-own-gpu-driver D=$HOME/workspace/aienos-devc-gpu O=/tmp/l332-k
cd "$D" && cargo test -q -p aienos-devc 2>&1 | tail -3 && cargo build -q --release -p aienos-devc || { echo "GATE devc build FAIL"; exit 1; }
DEVC=$D/target/release/aienos-devc
t=$(mktemp -d); "$DEVC" gen "$A/native/kernel/gpu/contract/gb10_chan.devc" "$t" >/dev/null || { echo "GATE gen FAIL"; exit 1; }
for f in "$t"/*; do cmp -s "$f" "$A/native/kernel/gpu/contract/gen/$(basename "$f")" && echo "drift: $(basename "$f") identical" || echo "GATE drift FAIL: $(basename "$f")"; done
cd "$A"
make -s -C native/kernel OUT=$O test-gpu-contract 2>&1 | tail -4
make -s -C native/kernel OUT=$O gpu-contract-trace 2>&1 | tail -2
make -s -C native/kernel OUT=$O gpu-contract-freestanding 2>&1 | tail -2
make -s -C native/kernel OUT=$O test-gpu-sim 2>&1 | tail -2
bash evidence/own-gpu-driver/rung1/contract_mutants.sh "$DEVC" "$A/native/kernel" /tmp/l332-km 2>&1 | tail -14
make -s -C native/kernel OUT=$O test-gsp-mmio 2>&1 | tail -3
make -s -C native/kernel OUT=$O gsp-mmio-freestanding 2>&1 | tail -2
bash evidence/own-gpu-driver/rung1/gsp_mmio_mutants.sh "$A/native/kernel" 2>&1 | tail -3
t2=$(mktemp -d); "$DEVC" gen "$A/native/kernel/gpu/contract/gb10_gsp.devc" "$t2" >/dev/null || { echo "GATE gsp gen FAIL"; exit 1; }
for f in "$t2"/*; do cmp -s "$f" "$A/native/kernel/gpu/contract/gen/$(basename "$f")" && echo "drift: $(basename "$f") identical" || echo "GATE drift FAIL: $(basename "$f")"; done
make -s -C native/kernel OUT=$O test-gsp-contract 2>&1 | tail -3
make -s -C native/kernel OUT=$O gsp-contract-trace 2>&1 | tail -2
make -s -C native/kernel OUT=$O gsp-contract-freestanding 2>&1 | tail -2
bash evidence/own-gpu-driver/rung1/gsp_contract_mutants.sh "$DEVC" "$A/native/kernel" 2>&1 | tail -3
echo "gates exit done"
