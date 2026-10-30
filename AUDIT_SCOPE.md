# Audit scope

Written for an outside, independent security audit. Revision described:
`659ddd2d7e3e140cb9bf942c3171cbd9cf3d092e` on `main`, 2026-10-26. Everything below is taken from
the repository's own files at that revision; items marked NOT_RUN were not executed while writing
this document.

## What this repository is

Aienux is the AienOS kernel, built to replace Linux on the NVIDIA DGX Spark. The kernel is being
rewritten in C (`native/`); a legacy Rust workspace (`crates/`, `Cargo.toml`) is frozen scaffolding
and still checked. It covers UEFI boot with image slots, the kernel, capability enforcement,
cryptography (signatures, M5 keyed authentication, chunked AEAD), a torn-write-safe Store, disk,
network and a containment component (Argus). Status (README): experimental, pre-alpha, nothing
physically qualified. The C kernel is IMPLEMENTED / NOT QUALIFIED; its emulator gates pass in QEMU
only. It has run on a Raspberry Pi 5 (2026-10-13) but has not been booted on the DGX Spark. A first
native boot of the earlier kernel on the DGX Spark is recorded for 2026-09-24.

## What to review, in priority order

1. Cryptographic boundaries (`native/crypto/`, `native/sig/`, `native/m5/`,
   `crates/aienos-crypto/`, `docs/CRYPTO_CONSTANT_TIME.md`, `docs/ED25519_DEPENDENCY_REVIEW.md`).
   AGENTS.md requires constant-time tag and key comparison, zeroization of keys and scratchpads,
   decryption into a private scratchpad with no plaintext released before tag verification, and a
   fixed AAD layout with no self-referential `ObjectId` or generation.
2. Store and recovery (`native/store/`, `native/disk/`, `native/kernel/CONTINUITY_RECOVERY_CONTRACT.md`).
   The three proofs (unkeyed structural integrity, keyed authentication, anti-rollback freshness)
   must stay separate. Store v1 superblock bytes 172 to 4095 are frozen zero.
3. Boot and update (`native/boot/`, `docs/NATIVE_BOOT_ROLLBACK_CONTRACT.md`,
   `docs/NATIVE_IMAGE_SLOTS.md`, `docs/BOOT_HANDOFF_CONTRACT.md`). Slot selection, update and
   rollback; artifact and model loading; unlock path (`efi_unlock.c`).
4. Kernel (`native/kernel/`, `native/kernel/GATES.md`). Exception handling, MMU, EL0 isolation,
   IPC, scheduling, SMMU and DMA routing (`docs/GB10_DMA_ROUTE_INVENTORY.md`).
5. Capability authority (`native/capability/`, `docs/CAPABILITY_ABI.md`,
   `scripts/cap_abi_isolation_test.sh`). The C and Rust capability ABIs must stay separate.
6. Network and containment (`native/net/`, `native/argus/`).
7. Owner keys (`docs/OWNER_KEY_CEREMONY.md`, `scripts/ck_owner_keys_check.sh`).
8. `.github/workflows/`: `aien-verification.yml` (pull request and push, `contents: read`, trust
   anchors from `origin/main`, builds only pinned verifier tools, verifies signed Spark bundles on
   `alpha/*` tags) and `copilot-setup-steps.yml` (manual only, installs Rust, QEMU, AAVMF and
   swtpm). Actions are pinned by commit sha.
9. `tests/gates/run.sh` and `tests/gates/*.gate`; `scripts/ck_gates.sh`; `scripts/verify_all.sh`.

## Trust boundaries

- Untrusted: disk contents (a Store may be torn, foreign or tampered), boot artifacts and update
  images before signature verification, network input, user-mode tasks, the agent and the model
  (README: the agent is never the root of trust and the model is never the kernel).
- Trusted: firmware and the hardware anti-rollback source (TPM NV counter or platform anchor), the
  owner keys, the kernel itself, the vendored crates in `vendor/` and the Spark signing key in
  `.aien/verify.pub`.

## Threat model summary

- Assets: volume keys (`K_root_auth`, `K_vol`), Store contents and history, boot slot integrity,
  capability state, owner identity.
- Attackers considered: physical tampering with storage, replay of an older authorized Store
  history, a malicious boot or update image, a hostile user-mode task, network input.
- Defended (by design per AGENTS.md and docs): structural Store verification without keys, keyed
  authentication with HMAC-SHA256 and AEAD, anti-rollback against a monotonic source, constant-time
  comparisons, zeroization, signed boot slots with rollback, capability enforcement in the kernel.
- Not defended / explicitly out of scope: nothing is physically qualified; QEMU qualifies nothing
  physical (`native/kernel/GATES.md`); side channels beyond constant-time comparison are not
  claimed; the Rust workspace is legacy scaffolding.

## Build from source at a pin

C kernel: gcc, binutils, make and a shell. Emulator gates: `qemu-system-aarch64` and AAVMF
(Ubuntu `qemu-system-arm`, `qemu-efi-aarch64`). Legacy Rust: a Rust toolchain with targets
`aarch64-unknown-uefi` and `aarch64-unknown-none`; there is no `rust-toolchain.toml`, so the exact
Rust version is unknown from the repository. Cargo builds offline from `vendor/` (`.cargo/config.toml`).

Pins: `omega.lock` names omega commit `362c9ab50c8ecc3012dd2e3ac20329865ba80982` and the sha256 of
the vendored `native/kernel/third_party/omega/rx_cortex.c` and `rx_cortex.h`; `crumb.lock` names
`d0f0b95d344b4c6628c3d868346932fd247ce168`.

## Run every test

```sh
make -C native/kernel test
make -C native/kernel sanitize
make -C native/capability test
bash scripts/cap_abi_isolation_test.sh
make -C native/store test mutants
bash scripts/ck_gates.sh
bash scripts/ck_pi5_build_check.sh
bash scripts/verify_all.sh
tests/gates/run.sh crypto-boundary
tests/gates/run.sh zero-disk-secrets
```

Every gate is `tests/gates/run.sh <step>`; steps include `crumb-verify`, `ck-host-tests`,
`ck-stage-tests`, `ck-gate-selftest`, `ck-hold-selftest`, `ck-owner-keys`, `ck-qemu-gates`,
`ck-b7b-probe`, `native-net`, `native-sig`, `native-crypto`, `native-m5`, `native-disk`,
`native-argus`, `native-capability`, `disk-qemu`, `crypto-boundary`, `infer-crate`,
`store-native`, `verify-all` and `zero-disk-secrets`. Native suites must print lines such as
`AIENOS_CRYPTO_NATIVE: PASS`, `M5_NATIVE: PASS` and `NET_SECURE_MUTANTS: N/N killed`.
`scripts/ck_gates.sh` writes `evidence/ck_gates_<sha256>.json` and refuses a dirty tree; it reports
NOT_RUN while another gate run holds the lock. All: NOT_RUN while writing this document.

The DGX Spark is the test judge: it runs these gates and signs a bundle. The workflow header says
nothing on GitHub gates a merge (2026-10-24) and GitHub checks are advisory; the README still lists
five required checks, so the README text and the workflow disagree. Anything that needs the real
DGX Spark is labelled `needs-hardware` and is NOT_RUN here.

## Known limitations and NOT_RUN

- Not booted on the DGX Spark with the C kernel; physical qualification NOT_RUN.
- The newest committed C gate receipt covers 24 gates; `scripts/ck_gates.sh` now lists 26, so a
  newer receipt is owed (README).
- `ROADMAP.md` status block is dated 2026-10-01.
- A head that changes `.aien/`, `tests/gates/` or `.github/workflows/` is run by the operator only
  under the shared AIEN pipeline rules; this repository has no pipeline document of its own.

## Dependencies and provenance

- Rust crates vendored in `vendor/` (including `ed25519-dalek`, `curve25519-dalek`, `sha2`,
  `subtle`, `zeroize`, `uefi`, `serde_json`) with `Cargo.lock`.
- Vendored C from omega pinned by `omega.lock`.
- Host tools: gcc, binutils, make, QEMU, AAVMF, swtpm (copilot setup only).
- No Python, no CUDA, no network fetch in the build (CONTRIBUTING.md).
- Licence Apache-2.0 (`LICENSE`, `NOTICE`).

## Evidence

- `evidence/` holds content-addressed gate receipts (`ck_gates_<sha256>.json`,
  `trust1_m5_qualification_<sha256>.json`), QEMU logs, Raspberry Pi 5 runs
  (`pi5_hardware_runs_2026-10-13`) and earlier DGX Spark records (`m2_first_boot_2026-09-24.md`).
- Signed Spark bundles: `aien-dev/aien-receipts` at `aienux/<sha>/bundle.json` (workflow path).
- Receipts are append-only and never edited.
