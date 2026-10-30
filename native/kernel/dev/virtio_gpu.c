/* virtio_gpu.c -- virtio-gpu 2D screen (virtio_gpu.h): pure command
 * builders, and (kernel build) the PCI binding behind ck_vgpu_open. */
#include "virtio_gpu.h"
#include <string.h>

static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void put64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }
static uint32_t get32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

/* struct virtio_gpu_ctrl_hdr: type, flags, fence_id, ctx_id, ring_idx, padding[3] */
static size_t hdr(uint8_t *out, size_t cap, size_t len, uint32_t type)
{
    if (!out || cap < len)
        return 0;
    memset(out, 0, len);
    put32(out, type);
    return len;
}

/* struct virtio_gpu_rect: x, y, width, height */
static void rect(uint8_t *p, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    put32(p, x), put32(p + 4, y), put32(p + 8, w), put32(p + 12, h);
}

size_t vgpu_cmd_display_info(uint8_t *out, size_t cap) { return hdr(out, cap, VGPU_HDR_BYTES, VGPU_CMD_GET_DISPLAY_INFO); }

/* struct virtio_gpu_resource_create_2d: hdr, resource_id, format, width, height */
size_t vgpu_cmd_create_2d(uint8_t *out, size_t cap, uint32_t res, uint32_t fmt, uint32_t w, uint32_t h)
{
    size_t n = hdr(out, cap, VGPU_HDR_BYTES + 16u, VGPU_CMD_RESOURCE_CREATE_2D);
    if (n)
        put32(out + 24, res), put32(out + 28, fmt), put32(out + 32, w), put32(out + 36, h);
    return n;
}

/* struct virtio_gpu_resource_attach_backing: hdr, resource_id, nr_entries;
 * then one struct virtio_gpu_mem_entry: addr, length, padding */
size_t vgpu_cmd_attach_backing(uint8_t *out, size_t cap, uint32_t res, uint64_t addr, uint32_t len)
{
    size_t n = hdr(out, cap, VGPU_HDR_BYTES + 8u + 16u, VGPU_CMD_RESOURCE_ATTACH_BACKING);
    if (n)
        put32(out + 24, res), put32(out + 28, 1), put64(out + 32, addr), put32(out + 40, len);
    return n;
}

/* struct virtio_gpu_set_scanout: hdr, rect, scanout_id, resource_id */
size_t vgpu_cmd_set_scanout(uint8_t *out, size_t cap, uint32_t scanout, uint32_t res, uint32_t w, uint32_t h)
{
    size_t n = hdr(out, cap, VGPU_HDR_BYTES + 16u + 8u, VGPU_CMD_SET_SCANOUT);
    if (n)
        rect(out + 24, 0, 0, w, h), put32(out + 40, scanout), put32(out + 44, res);
    return n;
}

/* struct virtio_gpu_transfer_to_host_2d: hdr, rect, offset (64), resource_id, padding */
size_t vgpu_cmd_transfer_2d(uint8_t *out, size_t cap, uint32_t res, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                            uint64_t offset)
{
    size_t n = hdr(out, cap, VGPU_HDR_BYTES + 16u + 16u, VGPU_CMD_TRANSFER_TO_HOST_2D);
    if (n)
        rect(out + 24, x, y, w, h), put64(out + 40, offset), put32(out + 48, res);
    return n;
}

/* struct virtio_gpu_resource_flush: hdr, rect, resource_id, padding */
size_t vgpu_cmd_flush(uint8_t *out, size_t cap, uint32_t res, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    size_t n = hdr(out, cap, VGPU_HDR_BYTES + 16u + 8u, VGPU_CMD_RESOURCE_FLUSH);
    if (n)
        rect(out + 24, x, y, w, h), put32(out + 40, res);
    return n;
}

uint32_t vgpu_resp_type(const uint8_t *resp, size_t len) { return resp && len >= VGPU_HDR_BYTES ? get32(resp) : 0; }

/* struct virtio_gpu_resp_display_info: hdr, then pmodes[16] of {rect, enabled, flags} */
int vgpu_display0(const uint8_t *resp, size_t len, uint32_t *w, uint32_t *h)
{
    if (len < VGPU_DISPLAY_INFO_BYTES || vgpu_resp_type(resp, len) != VGPU_RESP_OK_DISPLAY_INFO)
        return 0;
    const uint8_t *m = resp + VGPU_HDR_BYTES;
    uint32_t mw = get32(m + 8), mh = get32(m + 12), en = get32(m + 16);
    if (!en || !mw || !mh)
        return 0;
    *w = mw, *h = mh;
    return 1;
}

#ifndef CK_HOSTED
#include "ck.h"
#include "virtio_bind.h"

#define VGPU_SPINS 200000000u

static struct {
    vq_bind b;
    uint8_t *cmd, *resp; /* one page each after the rings */
    uint64_t cmd_bus, resp_bus;
    uint32_t w, h;
    int on;
} g;

/* One command, one reply: 0 when the reply's type is want. */
static int command(size_t len, uint32_t want, uint32_t *got)
{
    vq_buf b[2] = {{g.cmd_bus, (uint32_t)len, 0}, {g.resp_bus, 4096u, 1}};
    uint32_t wrote = 0;
    memset(g.resp, 0, VGPU_HDR_BYTES);
    int e = vq_call(&g.b.dev, 0, b, 2, VGPU_SPINS, &wrote);
    *got = e ? 0 : vgpu_resp_type(g.resp, wrote);
    return e ? e : (*got == want ? 0 : -1);
}

int ck_vgpu_open(const pci_system *pci, uint32_t max_w, uint32_t max_h, volatile uint32_t **fb, uint32_t *w,
                 uint32_t *h)
{
    /* 0x1050 = 0x1040 + device id 16 (virtio 1.x 4.1.2.1): virtio-gpu-pci and virtio-vga.
     * DMA window: rings, a command page, a reply page, then the picture. */
    if (vq_bind_open(&g.b, "virtio_gpu", pci, 0x1050u, 1, 2u * 4096u + (size_t)max_w * max_h * 4u))
        return -1;
    vq_ready(&g.b.dev);
    g.cmd = g.b.mem + g.b.rings, g.cmd_bus = g.b.phys + g.b.rings;
    g.resp = g.cmd + 4096u, g.resp_bus = g.cmd_bus + 4096u;
    uint8_t *px = g.resp + 4096u;
    uint64_t px_bus = g.resp_bus + 4096u;

    uint32_t got = 0, dw = 0, dh = 0;
    if (command(vgpu_cmd_display_info(g.cmd, 4096), VGPU_RESP_OK_DISPLAY_INFO, &got) ||
        !vgpu_display0(g.resp, VGPU_DISPLAY_INFO_BYTES, &dw, &dh)) {
        ck_printf("virtio_gpu: no display (reply 0x%x)\n", got);
        return -7;
    }
    g.w = dw < max_w ? dw : max_w, g.h = dh < max_h ? dh : max_h;
    uint32_t bytes = g.w * g.h * 4u;
    if (command(vgpu_cmd_create_2d(g.cmd, 4096, VGPU_RESOURCE, VGPU_FORMAT_B8G8R8X8_UNORM, g.w, g.h),
                VGPU_RESP_OK_NODATA, &got) ||
        command(vgpu_cmd_attach_backing(g.cmd, 4096, VGPU_RESOURCE, px_bus, bytes), VGPU_RESP_OK_NODATA, &got) ||
        command(vgpu_cmd_set_scanout(g.cmd, 4096, 0, VGPU_RESOURCE, g.w, g.h), VGPU_RESP_OK_NODATA, &got)) {
        ck_printf("virtio_gpu: screen setup refused (reply 0x%x)\n", got);
        return -8;
    }
    g.on = 1;
    ck_printf("virtio_gpu: screen %ux%u (display %ux%u) bgrx, picture %u KiB in the confined window\n", g.w, g.h,
              dw, dh, bytes >> 10);
    *fb = (volatile uint32_t *)px, *w = g.w, *h = g.h;
    return 0;
}

void ck_vgpu_present(void *ctx)
{
    (void)ctx;
    uint32_t got;
    if (!g.on)
        return;
    if (command(vgpu_cmd_transfer_2d(g.cmd, 4096, VGPU_RESOURCE, 0, 0, g.w, g.h, 0), VGPU_RESP_OK_NODATA, &got) ||
        command(vgpu_cmd_flush(g.cmd, 4096, VGPU_RESOURCE, 0, 0, g.w, g.h), VGPU_RESP_OK_NODATA, &got)) {
        ck_printf("virtio_gpu: present refused (reply 0x%x); screen off\n", got);
        g.on = 0;
    }
}
#endif
