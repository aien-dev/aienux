/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */
/* aien_gpu_uapi.h: the /dev/aien-gpu interface, shared by the module (aien_gpu.c) and its user-space tools
 * (aien_gpu_check.c, aien_gsp_boot.c). L332 rung 2. */
#ifndef AIEN_GPU_UAPI_H
#define AIEN_GPU_UAPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

struct aien_gpu_info {
	__u32 boot0, boot42;
	__u64 bar0_bytes;
	__u64 arena_bytes, arena_iova;   /* mmap offset 0 of /dev/aien-gpu */
	__u64 fw_bytes;
	__u32 fw_ok, dma_bits;
	__u64 heap_bytes, heap_iova;     /* GSP sysmem heap: chip only, not mapped to user space */
	__u32 allow_boot, doorbells;     /* doorbells: SEC2 QUEUE_HEAD(0) writes so far (at most 1 per load) */
	__u64 bar0_pa;
};

/* One 32-bit BAR0 register. Only the GSP start-up registers of gpu/contract/gb10_gsp.devc (plus the two
 * identity registers, for reads) are reachable; anything else is -EPERM. Writes need allow_boot=1 and
 * CAP_SYS_RAWIO. After remove, INFO and register ioctls return -ENODEV. */
struct aien_gpu_reg {
	__u32 off, val;
};

#define AIEN_GPU_INFO _IOR('A', 0x01, struct aien_gpu_info)
#define AIEN_GPU_REG_RD _IOWR('A', 0x02, struct aien_gpu_reg)
#define AIEN_GPU_REG_WR _IOW('A', 0x03, struct aien_gpu_reg)

#endif
