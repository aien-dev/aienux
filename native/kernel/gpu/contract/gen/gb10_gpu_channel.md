# gb10-gpu-channel (generated from gb10_chan.devc)

Contract sha256 `a138c84a7dbacf025cd3ee2a600c87476f52c07395eb85bcd0238056d3e69289`. Class `gpu-chan`, transport `pci-bar`, ring `gpfifo`.
66 statements compile to 196 lines of C with the API of `native/kernel/gpu/gpu_chan.h`.

## What the compiler refuses (class gpu-chan)

- a doorbell before the put pointer is published, or anything after the doorbell
- no fence between the ring write and the publish, or between the publish and the doorbell
- a ring write with no room, buffer, span or address check before it
- a ring size that is not a power of two, or no ring-size check at all
- a channel or runlist bound that does not fit the doorbell token field it lands in
- an address check that differs from the width of the GPFIFO address field, a length check that differs from the length field
- memory the CPU writes while the GPU reads it (or that the CPU polls) that is GPU-cached
- a buffer never zeroed, a page-directory store before the zeroing
- binding before a fence, or anything after the bind
- teardown that frees before disable and idle, or does not release every buffer

## Evidence

- NVIDIA open-gpu-kernel-modules 580.173.02, `src/common/sdk/nvidia/inc/class/clc96f.h:95`: "#define NVC96F_GP_ENTRY0_GET 31:2"
- NVIDIA open-gpu-kernel-modules 580.173.02, `src/common/sdk/nvidia/inc/class/clc96f.h:98`: "#define NVC96F_GP_ENTRY1_GET_HI 7:0"
- NVIDIA open-gpu-kernel-modules 580.173.02, `src/common/sdk/nvidia/inc/class/clc96f.h:102`: "#define NVC96F_GP_ENTRY1_LENGTH 30:10"
- NVIDIA open-gpu-kernel-modules 580.173.02, `src/common/sdk/nvidia/inc/class/clc96f.h:31`: "NvU32 GPPut; /* GP FIFO put offset 008c-008f*/"
- NVIDIA open-gpu-kernel-modules 580.173.02, `src/common/inc/swref/published/blackwell/gb100/dev_vm.h:620`: "#define NV_VIRTUAL_FUNCTION_DOORBELL 0x30090 /* -W-4R */"
- NVIDIA open-gpu-kernel-modules 580.173.02, `src/common/inc/swref/published/blackwell/gb100/dev_vm.h:622`: "#define NV_VIRTUAL_FUNCTION_DOORBELL_VECTOR 11:0 /* -WXUF */"
- NVIDIA open-gpu-kernel-modules 580.173.02, `src/common/inc/swref/published/blackwell/gb100/dev_vm.h:624`: "#define NV_VIRTUAL_FUNCTION_DOORBELL_RUNLIST_ID 22:16 /* -WXUF */"
- NVIDIA open-gpu-kernel-modules 580.173.02, `src/common/inc/swref/published/blackwell/gb10b/dev_ram.h:27`: "#define NV_RAMRL_ENTRY_CHAN_USERD_PTR_LO (31+0*32):(8+0*32) /* RWXUF */"
- NVIDIA open-gpu-kernel-modules 580.173.02, `src/common/inc/swref/published/blackwell/gb10b/dev_ram.h:28`: "#define NV_RAMRL_ENTRY_CHAN_USERD_PTR_HI_HW (19+1*32):(0+1*32) /* RWXUF */"

## Proof on machines

- gpu-sim aienos-host 2026-10-21 test_gpu_sim pending

## Registers

- DOORBELL at bar 0 + 0xbb0090 (4 bytes, wo)

## Formats

- TOKEN (32 bits): RUNLIST_ID 22:16, VECTOR 11:0
- GP_ENTRY (64 bits): GET 39:2 (address bits in place), LEVEL 41:41, LENGTH 62:42
- RL_USERD (64 bits): PTR_LO 31:8 (address bits in place), PTR_HI 51:32 (address bits in place)
- RAMIN_PDB (64 bits): TARGET 1:0, BIG_PAGE_64K 11:11, ADDR 63:12 (address bits in place)
