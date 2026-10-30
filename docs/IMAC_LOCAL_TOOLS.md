# Allen's local tools on Intel x86-64

This is the first compute-only program runner for the iMac. Allen may request three build-approved local Omega programs: checked signed-integer arithmetic, byte/ASCII-word/newline counts, and a nonidentifying machine summary. Requests never contain executable code, paths, access rights, or resource limits. No file, disk, network, device, process, or package APIs are exposed to a program.

## Execution contract

The BSP runs one program at CPL3 under a private CR3. Inherited kernel mappings are supervisor-only. User code and fixed return veneers are RX, input and the runtime table are read-only and NX, and workspace and stack are RW and NX. Unmapped guard pages separate the regions. The other cores retain their existing drawing roles. The BSP returns to device polling after each one-shot LAPIC interval (nominally 1 ms). CPU consumption is capped at 100 ms, elapsed lifetime at 500 ms, and a question at four invocations. Interrupt handling and teardown add overhead to a slice.

Per invocation: at most 64 KiB compiled code, 64 KiB stack, 128 KiB workspace, 4 KiB input, and 4 KiB published output. The fixed 4 KiB veneer and 4 KiB runtime-table pages are additional kernel-supplied mapping overhead. Current result schemas use only 8, 24, or 40 bytes. One fixed backing arena is reused after its private bytes and page tables are cleared. It is reserved kernel image memory, not a general allocator or multithreaded process system.

The supported processor state is integer registers plus x87/SSE2 state. Kernel and program FPU state are saved separately at each transition. OSXSAVE/AVX, FSGSBASE, user RDPMC, and protection-key writes are disabled during execution. SYSCALL and SYSENTER are disabled, and no firmware LDT survives kernel initialization. User exceptions terminate the invocation; exceptions from kernel code still take the kernel fault path. NMI, double fault, and machine check remain fatal even if they interrupted a program.

An invocation returns, traps with an Omega code (1..14), is refused, or ends abnormally (fault, CPU/elapsed limit, cancellation, invalid return). Only a return through the immutable veneer publishes output. Failed invocations publish zero output bytes. Ending the active question or sleeping invalidates its authority and cancels any invocation. Repeating an invocation ID with identical input returns the existing state/result; reusing it with different input is refused.

## Artifact and compiler contract

The existing BinaryArtifact parser and Ed25519 implementation are reused. Explicit local consumers require target `2` (x86-64 LE) and ABI `2` (Omega x86 local v1). Existing `cka_parse`, `cka_identify`, and `cka_verify` callers still require AArch64 target `1`, ABI `1`. This is an Intel-local extension, not a change to the ARM execution ABI.

The local profile permits no capabilities, one zero data-initializer byte, entry offset zero, and fixed resource ceilings. A separate owner public key and whole-artifact SHA-256 allowlist are embedded in `x86/local_bundle.h`. The private signing seed stays outside the repository. Boot verifies signatures and identities; each launch hashes the copied code before mapping it executable. Changing the model or courier cannot expand the allowlist.

Omega compiler repository: `aien-dev/omega-mv`, commit `bf54b19fa590e8ae925c0e80815516dbf41e7326`.

- `seed/out/omc` SHA-256: `62775d2a391cfef7cde64e387655caa9809a267093112910ec76bfa568f3712b`
- `seed/omc/omc.om` SHA-256: `96d51b63c8f451661ac715395078e04f39195e50c9e8fe37746cda121e4d7a1e`

The three sources are `native/kernel/x86/tools/*.om`. The leading `target x86linux` selects the compiler's x86 instruction emitter; absence of `main` selects bare function output. These are raw native functions, not Linux ELF programs, and receive no Linux syscall ABI. The wrapper follows the pinned compiler's private frame-slot ABI: old RBP argument slots, R14 immutable runtime table, R15 stack floor, RAX result length, and runtime slot 2 for a nonlocal trap. Three parameters are fixed-size input/output array views and input length. Arithmetic returns one little-endian signed 64-bit integer; text returns three unsigned 64-bit counts; system returns five unsigned 64-bit fields (cores, available frame-pool RAM bytes, width, height, network state).

Reference: the pinned compiler's `docs/X86-LINUX.md` and `seed/omc/omc.om` (`xpro`, `xunit`, `xfinish`). Hardware reference: Intel SDM combined PDF in the project's iMac hardware library, PDF p3293 (Vol 3A 5.6 access rights), p3372 (Vol 3A 7.14.3-4 IRET and stack switching), Vol 1 10.5 (FXSAVE/FXRSTOR). Test oracles use existing C tools, Python, QEMU/OVMF, and the pinned compiler; no new production runtime or dependency is introduced.

## Courier

A kernel job advertises `local-tools=v1`. The relay accepts a closed JSON action schema from the model and sends ordered `CALL request invocation tool total offset hex` chunks, at most 96 decoded bytes per line. The kernel independently validates active request, IDs, chunk lengths/order, tool, input shape, replay identity, and budget. `POLL` and `CANCEL` use the same active request and invocation IDs. `RESULT` carries a state, reason, and hex-encoded result. Unknown or malformed actions never execute.

The courier has its own four-call ceiling, but the kernel does not depend on it. Results are included as data in the next model request. A completed answer ends the kernel's request authority. Legacy jobs without the feature marker keep their original path. This first version waits for each complete structured model response before acting; it does not stream partial JSON or execute partial tool requests.

An exact, single signed-integer arithmetic request (for example, `What is 123 multiplied by 456?`, `20 + 22`, or `subtract 7 from 2`) selects the same approved native arithmetic program directly. The courier only parses the operation and operands; it does not calculate. It formats the validated kernel result or failure without contacting the model, including when the model is unavailable. Requests containing units, decimals, explanations, references, or additional operations retain the model path. Integer division is labelled as an integer quotient. Compact dates and ranges retain the model path unless phrased explicitly as calculations. Fixed native results are never clipped mid-number by the conversational length setting. This shortcut changes only the courier and works with the existing iMac kernel over the network. Successful direct requests are logged as `local-direct`, with their measured end-to-end time.

The existing lab UDP transport is unauthenticated. It must be treated as hostile: it can inject permitted bounded compute work during an active question, or disrupt a reply. It cannot upload a program, expand kernel permissions, or access device/file APIs. This work does not add authenticated networking, DMA confinement, speculative-execution side-channel defenses, or whole-system security qualification.

## Verification and reproduction

`OMEGA_SOURCE=/path/to/pinned/omega bash scripts/check_local_tools.sh` compiles the exact native sources, compares each signed embedded payload, tests signatures/identities/profile refusal/truncation, and runs courier unit tests. Existing ARM artifact tests remain applicable.

`bash scripts/qemu_ck_local_tools_test.sh` uses two QEMU CPU profiles. It probes real CPL3 isolation, protected writes, NX execution, guards, privileged instructions, Linux syscall rejection, forged returns, discarded partial output, timeouts, cancellation, replay, request limits, register preservation, and 1,000 repeated runs. A deterministic model oracle then selects all three tools through the real Rust courier and COM2; it verifies the results returned by the emulator kernel. This is an integration oracle, not evidence that a live model selected the right action. The oracle is C (`native/kernel/x86/tests/local_tools_model_oracle.c`); `bash scripts/test_local_tools_udp.sh` runs the same wire checks and three direct-arithmetic jobs against the Rust courier without QEMU.

Hostile fixtures require `CK_LOCAL_TEST=1` and build into a separate directory and `IMACKERN-LOCAL-TEST.EFI`. Release images reject the fixture marker. Do not deploy test images. Full strict repository validation and a physical boot of the release image remain separate requirements; QEMU is not physical iMac proof.
