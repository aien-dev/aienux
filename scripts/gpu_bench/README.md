# scripts/gpu_bench -- bench scripts for the ck_gpu_bench console

The console (`ck_gpu_bench`, docs/IMAC_GPU_BENCH.md on feat/imac-gpu-bench) reads these.
Check them before a run:

    bash scripts/gpu_bench/check_bench.sh          # exit 0 clean, 1 grammar error, 3 allowlist gaps

Grammar is checked against the console's commands and argument counts. An allowlist gap
(exit 3) is not a grammar error: the console refuses that write with BENCH_ERR_RANGE until
the entry below is added to its allowlist. The console lives on the bench branch, so this
branch cannot add the entries itself.

## Register numbers are checked against AMD's headers

    bash scripts/gpu_bench/check_regs.sh           # exit 0 when every number matches, 1 otherwise

It fetches AMD's MIT register headers from the Linux kernel (v6.12, cached in
~/.cache/aienos-amd-hdr) and checks every `mm...` index, mask and shift in
`native/kernel/gpu/amd_gpu_regs.h` (value and the cited `header:line`), the SMU constants, and
every `bar5_*` index in the STEP files. Earlier versions of these files used numbers that were
never checked (GRPH 0x5045 where the header says 0x1a04, SMC 0x178 where it says 0x94).

## Write allowlist entries the STEPs need

The console on feat/imac-gpu-bench allowlists BAR5 DWORD 0x4800-0x4900, 0x4A00-0x4C00,
GRPH 0x1a04-0x1a12 (DCP0), SMC 0x94-0x95 and 0xa4.

| Step  | Writes (BAR5 DWORD index)                          | In the allowlist |
|-------|----------------------------------------------------|------------------|
| STEP1 | 0x1a04, 0x1a07, 0x1a11, 0x1a12 (GRPH, controller 0) | yes |
| STEP3 | none (reads 0x94, 0x95, 0xa4)                      | not needed |

## Entries STEP4-6 need (the same console change, for those runs)

| Step  | Writes (BAR5 DWORD index)                        | Entry to add |
|-------|--------------------------------------------------|--------------|
| STEP4 | 0x2008, 0xf814, 0xf83c, 0xec00, 0x3041, 0x3045   | those indices (GRBM_SOFT_RESET, CP_PFP_UCODE_ADDR, RLC_GPM_UCODE_ADDR, RLC_CNTL, CP_RB0_CNTL, CP_RB0_WPTR) |
| STEP5 | 0x3480, 0x3483, 0x3484                           | 0x3480-0x3489 (SDMA0 gfx ring) |
| STEP6 | 0xc200, 0x3247, 0x324f, 0x3256, 0x326c           | 0xc200, 0x3247-0x326c (review the queue-select step first, see STEP6) |

## Changes from the first version of these files

- Commands were in an older form (`r32`, `w32`, `poll`) with byte offsets. Now they use
  the console's commands (`bar5_read`, `bar5_write`, `bar5_poll`) and DWORD indices
  (byte offset / 4, given in each comment).
- STEP1: CRTC_STATUS is 0x1ba3 (byte 0x6E8C), not 0x604e; GRPH registers are 0x1a04-0x1a12, not 0x5045-0x5048 (Linux dce_11_2_d.h).
- STEP2: all indices now from gmc_8_1_d.h (AGP_TOP 0x80a, AGP_BOT 0x80b).
- STEP3: now reads only; the old 0x201 "GetSMFWVersion" message does not exist in smu7_ppsmc.h.
- STEP4 and STEP6: a poll line had no arguments; fixed.
- Inline comments moved to their own lines (the console doc says comments start with `#`).

## Not verified (do not run blind)

- SMU message 0x0201 (GetSMFWVersion): not in smu7_ppsmc.h (Linux v6.6,
  drivers/gpu/drm/amd/pm/powerplay/inc). PPSMC_Result_OK = 0x01 is verified (same file, line 83).
- STEP4 GRBM_SOFT_RESET bit values (0x00010005, 0x0): not checked against gfx_v8_0.c.
- The checker assumes the console parses 0x-prefixed hex. Confirm in the console source.
- STEP5's verify read at BAR0 + 0x2400000 (DWORD 0x900000) is outside the console's BAR0
  window (DWORD 0-65535). It needs a wider window or a readback by the driver.
- Ring, PQ, EOP and fence addresses are physical addresses from the driver's boot log.
  The scripts leave those lines commented.

Register indices are from native/kernel/gpu/amd_gpu_regs.h. Every STEP index was checked
against that header on 2026-10-19; the STEP2/3/4/5/6 byte offsets match except where noted.
