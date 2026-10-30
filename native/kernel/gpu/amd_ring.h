/* native/kernel/gpu/amd_ring.h -- Public PM4/SDMA ring submission interface.
 *
 * Owned by feat/imac-gpu-core; consumed by feat/imac-gpu-shader.
 * Every call is AIENOS bare-metal (no OS services, no floats, no CUDA).
 *
 * The GFX ring (RING_GFX) accepts PM4 IT_DRAW and compute PM4 for the CP.
 * The SDMA ring (RING_SDMA) accepts SDMA operation packets.
 * The compute queue (RING_MEC) is an HQD-backed MQD pipe for DISPATCH_DIRECT.
 *
 * Sources consulted (MIT-licensed, not reproduced):
 *   Linux drivers/gpu/drm/amd/amdgpu/gfx_v8_0.c -- gfx_v8_0_ring_emit_*
 *   Linux drivers/gpu/drm/amd/amdgpu/sdma_v3_0.c -- sdma_v3_0_ring_emit_*
 * Attribution in THIRD_PARTY_NOTICES. */
#pragma once
#include <stdint.h>

/* Which ring to submit to. */
typedef enum {
    AMD_RING_GFX  = 0,   /* CP GFX ring 0 (CP_RB0) */
    AMD_RING_SDMA = 1,   /* SDMA0 GFX ring */
    AMD_RING_MEC  = 2,   /* MEC HQD compute pipe 0 queue 0 */
} amd_ring_id_t;

/* Return codes used by all ring and fence functions. */
#define AMD_RING_OK       0
#define AMD_RING_EFULL   -1   /* ring full, caller must retry later */
#define AMD_RING_ETIMEOUT -2  /* fence or vblank wait timed out */
#define AMD_RING_ENODEV  -3   /* ring not initialised */

/* Opaque ring state (defined in amd_gpu.h, included by gpu_x86.c). */
struct amd_ring;

/* Submit one PM4/SDMA dword packet to ring `id`.
 * `dw` points to `count` dwords.  count must be <= 256.
 * Advances the write pointer; hardware picks it up asynchronously.
 * Thread-safety: single-threaded bare-metal; no locking. */
int  ck_gpu_ring_submit(amd_ring_id_t id, const uint32_t *dw, unsigned count);

/* Wait until the fence value written to *fence_va equals `seq`.
 * `timeout_ms` is TSC-bounded; 0 means check once.
 * Returns AMD_RING_OK, AMD_RING_ETIMEOUT. */
int  ck_gpu_fence_wait(const volatile uint32_t *fence_va, uint32_t seq, unsigned timeout_ms);

/* Page flip: schedule the scan-out to switch to the back buffer at the next
 * vblank.  Waits for the flip to complete (at most 50 ms for a 60 Hz panel).
 * Returns AMD_RING_OK or AMD_RING_ETIMEOUT. */
int  ck_gpu_flip(void);

/* Return the CPU pointer to the current back buffer (where renderers draw).
 * NULL if the GPU driver is not initialised. */
volatile uint32_t *ck_gpu_backbuffer(void);

/* Return the back buffer physical address in VRAM (MC space).
 * 0 if not initialised. */
uint64_t ck_gpu_backbuffer_pa(void);

/* VRAM size in bytes as read from MC_VM_FB_LOCATION. 0 if not read yet. */
uint64_t ck_gpu_vram_size(void);
