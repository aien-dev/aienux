# Spark platform readback and control ownership

Dated observation: 2026-10-28. Aienux base: `659ddd2d7e3e140cb9bf942c3171cbd9cf3d092e`.
This is an offline resource reader and host qualification, not a physical driver
or control qualification. Every fan, power, clock and thermal-policy write is
**NOT_RUN**. No AML method was executed by this investigation. No register was
mapped/read directly, and no existing service was stopped or changed.

## Evidence and method

Read-only copies were taken using `sudo -n cat /sys/firmware/acpi/tables/DSDT`
and the same command for SSDT1 through SSDT5. Each copied binary was disassembled
individually with `iasl -d TABLE.aml` (ACPICA 20230628). The earlier `iasl -da`
combined invocation failed on duplicate declarations; it is INVALID setup, not
an AML or hardware failure. The individual disassemblies succeeded. AML remains
data: neither `_TMP`, `_CPC`, `_INI` nor any other method was invoked.

The external receipt bundle is `handoffs/spark-five-20261028/QualPlatform` on
the qualification machine. It includes command metadata, stdout, stderr, binary
tables, disassemblies, original distro source archives and the source descriptor,
compiler snapshot, host programs, results and a SHA256 manifest. The raw table
copies are not a new firmware authority. Re-discover resources after firmware
changes; this reader is explicitly tied to the captured board profile.

DSDT SHA256: `2871cbcb8992f7bd3915d6f19e7cfab577e068dcad58b6f00317a9f2d4e1b28e`.
`cppc-fixture.json` records the independent disassembled descriptors, including
all control fields so the qualification can verify the reader excludes them.

## AML dependency map

DSDT lines 16136-16280 declare seven thermal zones. Each `_TMP` calls serialized
`RREG(address)`. `RREG` declares a four-byte `SystemMemory` OperationRegion,
`DWordAcc`, `NoLock`, `Preserve`, and returns its 32-bit DAT0 field. `_CRT` returns
TCRT `0x0EC4`. `_PSL` is empty. There are no `_ACn`, `_ALn`, `_PSV`, fan or PWM
relationships in these captured tables.

| Zone / reader index | `_TMP` backing address | Exposed meaning |
|---|---|---|
| TSOC / 0 | `0x1C238818` | firmware-named SoC zone |
| TS0E / 1 | `0x1C238820` | firmware-named zone; physical placement UNKNOWN |
| TS0P / 2 | `0x1C238824` | firmware-named zone; physical placement UNKNOWN |
| TS1E / 3 | `0x1C238828` | firmware-named zone; physical placement UNKNOWN |
| TS1P / 4 | `0x1C23882C` | firmware-named zone; physical placement UNKNOWN |
| TGPU / 5 | `0x1C238830` | firmware-named GPU zone; not assumed identical to NVML |
| TUNC / 6 | `0x1C238834` | firmware-named uncore zone |

The firmware value has ACPI temperature units (tenths kelvin). The reader retains
the raw word; a critical shutdown threshold is not a fan or power control limit.
Linux's observed sysfs critical value is 104800 millidegrees Celsius. The
conversion convention must match the actual ACPI thermal driver, not an invented
ambient calibration.

DSDT lines 1073-6585 contain twenty revision-3, 23-entry `_CPC` packages, ordered
CLU0.CPU0..CPU9 then CLU1.CPU0..CPU9. Entries are `SystemMemory`, 32 bits,
bit offset 0, DWORD access, except reference/delivered counters: `FFixedHW`,
64 bits, addresses 1/0 respectively, with unspecified access size. These FFH
addresses are abstract register selectors, **not physical addresses**. They
must never be dereferenced as SystemMemory. The first capability address is
`0x051CC404`, with per-CPU stride `0xE8`; independent fixture parity checks every
selected descriptor, not just the stride's endpoints.

Linux `cppc_regs` indexes (package indexes omit the first two integers):

* Reader exposes 0..4: highest, nominal, lowest-nonlinear, lowest, guaranteed
  performance; 10: counter-wrap time; 11/12: reference/delivered counters;
  13: performance-limited; 18: reference performance; 19/20: lowest/nominal MHz.
* Reader refuses 5..9 and 14..17: desired/min/max performance, tolerance/window,
  enable/autonomous/activity-window/energy preference. These are not a safe
  control API merely because AML provides addresses.

SSDT2 declares CP0..CP19 SystemMemory regions at each highest-performance
address, with 24-bit fields. `_INI` calls INIT, which writes firmware-provided
values into those fields. This is an offline observation, not a command to replay
INIT or to write a supposedly read-only capability. Firmware changes and
concurrent firmware updates remain possible; a bundle of reads is not an atomic
snapshot. No performance/rate is calculated from unsynchronized CPU counters.

Other OperationRegions: DSDT debug SystemIO 0x80/0x90; GNVS/ANVS firmware memory;
USB-related EN0/EN1 SystemMemory; generic RTRG SystemMemory read helper; FFA0
fixed-hardware transport associated with ARM FF-A notifications; SSDT4 GPU
PCI_Config PEGR; SSDT5 TPM fixed-hardware transport. None is evidence of a fan,
EC, SCMI or PCC thermal-control channel. No `EmbeddedControl`, `PNP0C09`,
`PNP0C0B`, SCMI, PCC, `_FST`, `_FPS`, `_FSL`, fan or PWM declaration matched
DSDT/SSDT1..5. The table inventory has no PCCT. Negative table searches do not
prove the board lacks a private controller.

## Ownership and limits

R means an observed standard read interface; W means only an interface exists,
not that a write is safe or authorized. UNKNOWN is never zero or a guessed limit.

| Resource | Controller / transport | Firmware owner | Read / write | Safe limits source |
|---|---|---|---|---|
| Seven ACPI temperatures | Linux acpi_thermal/acpitz; AML RREG -> SystemMemory | MTKINC AML describes shared registers; producer/physical sensor controller UNKNOWN | sysfs R observed; native READ actions host-exercised only; W not exposed | `_CRT` is shutdown policy only; control envelope UNKNOWN |
| NVMe composite/sensor temperatures | Linux NVMe hwmon -> NVMe SMART log, PCI `0004:01:00.0` | Samsung controller firmware | sysfs R observed; no temperature W implemented | SMART critical threshold observed 84850 mC; sensor min/max sentinel values are invalid, not limits |
| Wi-Fi temperature | mt7925 hwmon -> driver MCU thermal query, PCI `0009:01:00.0` | MediaTek MCU firmware | sysfs R observed; no W implemented | safe-control envelope UNKNOWN |
| GPU temperature/power/clocks | nvidia-smi -> NVML -> NVIDIA driver; GPU `000f:01:00.0` | NVIDIA firmware stack; exact physical policy owner not established by query | standard R exercised; generic vendor W commands exist, board support/limits UNKNOWN | current/min/max/default GPU and module power limits N/A; Supported Clocks N/A |
| CPU capability/clock policy | cppc_cpufreq -> ACPI CPPC SystemMemory; FFH counters separately | MTKINC/ANGUSCPU AML and platform firmware; underlying DVFS implementation UNKNOWN | policy sysfs R observed; OS policy W exists but not qualified; reader excludes control fields | AML performance values and Linux policy bounds are not vendor electrical/thermal limits |
| CPU cooling_device0..19 | Linux Processor cooling -> CPU frequency QoS | OS governor acts on firmware-backed CPU policy | state R; W intentionally not attempted | max_state=3 is policy index, not fan speed or safe clock envelope |
| cooling_device20..25 | Linux pcie_cooling -> PCIe bandwidth controller on root ports | PCIe link hardware/firmware, controlled by OS | sysfs R; W not attempted; cd25 current state invalid | max_state=4 is link-speed state count, not temperature/fan setting |
| Chassis fan, PWM, tachometer | no exposed hwmon PWM/fan or bound ACPI fan/EC/SCMI found | UNKNOWN; do not infer NVIDIA GSP, CPU firmware or board EC ownership | no discovered qualified native R/W interface | vendor board-controller protocol, ownership and fail-safe envelope UNKNOWN |
| Board/module power cap | powercap/power_supply/devfreq expose no usable channel in scout inventory | UNKNOWN beyond NVIDIA query path | no qualified W | UNKNOWN; marketing TDP/PSU rating is not a writable envelope |

### NVIDIA query actually exercised

```
nvidia-smi -i 000f:01:00.0 -q -d TEMPERATURE,POWER,CLOCK,PERFORMANCE,SUPPORTED_CLOCKS
```

Exit 0, driver 580.173.02. At that sample: GPU 40 C; P8; average 4.64 W,
instantaneous 4.69 W; graphics/SM 208 MHz; video 598 MHz; memory N/A. Maximum
reported graphics/SM/video clock 3003 MHz and application/default graphics
2418 MHz are descriptive telemetry, not a validated allowed lock range.
Supported Clocks and all requested/default/min/max/current GPU/module power
limits are **UNKNOWN (N/A)**. The current SW Power Cap event was Not Active;
a historical nonzero cumulative counter does not say a cap is active now.

Vendor source: https://docs.nvidia.com/deploy/nvidia-smi/index.html . It defines
N/A as unsupported data and requires `--power-limit` values between reported
Min/Max Power Limit. It documents generic clock lock/reset commands, but generic
command availability and a maximum-clock report do not establish this board's
supported clock/control envelope or recovery semantics.

## cooling_device25 kernel defect

The reported `cur_state=-231` and root-port current-link-speed Unknown are ground
truth, not a reason to try a cooling-state write. This is a PCIe cooling device,
not a negative fan speed. Exact distro source package:
`linux-nvidia-7.0 7.0.0-1019.19~24.04.2`, published at
https://ports.ubuntu.com/pool/main/l/linux-nvidia-7.0/ . The original tarball and
Debian diff hashes were checked against that version's `.dsc`; neither
`drivers/thermal/pcie_cooling.c` nor `drivers/thermal/thermal_sysfs.c` is patched
by that diff. The reproduced reference file SHA256 is
`fff34da417c7f8cdb642a8b8ac20b7a5d1010392a2a7b8662178078abd1824c9`.

`get_cur_level` subtracts `(cur_bus_speed - PCIE_SPEED_2_5GT)` from max_state
without validating cur_bus_speed. At this source pin, the enum base is 0x14,
and PCI_SPEED_UNKNOWN is 0xff. With max_state=4 the result underflows unsigned
long; thermal_sysfs.c prints that word with `%ld`, exposing -231. This does NOT
establish why the port has unknown link speed. Do not mask it by returning a
fabricated state 0 or by interpreting -231 as a valid operating point.

`pcie-cooling-proposal.patch` proposes rejecting unknown/non-PCIe/unsupported
speeds and speeds above the registered maximum with `-EAGAIN`; output remains
untouched on error. It also rejects an invalid maximum speed with `-EINVAL`
during state-range discovery. Each speed is sampled once with READ_ONCE.
This is a source-level proposal only: no kernel/module build, installation,
reload or deployment was performed. No write callback is changed. A later kernel
integrator must run kernel build/thermal review and topology/resume tests before
shipping; signed sysfs formatting alone would conceal rather than fix the bug.

`kernel-repro.py` builds the exact original get_* functions with small C
reference structs, applies the actual proposed patch in a disposable directory,
checks all enum speeds and boundaries, and requires red/green controls in both
directions. This host oracle does not execute a PCIe transaction.

## Seeded reader contract and acceptance

`readback.om` follows the explicit action/acknowledgement separation used by
Aienux #465's protocol.om, rather than introducing a host AML interpreter.
`plstart(kind, unit, field, sequence)` emits a single bounded READ descriptor in
`Pl`: pending, sequence, space, address, bits, raw, valid, last-sequence. Kinds
are 0 thermal and 1 CPPC. `plack(sequence, space, address, bits, raw, ok)` consumes
it. A successful ACK sets raw and valid; a mismatch, I/O failure, replay or
out-of-width raw value cannot publish data. Sequences must increase and be
nonzero. A pending request cannot be replaced. One single-threaded executor is
required; this is not a cross-core or firmware lock. No command-write action
exists, and there is no physical executor or mapping capability in this module.
The acknowledged transport boundary is real: fixtures exercise the reader, not
the physical register path. Native sensor reads and FFH reads remain NOT_RUN.
Transport mapping/permissions and board-documentation checks are prerequisites
for any future physical integration, not bypassed by a passing host fixture.

Host-only acceptance, after writers have finished:

```
python3 native/omega/platform/gate.py /absolute/pinned/omc EXPECTED_SHA256 /absolute/receipts/readback
python3 native/omega/platform/kernel-repro.py /absolute/receipts/kernel-repro
```

Qualification compiler source `1fe5976d398646b17acabbf6d9dfae96b017e439`, committed
`seed/out/omc` SHA256
`d6290c7663c2b4bafb59eb3fdf812476e977f4c144def71095c6248ef9b96b84`.
This does not change Aienux's omega.lock runtime-vendor pin. Conductor/oracle
programs use Python, cc and patch only for verification; production behavior
is seeded Omega. Compiler failure does not count as a killed mutant.

Observed acceptance: both commands exited 0. The readback gate passed all 247
descriptor reads (twenty CPUs times twelve selected descriptors, plus seven
thermal zones), its boundary suite, and six compiled/runnable mutants:
stale ACK exit 6, wrong address 12, writable selector 26, wide raw word 18,
thermal address 5, CPPC stride 2. The original C reproducer exited 0 and printed
`cur_state=-231 unsigned=18446744073709551385`; the patched reproducer exited 3
(expected negative control), patched boundary suite exited 0, and the original
code failed that boundary suite with exit 4. Kernel compilation/deployment and
physical native-register readback remain NOT_RUN.

The first host attempt exposed a test-oracle defect: writable-field rejection
was selected by the reader's own `ploff`, allowing a writable-selector mutant
to survive. The boundary oracle now independently enumerates the excluded
index ranges, and that same runnable mutant is rejected. The first C harness
compile also refused an unused static function under `-Werror`; its reproducer
now checks the maximum-state function too. Both failed attempts are retained
in receipts, not counted as passing controls.

## Exact authorization procedure and control disposition

**No fan/power/clock control currently meets the vendor-documented, board-specific
safe-limits requirement. Consequently no validated executable control-write,
readback-and-revert procedure exists for this machine. All control writes are
NOT_RUN, and none should be authorized from this document.** This is a measured
refusal, not permission to experiment with generic nvidia-smi options or sysfs.

The currently executable procedure is read-only:

1. Keep the resident model/service and its GPU reservation untouched. Do not
   reboot, unload drivers, evaluate AML or open `/dev/mem`.
2. Run exactly the NVIDIA query above; save stdout/stderr and exit status.
   Read the existing thermal/hwmon/cpufreq sysfs attributes only. A negative
   PCIe cooling state is invalid data, not a control target.
3. Preserve N/A as UNKNOWN. With these captured results, stop before any write.
   No revert is necessary because nothing was modified.
4. A future control authorization must name the exact controller/firmware
   revision, vendor protocol and board-supported limit source, exact old/requested
   values and units, original autonomous/manual mode, exact write operation,
   independent readback plus tolerance/settling source, and a documented inverse
   that restores both value and mode. Generic `--reset-gpu-clocks` restores a
   default, not necessarily the previous lock, so is not automatically a revert.
   If any of these are UNKNOWN, authorization remains blocked. This document
   deliberately supplies no guessed numbers or unsafe write command.
5. Any kernel-fix boot or native MMIO reader integration is a separate attended
   authorization. The existing AIENOSRECOV stick (serial 5D0344B4, PARTUUID
   f227357c-1e80-4fd0-a55e-2efbef1dd25c, firmware Boot0002 as observed today) is
   recovery-only and must remain read-only. Its 2026-09-25 attended recovery
   proof is historical, not a reboot performed today. Any candidate image needs
   a separate blank medium and the boot lane's exact procedure. No service
   stop/restore or recovery action is needed for the host acceptance above.

## Arithmetic evidence

WolframLanguageEvaluator:
`FromDigits["EC4",16] -> 3780`; `(3780-2732)*100 -> 104800` mC;
`4-(255-20) -> -231`; `Mod[4-(255-20),2^64] -> 18446744073709551385`.
Address conversion and CPPC stride/offset derivations are preserved with the
same evaluator query/results in the external receipt bundle. These are MODELED
arithmetic checks, not additional temperature or register measurements.
