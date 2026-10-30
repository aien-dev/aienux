/* boot_x86.c -- the boot sequence on the x86 kernel (ui/boot_show.h): the
 * night on the GOP framebuffer (in a VM, QEMU virtio-vga's linear
 * framebuffer, which is the aienos-run window), its chirp on the VM's
 * virtio-sound when there is one (dev/virtio_snd.c, the same driver as the
 * aarch64 kernel), timed by the TSC. Built when CK_BOOT_SHOW=1.
 * DMA: this kernel has no IOMMU support yet (VT-d is a later cut), so the
 * sound device's DMA is not confined; it is the only device writing to
 * memory here (four 100 ms sample buffers and two command pages, read by the
 * device, plus 8-byte status words it writes), and it is reset after the
 * chirp. */
#include "x86.h"
#include "../core/ck_internal.h"
#include "../dev/virtio_snd.h"
#include "../mm/frames.h"
#include "../ui/allen_ui.h"
#include "../ui/boot_show.h"
#include "aienos_virtio_pci.h"
#include "ck.h"

static uint64_t hz;
static uint64_t counter(void) { return ck_rdtsc(); }

static struct {
    volatile uint8_t *win[4];
} vs;

static uint32_t o_read(void *ctx, vnet_window w, uint32_t off, unsigned width)
{
    (void)ctx;
    volatile uint8_t *p = vs.win[w] + off;
    if (width == 1)
        return *p;
    if (width == 2)
        return *(volatile uint16_t *)p;
    return *(volatile uint32_t *)p;
}
static void o_write(void *ctx, vnet_window w, uint32_t off, unsigned width, uint32_t v)
{
    (void)ctx;
    volatile uint8_t *p = vs.win[w] + off;
    if (width == 1)
        *p = (uint8_t)v;
    else if (width == 2)
        *(volatile uint16_t *)p = (uint16_t)v;
    else
        *(volatile uint32_t *)p = v;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}
static void o_notify(void *ctx, uint32_t off, uint16_t q)
{
    (void)ctx;
    __atomic_thread_fence(__ATOMIC_SEQ_CST); /* ring writes visible before the doorbell */
    *(volatile uint16_t *)(vs.win[VNET_WIN_NOTIFY] + off) = q;
}

/* The BAR's address (64-bit aware); 0 for an I/O or unassigned BAR. */
static uint64_t bar_addr(volatile uint8_t *cfg, unsigned bar)
{
    if (bar > 5)
        return 0;
    uint32_t lo = *(volatile uint32_t *)(cfg + 0x10 + 4 * bar);
    if (lo & 1)
        return 0;
    uint64_t a = lo & ~0xfull;
    if ((lo & 6) == 4 && bar < 5)
        a |= (uint64_t)*(volatile uint32_t *)(cfg + 0x14 + 4 * bar) << 32;
    return a;
}

static vq_dev snd_dev;

/* virtio-sound (1af4:1059) behind virtio_q: 0 when the stream is ready. */
static int sound_open(struct ck_frames *fr)
{
    uint8_t bdf[3];
    volatile uint8_t *cfg = ck_x86_pci_find(0x1af4u, 0x1059u, bdf);
    if (!cfg) {
        ck_puts("virtio_snd: none (no 1af4:1059 function)\n");
        return -1;
    }
    static uint8_t img[256];
    for (unsigned off = 0; off < sizeof img; off += 4) {
        uint32_t v = *(volatile uint32_t *)(cfg + off);
        memcpy(img + off, &v, 4);
    }
    virtio_pci_caps caps;
    virtio_pci_err pe = virtio_pci_parse_caps(img, sizeof img, &caps);
    if (pe != VIRTIO_PCI_OK) {
        ck_printf("virtio_snd: %02x:%02x.%u not used (capabilities: %s)\n", bdf[0], bdf[1], bdf[2],
                  virtio_pci_err_name(pe));
        return -2;
    }
    const virtio_pci_region *r[4] = {&caps.common_cfg, &caps.notify_cfg, &caps.isr_cfg, &caps.device_cfg};
    for (int i = 0; i < 4; i++) {
        uint64_t a = r[i]->present ? bar_addr(cfg, r[i]->bar) : 0;
        if (!a || ck_x86_map_mmio(a + r[i]->offset, r[i]->length)) {
            ck_printf("virtio_snd: not used (capability window %d not mappable)\n", i);
            return -3;
        }
        vs.win[i] = (volatile uint8_t *)(uintptr_t)(a + r[i]->offset);
    }
    size_t len = vq_mem_bytes(3) + VSND_EXTRA_BYTES;
    uint64_t pa;
    if (ck_frames_alloc(fr, (len + 4095u) / 4096u, 4096u, &pa)) {
        ck_puts("virtio_snd: not used (no memory for its buffers)\n");
        return -4;
    }
    uint8_t *mem = (uint8_t *)(uintptr_t)pa;
    memset(mem, 0, len);
    *(volatile uint16_t *)(cfg + 4) |= 0x6; /* memory decode + bus master */
    vnet_ops ops = {0, o_read, o_write, o_notify};
    int e = vq_init(&snd_dev, &ops, &caps, mem, pa, vq_mem_bytes(3), 3, 0);
    if (e) {
        ck_printf("virtio_snd: init FAIL (%s)\n", vq_err_name(e));
        *(volatile uint16_t *)(cfg + 4) &= (uint16_t)~0x4;
        return -5;
    }
    ck_printf("virtio_snd: %02x:%02x.%u bus master on, DMA not confined (no IOMMU in this kernel yet)\n", bdf[0],
              bdf[1], bdf[2]);
    return ck_vsnd_attach(&snd_dev, mem + vq_mem_bytes(3), pa + vq_mem_bytes(3));
}

#define BOOT_X86_MAX_W 1920u
#define BOOT_X86_MAX_H 1080u

void ck_x86_boot_show(volatile uint32_t *fb, uint32_t w, uint32_t h, uint32_t pitch, uint32_t format,
                      struct ck_frames *fr, uint64_t tsc_hz)
{
    if (!fb || !tsc_hz) {
        ck_puts("boot-show: skipped (no screen or no clock)\n");
        return;
    }
    hz = tsc_hz;
    w = w < BOOT_X86_MAX_W ? w : BOOT_X86_MAX_W;
    h = h < BOOT_X86_MAX_H ? h : BOOT_X86_MAX_H;
    size_t need = ck_allen_ui_mem_bytes(w, h);
    uint64_t pa;
    if (ck_frames_alloc(fr, (need + 4095u) / 4096u, 4096u, &pa)) {
        ck_printf("boot-show: skipped (no %zu KiB of drawing memory for %ux%u)\n", need >> 10, w, h);
        return;
    }
    int snd = sound_open(fr);
    ck_x86_console_fb_pause();
    if (ck_boot_show_start(fb, w, h, pitch, format, (void *)(uintptr_t)pa, need, hz, counter(), counter, 0, 0)) {
        ck_x86_console_fb_resume();
        ck_printf("boot-show: skipped (screen %ux%u pitch %u format %u refused)\n", w, h, pitch, format);
        ck_frames_free(fr, pa, (need + 4095u) / 4096u);
        return;
    }
    ck_printf("boot-show: the night starts on %ux%u (gop)\n", w, h);
    int playing = 1, sounding = snd == 0;
    while (playing || sounding) {
        uint64_t now = counter();
        if (!playing && ck_boot_show_ms(now) > CK_BOOT_SHOW_MS + 2000u) {
            ck_puts("sound: the speakers stopped answering; chirp abandoned 2 s after the night\n");
            break;
        }
        if (playing)
            playing = ck_boot_show_tick(now);
        if (sounding)
            sounding = ck_vsnd_step(ck_boot_show_ms(now), ck_boot_show_audio_frame(now));
    }
    if (snd == 0)
        vq_reset(&snd_dev); /* the device stops touching memory before the buffers go back */
    ck_x86_console_fb_resume();
    struct ck_boot_show_stats st;
    ck_boot_show_stats(&st);
    ck_printf(CK_BOOT_SHOW_LINE, CK_BOOT_SHOW_ARGS(st, w, h, "gop"));
    if (snd)
        ck_puts("sound: none for the chirp (no virtio-sound); the night played silent\n");
    ck_frames_free(fr, pa, (need + 4095u) / 4096u);
}
