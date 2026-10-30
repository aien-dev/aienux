#!/usr/bin/env python3
"""Host verification oracle only. No hardware access; implementation is readback.om."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

here = Path(__file__).resolve().parent
omc, expected, out = sys.argv[1:]
out = Path(out).resolve()
out.mkdir(parents=True, exist_ok=True)
assert hashlib.sha256(Path(omc).read_bytes()).hexdigest() == expected, 'compiler pin mismatch'
source = (here / 'readback.om').read_text()
unit = (here / 'unit.om').read_text()
fixture = json.loads((here / 'cppc-fixture.json').read_text())
assert len(fixture['cpus']) == 20
checks = ['fn main to int']
seq = 0
for cpu, desc in enumerate(fixture['cpus']):
    assert len(desc['registers']) == 21
    for field in [0, 1, 2, 3, 4, 10, 11, 12, 13, 18, 19, 20]:
        reg = desc['registers'][field]
        space = {'SystemMemory': 0, 'FFixedHW': 127}[reg['space']]
        bits = int(reg['bits'], 16)
        addr = int(reg['address'], 16)
        assert int(reg['offset'], 16) == 0
        assert int(reg['access'], 16) == (3 if space == 0 else 0)
        seq += 1
        checks += [f'if ne plstart 1 {cpu} {field} {seq} 0', 'ret 1', 'end',
                   f'if ne plack {seq} {space} {addr} {bits} 3132 1 0', 'ret 2', 'end',
                   'if or ne get Pl 5 3132 ne get Pl 6 1', 'ret 3', 'end']
for zone, addr in enumerate([0x1C238818, 0x1C238820, 0x1C238824, 0x1C238828, 0x1C23882C, 0x1C238830, 0x1C238834]):
    seq += 1
    checks += [f'if ne plstart 0 {zone} 0 {seq} 0', 'ret 4', 'end',
               f'if ne plack {seq} 0 {addr} 32 3132 1 0', 'ret 5', 'end']
checks += ['ret 0', 'end', '']
parity = '\n'.join(checks)
results = []
def run(name, code, should_pass):
    src = out / (name + '.om')
    binary = out / name
    src.write_text(code)
    with src.open('rb') as stdin, binary.open('wb') as stdout, (out / (name + '.compile.stderr')).open('wb') as stderr:
        p = subprocess.run(['nice', '-n', '15', omc], stdin=stdin, stdout=stdout, stderr=stderr)
    assert p.returncode == 0, f'{name}: compile failed, not a valid mutant kill'
    binary.chmod(0o755)
    p = subprocess.run(['nice', '-n', '15', str(binary)], capture_output=True)
    (out / (name + '.stdout')).write_bytes(p.stdout)
    (out / (name + '.stderr')).write_bytes(p.stderr)
    assert (p.returncode == 0) == should_pass, f'{name}: unexpected exit {p.returncode}'
    results.append({'name': name, 'compile_exit': 0, 'run_exit': p.returncode,
                    'sha256': hashlib.sha256(binary.read_bytes()).hexdigest()})
run('unit', source + unit, True)
run('descriptor-parity', source + parity, True)
mutants = {
    'stale-ack': ('if or ne seq get Pl 1 ne space get Pl 2', 'if ne space get Pl 2'),
    'wrong-address': ('if or ne addr get Pl 3 ne bits get Pl 4', 'if ne bits get Pl 4'),
    'write-selector': ('if lt field 5', 'if lt field 6'),
    'wide-raw': ('if gt raw 4294967295', 'if gt raw 18446744073709551615'),
    'thermal-address': ('ret 472090648', 'ret 472090649'),
    'cppc-stride': ('mul unit 232', 'mul unit 233'),
}
for name, (old, new) in mutants.items():
    assert source.count(old) == 1
    test = parity if name in ['thermal-address', 'cppc-stride'] else unit
    run(name, source.replace(old, new) + test, False)
(out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
print(f'PLATFORM READBACK PASS: {seq} descriptor reads; boundary suite; {len(mutants)} runnable mutants rejected')
