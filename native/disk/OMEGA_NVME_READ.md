# Seeded Omega NVMe read protocol and Path 3 qualification

## Status and boundary

IMPLEMENTED and EXERCISED on host fixtures and a disposable QEMU NVMe namespace.
PHYSICAL NOT_RUN. Native kernel/VFIO execution NOT_RUN. This is not a TRUST-1 Gate 7
pass, a bootable native storage stack, or a replacement for the existing kernel
binding. Nothing in this work opens a live block device, resets a physical
controller, binds VFIO, mounts a filesystem, or touches 18091.

The protocol engine is `omega_nvme.om`. `omega_nvme_main.om` exposes only bounded
reads. `omega_nvme_transport.om` is an explicitly hosted verification transport:
24-byte little-endian byte-access RPCs on stdin/stdout. The existing C reference
and QEMU are verification oracles, not a C implementation hidden behind an Omega
API. Queue construction, completion polling/correlation, Identify parsing,
identity/extent approval, PRPs and successful copy-out are all Omega code.
The C relay translates memory/register accesses, not NVMe operations. Its C model
contains writes for adversarial verification, but the normal Omega entrypoint
cannot submit them. Host and emulator results establish neither DMA coherence nor
SSD latency/throughput on the Spark.

## Reuse search and ownership

Before implementing, searched `*.om` for case-insensitive `nvme` or `submission queue`:
all 5,321 fetched advertised branch refs in all 106 aien-dev repositories, and
190,008 committed-ref instances in 1,933 local common stores under `work`,
`workspace` and `/tmp` (discovery depth four). No matching existing implementation
was found. All twelve no-ref working copies and the current OwnDriver, aienux and
synapse working-copy scopes were searched separately. Three stale alternate-object
stores were resolved through successful redundant scans and an environment-only
existing object directory for their two otherwise-unscanned commits.
`tests/omega_nvme_reuse_search.txt` records exact coverage, branch inventory and
the limits of that finding; the raw command evidence remains with the operator.

The disk crumb's historical lane11/PR190 is not an active `.om` migration claim.
BOARD assigns this isolated `native/disk` candidate to QualNvmeOm; the GPU
access/lifecycle work remains in its existing driver lane. Synapse #54 owns the
R1/J1 raw-store consumer, not this hardware protocol engine. This change does not
replace or claim the System Store journal or that consumer.

## Pins and reproducible acceptance

- Compiler source: aien-dev/omega-mv `397e67040fb74853c6814097fe02ae4e1ebd6f66`,
  the existing omega-gpu-driver `feat/vfio-dma-bar` pin, not an invented profile.
- Seeded omc SHA-256:
  `bcfa582892afca8f65e551fe56867d918d86e84540c7381be8fa9ef18d0cf0f6`.
- Existing C reference: `disk_nvme.c`; existing model: `tests/nvme_test.c`.
  Their exact hashes and the compiled candidate hash are in
  `tests/omega_nvme_receipt.txt`. They were not modified by this change.
- Verification dependencies: GCC 13.3.0 (Ubuntu 13.3.0-6ubuntu2~24.04.1),
  QEMU 8.2.2 (Debian 1:8.2.2+ds-0ubuntu1.18), POSIX shell and ordinary host tools.
  These are read-only verification oracles/orchestration, not production runtime
  dependencies or an adopted replacement language implementation.

From the repository root on this Spark:

```sh
AIEN_OMC=/home/drakestapleton/work/OwnDriver/omega-gpu-driver/build/omc \
OMEGA_NVME_EVIDENCE=/home/drakestapleton/handoffs/spark-five-20261028/QualNvmeOm/replay \
sh native/disk/tests/omega_nvme_test.sh
```

The gate checks omc's independent hash before compilation. It creates a new,
regular namespace image under a private `/tmp/omega-nvme-*` directory; there is
no block-device filename option. QEMU uses TCG, a stopped CPU, 64 MiB guest RAM,
no network, and no host device passthrough. The verifier alone assigns the emulated
PCI BAR and enables that emulated PCI function. The Omega engine then configures
NVMe admin/IO queues and reads the namespace. Cleanup terminates only its own
QEMU process and removes only its disposable image/socket/log directory.

The filed receipt has 37 passing host cases, seven killed guard-removal mutants,
and ten passing QEMU cases. Successful payloads are byte-identical to
`nvme_read` through the existing C reference. Both supported LBA sizes (512 and
4096 bytes), one/two PRP pages, repeated IO wrap and repeated admin wrap are
exercised. Refusals cover identity, approval, NSID, extent, request range/count,
metadata, format, queue entry sizes, controller capability, completion CID/SQID/
SQ-head/phase, timeout, CFS and unsuccessful status. The no-write hostile caller
attempts IO Flush, Write, Write Zeroes, Dataset Management, Format and Sanitize
opcodes, plus administrative Format; all are refused before submission. Removing
the opcode guard makes the existing model observe an IO Write command and kills
the mutant. Removing CID, SQID, SQ-head, status, failed-reopen or retained-DMA
checks also kills their respective mutants.

The C model does not populate serial/firmware Identify fields. The verifier
supplies those two raw identity fields from the explicit synthetic fixture;
it does not alter queue or data behavior. QEMU supplies its own Identify data.
An inactive NSID is refused differently: the C fixture rejects it against NN
(code 24), while QEMU 8.2.2 returns a zero-size Identify Namespace (code 25).
The initial test expected identical refusal codes; the original failed receipt
is retained in the handoff directory. The final test encodes both independent
controller behaviors and still requires no IO/copy-out. No guard was weakened.

## Approved identity manifest

The normal entrypoint reads exactly 80 bytes before performing any operation.
Integer fields are little endian; identity fields are exact space-padded NVMe
Identify bytes, not trimmed string prefixes. The approval word must equal one;
it binds an already reviewed fixture/manifest, and does not grant device access
or replace operator authorization. Blank serial/firmware is refused. The final
reserved word must be zero in the normal read entrypoint.

| Offset | Bytes | Meaning |
| --- | --- | --- |
| 0 | 20 | Exact approved controller serial (Identify bytes 4..23) |
| 20 | 8 | Exact approved firmware revision (Identify bytes 64..71) |
| 28 | 4 | Explicit NSID; no first-namespace discovery or fallback |
| 32 | 8 | Approved extent start LBA |
| 40 | 8 | Approved extent block count |
| 48 | 8 | Requested start LBA |
| 56 | 4 | Requested block count |
| 60 | 4 | Repetitions, 1..64 |
| 64 | 4 | Reviewed manifest approval marker, exactly 1 |
| 68 | 4 | Reserved, exactly 0 |
| 72 | 8 | Device-visible base of the transport-owned DMA pool |

The synthetic fixture binds serial `OMEGA-FIXTURE-000001 `, firmware `8.2.2   `,
NSID 1 and extent [8,72). It requests LBA 8, 16 blocks, 40 repetitions for the
512-byte fixture. The 4096-byte host fixture requests two blocks. The DMA IOVA is
`0x123456000` in the C model (tests high address dwords) and `0x40000000` in QEMU.
These are guest/model addresses, never host pointer addresses or approved
physical addresses. The host probe entrypoints under `tests/` are never composed
into the normal read entrypoint.

## Engine invariants and limits

- One admin queue and one IO queue, each depth 16; one command outstanding.
  SQ tail and CQ head wrap independently; CQ phase starts at one after zeroing.
- The only admin opcodes admitted by the central submit guard are Identify,
  Create IO CQ and Create IO SQ. Only Read (2) is admitted on the IO queue.
- Completion phase, CID, SQID, consumed SQ head and success status must all match.
  Failed status never copies bounce data to the caller. Wrong correlation or
  exhausted poll budget latches failure and retains ownership of DMA storage.
  Subsequent reads and reinitialization are refused, without a reset or reuse.
  Queue backing is marked retained before CC.EN; readiness timeout/CFS also keeps
  it retained. Only a successful ready observation clears that activation marker.
- A 32 KiB transport-owned DMA pool holds ASQ at +0, ACQ +4096, Identify +8192,
  IOSQ +12288, IOCQ +16384, one unused page, and bounce pages +24576/+28672.
  No PRP list exists. Only aligned, at-most-two-page transfers are admitted.
- CAP must support NVM, 4 KiB minimum pages and at least 16 queue entries; major
  VS must be one or two. DSTRD is honored through 12; larger strides are refused
  before initialization because the transport's mapped register window is 64 KiB.
  SQES/CQES must support 64/16-byte entries.
- Namespace capacity and format are parsed. Only 512/4096-byte LBAs with no
  metadata or protection information are accepted. Extent and request bounds are
  checked by subtraction before arithmetic; namespace capacity must cover them.
- MDTS zero means no advertised limit. With MPSMIN zero, every nonzero MDTS
  permits at least two pages; the fixed 8192-byte software limit cannot exceed it.
- Controller-ready and command waits are bounded to 256 transport polls. This is
  a hosted, bounded attempt budget, not a claim of milliseconds or hardware
  timeout qualification. Blocking RPCs also have the verifier's wall-clock guard.
  Queue/DMA storage stays alive until the verifier tears down its controller.

## Native transport integration contract (not exercised)

The existing compiler pin already supplies `vfiomapw`, width-checked
`mapget/mapset` with DSB SY, and checked `IOMMU_MAP_DMA/UNMAP_DMA`; this change does
not claim those words are missing. A native adapter must reuse the existing
omega-gpu-driver device-ownership setup, rather than reimplement it in C or let a
manifest select arbitrary host physical memory. The exact seam is:

- `load32/load64(addr)`, `store32/store64(addr,value)`: translate the fixed engine
  BAR window or the explicitly owned DMA IOVA range into distinct live mapping
  handles; one correctly sized aligned access, bounds checked. No integer-to-CPU
  pointer conversion. Preserve ordered SQ writes before doorbell and completion
  observation before DMA copy-out.
- `zero_dma`, `fetchid`, `fetchdata`, `publish`: operate only on the owned pinned
  DMA pool/caller storage. The IOVA is not the CPU map address. Require documented
  coherent-DMA support or implement/qualify the required cache-maintenance profile;
  DSB alone is not evidence of cache coherence.
- Retain mappings, pinned pages, container and confinement on uncertain completion.
  Do not implement a process-exit/error cleanup that silently drops DMA mappings.
  Reclaim only after a separately proven controller-quiescence/IOMMU teardown.
- Replace transport-poll timing with a qualified monotonic deadline and a bounded
  poll envelope; report timeout together with retained resources. Add raw command
  submit/completion/copy timestamps without tuning queue depth, transfer size,
  affinity, scheduler, clocks, firmware settings or cache state.

The host RPC transport is not this adapter. The kernel boot/effects ABI and
coherency/retention proof need a concrete driver-owned integration before the
physical procedure below is executable. In particular, using the existing C
`nvme_bind.c` as a shortcut is forbidden: it reaches the write/flush/restore probe.
No physical command is supplied that pretends an unimplemented native adapter
already exists.

## Exact physical qualification procedure: measurement first

### Target and authority

Current read-only sysfs observation (not a block-device open): controller
`0004:01:00.0`, model `SAMSUNG MZALC4T0HBL1-00B07`, serial `S8C2NG0Y801377`, firmware
`NXHB202Q`. This is the live root NVMe, not a disposable namespace. These facts are
identity observations, NOT an approved physical manifest or permission to reset it.
Sysfs `nvme0/nvme0n1/nsid` is `1`; its `queue/logical_block_size` is `512`.
These are kernel metadata reads only, not Identify commands sent by this candidate.

The proposed read-only baseline manifest targets **that controller, NSID 1,
extent [0,64) logical blocks**, request start LBA 0, maximum 8192 bytes per command,
64 repetitions. Actual Identify must first confirm NSID, capacity, firmware and
512/4096-byte geometry against Drake's signed-off exact manifest. Use 16 blocks per
command for 512-byte LBAs, or two for 4096-byte LBAs. The sample is deliberately a
fixed repeated read of the initial 8192 bytes, not a sequential-bandwidth or
cold-cache maximum-performance claim. No writes or scratch extent on the NVMe are
needed for these reads. A separate medium is needed for the candidate image and
durable evidence; neither shrinking root nor carving a partition is authorized.
If Drake prefers a scratch NVMe instead, its complete BDF/serial/firmware/NSID and
extent require a new manifest and separate approval; never substitute silently.

The exact manifest, candidate hash, boot medium and physical action must be
approved afresh by Drake through the parent. Historical issue #336 permission or
backup progress is not authorization today. TRUST-1 Gate 7 prerequisites, including
applicable earlier gates, backup and tested recovery, must be satisfied separately.
A read-only NVMe opcode does not make controller reset, PCI ownership changes or
raw DMA safe while Linux is using the controller.

### Recovery and 18091

The existing recovery USB is **read-only and must be preserved**: Generic Flash
Disk serial `5D0344B4`, PARTUUID `f227357c-1e80-4fd0-a55e-2efbef1dd25c`, label
`AIENOSRECOV`, currently firmware entry **Boot0002**. Parent evidence records an
attended recovery boot on 2026-09-25 with NVMe root/ESP mounted read-only and
`RECOVERY_BOOT_GATE: PASS`; it has not been rebooted for this work. Do not overwrite,
format, mount read/write or repartition it. Do not use obsolete Boot0004 numbering.
A separate blank boot medium for the candidate has not yet been supplied.

All host/QEMU work in this receipt leaves resident 18091 alone. A native boot on
this sole-root-disk machine necessarily interrupts Linux and 18091; it requires an
explicit downtime/restore window through the parent. Never infer permission to
stop 18091, reboot, unbind root NVMe, bind VFIO, reset, or change firmware entries
from permission for read-only telemetry. A separate authorized scratch controller
could avoid the root-disk reboot, but none is currently approved.

### Ordered operator run (all steps below NOT_RUN)

1. Finish and exercise the native transport contract above in disposable emulation.
   Freeze compiler/candidate/manifest hashes and the unoptimized baseline policy.
   Verify recovery evidence and separate candidate/evidence medium with the parent.
   Confirm backup/recovery and the precise downtime/restore authorization. Stop if
   any prerequisite or exact identity differs.
2. Parent executes only the separately approved boot/image procedure. Never boot
   Boot0000 ALLEN.EFI or Boot0005 CORE.EFI: existing paths include a write probe.
   Do not reuse the normal C NVMe binder. Do not touch ESP, BootNext or firmware
   entries as part of this read qualification. The approved candidate must own the
   NVMe exclusively, with bounded DMA confinement, before queue initialization.
3. Print candidate and manifest hashes, PCI identity, raw serial/firmware,
   namespace geometry, IOVA pool bounds, fixed queue depth 16, one outstanding
   command, PRP limit, deadline and repetition count. Require exact matches before
   issuing reads. Record controller-ready/Identify outcomes separately from reads.
4. **Measure before changing anything.** With the fixed baseline above, preserve
   every raw `submit_ns`, `complete_ns`, `copy_done_ns`, LBA, byte count, CID, SQID,
   SQ/CQ position/phase and status for all 64 reads. Report both controller
   completion and end-to-caller timings and observed bytes. Record cache state as
   uncontrolled/repeated-read, not a claimed cold-device workload. No tuning,
   warmup suppression, queue-depth experiments, prefetch, affinity or clock changes
   precede this immutable baseline receipt. A timeout/error stops the run, with no
   reset/retry or DMA reclamation, and produces a retained-resource record.
5. Save raw records to the separately authorized evidence medium, not NVMe. Have
   Wolfram evaluate latency distribution and aggregate throughput from those exact
   records, quoting query/result and units; never infer numbers from QEMU timings.
   Example query shape uses measured integer-nanosecond lists `submit`, `complete`,
   `copied` and measured integer byte counts `bytes`:
   `With[{d=complete-submit}, {Min[d],Median[d],Quantile[d,95/100],Max[d]}]` in ns;
   `Total[bytes]/((Last[copied]-First[submit])/10^9)` in bytes/s. These are procedure
   queries only: **NOT_RUN, no fabricated physical result**. Keep controller and
   copy-inclusive statistics distinct. Record SSD/model service impact separately.
6. Release DMA/controller ownership only after the native quiescence proof succeeds.
   If it fails, retain confinement/pages and use the preapproved attended recovery
   route; do not try a reset/rebind loop. Parent returns via the authorized boot
   path, verifies normal Linux root and 18091 health, and records restoration.
7. Freeze the baseline receipt as measured, even if slow or failed. Only a later,
   separately approved tuning experiment may compare configurations. Do not rewrite
   historical atomicity failures, recovery receipts or this NOT_RUN status as PASS.

### Checked arithmetic and references

WolframLanguageEvaluator:
`{6*4096+8192,16*64,16*16,BitOr[6*2^16,4*2^20,1],BitOr[15*2^16,15],8192/512,4096/512}`
returned `{32768,1024,256,4587521,983055,16,8}` (byte counts where applicable).
`{2^64-32769,BitOr[1,15*2^16],8*512,64*512,(8+64)*512,8+64-1,Mod[44,16],Mod[40,16],16^^123456000}`
returned `{18446744073709518847,983041,4096,32768,36864,71,12,8,4886716416}`.
`Table[{d,4096+3*4*2^d+4,4096+3*4*2^d+4<=65536},{d,12,13}]`
returned `{{12,53252,True},{13,102404,False}}` (byte window upper endpoints).
These are layout/bounds calculations, not measured device performance.

- [aienux #31](https://github.com/aien-dev/aienux/issues/31), forward seeded Omega driver.
- [aienux #336](https://github.com/aien-dev/aienux/issues/336), single SSD/backup/recovery decisions.
- [synapse #54](https://github.com/aien-dev/synapse/issues/54), store work, not a queue implementation.
- Existing `disk_nvme.c`, `tests/nvme_test.c` and `scripts/qemu_native_nvme_test.sh` remain oracles.
- [QEMU QTest protocol](https://www.qemu.org/docs/master/devel/testing/qtest.html),
  byte-access request/response semantics. No QTest socket exists in production.
- [NVM Express Base Specification 2.0c](https://www.nvmexpress.org/wp-content/uploads/NVM-Express-Base-Specification-2.0c-2022.10.04-Ratified.pdf),
  sections 3.3.3.2/3.3.3.2.2 and Figure 275. © 2008 to 2022 NVM Express, Inc.
  ALL RIGHTS RESERVED. Used as a reference, not copied implementation code.
- omega-mv at the pin above, `seed/host/MAPPING.md`, exact VFIO/map access contracts.
