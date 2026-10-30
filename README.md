# Aienux

> **Status: experimental, pre-alpha. Nothing here is qualified for real use.** The C kernel has run on a Raspberry Pi 5 and on a 2017 iMac, but only in test builds with TEST keys or throwaway secrets. It has not been booted on the NVIDIA DGX Spark. There are no support, stability or compatibility promises. Expect breaking changes.

Aienux is the AienOS kernel: our own operating system kernel, built to replace Linux on the DGX Spark. AienOS is the whole system built on it. It is agent-native: the agent is the control plane, and the model is never the kernel. The kernel stays small, deterministic and non-intelligent, and the agent is never the root of trust. Most of the kernel is C in `native/`. An older Rust kernel and tools live in `crates/`.

```text
Firmware -> AIEN Boot -> AIEN Kernel -> AIEN Runtime -> AIEN Agent -> You
```

## Where this fits in AIEN

Aienux owns the trusted base: boot, kernel, memory, storage, cryptography and device access. The sibling repositories are:

- [aien-dev/aien-architecture-mv](https://github.com/aien-dev/aien-architecture-mv): the whole-system architecture, ADRs and sequencing. It is the authority over plans in this repository (see [docs/PLAN_AUTHORITY.md](docs/PLAN_AUTHORITY.md)).
- [aien-dev/omega-mv](https://github.com/aien-dev/omega-mv): Omega, the C reaction runtime and compiler. Under aien-architecture ADR 0024, Omega is where system code is headed. AIENOS carries a byte-for-byte copy of Omega's Cortex store in [native/kernel/third_party/omega/](native/kernel/third_party/omega/README.md), pinned by `omega.lock`.
- [aien-dev/physics-mv](https://github.com/aien-dev/physics-mv): FORGE, the machine-realization layer for the GB10 hardware path.
- [aien-dev/aien-protocols-mv](https://github.com/aien-dev/aien-protocols-mv): specifications and wire formats shared across repositories.
- [aien-dev/aien-sovereign-core-mv](https://github.com/aien-dev/aien-sovereign-core-mv): the host runtime, and `aien-test`, the test judge that runs on the Spark.
- [aien-dev/benchmarks-mv](https://github.com/aien-dev/benchmarks-mv): measurement harnesses and evidence.
- [aien-dev/aien-receipts](https://github.com/aien-dev/aien-receipts): the signed test receipts the Spark publishes for each commit.

## Current state (2026-10-19)

Every item below names the file that holds its evidence. "QEMU" means the QEMU emulator: a QEMU pass says nothing about real hardware.

**The C kernel is FROZEN** (Drake, 2026-10-24; ADR 0039, program L802): it takes bug fixes and keeps the lab iMac booting while Aienux is rewritten in Omega; it takes no new features. See [native/kernel/README.md](native/kernel/README.md).

**C kernel in QEMU.** `scripts/ck_gates.sh` lists 27 emulator gates. The newest receipt committed here is [evidence/ck_gates_29ed03c9...json](evidence/ck_gates_29ed03c97454354a18676f768273dd7f7a8e15462c4be1939d9994d277d0d639.json): 24 gates, all PASS, run at commit 7f0de7e on 2026-10-09. Gates added after that date have no committed full receipt yet. What each gate checks: [native/kernel/GATES.md](native/kernel/GATES.md).

**Raspberry Pi 5 (test builds only).** First runs on 2026-10-13: the kernel started all four cores, ran its timer and wrote its log to the SD card ([evidence/pi5_hardware_runs_2026-10-13/](evidence/pi5_hardware_runs_2026-10-13/README.md)). Later Pi runs, each with its own record in [docs/native/](docs/native/):

- Store unlock with a throwaway recovery secret read from the card ([PI5-4-UNLOCK-RUNS.md](docs/native/PI5-4-UNLOCK-RUNS.md)).
- A fact remembered and recalled after a reboot ([PI5-5-CORTEX-RUNS.md](docs/native/PI5-5-CORTEX-RUNS.md)).
- First model inference on the Pi: Llama-3.2-1B at 1.20 tokens per second ([PI5-6-INFER-RUNS.md](docs/native/PI5-6-INFER-RUNS.md)). A model reply was saved in the encrypted Store and read back after a reboot ([PI5-6-AGENT-RUNS.md](docs/native/PI5-6-AGENT-RUNS.md)).
- The agent lifecycle, including recall after a full power cut ([PI5-7-LIFECYCLE-RUNS.md](docs/native/PI5-7-LIFECYCLE-RUNS.md)), and signed user-mode programs loaded with TEST keys ([PI5-7-ARTIFACT-RUNS.md](docs/native/PI5-7-ARTIFACT-RUNS.md)).
- One Ethernet frame sent on the wire, with device memory access kept inside a fixed window ([PI5-NET-3-RUNS.md](docs/native/PI5-NET-3-RUNS.md)). Receiving from the network is not done.
- The owner key ceremony on a YubiKey, performed once on 2026-10-14 ([PI5-8-OWNER-CEREMONY.md](docs/native/PI5-8-OWNER-CEREMONY.md)), and reproducible Pi image builds ([PI5-8-QUALIFICATION.md](docs/native/PI5-8-QUALIFICATION.md)).

**2017 iMac (x86_64).** A diagnostic boot on 2026-10-16 recorded the hardware ([evidence/imac_boot0_2026-10-16/](evidence/imac_boot0_2026-10-16/README.md)). The C kernel started on the iMac on 2026-10-16 ([evidence/imac_kernel0_2026-10-16/](evidence/imac_kernel0_2026-10-16/README.md)). On 2026-10-17 it started all four cores, read both disks without writing, and found the Ethernet chip ([evidence/imac_kernel1_2026-10-17/](evidence/imac_kernel1_2026-10-17/README.md)). Code that sends the test report over Ethernet is merged (PR [#408](https://github.com/aien-dev/aienux/pull/408)); no hardware run of it is recorded in this repository.

**Network boot and audio (code only).** The iMac network loader that fetches signed kernels from the Spark is merged (PR [#414](https://github.com/aien-dev/aienux/pull/414)), and so is the first step of USB audio output for the C kernel (PR [#423](https://github.com/aien-dev/aienux/pull/423)); this repository records no hardware run of either.

**DGX Spark.** The first native boot on the Spark passed on 2026-09-24, using the older Rust path ([evidence/m2_first_boot_2026-09-24.md](evidence/m2_first_boot_2026-09-24.md)). The C kernel has not been booted on the Spark.

**Not qualified.** The trust chain and encrypted storage on the Spark (TRUST-1 and M5) still need hardware and operator steps: [docs/TRUST-1-M5-GATE-MATRIX.md](docs/TRUST-1-M5-GATE-MATRIX.md), [docs/TRUST-1-OPERATOR-STEPS.md](docs/TRUST-1-OPERATOR-STEPS.md).

**Open work.** Open tracking issues include the native OSH pipeline ([#428](https://github.com/aien-dev/aienux/issues/428) to [#432](https://github.com/aien-dev/aienux/issues/432)), iMac drivers ([#422](https://github.com/aien-dev/aienux/issues/422)), Pi 5 networking with the Spark ([#398](https://github.com/aien-dev/aienux/issues/398)), native NVMe and Store ([#31](https://github.com/aien-dev/aienux/issues/31)) and TRUST-1/M5 on the Spark ([#32](https://github.com/aien-dev/aienux/issues/32)).

For component status, [ROADMAP.md](ROADMAP.md) is the AIENOS plan, but its newest status block is dated 2026-10-01 and does not cover the Pi 5 or iMac work above.

## Rules this code follows

- **Sovereignty:** no outside organization is needed to boot the machine, reach your data, authenticate you, authorize the agent, build the trusted core, recover, change models, move hardware or keep running.
- **Open trusted base:** boot, kernel, memory, scheduling, storage, cryptography, identity, AEGIS (the authorization layer), capability checks, update checks and recovery build from readable source, offline.
- **Free inside reversible state; explicit authorization at irreversible steps.**
- **Language:** Rust is scaffolding, Omega is the destination, and C or assembly stay only where hardware, boot, ABI or measurement needs them (aien-architecture ADR 0024). No Python, no CUDA toolkit, no CUDA-shaped APIs, and no systemd or Linux init in boot, services or tooling. Dependencies in the trusted base are rare and built offline.

## Build and test

The C code needs only gcc, binutils, make and a shell. On a host that is not AArch64, the kernel Makefile uses the `aarch64-linux-gnu-` cross tools. Host tests:

```bash
make -C native/kernel test        # C kernel host tests, ends CK_CORE_HOST: PASS
make -C native/kernel sanitize    # same under ASan and UBSan
make -C native/capability test    # capability authority (docs/CAPABILITY_ABI.md)
bash scripts/cap_abi_isolation_test.sh  # C and Rust capability ABIs stay separate
make -C native/store test         # Store v1 and the sealed Store
```

Each `native/` folder has its own README with its own test targets (`make test`, often also `sanitize` and `mutants`).

The emulator gates need `qemu-system-aarch64` and the AAVMF firmware (Ubuntu packages `qemu-system-arm` and `qemu-efi-aarch64`):

```bash
bash scripts/ck_gates.sh --self-test   # checks the gate runner itself, runs no QEMU
bash scripts/ck_gates.sh               # all C kernel gates; refuses a dirty tree; writes evidence/ck_gates_<sha256>.json
bash scripts/ck_pi5_build_check.sh     # builds the Pi 5 image (build only; it cannot boot in QEMU)
```

Other images: `make -C native/kernel pi5` (Raspberry Pi 5) and `make -C native/kernel imac-kernel` (2017 iMac). See [native/kernel/README.md](native/kernel/README.md).

The Rust workspace in `crates/` is checked by `bash scripts/verify_all.sh` (needs Rust with the `aarch64-unknown-uefi` and `aarch64-unknown-none` targets).

### Who judges a change

Since 2026-10-18 (PR [#427](https://github.com/aien-dev/aienux/pull/427)), all testing runs on the Spark, not on GitHub. The Spark runs the gate manifests in [tests/gates/](tests/gates/) (each one calls `tests/gates/run.sh <step>`) and publishes a signed result to [aien-dev/aien-receipts](https://github.com/aien-dev/aien-receipts). GitHub keeps one required check on `main`, `AIEN Verification`, which only checks that signed result.

## Where the evidence lives

- [evidence/](evidence/): emulator receipts (`ck_gates_*.json`), hardware run records (Pi 5, iMac, Spark) and TRUST-1 captures.
- [docs/native/](docs/native/): Pi 5 run records.
- [native/kernel/GATES.md](native/kernel/GATES.md): every emulator gate row and its status.
- [docs/adr/](docs/adr/README.md): AIENOS decision records.

## Contributing

Read [CONTRIBUTING.md](CONTRIBUTING.md) first. Good places to start are issues labelled [`good first issue`](https://github.com/aien-dev/aienux/labels/good%20first%20issue) or [`emulator-ok`](https://github.com/aien-dev/aienux/labels/emulator-ok); they need no special hardware. Work that needs the real Spark is labelled `needs-hardware` and is run by maintainers. Open pull requests against `main` and lead with the commands you ran and their output. Questions go in [Discussions](https://github.com/aien-dev/aienux/discussions). Report vulnerabilities privately through GitHub security advisories.

## License

Licensed under the Apache License, Version 2.0 (Apache-2.0). See [LICENSE](LICENSE) and [NOTICE](NOTICE). Contact: aien@aienos.com.
