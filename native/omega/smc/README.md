# Native Omega SMC readback protocol

This is an implemented transport-independent SMC protocol engine and native
fixture diagnostic, **not a hardware-capable driver**. Physical readback is
**NOT_RUN**. `smphysical` returns `SMUNSUP`; no executor performs port I/O. No
boot path, firmware, fan policy, reset behavior or existing kernel driver changes.

## Ownership and exact source boundary

- Kernel owner: `aien-dev/aienux`, main
  `659ddd2d7e3e140cb9bf942c3171cbd9cf3d092e`.
- This change is stacked on existing native Omega work, draft #464,
  `4a6bcf2580d6f8918a762daea384a3fc176d8bd9`. Its Allen application keeps
  UEFI Boot Services active; it is not an Omega kernel. No duplicate C driver
  is added. The main C scanner (`native/kernel/core/acpi_dev.c`, `acpi_dev.h`)
  discards small I/O resource descriptors and exports MMIO resources only.
- Compiler owner: `aien-dev/omega-mv`, draft #895,
  `d9144676c5938f877a1d89f0e246842a760c62a7`.
  `seed/out/omc` SHA-256:
  `7d5b824f0c854063b851b7f1002ea9cb735c9f9e3ce96e3393ca6f4a46f5bfb9`.
  Compiler source SHA-256:
  `057b8698bb27cf53378c020f0e1e6149f4c18d8a372b1eee52af59bb408c1546`.
  This is a candidate compiler, not a merged or hardware-qualified pin.
- The mounted Omega MCP compiler is a different ARM-only authority:
  `omega-mv@e5b32567`, compiler SHA-256
  `2c82480596bbdc50cda9626866209ef662d5834f278f839ff4ce0250c495029f`.
  Its isolated syntax example is not x86 evidence.

## Protocol sources and licensing

Apple's [SMC support article](https://support.apple.com/en-us/102605) describes
power, thermal/fan and sensor responsibilities. Apple's
[fan article](https://support.apple.com/en-us/101576) describes temperature-driven
cooling. Neither publishes register framing, timeouts, or an autonomous cooling
qualification for this installation. No reset instructions were performed.

The pinned maintained Linux reference is
[`applesmc.c`, Linux v6.17](https://github.com/torvalds/linux/blob/v6.17/drivers/hwmon/applesmc.c):
`wait_status`, `send_byte`, `send_command`, `read_smc`,
`applesmc_get_entry_by_index`, temperature/fan readers. It is GPL-2.0-only,
Copyright Nicolas Boichat (2007), Henrik Rydberg (2010); it credits Robert Love,
Jesper Juhl and Hendrik Holtmann. This implementation uses independently written
Omega state transitions and tests of protocol facts, not copied/transliterated
Linux code. No GPL source is redistributed or linked. New files are Apache-2.0;
the stacked repository's broader license is not changed. Apple firmware bytes,
logos, disassembly and package payloads are excluded.

Existing captured ACPI evidence at main's
`evidence/imac_boot0_2026-10-16/ACPI` identifies APP0001 separately from the ACPI
EC. The EC is not used for temperature/fan readings. The package-research lane
found no new firmware-derived protocol authority. Package versions and
EmbeddedOS/bridge names are not SMC revisions or proof of applicability.

## API and ownership contract

`protocol.om` has no host or firmware words. Source units compose by
concatenation, matching the existing native Omega convention.

`Smbase`, `Smlen`, `Smen` and flattened seven-byte `Smhid` hold up to eight
**trusted discovery descriptors**. `smdiscover(count)` scans this bounded input;
it is not an AML interpreter or actual controller enumeration. It requires
exactly one enabled APP0001, I/O base 0x300, length 32. Missing, ambiguous,
disabled, malformed or unsupported resources never cause a port probe.

`smstart(owner, key[4], monotonic_ms, permit)` freezes the key and begins a
metadata-then-value operation. `permit` is a caller policy decision, not an
authorization capability. The key allowlist is `TC0P`, `TG0P`, `TH0P`, `FNum`
and `F0Ac` through `F9Ac`. Unsupported sensor sets require an explicit source
change, not arbitrary privileged key enumeration. `FS! `, target/minimum fan
keys and all arbitrary reads are refused before I/O.

The caller observes `Sm[2]` action, `Sm[3]` sequence, `Sm[4]` argument and
`Sm[20]` outstanding flag:

| Action | Meaning | Physical port, if separately authorized |
|---|---|---|
| 1 | Read status byte | 0x304 |
| 2 | Read response byte | 0x300 |
| 3 | Write command byte, only 0x13 or 0x10 | 0x304 |
| 4 | Write frozen key byte or response length | 0x300 |

`smack(owner,sequence,kind,value,monotonic_ms)` acknowledges exactly one action.
Reads return one byte; writes return zero for completed transport, negative
for an error. An acknowledgement does not assert controller acceptance.
Repeated observation of an outstanding action must not execute it twice.
Wrong-owner calls refuse without stealing the lease. Stale/duplicate/wrong-kind
acknowledgements from the owner abort. Request generation cannot wrap.

`smtick(owner,generation,monotonic_ms)` expires an unacknowledged action without
implicitly acknowledging it. `smcancel(owner,generation)` aborts. Capture the
generation from `Sm[18]` at start: callbacks and acknowledgements from an older
generation refuse without changing the current operation. A transport must provide
a real monotonic clock, periodic ticks, bounded I/O and correct acknowledgements.
Fixture clocks do not establish any physical timing property.

Serialization is a **cooperative single-executor lease**, held across both
metadata and value exchanges. It is not SMP atomic, interrupt safe, an access
capability or exclusion against firmware/SMM. There must be one instance and
one executor. Concurrent physical integration is not permitted by this API.

Protocol ordering:

1. Observe no busy or residual data, then separately observe input-open.
2. Write read-metadata command. Before every key and length byte, separately
   observe input-open, then busy set; never combine those observations.
3. Require data-ready **and busy** before each byte. Require data-ready clear
   and busy clear after the exact response length; extra bytes are malformed.
4. Validate exact length/type (`sp78`, `fpe2`, `ui8 `), readable metadata flag
   and absence of functional/unknown low flags before issuing the value read.
5. Repeat under the same owner; publish a value only after terminal validation.

The engine never issues WRITE_KEY (0x11), key-index enumeration, recovery reads,
SMC reset, drain loops, fan mode/policy writes, persistent writes or flash access.
**Read transactions still require command and argument I/O writes.** Those are
not fan-control/key-value writes, but they still require exact physical approval.
No such transactions were performed in this lane.

After a possibly issued command, any error quarantines the channel. An offered
but unacknowledged write is also uncertain. Initial stuck-busy quarantines it.
Neither discovery nor a new request clears quarantine. There is no production
reset API; fixture `fresh` resets only private test memory. No automatic retry
or recovery hides a framing error.

## Results and bounds

`Sm[14]` holds terminal status; `Sm[15] == 1` alone publishes `Smval[0]` and
`Smmeta[0..5]`. After failure, value and metadata are scrubbed. Do not consume
internal response storage or metadata while a transaction is pending.

| Status | Meaning |
|---|---|
| SMOK | Validated complete value |
| SMABS | No descriptor or all-ones status |
| SMBUSY | Another owner, or initial stuck-busy |
| SMTIME | Wait, action or total deadline exhausted |
| SMUNSUP | Unsupported resource/type/key metadata or missing physical executor |
| SMMAL | Invalid length/byte/ack value or excess response |
| SMREF | Policy, flags, ownership, sequence or cancellation refusal |
| SMTRUNC | Response stops after at least one byte |
| SMIO | Transport error |
| SMKEY | Key not allowlisted |
| SMQUAR | Channel quarantined |
| SMPEND | One action is outstanding |
| SMCLOCK | Monotonic clock regressed |

Bounds are deliberately conservative **modeled policy**, not measured controller
requirements: eight descriptors, 32 repeated polls per waiting phase, 512 total
actions, one second for the whole metadata/value request. The executor may
space polls but cannot extend the total deadline. Time units are milliseconds.

Temperature output is a signed sp78 numerator with denominator 256 degrees C;
fan speed is an unsigned fpe2 numerator with denominator 4 RPM; FNum is an
integer count. Fractional RPM and negative temperatures are not silently lost.

WolframLanguageEvaluator:
`{FromDigits["300",16],FromDigits["304",16],FromDigits["10",16],FromDigits["13",16],FromDigits["80",16],FromDigits["FE80",16]-2^16,(FromDigits["FE80",16]-2^16)/2^8,FromDigits["1900",16]/2^2,2^8,2^16}`
returned `{768,772,16,19,128,-384,-3/2,1600,256,65536}`.
Thus fixture FE80 represents -3/2 degrees C, and 1900 represents 1600 RPM.
WolframLanguageEvaluator:
`{UnitConvert[Quantity[1,"Seconds"],"Milliseconds"],FromDigits["FE",16],FromDigits["1900",16],FromDigits["8000",16]}`
returned `{Quantity[1000,"Milliseconds"],254,6400,32768}`.
These are arithmetic checks, not temperature measurements.

## Missing native extension (omega-mv owner)

Concrete owner proposal and smallest refusal reproduction:
[omega-mv #917](https://github.com/aien-dev/omega-mv/issues/917).

`seed/omc/omc.om:9203-9212` restricts `efi` to x86efi and literal operations
0 through 10; `:9582-9618` dispatches display, input, delay, clock, entropy and
network operations, not port I/O. `:4510-4513` refuses host operations on x86.
`unsupported-port.om` is the smallest refusal fixture for proposed operation 11,
not an invented supported word. A compiler refusal must never produce a usable
artifact.

The extension proposal requires all of the following before physical integration:

- Native Omega discovery of bounded, validated ACPI APP0001 I/O descriptors,
  including length/checksum/range/duplicate/disabled-resource refusals. No
  arbitrary scalar addresses or fallback probing. The current static C scanner
  is reference context only, not an implementation to hide behind this module.
- A resource handle authorizing exactly byte accesses to 0x300 and 0x304,
  with ordered single-instruction reads/writes. Proposed `efi 11` packet ABI:
  fixed 64-byte writable buffer; operation byte (acquire/status/data/command/
  argument/release), explicit opaque handle, byte value and full-width status.
  Compiler typing and lowering must refuse other widths, addresses, command
  opcodes and targets. This is a proposal, not a shipped ABI or trust grant.
- A native ownership/critical-section mechanism or a proven single-owner
  environment covering all participating cores and callbacks, plus an explicit
  firmware/SMM coexistence decision. A boolean flag is insufficient.
- Checked monotonic time, bounded I/O, invalid/revoked-handle failures and no
  automatic reset/drain/key-write effects. Protocol state remains in this
  Omega unit, not in a host library or firmware shim.
- Compiler positive/negative and wrong-word-mutant tests, then an emulator
  byte-I/O trace model; physical transaction approval and recovery/thermal
  observation are separate gates. No current permission covers those actions.

This missing platform feature blocks hardware completion; the diagnostic is
not a replacement SMC chip, SMC firmware, production kernel hook or thermal
qualification. Automatic cooling and thermal shutdown remain untouched.

## Reproduce the exercised diagnostic

Use a fresh output directory and the exact compiler above. These are external
development commands, not a host implementation of the driver:

```sh
set -eu
OMC=/path/to/pinned/omega-mv/seed/out/omc
test "$(sha256sum "$OMC" | cut -d' ' -f1)" = \
  7d5b824f0c854063b851b7f1002ea9cb735c9f9e3ce96e3393ca6f4a46f5bfb9
mkdir smc-fresh
{ cat native/omega/smc/protocol.om; printf '\n'; \
  cat native/omega/smc/diagnostic.om; } > smc-fresh/arm.om
{ printf 'target x86linux\n'; cat smc-fresh/arm.om; } > smc-fresh/x86.om
"$OMC" < smc-fresh/arm.om > smc-fresh/arm
"$OMC" < smc-fresh/x86.om > smc-fresh/x86
chmod 700 smc-fresh/arm smc-fresh/x86
./smc-fresh/arm
qemu-x86_64 ./smc-fresh/x86
```

Each observed clean compile and run exited 0 and printed
`SMC protocol fixtures PASS; physical NOT_RUN`. The x86 output is native x86-64
Linux ELF executed in QEMU 8.2.2 user mode, not an iMac boot or device test.
`exercise.json` records composed source hashes, compiler pin, artifact hashes,
commands, output and exact negative-control substitutions. On **both** targets,
removing type validation, removing the busy requirement on response reads, and
removing callback generation checks each compiled successfully but exited 1,
printing `SMC protocol fixtures FAIL`. A compile failure is not counted as a
successful mutant check.

The diagnostic covers full action ordering; metadata plus signed temperature,
fan speed and fan count; missing/ambiguous/disabled resources; invalid keys;
wrong owner, stale sequence and same-owner stale-generation callbacks;
uncertain-write cancellation; every wait phase; truncated and excess output;
invalid byte and transport errors; clock regression; deadline expiry without
an acknowledgement; and oscillation against the global action bound.

The proposed EFI operation fixture exits 25 at compilation and is never run.
No ELF fixture contains physical IN/OUT or firmware calls. Full kernel gates,
EFI boot, physical SMC transactions and thermal qualification are **NOT_RUN**.
No existing gate or compiler pin was weakened or changed. No Omega harness
guide changes are required: this adds a component at an explicitly candidate
pin and proposes an extension, without changing supported language or ownership.

