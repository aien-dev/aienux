#!/usr/bin/env python3
"""Exact distro C reference oracle. Never opens sysfs or a PCI device."""
from pathlib import Path
import hashlib
import json
import re
import subprocess
import sys
here = Path(__file__).resolve().parent
out = Path(sys.argv[1]).resolve()
out.mkdir(parents=True, exist_ok=True)
ref = (here / 'pcie-cooling-ref.c').read_text()
assert hashlib.sha256(ref.encode()).hexdigest() == 'fff34da417c7f8cdb642a8b8ac20b7a5d1010392a2a7b8662178078abd1824c9'
# Apply the actual proposed patch to a disposable copy, not a hand-coded fixed oracle.
tree = out / 'patch-tree/drivers/thermal'
tree.mkdir(parents=True, exist_ok=True)
(tree / 'pcie_cooling.c').write_text(ref)
p = subprocess.run(['patch', '--batch', '-p1', '-i', str(here / 'pcie-cooling-proposal.patch')], cwd=out / 'patch-tree', capture_output=True)
(out / 'patch.stdout').write_bytes(p.stdout)
assert p.returncode == 0, p.stderr
fixed = (tree / 'pcie_cooling.c').read_text()
header = '''#include <stdio.h>
#include <errno.h>
#define READ_ONCE(x) (x)
enum pci_bus_speed { PCIE_SPEED_2_5GT=0x14, PCIE_SPEED_64_0GT=0x19, PCI_SPEED_UNKNOWN=0xff };
struct pci_bus { unsigned char max_bus_speed, cur_bus_speed; };
struct pci_dev { struct pci_bus *subordinate; };
struct thermal_cooling_device { void *devdata; unsigned long max_state; };
'''
def functions(text):
    return '\n'.join(re.findall(r'static int pcie_cooling_get_\w+\([^}]+\n}', text))
assert len(re.findall('static int ', functions(ref))) == 2
bug = '''int main(void) {
struct pci_bus bus={0x18,0xff}; struct pci_dev dev={&bus};
struct thermal_cooling_device c={&dev,4}; unsigned long s=999;
if (pcie_cooling_get_max_level(&c,&s) || s!=4) return 7;
s=999;
int r=pcie_cooling_get_cur_level(&c,&s);
if (r || (long)s != -231) return 3;
printf("REPRODUCED unknown=0xff max_state=4 cur_state=%ld unsigned=%lu\\n",(long)s,s);
return 0; }
'''
test = '''int main(void) {
struct pci_bus bus={0x18,0x14}; struct pci_dev dev={&bus};
struct thermal_cooling_device c={&dev,4}; unsigned long s;
for (unsigned int i=0x14; i<=0x19; i++) {
bus.max_bus_speed=i; s=999;
if (pcie_cooling_get_max_level(&c,&s) || s!=i-0x14) return 1;
c.max_state=i-0x14;
for (unsigned int j=0x14; j<=i; j++) {
bus.cur_bus_speed=j; s=999;
if (pcie_cooling_get_cur_level(&c,&s) || s!=i-j) return 2;
}}
unsigned int bad[]={0,0x13,0x1a,0xff};
for (unsigned int i=0;i<4;i++) {
bus.cur_bus_speed=bad[i]; s=999;
if (pcie_cooling_get_cur_level(&c,&s)!=-EAGAIN || s!=999) return 4;
bus.max_bus_speed=bad[i]; s=999;
if (pcie_cooling_get_max_level(&c,&s)!=-EINVAL || s!=999) return 5;
}
c.max_state=4; bus.cur_bus_speed=0x19; s=999;
if (pcie_cooling_get_cur_level(&c,&s)!=-EAGAIN || s!=999) return 6;
printf("FIX PASS all enum speeds, unknown/non-PCIe, stale-above-max; outputs unchanged on errors\\n");
return 0; }
'''
results=[]
for name, source, main, expected in [('old-repro',ref,bug,0),('fixed-repro-negative',fixed,bug,3),('fixed-boundaries',fixed,test,0),('old-boundaries-negative',ref,test,4)]:
    src=out/(name+'.c'); binary=out/name
    src.write_text(header+functions(source)+main)
    p=subprocess.run(['nice','-n','15','cc','-std=c11','-Wall','-Wextra','-Werror','-o',str(binary),str(src)],capture_output=True)
    (out/(name+'.compile.stderr')).write_bytes(p.stderr)
    assert p.returncode==0,p.stderr
    p=subprocess.run([str(binary)],capture_output=True)
    (out/(name+'.stdout')).write_bytes(p.stdout)
    assert p.returncode==expected,(name,p.returncode,expected)
    results.append({'name':name,'compile_exit':0,'run_exit':p.returncode,'expected':expected,'binary_sha256':hashlib.sha256(binary.read_bytes()).hexdigest()})
(out/'results.json').write_text(json.dumps(results,indent=2)+'\n')
print('PCIE COOLING REPRO PASS: old reproduces -231; proposed fix refuses; full boundary oracle passes and kills old code')
