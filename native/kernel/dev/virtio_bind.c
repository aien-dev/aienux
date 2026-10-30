/* virtio_bind.c -- a virtio 1.x PCI function behind virtio_q (virtio_bind.h). */
#include "virtio_bind.h"
#include "ck.h"
#include <string.h>

/* The core's register hooks: ctx is the vq_bind. */
static uint32_t o_read(void *ctx, vnet_window w, uint32_t off, unsigned width)
{
    volatile uint8_t *p = ((vq_bind *)ctx)->win[w] + off;
    if (width == 1)
        return *p;
    if (width == 2)
        return *(volatile uint16_t *)p;
    return *(volatile uint32_t *)p;
}
static void o_write(void *ctx, vnet_window w, uint32_t off, unsigned width, uint32_t v)
{
    volatile uint8_t *p = ((vq_bind *)ctx)->win[w] + off;
    if (width == 1)
        *p = (uint8_t)v;
    else if (width == 2)
        *(volatile uint16_t *)p = (uint16_t)v;
    else
        *(volatile uint32_t *)p = v;
    ck_mb();
}
static void o_notify(void *ctx, uint32_t off, uint16_t q)
{
    ck_mb(); /* ring writes visible before the doorbell */
    *(volatile uint16_t *)(((vq_bind *)ctx)->win[VNET_WIN_NOTIFY] + off) = q;
}

static volatile uint8_t *map_window(vq_bind *b, const virtio_pci_region *r)
{
    if (!r->present || r->bar >= PCI_MAX_BARS)
        return 0;
    const pci_bar *bar = &b->pf->bar[r->bar];
    if (bar->io || bar->addr == 0 || bar->size == 0 || (uint64_t)r->offset + r->length > bar->size)
        return 0;
    if (!b->bar_va[r->bar])
        b->bar_va[r->bar] = (volatile uint8_t *)ck_mmio_try_map(bar->addr, (size_t)bar->size);
    return b->bar_va[r->bar] ? b->bar_va[r->bar] + r->offset : 0;
}

int vq_bind_open(vq_bind *b, const char *name, const pci_system *pci, uint16_t device, unsigned nq, size_t extra)
{
    memset(b, 0, sizeof *b);
    b->name = name;
    const pci_func *f = pci_find_id(pci, 0x1af4u, device);
    if (!f) {
        ck_printf("%s: none (no 1af4:%04x function)\n", name, device);
        return -1;
    }
    b->pf = f;
    static uint8_t cfg_image[256];
    for (uint32_t off = 0; off < sizeof cfg_image; off += 4) {
        uint32_t v = pci_r32(f->cfg, off);
        memcpy(cfg_image + off, &v, 4);
    }
    virtio_pci_caps caps;
    virtio_pci_err pe = virtio_pci_parse_caps(cfg_image, sizeof cfg_image, &caps);
    if (pe != VIRTIO_PCI_OK) {
        ck_printf("%s: %02x:%02x.%u not used (capabilities: %s)\n", name, f->bus, f->dev, f->fn,
                  virtio_pci_err_name(pe));
        return -2;
    }
    const virtio_pci_region *r[4] = {&caps.common_cfg, &caps.notify_cfg, &caps.isr_cfg, &caps.device_cfg};
    for (int i = 0; i < 4; i++)
        if (!(b->win[i] = map_window(b, r[i]))) {
            ck_printf("%s: not used (capability window %d outside its BAR)\n", name, i);
            return -3;
        }
    b->rings = vq_mem_bytes(nq);
    b->len = (b->rings + extra + 0xfffu) & ~(size_t)0xfffu;
    if (!(b->mem = ck_dma_alloc(b->len, 4096, &b->phys))) {
        ck_printf("%s: not used (DMA allocation of %zu KiB failed)\n", name, b->len >> 10);
        return -4;
    }
    struct ck_dma_confinement cf;
    uint32_t rid = (uint32_t)f->bus << 8 | (uint32_t)f->dev << 3 | f->fn;
    int src = ck_dma_confine(f->segment, rid, b->phys, b->len, &cf);
    if (src) {
        ck_printf("dma_gate: %s denied (rc=%d), bus master stays off\n", name, src);
        return -5;
    }
    b->stream_id = cf.stream_id;
    pci_enable(f, 1);
    vnet_ops ops = {b, o_read, o_write, o_notify};
    int e = vq_init(&b->dev, &ops, &caps, b->mem, b->phys, b->rings, nq, VQ_INIT_REQUIRE_ACCESS_PLATFORM);
    if (e) {
        ck_printf("%s: init FAIL (%s)\n", name, vq_err_name(e));
        pci_bus_master_off(f);
        return -6;
    }
    ck_printf("dma_gate: %s granted (Confined) %02x:%02x.%u stream 0x%x window %zu KiB\n", name, f->bus, f->dev,
              f->fn, cf.stream_id, b->len >> 10);
    return 0;
}
