/* virtio_gpu.h -- the virtio-gpu 2D screen (virtio 1.x section 5.7), polled,
 * for the boot sequence in a VM (QEMU virtio-gpu-pci / virtio-vga, the
 * aienos-run window). One resource the size of scanout 0, backed by guest
 * memory the caller draws into; a frame is shown with TRANSFER_TO_HOST_2D
 * and RESOURCE_FLUSH. Command layouts follow the virtio spec (checked
 * against the Linux UAPI copy, linux/virtio_gpu.h, in the host test).
 * The command builders are pure (host tested in dev/tests/virtio_gpu_test.c);
 * the device goes through virtio_q.h. */
#ifndef AIENOS_CK_VIRTIO_GPU_H
#define AIENOS_CK_VIRTIO_GPU_H
#include <stddef.h>
#include <stdint.h>
#include "virtio_q.h"

#define VGPU_CMD_GET_DISPLAY_INFO 0x0100u
#define VGPU_CMD_RESOURCE_CREATE_2D 0x0101u
#define VGPU_CMD_SET_SCANOUT 0x0103u
#define VGPU_CMD_RESOURCE_FLUSH 0x0104u
#define VGPU_CMD_TRANSFER_TO_HOST_2D 0x0105u
#define VGPU_CMD_RESOURCE_ATTACH_BACKING 0x0106u
#define VGPU_RESP_OK_NODATA 0x1100u
#define VGPU_RESP_OK_DISPLAY_INFO 0x1101u
#define VGPU_FORMAT_B8G8R8X8_UNORM 2u /* bytes B, G, R, X: the GOP BGRX format 1 */
#define VGPU_MAX_SCANOUTS 16u
#define VGPU_HDR_BYTES 24u
#define VGPU_DISPLAY_INFO_BYTES (VGPU_HDR_BYTES + VGPU_MAX_SCANOUTS * 24u)
#define VGPU_RESOURCE 1u

/* Command builders: write the command into out (cap bytes) and return its
 * length, 0 if cap is short. All fields little-endian. */
size_t vgpu_cmd_display_info(uint8_t *out, size_t cap);
size_t vgpu_cmd_create_2d(uint8_t *out, size_t cap, uint32_t res, uint32_t fmt, uint32_t w, uint32_t h);
size_t vgpu_cmd_attach_backing(uint8_t *out, size_t cap, uint32_t res, uint64_t addr, uint32_t len);
size_t vgpu_cmd_set_scanout(uint8_t *out, size_t cap, uint32_t scanout, uint32_t res, uint32_t w, uint32_t h);
size_t vgpu_cmd_transfer_2d(uint8_t *out, size_t cap, uint32_t res, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                            uint64_t offset);
size_t vgpu_cmd_flush(uint8_t *out, size_t cap, uint32_t res, uint32_t x, uint32_t y, uint32_t w, uint32_t h);
/* The response type of a reply (0 if short). */
uint32_t vgpu_resp_type(const uint8_t *resp, size_t len);
/* Scanout 0's size from a display-info reply; 0 if the reply is not one or
 * scanout 0 is disabled or empty. */
int vgpu_display0(const uint8_t *resp, size_t len, uint32_t *w, uint32_t *h);

#ifndef CK_HOSTED
#include "pci.h"
/* Kernel side (dev/virtio_gpu_bind.c). Finds a virtio-gpu PCI function,
 * confines its DMA, brings it up and makes a w x h BGRX screen whose pixels
 * are *fb (pitch = w). 0 and a line "virtio_gpu: ..." on success; < 0 with a
 * line saying why not. At most max_w x max_h (the drawing memory budget). */
int ck_vgpu_open(const pci_system *pci, uint32_t max_w, uint32_t max_h, volatile uint32_t **fb,
                 uint32_t *w, uint32_t *h);
/* Shows the whole picture (transfer + flush); the boot show's present hook. */
void ck_vgpu_present(void *ctx);
#endif
#endif
