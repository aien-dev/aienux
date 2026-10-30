/* virtio_q.h -- a small polled virtio 1.x device core for request/response
 * devices (virtio-gpu, virtio-sound): reset and feature handshake, split
 * virtqueues, and chained requests (a driver-written part, then a
 * device-written part). Written from the virtio 1.x specification sections
 * 2.6 (split virtqueues), 3.1 (device initialization) and 4.1.4 (PCI
 * capabilities); register access goes through the same vnet_ops windows as
 * the virtio-net core (net/aienos_virtio_net.h), whose common-configuration
 * offsets it reuses. No heap, no clock, no interrupts. All rings live in one
 * caller region (mem, len) the device sees at bus address dma; request
 * buffers are bus addresses the caller owns and has checked. The device is
 * untrusted: a used id out of range or not outstanding, or a used index
 * running ahead of what was posted, marks it broken (sticky). Features:
 * VERSION_1 required, ACCESS_PLATFORM accepted when offered (and required
 * with VQ_INIT_REQUIRE_ACCESS_PLATFORM), every other feature declined. */
#ifndef AIENOS_CK_VIRTIO_Q_H
#define AIENOS_CK_VIRTIO_Q_H
#include <stddef.h>
#include <stdint.h>
#include "aienos_virtio_net.h"

#define VQ_MAX_QUEUES 4u
#define VQ_SIZE 16u /* descriptors a queue uses (the device may offer more) */
#define VQ_INIT_REQUIRE_ACCESS_PLATFORM 1u

enum {
    VQ_OK = 0,
    VQ_E_ARG = 1,
    VQ_E_MEM = 2,
    VQ_E_RESET = 3,
    VQ_E_NO_VERSION_1 = 4,
    VQ_E_FEATURES = 5,
    VQ_E_QUEUES = 6,
    VQ_E_FULL = 7,
    VQ_E_DEVICE = 8,  /* hostile or broken completion; the device is now broken */
    VQ_E_TIMEOUT = 9, /* no completion within the spin bound */
    VQ_E_NO_ACCESS_PLATFORM = 10,
};

typedef struct {
    uint64_t addr; /* bus address */
    uint32_t len;
    uint32_t device_writes; /* 1: the device writes this part (a response) */
} vq_buf;

typedef struct {
    uint16_t size, avail_idx, last_used, in_flight;
    uint32_t notify_off;
    size_t desc_off, avail_off, used_off;
    uint8_t posted[VQ_SIZE]; /* head ids outstanding */
    uint16_t free_next;      /* descriptors are handed out in rounds of a chain */
} vq_queue;

typedef struct {
    vnet_ops ops;
    uint8_t *mem;
    uint64_t dma;
    size_t len;
    unsigned nq;
    uint32_t notify_mult;
    uint64_t features;
    int broken;
    vq_queue q[VQ_MAX_QUEUES];
} vq_dev;

/* Bytes of ring memory nq queues need (4 KiB aligned queues). */
size_t vq_mem_bytes(unsigned nq);
/* Resets the device and brings up queues 0..nq-1. mem must be 4 KiB aligned
 * and zeroed. The device stays without DRIVER_OK until vq_ready. */
int vq_init(vq_dev *d, const vnet_ops *ops, const virtio_pci_caps *caps, uint8_t *mem, uint64_t dma, size_t len,
            unsigned nq, unsigned flags);
/* Sets DRIVER_OK (the device may start using the queues). */
void vq_ready(vq_dev *d);
/* Posts bufs[0..n) as one chain on queue q and rings its doorbell; the
 * head id comes back in *head. VQ_E_FULL when the queue has no room. */
int vq_post(vq_dev *d, unsigned q, const vq_buf *bufs, unsigned n, uint16_t *head);
/* Takes one completion off queue q: VQ_OK with its head id and the bytes
 * the device wrote, VQ_E_FULL when there is none yet (nothing changes). */
int vq_reap(vq_dev *d, unsigned q, uint16_t *head, uint32_t *written);
/* vq_post, then spins (at most spins polls) for that chain to complete. */
int vq_call(vq_dev *d, unsigned q, const vq_buf *bufs, unsigned n, uint32_t spins, uint32_t *written);
/* Writes status 0 (device reset) and marks the core unusable. */
void vq_reset(vq_dev *d);
/* Reads the device-specific config (32-bit, after the generation settles). */
uint32_t vq_cfg32(vq_dev *d, uint32_t off);
const char *vq_err_name(int e);
#endif
