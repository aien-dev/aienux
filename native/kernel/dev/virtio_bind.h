/* virtio_bind.h -- kernel glue that puts a virtio 1.x PCI function behind
 * the polled core (virtio_q.h): capability parse, the four register
 * windows mapped, one DMA window allocated and confined to the function
 * through the SMMU (ck_dma_confine; no confinement, no bus master), and the
 * core brought up with ACCESS_PLATFORM required. Used by the screen
 * (virtio_gpu.c) and the speakers (virtio_snd.c). */
#ifndef AIENOS_CK_VIRTIO_BIND_H
#define AIENOS_CK_VIRTIO_BIND_H
#include "pci.h"
#include "virtio_q.h"

typedef struct {
    const char *name; /* for the report lines: "virtio_gpu" */
    const pci_func *pf;
    volatile uint8_t *win[4];
    volatile uint8_t *bar_va[PCI_MAX_BARS];
    uint8_t *mem; /* the DMA window: rings first, then the caller's part */
    uint64_t phys;
    size_t len, rings;
    uint32_t stream_id;
    vq_dev dev;
} vq_bind;

/* Finds 1af4:device, then sets b up with nq queues and extra bytes of DMA
 * memory after the rings (b->mem + b->rings, bus b->phys + b->rings). The
 * device is left before DRIVER_OK (call vq_ready). 0, or < 0 after printing
 * "<name>: ..." with the reason. */
int vq_bind_open(vq_bind *b, const char *name, const pci_system *pci, uint16_t device, unsigned nq, size_t extra);
#endif
