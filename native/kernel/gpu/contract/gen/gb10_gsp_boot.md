# gb10-gsp-boot (generated from gb10_gsp.devc)

Contract sha256 `0bc4fee8da8fa819dd3230c3f3ff9c9d4cf35476b2684676c9b3f8de55b98cc0`. Class `gpu-boot`, transport `pci-bar`.
71 statements compile to 142 lines of C with the API of `native/kernel/gpu/gsp_mmio.h`.

## What the compiler refuses (class gpu-boot)

- any chip access before the packet length is checked against the EMEM channel
- writing EMEM before the SEC2 channel is seen empty (HEAD equal to TAIL)
- the doorbell (HEAD) before the packet and TAIL are written, or a SEC2 write after it
- an EMEM layout where the dword offset field and the dwords-per-block constant disagree
- touching any GSP register before SEC2 releases the GSP target mask
- a write to the GSP (FALCON_OS) before the FMC error mailbox is checked
- a wait with no poll budget, or budgets out of the API's slot order
- writing a read-only register or reading a write-only one

## Steps

- RequirePacket { max: "EMEM_CHANNEL_BYTES", err: "E_ARG" }
- WaitEqual { a: "QHEAD0", b: "QTAIL0", budget: "polls_can_send", err: "E_BUSY" }
- EmemSend { ctrl: "EMEMC0", data: "EMEMD0", ainc: "AINCW", offs: "OFFS", blk: "BLK", per_blk: "EMEM_DW_PER_BLK", err: "E_EMEM" }
- PostTail("QTAIL0")
- Doorbell("QHEAD0")
- WaitNot { reg: "SEC2_MAILBOX0", val: "SEC2_CMD_BUSY", budget: "polls_cmd", into: "sec2_mb0", tread: Some(("SEC2_MAILBOX1", "sec2_mb1")), err: "E_SEC2" }
- WaitReleased { reg: "HWCFG2", locked: "PRIV_LOCKED", mask: "PRIV_LOCKED_MASK", budget: "polls_mask", into: "hwcfg2", err: "E_MASK" }
- WaitLockdown { mb0: "GSP_MAILBOX0", mb1: "GSP_MAILBOX1", bit: "LOCKDOWN", budget: "polls_lockdown", err: "E_LOCKDOWN" }
- RefuseNonzero { reg: "GSP_MAILBOX0", into: "gsp_mb0", err: "E_FMC" }
- Write { reg: "FALCON_OS", val: "app_version" }
- RequireBit { bit: "ACTIVE", into: "cpuctl", err: "E_RISCV" }

## Evidence

- NVIDIA open-gpu-kernel-modules 580.173.02, `src/common/inc/swref/published/blackwell/gb10b/dev_sec_pri.h:27`: "#define NV_PSEC_FALCON_MAILBOX0 0x00840040 /* RW-4R */"
- NVIDIA open-gpu-kernel-modules 580.173.02, `src/common/inc/swref/published/blackwell/gb10b/dev_sec_pri.h:30`: "#define NV_PSEC_FALCON_MAILBOX1 0x00840044 /* RW-4R */"
- NVIDIA open-gpu-kernel-modules 580.173.02, `src/common/inc/swref/published/blackwell/gb10b/dev_sec_pri.h:32`: "#define NV_PSEC_QUEUE_HEAD(i) (0x00840c00+(i)*8) /* RW-4A */"
- NVIDIA open-gpu-kernel-modules 580.173.02, `src/common/inc/swref/published/blackwell/gb10b/dev_sec_pri.h:36`: "#define NV_PSEC_QUEUE_TAIL(i) (0x00840c04+(i)*8) /* RW-4A */"
- NVIDIA open-gpu-kernel-modules 580.173.02, `src/common/inc/swref/published/blackwell/gb10b/dev_sec_pri.h:40`: "#define NV_PSEC_EMEMC(i) (0x00840ac0+(i)*8) /* RW-4A */"
- NVIDIA open-gpu-kernel-modules 580.173.02, `src/common/inc/swref/published/blackwell/gb10b/dev_sec_pri.h:42`: "#define NV_PSEC_EMEMC_OFFS 7:2 /* RWIVF */"
- NVIDIA open-gpu-kernel-modules 580.173.02, `src/common/inc/swref/published/blackwell/gb10b/dev_sec_pri.h:44`: "#define NV_PSEC_EMEMC_BLK 15:8 /* RWIVF */"
- NVIDIA open-gpu-kernel-modules 580.173.02, `src/common/inc/swref/published/blackwell/gb10b/dev_sec_pri.h:46`: "#define NV_PSEC_EMEMC_AINCW 24:24 /* RWIVF */"
- NVIDIA open-gpu-kernel-modules 580.173.02, `src/common/inc/swref/published/blackwell/gb10b/dev_sec_pri.h:54`: "#define NV_PSEC_EMEMD(i) (0x00840ac4+(i)*8) /* RW-4A */"
- NVIDIA open-gpu-kernel-modules 580.173.02, `src/common/inc/swref/published/blackwell/gb10b/dev_gsp.h:27`: "#define NV_PGSP 0x113fff:0x110000 /* RW--D */"
- NVIDIA open-gpu-kernel-modules 580.173.02, `src/common/inc/swref/published/hopper/gh100/dev_falcon_v4.h:27`: "#define NV_PFALCON_FALCON_MAILBOX0 0x00000040 /* RW-4R */"
- NVIDIA open-gpu-kernel-modules 580.173.02, `src/common/inc/swref/published/hopper/gh100/dev_falcon_v4.h:30`: "#define NV_PFALCON_FALCON_MAILBOX1 0x00000044 /* RW-4R */"
- NVIDIA open-gpu-kernel-modules 580.173.02, `src/common/inc/swref/published/hopper/gh100/dev_falcon_v4.h:34`: "#define NV_PFALCON_FALCON_OS 0x00000080 /* RW-4R */"
- NVIDIA open-gpu-kernel-modules 580.173.02, `src/common/inc/swref/published/hopper/gh100/dev_falcon_v4.h:39`: "#define NV_PFALCON_FALCON_HWCFG2 0x000000f4 /* R--4R */"
- NVIDIA open-gpu-kernel-modules 580.173.02, `src/common/inc/swref/published/hopper/gh100/dev_falcon_v4.h:79`: "#define NV_PFALCON_FALCON_HWCFG2_RISCV_BR_PRIV_LOCKDOWN 13:13 /* R--VF */"
- NVIDIA open-gpu-kernel-modules 580.173.02, `src/common/inc/swref/published/ampere/ga102/dev_riscv_pri.h:27`: "#define NV_FALCON2_GSP_BASE 0x00111000"
- NVIDIA open-gpu-kernel-modules 580.173.02, `src/common/inc/swref/published/hopper/gh100/dev_riscv_pri.h:58`: "#define NV_PRISCV_RISCV_CPUCTL 0x00000388 /* RW-4R */"
- NVIDIA open-gpu-kernel-modules 580.173.02, `src/common/inc/swref/published/hopper/gh100/dev_riscv_pri.h:68`: "#define NV_PRISCV_RISCV_CPUCTL_ACTIVE_STAT 7:7 /* R-IVF */"
- NVIDIA open-gpu-kernel-modules 580.173.02, `src/common/inc/swref/published/blackwell/gb20b/dev_gsp.h:33`: "#define NV_PGSP_QUEUE_HEAD(i) (0x00110c00+(i)*8) /* RW-4A */"
- NVIDIA open-gpu-kernel-modules 580.173.02, `src/nvidia/src/kernel/gpu/sec2/arch/blackwell/kernel_sec2_gb10b.c:39`: "#define SEC2_EMEM_CHANNEL_RM_SIZE 1024"
- NVIDIA open-gpu-kernel-modules 580.173.02, `src/nvidia/src/kernel/gpu/sec2/arch/blackwell/kernel_sec2_gb10b.c:271`: "const NvU32 privErrTargetLocked = 0xBADF4100U;"
- NVIDIA open-gpu-kernel-modules 580.173.02, `src/nvidia/src/kernel/gpu/sec2/arch/blackwell/kernel_sec2_gb10b.c:272`: "const NvU32 privErrTargetLockedMask = 0xFFFFFF00U;"
- NVIDIA open-gpu-kernel-modules 580.173.02, `src/nvidia/src/kernel/gpu/sec2/arch/blackwell/kernel_sec2_gb10b.c:292`: "const NvU32 cmdHandlingState = 0xc001cafe;"
- NVIDIA open-gpu-kernel-modules 580.173.02, `src/nvidia/src/kernel/gpu/sec2/arch/blackwell/kernel_sec2_gb10b.c:730`: "_ksec2UpdateQueueHeadTail_GB10B(pGpu, pKernelSec2, 0, paddedSize - sizeof(NvU32));"
- NVIDIA open-gpu-kernel-modules 580.173.02, `src/nvidia/src/kernel/gpu/gsp/arch/hopper/kernel_gsp_gh100.c:956`: "kflcnRegWrite_HAL(pGpu, pKernelFalcon, NV_PFALCON_FALCON_OS, pRiscvDesc->appVersion);"

## Proof on machines

- gpu-sim aienos-host 2026-10-21 test_gsp_mmio pending

## Registers

- SEC2_MAILBOX0 at bar 0 + 0x840040 (4 bytes, rw)
- SEC2_MAILBOX1 at bar 0 + 0x840044 (4 bytes, rw)
- EMEMC0 at bar 0 + 0x840ac0 (4 bytes, rw)
- EMEMD0 at bar 0 + 0x840ac4 (4 bytes, rw)
- QHEAD0 at bar 0 + 0x840c00 (4 bytes, rw)
- QTAIL0 at bar 0 + 0x840c04 (4 bytes, rw)
- GSP_MAILBOX0 at bar 0 + 0x110040 (4 bytes, rw)
- GSP_MAILBOX1 at bar 0 + 0x110044 (4 bytes, rw)
- FALCON_OS at bar 0 + 0x110080 (4 bytes, rw)
- HWCFG2 at bar 0 + 0x1100f4 (4 bytes, ro)
- GSP_QHEAD0 at bar 0 + 0x110c00 (4 bytes, rw)
- CPUCTL at bar 0 + 0x111388 (4 bytes, rw)

## Bits and fields

- AINCW: EMEMC0 bit 24
- LOCKDOWN: HWCFG2 bit 13
- ACTIVE: CPUCTL bit 7
- OFFS: EMEMC0 bits 7:2
- BLK: EMEMC0 bits 15:8
