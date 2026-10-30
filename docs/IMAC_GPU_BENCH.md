# IMAC GPU Bench: interfaces for feat/imac-gpu-bench

## Status (read this first)

- **Live kernel swap over UDP is NOT BUILT.** `native/kernel/x86/swap_x86.c`
  `recv_swap_udp` is gone; the receiver on port 47034 does not exist yet. The
  verify, quiesce and jump path works, but only with the slot object staged on a
  disk (QEMU test). A running iMac cannot receive a kernel over the network.
- **GPU bench over UDP is built but NOT TESTED on hardware.** The lab image has the
  tg3 receive and transmit path for port 47035 (`tg3_x86.c`). QEMU has no BCM57766,
  so nothing here exercises it. The QEMU test drives the same protocol code over a
  COM1 socket instead.
- The BARs in the QEMU bench test are **RAM stand-ins**, not PCI emulation.
- Nothing in this document has been run on an iMac.

## Install instructions (corrected)

1. **The first lab kernel is installed by one normal netboot** (`NETLDR.EFI`, signed
   with the owner key). The kernel that runs on the iMac today has no swap receiver,
   so a live swap cannot deliver the first lab kernel.
2. **Later lab kernels are meant to arrive by live swap, with no firmware boot.**
   That path is not working yet (see Status). Until the UDP receiver is built and
   tested, each kernel is installed by netboot.
3. Only a lab build (`CK_X86_REPORT=1`) contains the swap receiver and the bench
   console. A default image contains neither; the Makefile link step refuses a
   default image that has the swap verifier or trampoline symbols.

## Interfaces

### Ports

| Port  | Purpose       | Direction    | Auth                  | Status |
|-------|---------------|--------------|-----------------------|--------|
| 47034 | Live swap     | Spark to iMac | Ed25519 via ck_slot_verify | Not built (receiver stub removed) |
| 47035 | GPU bench     | Spark to iMac | HMAC-SHA256           | Lab build; tg3 path untested |

### Live swap: slot object and verification

The swap input is a signed slot object: one 4096-byte manifest unit
(`native/boot/slot_format.h`) followed by the raw kernel PE image. It is verified by
`ck_slot_verify` (`native/boot/slot_select.c`), the same function NETLDR.EFI calls
(`native/boot/net_loader.c`, the `ck_slot_verify(m, obj + CK_SLOT_UNIT, ...)` call).
It checks the manifest structure, that `signer_id` names a trusted key, the Ed25519
signature over the 448 signed bytes, and the image SHA-512. The trusted keys are the
same table NETLDR uses, generated with `ck_slot_tool keys-c`.

Make a slot object (the `ck_imac_swap` tool does not sign):

```
ck_slot_tool make-manifest --image IMACKERN-LAB.EFI --generation N --out M.bin
ck_slot_tool sign-test M.bin --out MS.bin         # RFC 8032 TEST key: QEMU only
ck_slot_tool verify MS.bin --image IMACKERN-LAB.EFI --test-key
cat MS.bin IMACKERN-LAB.EFI > SLOT_OBJECT
```

For hardware, sign with the owner key (`ck_slot_tool tbs`, an offline signer, then
`attach-sig`). The TEST key is refused for hardware by the tool.

### GPU bench protocol (port 47035)

Authentication key: 32-byte HMAC-SHA256 key, `bench_proto.h` `BENCH_TEST_KEY_INIT`
by default (16 zero bytes followed by 0x01..0x10). A production build sets it with
`CK_X86_BENCH_KEY=<32 bytes as C initialiser values>`. The Spark tool uses the built-in
test key unless `--key <file>` gives a 32-byte binary key file.
`make imac-kernel CK_X86_BENCH_KEY_FILE=<that file>` binds the same file into the
image without the key ever appearing in a command line or build log (the generated
header is mode 600 inside the build directory); the ready line then says `this
image's own key` instead of `the TEST key`. The lab image of bundle 1 uses
`~/.config/aienos/imac-bench.key` on the Spark.

Replay protection: a 64-bit counter. The kernel refuses any request whose counter is
not above the last one it accepted (the kernel keeps this until it reboots).

```
Request (Spark to iMac, 64 bytes):
  magic[4]  = "ABCH"
  version   u8 = 1
  cmd       u8 = BENCH_CMD_*
  pad[2]    = 0
  counter   u64 be   (must be above the kernel's last accepted counter)
  arg0..3   u32 be each (meaning per command, below)
  hmac[32]  = HMAC-SHA256(key, bytes 0..31)

Response (iMac to Spark, 64 bytes):
  magic[4]  = "ABCR"
  version   u8 = 1
  status    u8 = BENCH_OK | BENCH_ERR_*
  pad[2]    = 0
  counter   u64 be   (echoed; 0 on a bad-HMAC refusal)
  tsc       u64 be
  result    u32 be
  before    u32 be
  hmac[32]  = HMAC-SHA256(key, bytes 0..31)     every response, errors included
```

Commands (arg0..arg3):

| Code | Name                | arg0       | arg1        | arg2     | arg3       |
|------|---------------------|------------|-------------|----------|------------|
| 0x00 | BENCH_CMD_PING      | 0          | 0           | 0        | 0          |
| 0x01 | BENCH_CMD_PCI_CFG   | offset (dword aligned, 0..252) | 0 | 0 | 0 |
| 0x02 | BENCH_CMD_BAR5_READ | reg_index  | 0           | 0        | 0          |
| 0x03 | BENCH_CMD_BAR5_WRITE| reg_index  | value       | 0        | 0          |
| 0x04 | BENCH_CMD_BAR0_READ | word_off   | 0           | 0        | 0          |
| 0x05 | BENCH_CMD_BAR0_WRITE| word_off   | value       | 0        | 0          |
| 0x06 | BENCH_CMD_BAR5_POLL | reg_index  | mask        | expected | timeout_ms |
| 0x07 | BENCH_CMD_BAR5_RMW  | reg_index  | clear_mask  | set_bits | 0          |

`reg_index` is a DWORD index (byte offset = index x 4). `word_off` is a DWORD
offset into BAR0.

Status codes: `0x00` OK; `0x01` ERR_AUTH (bad HMAC, or counter not above the last);
`0x02` ERR_RANGE (not on the allowlist, or offset out of range); `0x03` ERR_ALLONES
(the read returned 0xFFFFFFFF: device gone); `0x04` ERR_TIMEOUT (TSC-bounded poll
expired); `0x05` ERR_NODEV; `0x06` ERR_BUSY; `0x07` ERR_CMD.

The kernel answers an ALLONES read and carries on. The Spark tool is what stops the
batch: `ck_gpu_bench` sends nothing after an ERR_ALLONES reply.

BAR5 write allowlist (DWORD indexes, inclusive). Every number is from AMD's MIT register
headers in Linux, `drivers/gpu/drm/amd/include/asic_reg/dce/dce_11_2_d.h` and
`smu/smu_7_1_3_d.h`:

| Range | What sits there |
|---|---|
| `0x4800`-`0x4900` | UNIPHY link control, LVTMA panel power sequencer (`mmLVTMA_PWRSEQ_CNTL` 0x481b), backlight PWM (`mmBL_PWM_CNTL` 0x4820) |
| `0x4A00`-`0x4C00` | DIG 0 and DIG 1 front ends: the HDMI and DisplayPort encoder blocks (`mmDIG_FE_CNTL` 0x4a00, `mmDIG1_DIG_FE_CNTL` 0x4b00, `mmDIG2_DIG_FE_CNTL` 0x4c00). Not the CRTC (`mmCRTC_STATUS` is 0x1ba3, not writable here). |
| `0x1A04`-`0x1A12` | DCP0 graphics surface: `mmGRPH_PRIMARY_SURFACE_ADDRESS` 0x1a04, `mmGRPH_PITCH` 0x1a06, `mmGRPH_PRIMARY_SURFACE_ADDRESS_HIGH` 0x1a07, `mmGRPH_UPDATE` 0x1a11, `mmGRPH_FLIP_CONTROL` 0x1a12 (controller 0 only) |
| `0x0094`-`0x0095` | SMC mailbox 0: `mmSMC_MESSAGE_0` 0x94, `mmSMC_RESP_0` 0x95 (the pair Linux `smu7_send_msg_to_smc` uses) |
| `0x00A4` | `mmSMC_MSG_ARG_0` 0xa4 (the message argument) |

BAR5 reads: any index 0..65535. BAR0 (VRAM): words 0..65535 (the first 256 KiB only).

## Build

```
make -C native/kernel imac-kernel CK_X86_REPORT=1 CK_TEST_SLOT_KEY=1 CK_X86_BENCH=1
make -C native/kernel imac-kernel CK_X86_REPORT=1 CK_SLOT_KEYS=<owner public keys file> CK_X86_BENCH=1
make -C native/kernel ck-gpu-bench ck-imac-swap      # host tools -> $(OUT)/host/
```

| Variable | Meaning |
|----------|---------|
| `CK_X86_REPORT=1` | Lab build: swap receiver, bench console, tg3 transmit, network report. Required for the two below. |
| `CK_X86_BENCH=1` | Compile the bench console (lab only). |
| `CK_X86_BENCH_KEY=` | Bench HMAC key bound into the image (default: test key). |
| `CK_SLOT_KEYS=<file>` | Owner public keys for the swap verifier (same format as the NETLDR keys). Required in a lab build unless the next line is set. |
| `CK_TEST_SLOT_KEY=1` | RFC 8032 TEST key for the swap verifier. QEMU tests only. |
| `CK_X86_SWAP_TEST_DISK=1` | QEMU test: swap slot object read raw from SATA disk 2, LBA 2048. Lab only. |
| `CK_X86_BENCH_TEST=1` | QEMU test: RAM stand-in BARs, bench packets as COM1 text lines. Lab only. |

The default `make imac-kernel` produces `IMACKERN.EFI` with none of the lab code.

## Host tools

### ck_gpu_bench

```
ck_gpu_bench [--key FILE] [--target IP | --serial PATH] [--transcript FILE] <script.bench>
ck_gpu_bench ping [same options]
```

Script lines: `ping`, `pci_cfg OFF`, `bar5_read REG`, `bar5_write REG VAL`,
`bar0_read W`, `bar0_write W VAL`, `bar5_poll REG MASK EXPECTED MS`,
`bar5_rmw REG CLEAR SET`. Prefixes for negative tests: `badauth CMD` (one HMAC byte
flipped), `stale CMD` (counter 1), `replay` (previous request again). Numbers accept
decimal or `0x`.

Output: one line per transaction, `<label> <STATUS>: result=0x.. before=0x..`.
Exit 0 all OK; 1 an error status or an ALLONES stop; 2 transport failure.

The counter starts from the clock, so a second run is not refused by a kernel that
has seen a previous run. `--transcript` writes every byte received on the serial
link and every request sent.

### ck_imac_swap (not usable against an iMac yet)

```
ck_imac_swap push <SLOT_OBJECT> [--target IP]
```

Sends a signed slot object (see above) to port 47034. The kernel side is not built,
so the tool times out against a real iMac. It does not sign anything.

## QEMU tests

- `scripts/qemu_ck_imac_swap_test.sh`: boots kernel A, which teaches Allen one fact
  and saves Allen's memory; A verifies slot B with `ck_slot_verify`, quiesces and
  jumps into B in the same QEMU process; B must load the memory A saved. A second run
  refuses a bad signature.
- `scripts/qemu_ck_gpu_bench_test.sh`: boots the lab kernel with the test transport
  and runs a script through the real `ck_gpu_bench --serial`. Covers ping, config and
  BAR reads and writes, allowlist refusal, poll and read-modify-write, replay, bad
  HMAC, stale counter, and the all-ones stop.

Both tests run QEMU's emulated CPU and devices only. A PASS says the code does what
it says in QEMU. It says nothing about the GPU, the BCM57766 or the iMac firmware.

## Honest limits

- The swap trampoline uses the AP trampoline page (`tramp_pa`, below 1 MiB). If the
  frame allocator did not give a page there, the swap is refused.
- The swap does not apply PE base relocations: both kernels must be linked at the
  same addresses.
- A wrong GPU write can wedge the PCIe link; only a power hold (firmware boot)
  recovers. The allowlist and the before/after logging make this rare.
- No multi-kernel in flight: a second swap request during a swap is refused.
- The bench key in the QEMU test build is the public test key.
