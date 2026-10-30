/* boot_screen.c -- the boot sequence on the aarch64 kernel (boot_screen.h). */
#include "boot_screen.h"
#include "devices.h"
#include "../ui/allen_ui.h"
#include "../ui/boot_show.h"
#include "arch.h"
#include "ck.h"
#include "ck_internal.h"
#include "virtio_gpu.h"
#include "virtio_snd.h"
#include "../../boot/handoff.h"

/* The largest picture the night is drawn at: its drawing memory comes from
 * the kernel heap (17 MiB at this size), the VM picture from the DMA pool. */
#define BOOT_SCREEN_MAX_W 1920u
#define BOOT_SCREEN_MAX_H 1080u

static uint64_t counter(void) { return ck_rd(cntvct_el0); }

void ck_boot_screen_run(void)
{
    const pci_system *pci = ck_dev_pci();
    volatile uint32_t *fb = 0;
    uint32_t w = 0, h = 0, pitch = 0, format = 1;
    ck_boot_present_fn present = 0;
    const char *how = "gop";
    if (pci && ck_vgpu_open(pci, BOOT_SCREEN_MAX_W, BOOT_SCREEN_MAX_H, &fb, &w, &h) == 0) {
        pitch = w, format = 1, present = ck_vgpu_present, how = "virtio-gpu";
    } else {
        const struct ck_handoff *hd = ck_handoff_get();
        const struct ck_mm_report *mm = ck_mm_report();
        if (!hd || hd->fb_status != CK_HANDOFF_FB_PRESENT || mm->fb_map != CK_MM_FB_MAPPED) {
            ck_puts("boot-show: skipped (no screen: no virtio-gpu, no linear GOP framebuffer)\n");
            return;
        }
        fb = (volatile uint32_t *)(uintptr_t)hd->fb_base;
        w = hd->fb_width < BOOT_SCREEN_MAX_W ? hd->fb_width : BOOT_SCREEN_MAX_W;
        h = hd->fb_height < BOOT_SCREEN_MAX_H ? hd->fb_height : BOOT_SCREEN_MAX_H;
        pitch = hd->fb_pitch, format = hd->fb_format;
    }
    size_t need = ck_allen_ui_mem_bytes(w, h);
    void *mem = ck_alloc(need);
    if (!mem) {
        ck_printf("boot-show: skipped (no %zu KiB of drawing memory for %ux%u)\n", need >> 10, w, h);
        return;
    }
    int snd = pci ? ck_vsnd_open(pci) : -1;
    uint64_t hz = ck_rd(cntfrq_el0);
    ck_console_fb_pause();
    if (ck_boot_show_start(fb, w, h, pitch, format, mem, need, hz, counter(), counter, present, 0)) {
        ck_console_fb_resume();
        ck_printf("boot-show: skipped (screen %ux%u pitch %u format %u refused)\n", w, h, pitch, format);
        ck_free(mem);
        return;
    }
    ck_printf("boot-show: the night starts on %ux%u (%s)\n", w, h, how);
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
    ck_console_fb_resume();
    struct ck_boot_show_stats st;
    ck_boot_show_stats(&st);
    ck_printf(CK_BOOT_SHOW_LINE, CK_BOOT_SHOW_ARGS(st, w, h, how));
    if (snd)
        ck_puts("sound: none for the chirp (no virtio-sound); the night played silent\n");
    ck_free(mem);
}
