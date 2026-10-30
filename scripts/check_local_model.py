#!/usr/bin/env python3
"""Qualification only. Compare native Omega to the pinned independent C decoder.

Usage: check_local_model.py EVIDENCE_DIR
Directory contains omega-model-probe, reference-probe, llama.raw and checkpoints.
No Python or C inference runtime is linked into the iMac image.
"""
import hashlib
import json
import math
from pathlib import Path
import platform
import struct
import subprocess
import sys

root = Path(sys.argv[1]).resolve()
pins = {
    'stories15M.bin': 'cd590644d963867a2b6e5a1107f51fad663c41d79c149fbecbbb1f95fa81f49a',
    'stories260K.bin': 'b0a507e7ad0f626624f17112325e66691f9076d622e1d3274d103d00299f2696',
}
for name, pin in pins.items():
    assert hashlib.sha256((root / name).read_bytes()).hexdigest() == pin, name
prefix = [] if platform.machine() == 'x86_64' else ['qemu-x86_64']
results = []
for model, count, prompt in [
    ('stories260K', 1, [1]), ('stories260K', 8, [1]),
    ('stories15M', 1, [1]), ('stories15M', 8, [1]),
    ('stories15M', 32, [1]), ('stories15M', 8, [1, 9038, 2501, 263, 931]),
]:
    case = f'{model}-{count}-p{len(prompt)}'
    omega_out, ref_out = root / f'{case}-omega.f32', root / f'{case}-reference.f32'
    native = subprocess.check_output(prefix + [str(root / 'omega-model-probe'),
        str(root / 'llama.raw'), str(root / f'{model}.bin'), str(count), str(omega_out)]
        + list(map(str, prompt)), text=True)
    reference = subprocess.check_output([str(root / 'reference-probe'),
        str(root / f'{model}.bin'), str(count), str(ref_out)] + list(map(str, prompt)), text=True)
    assert native.split('ids=')[1].strip() == reference.split('ids=')[1].strip(), case
    a_bytes, b_bytes = omega_out.read_bytes(), ref_out.read_bytes()
    assert len(a_bytes) == len(b_bytes)
    a = struct.unpack(f'<{len(a_bytes)//4}f', a_bytes)
    b = struct.unpack(f'<{len(b_bytes)//4}f', b_bytes)
    assert all(math.isfinite(v) for v in a + b)
    error = max(abs(x-y) / (1e-3 + 1e-4 * abs(y)) for x, y in zip(a, b))
    assert error <= 1, (case, error)
    results.append(dict(case=case, tokens_match=True, worst_scaled_error=error,
                        max_abs_error=max(abs(x-y) for x,y in zip(a,b)), native=native.strip()))
    (root / f'{case}.txt').write_text(native + reference)
# Invalid inputs must trap instead of returning any generated tokens.
for count, prompt in [(0, [1]), (33, [1]), (1, [32000]), (1, [1]*33)]:
    result = subprocess.run(prefix + [str(root / 'omega-model-probe'), str(root / 'llama.raw'),
        str(root / 'stories15M.bin'), str(count), str(root / 'refused.f32')]
        + list(map(str,prompt)), text=True, capture_output=True)
    assert result.returncode == 1 and 'trap=3' in result.stdout and 'REFUSED' in result.stdout
    results.append(dict(invalid_count=count, prompt_length=len(prompt), refused=True))
(root / 'host-parity.json').write_text(json.dumps(results, indent=2) + '\n')
print(f'LOCAL_MODEL_HOST: PASS cases={len(results)} independent_reference=yes')
