/* boot_screen.h -- the aarch64 kernel plays the boot sequence (ui/boot_show.h)
 * once, after the devices stage, when built with CK_BOOT_SHOW=1. */
#ifndef AIENOS_CK_BOOT_SCREEN_H
#define AIENOS_CK_BOOT_SCREEN_H
/* Picks the screen (the VM's virtio-gpu first: in aienos-run that is the
 * window; else the GOP framebuffer from the handoff), the speakers (the VM's
 * virtio-sound, else silent), plays the 15.2 s night on the boot core and
 * prints its receipt line. Returns when the night is over, or at once with
 * a "boot-show: skipped (...)" line. */
void ck_boot_screen_run(void);
#endif
