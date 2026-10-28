/* panel_x86.c -- the iMac's built-in display panel switched off for real while
 * Allen rests, and back on when he wakes (AIENOS-KERNEL-0, x86_64).
 *
 * Painting the screen black leaves the backlight lit. This file turns the
 * panel itself off through the display chip's own panel power sequencer, the
 * way the chip's firmware does at power-off: the backlight PWM duty first
 * (BL_PWM_CNTL), then the sequencer's target state to "off" (LVTMA_PWRSEQ_CNTL),
 * which drops the backlight enable pin (BLON), waits the panel's power-down
 * delay as the firmware programmed it (LVTMA_PWRSEQ_DELAY1/2) and then the
 * panel's digital power (DIGON). Waking is the reverse: target state "on"
 * (DIGON up, the panel's power-up delay, BLON up), then the PWM duty restored,
 * and only then the caller brings the picture back.
 *
 * Only on the chip it was written for: an AMD display controller (PCI vendor
 * 0x1002, class 0x0300) whose device id is a Polaris part (DCE 11.2, the
 * 2017 iMac's Radeon Pro 570/575/580 among them), whose VRAM aperture (BAR0)
 * holds the firmware's framebuffer, and whose 256 KiB register aperture
 * (BAR5) answers.
 * The captured iMac inventory has both AMD 1002:67df at 01:00.0 and Intel
 * 8086:5912, with GOP at 0xc0300000 (evidence/imac_boot0_2026-10-16).
 * PCI presence alone does not identify the panel owner: pci_x86.c selects
 * the controller whose BAR0 contains the GOP allocation; recheck it here.
 * Never use the Intel CPU or the mere presence of its GPU as that proof.
 * Register indices and bit masks are from Linux
 * drivers/gpu/drm/amd/include/asic_reg/dce/dce_11_2_d.h and
 * dce_11_2_sh_mask.h (MIT licensed, Copyright (C) 2016 Advanced Micro Devices,
 * Inc.), the MMIO byte offset being the index times 4 (amdgpu_device.c,
 * amdgpu_mm_rreg: registers below the 256 KiB aperture are read directly). The
 * sequence follows Linux's dce110_hwseq.c (dce110_edp_backlight_control then
 * dce110_edp_power_control, which the VBIOS command table turns into the same
 * target-state write) and dce_panel_cntl.c (dce_is_panel_powered_on,
 * dce_driver_set_backlight: lock BL_PWM_GRP1_REG_LOCK, write the duty, unlock,
 * wait for the update to land). Linux also keeps the panel off at least 500 ms
 * (eDP T12) before powering it on again; so does this file.
 *
 * Every write is read back and logged; bounded waits require the sequencer's
 * reported BLON/DIGON state to match, including firmware override mode.
 * Off returns 0 only after these checks, not merely after painting black.
 * A failed off remains eligible for a restore attempt, but further off
 * attempts are disabled. The rest policy in kmain_x86.c uses ACPI S5 when
 * off cannot be confirmed. These register checks are not optical evidence.
 * Built with CK_X86_PANEL_OFF=1; disabled or unknown hardware returns -1.
 * QEMU runs of this file qualify nothing physical. */
#include "../core/ck_internal.h"
#include "x86.h"

#ifndef CK_X86_PANEL_OFF
#define CK_X86_PANEL_OFF 0
#endif

/* dce_11_2_d.h: register indices (MMIO byte offset = index * 4) */
#define mmLVTMA_PWRSEQ_CNTL 0x481b
#define mmLVTMA_PWRSEQ_STATE 0x481c
#define mmLVTMA_PWRSEQ_REF_DIV 0x481d
#define mmLVTMA_PWRSEQ_DELAY1 0x481e
#define mmLVTMA_PWRSEQ_DELAY2 0x481f
#define mmBL_PWM_CNTL 0x4820
#define mmBL_PWM_PERIOD_CNTL 0x4822
#define mmBL_PWM_GRP1_REG_LOCK 0x4823
/* dce_11_2_sh_mask.h: bit masks */
#define PWRSEQ_EN 0x1u             /* LVTMA_PWRSEQ_CNTL__LVTMA_PWRSEQ_EN_MASK */
#define PWRSEQ_TARGET_STATE 0x10u  /* LVTMA_PWRSEQ_CNTL__LVTMA_PWRSEQ_TARGET_STATE_MASK */
#define PWRSEQ_DIGON 0x10000u      /* LVTMA_PWRSEQ_CNTL__LVTMA_DIGON_MASK */
#define PWRSEQ_DIGON_OVRD 0x20000u /* LVTMA_PWRSEQ_CNTL__LVTMA_DIGON_OVRD_MASK */
#define PWRSEQ_BLON 0x1000000u     /* LVTMA_PWRSEQ_CNTL__LVTMA_BLON_MASK */
#define PWRSEQ_BLON_OVRD 0x2000000u /* LVTMA_PWRSEQ_CNTL__LVTMA_BLON_OVRD_MASK */
#define STATE_TARGET_R 0x1u        /* LVTMA_PWRSEQ_STATE__LVTMA_PWRSEQ_TARGET_STATE_R_MASK */
#define STATE_DIGON 0x2u           /* LVTMA_PWRSEQ_STATE__LVTMA_PWRSEQ_DIGON_MASK */
#define STATE_BLON 0x8u            /* LVTMA_PWRSEQ_STATE__LVTMA_PWRSEQ_BLON_MASK */
#define STATE_DONE 0x10u           /* LVTMA_PWRSEQ_STATE__LVTMA_PWRSEQ_DONE_MASK */
#define BL_DUTY 0xffffu            /* BL_PWM_CNTL__BL_ACTIVE_INT_FRAC_CNT_MASK */
#define BL_PWM_EN 0x80000000u      /* BL_PWM_CNTL__BL_PWM_EN_MASK */
#define BL_LOCK 0x1u               /* BL_PWM_GRP1_REG_LOCK__BL_PWM_GRP1_REG_LOCK_MASK */
#define BL_UPDATE_PENDING 0x100u   /* BL_PWM_GRP1_REG_LOCK__BL_PWM_GRP1_REG_UPDATE_PENDING_MASK */
#define BL_IGNORE_MASTER_LOCK 0x80000000u /* BL_PWM_GRP1_REG_LOCK__BL_PWM_GRP1_IGNORE_MASTER_LOCK_EN_MASK */

#define REG_BYTES 0x40000u            /* the register aperture at BAR5: 256 KiB */
#define VRAM_BAR_BYTES (256ull << 20) /* the smallest VRAM aperture these parts expose */
#define SEQ_MS 3000u                  /* the sequencer gets this long to reach its target */
#define BL_UPDATE_MS 10u              /* the PWM duty update gets this long to land */
#define T_BL_OFF_MS 50u               /* override mode: backlight off to panel power off */
#define T_ON_MS 200u                  /* override mode: panel power on to backlight on */
#define T12_MS 500u                   /* the panel stays off at least this long (eDP T12) */


#if CK_X86_PANEL_OFF
static struct {
    volatile uint32_t *regs; /* BAR5, mapped uncached */
    uint64_t hz;
    int ready;    /* every guard passed: the panel can be switched */
    int ovrd;     /* 1: firmware left the override bits set, so they are driven directly */
    int off;      /* something was written towards "off" (a wake must undo it) */
    int broken;   /* a readback or a wait failed: no further "off" is attempted */
    int blpwm;    /* the backlight PWM is on, its duty is taken down and put back */
    uint32_t bl0; /* BL_PWM_CNTL as found awake */
    uint64_t off_t; /* TSC when the panel went off */
    const char *why; /* why the panel is left alone (ready == 0) */
} p;

static uint32_t cr32(volatile uint8_t *cfg, unsigned off) { return *(volatile uint32_t *)(cfg + off); }
static uint32_t rd(unsigned idx) { return p.regs[idx]; }

/* Writes v, reads back, logs both; -1 (and broken) when the bits in `mask` differ. */
static int wr(const char *when, const char *name, unsigned idx, uint32_t v, uint32_t mask)
{
    uint32_t before = rd(idx);
    p.regs[idx] = v;
    uint32_t after = rd(idx);
    ck_printf("screen: panel %s: %s 0x%08x -> 0x%08x\n", when, name, before, after);
    if ((after & mask) != (v & mask)) {
        ck_printf("screen: panel %s: %s did not take the write (wanted 0x%08x); the panel is left alone from here\n",
                  when, name, v);
        p.broken = 1;
        return -1;
    }
    return 0;
}

static void wait_ms(unsigned ms)
{
    uint64_t t0 = ck_rdtsc(), n = p.hz / 1000 * ms;
    while (ck_rdtsc() - t0 < n)
        ck_pause();
}

/* Waits until (reg & mask) == want, at most ms; -1 (and broken) when it runs out. */
static int wait_reg(const char *when, const char *name, unsigned idx, uint32_t mask, uint32_t want, unsigned ms)
{
    uint64_t t0 = ck_rdtsc(), n = p.hz / 1000 * ms;
    uint32_t s;
    do {
        s = rd(idx);
        if ((s & mask) == want) {
            ck_printf("screen: panel %s: %s 0x%08x after %llu ms\n", when, name, s,
                      (unsigned long long)((ck_rdtsc() - t0) * 1000 / p.hz));
            return 0;
        }
        ck_pause();
    } while (ck_rdtsc() - t0 < n);
    ck_printf("screen: panel %s: %s still 0x%08x after %u ms (wanted 0x%08x under 0x%08x); the panel is left alone "
              "from here\n", when, name, s, ms, want, mask);
    p.broken = 1;
    return -1;
}

/* The backlight PWM duty: lock the group, write, unlock, wait for the update
 * to land (dce_panel_cntl.c dce_driver_set_backlight), then read back: the
 * register is double buffered, so the readback counts only once the pending
 * bit has cleared. */
static int bl_duty(const char *when, uint32_t cntl)
{
    uint32_t before = rd(mmBL_PWM_CNTL), lock = rd(mmBL_PWM_GRP1_REG_LOCK);
    p.regs[mmBL_PWM_GRP1_REG_LOCK] = lock | BL_IGNORE_MASTER_LOCK | BL_LOCK;
    p.regs[mmBL_PWM_CNTL] = cntl;
    p.regs[mmBL_PWM_GRP1_REG_LOCK] = (rd(mmBL_PWM_GRP1_REG_LOCK) | BL_IGNORE_MASTER_LOCK) & ~BL_LOCK;
    if (wait_reg(when, "BL_PWM_GRP1_REG_LOCK", mmBL_PWM_GRP1_REG_LOCK, BL_UPDATE_PENDING, 0, BL_UPDATE_MS))
        return -1;
    uint32_t after = rd(mmBL_PWM_CNTL);
    ck_printf("screen: panel %s: BL_PWM_CNTL 0x%08x -> 0x%08x\n", when, before, after);
    if ((after & (BL_DUTY | BL_PWM_EN)) != (cntl & (BL_DUTY | BL_PWM_EN))) {
        ck_printf("screen: panel %s: BL_PWM_CNTL did not take the write (wanted 0x%08x); the panel is left alone from "
                  "here\n", when, cntl);
        p.broken = 1;
        return -1;
    }
    return 0;
}

static int polaris(uint16_t device)
{
    /* Polaris 10 (the iMac18,3: 67df rev c5): 67c0..67df; Polaris 11 and 21: 67e0..67ff; Polaris 12: 6980..699f */
    return (device >= 0x67c0 && device <= 0x67ff) || (device >= 0x6980 && device <= 0x699f);
}

void ck_x86_panel_start(volatile uint8_t *cfg, unsigned bus, unsigned dev, unsigned fn, uint16_t vendor,
                        uint16_t device, uint32_t cls, uint64_t fb_pa, uint64_t fb_bytes, uint64_t tsc_hz)
{
    static int seen;
    if (seen++ || p.ready)
        return;
    p.hz = tsc_hz;
    p.why = "no display chip on the PCI bus owns the framebuffer";
    if (!cfg) {
        ck_printf("screen: panel: %s; reversible panel off unavailable (rest uses ACPI S5)\n", p.why);
        return;
    }
    uint32_t lo0 = cr32(cfg, 0x10), hi0 = (lo0 & 6) == 4 ? cr32(cfg, 0x14) : 0;
    uint64_t bar0 = ((uint64_t)hi0 << 32 | lo0) & ~0xfull;
    uint32_t lo5 = cr32(cfg, 0x24);
    uint64_t bar5 = lo5 & ~0xfu;
    uint16_t cmd = (uint16_t)cr32(cfg, 4);
    ck_printf("pci: display %02x:%02x.%u id=%04x:%04x class=%06x bar0=0x%llx bar5=0x%llx mem_decode=%u\n", bus, dev,
              fn, vendor, device, cls, (unsigned long long)bar0, (unsigned long long)bar5, cmd & 2 ? 1u : 0u);
    const char *why = 0;
    if (vendor != 0x1002 || (cls >> 8) != 0x0300)
        why = "the display chip is not an AMD VGA controller";
    else if (!polaris(device))
        why = "the AMD chip is not a Polaris part (DCE 11.2), whose panel registers this kernel knows";
    else if ((lo0 & 1) || (lo0 & 6) != 4 || !bar0)
        why = "BAR0 is not a 64-bit memory aperture";
    else if (!fb_pa || fb_pa < bar0 || fb_pa + fb_bytes < fb_pa || fb_pa + fb_bytes > bar0 + VRAM_BAR_BYTES)
        why = "the framebuffer is not inside the chip's VRAM aperture (BAR0)";
    else if ((lo5 & 1) || (lo5 & 6) != 0 || !bar5)
        why = "BAR5 is not a 32-bit memory aperture (the registers)";
    else if (!(cmd & 2))
        why = "memory decode is off on the display chip";
    else if (ck_x86_map_mmio(bar5, REG_BYTES))
        why = "the register aperture could not be mapped";
    if (why) {
        p.why = why;
        ck_printf("screen: panel: %s; reversible panel off unavailable (rest uses ACPI S5)\n", p.why);
        return;
    }
    p.regs = (volatile uint32_t *)(uintptr_t)bar5;
    uint32_t cntl = rd(mmLVTMA_PWRSEQ_CNTL), state = rd(mmLVTMA_PWRSEQ_STATE), bl = rd(mmBL_PWM_CNTL);
    ck_printf("screen: panel: DCE 11.2 registers at 0x%llx: PWRSEQ_CNTL=0x%08x PWRSEQ_STATE=0x%08x REF_DIV=0x%08x "
              "DELAY1=0x%08x DELAY2=0x%08x BL_PWM_CNTL=0x%08x BL_PWM_PERIOD=0x%08x BL_LOCK=0x%08x\n",
              (unsigned long long)bar5, cntl, state, rd(mmLVTMA_PWRSEQ_REF_DIV), rd(mmLVTMA_PWRSEQ_DELAY1),
              rd(mmLVTMA_PWRSEQ_DELAY2), bl, rd(mmBL_PWM_PERIOD_CNTL), rd(mmBL_PWM_GRP1_REG_LOCK));
    if (cntl == 0xffffffffu || state == 0xffffffffu) {
        p.why = "the panel registers answer all ones (the chip is not awake, or not this register map)";
    } else if ((cntl & PWRSEQ_EN) && (cntl & PWRSEQ_TARGET_STATE) &&
               (state & (STATE_TARGET_R | STATE_DONE | STATE_BLON | STATE_DIGON)) ==
               (STATE_TARGET_R | STATE_DONE | STATE_BLON | STATE_DIGON)) {
        p.ovrd = 0;
        p.ready = 1;
    } else if ((cntl & (PWRSEQ_DIGON | PWRSEQ_DIGON_OVRD | PWRSEQ_BLON | PWRSEQ_BLON_OVRD)) ==
               (PWRSEQ_DIGON | PWRSEQ_DIGON_OVRD | PWRSEQ_BLON | PWRSEQ_BLON_OVRD) &&
               (state & (STATE_BLON | STATE_DIGON)) == (STATE_BLON | STATE_DIGON)) {
        p.ovrd = 1; /* the firmware drives the pins directly: so will this kernel */
        p.ready = 1;
    } else {
        p.why = "the panel power sequencer/override state does not confirm an awake panel";
    }
    if (!p.ready) {
        ck_printf("screen: panel: %s; reversible panel off unavailable (rest uses ACPI S5)\n", p.why);
        return;
    }
    p.blpwm = (bl & BL_PWM_EN) != 0;
    p.bl0 = bl;
    ck_printf("screen: panel: ready to switch off while Allen rests (%s; backlight PWM %s)\n",
              p.ovrd ? "driving the BLON and DIGON pins directly, as the firmware left them" :
                       "through the panel power sequencer, with the delays the firmware set",
              p.blpwm ? "on, its duty goes to 0 and back" : "not on, left alone");
}

int ck_x86_panel_ready(void) { return p.ready && !p.broken; }

int ck_x86_panel_off(void)
{
    if (!p.ready || p.broken || p.off)
        return -1;
    p.off = 1;
    p.off_t = ck_rdtsc();
    /* backlight first */
    if (p.blpwm && bl_duty("off", p.bl0 & ~BL_DUTY))
        return -1;
    uint32_t cntl = rd(mmLVTMA_PWRSEQ_CNTL);
    if (!p.ovrd) {
        if (wr("off", "PWRSEQ_CNTL", mmLVTMA_PWRSEQ_CNTL, cntl & ~PWRSEQ_TARGET_STATE, PWRSEQ_TARGET_STATE))
            return -1;
        /* the sequencer drops BLON, waits the panel's power-down delay, drops DIGON */
        if (wait_reg("off", "PWRSEQ_STATE", mmLVTMA_PWRSEQ_STATE, STATE_DONE | STATE_BLON | STATE_DIGON, STATE_DONE,
                     SEQ_MS))
            return -1;
    } else {
        if (wr("off", "PWRSEQ_CNTL", mmLVTMA_PWRSEQ_CNTL, cntl & ~PWRSEQ_BLON, PWRSEQ_BLON))
            return -1;
        wait_ms(T_BL_OFF_MS);
        if (wr("off", "PWRSEQ_CNTL", mmLVTMA_PWRSEQ_CNTL, rd(mmLVTMA_PWRSEQ_CNTL) & ~PWRSEQ_DIGON, PWRSEQ_DIGON))
            return -1;
        /* Control readback alone does not establish that the output pins
         * followed the firmware override. Check the same existing state
         * register used by the sequencer path; do not invent another path. */
        if (wait_reg("off", "PWRSEQ_STATE", mmLVTMA_PWRSEQ_STATE, STATE_BLON | STATE_DIGON, 0, SEQ_MS))
            return -1;
    }
    ck_puts("screen: panel off (register state confirmed; physical darkness not measured)\n");
    return 0;
}

int ck_x86_panel_on(void)
{
    if (!p.ready || !p.off)
        return -1;
    /* the panel stays off at least T12 before it is powered again */
    uint64_t since = (ck_rdtsc() - p.off_t) * 1000 / p.hz;
    if (since < T12_MS)
        wait_ms((unsigned)(T12_MS - since));
    int rc = 0;
    uint32_t cntl = rd(mmLVTMA_PWRSEQ_CNTL);
    if (!p.ovrd) {
        rc |= wr("on", "PWRSEQ_CNTL", mmLVTMA_PWRSEQ_CNTL, cntl | PWRSEQ_TARGET_STATE, PWRSEQ_TARGET_STATE);
        /* the sequencer raises DIGON, waits the panel's power-up delay, raises BLON */
        rc |= wait_reg("on", "PWRSEQ_STATE", mmLVTMA_PWRSEQ_STATE, STATE_DONE | STATE_BLON | STATE_DIGON,
                       STATE_DONE | STATE_BLON | STATE_DIGON, SEQ_MS);
    } else {
        rc |= wr("on", "PWRSEQ_CNTL", mmLVTMA_PWRSEQ_CNTL, cntl | PWRSEQ_DIGON, PWRSEQ_DIGON);
        wait_ms(T_ON_MS);
        rc |= wr("on", "PWRSEQ_CNTL", mmLVTMA_PWRSEQ_CNTL, rd(mmLVTMA_PWRSEQ_CNTL) | PWRSEQ_BLON, PWRSEQ_BLON);
        rc |= wait_reg("on", "PWRSEQ_STATE", mmLVTMA_PWRSEQ_STATE, STATE_BLON | STATE_DIGON,
                       STATE_BLON | STATE_DIGON, SEQ_MS);
    }
    /* then the backlight duty as it was */
    if (p.blpwm)
        rc |= bl_duty("on", p.bl0);
    if (!rc)
        p.off = 0; /* a failed restore must not be reported as already awake */
    ck_puts(rc ? "screen: panel on not confirmed (see above)\n" : "screen: panel on (register state confirmed)\n");
    return rc ? -1 : 0;
}
#else
void ck_x86_panel_start(volatile uint8_t *cfg, unsigned bus, unsigned dev, unsigned fn, uint16_t vendor,
                        uint16_t device, uint32_t cls, uint64_t fb_pa, uint64_t fb_bytes, uint64_t tsc_hz)
{
    static int said;
    (void)bus, (void)dev, (void)fn, (void)cls, (void)fb_pa, (void)fb_bytes, (void)tsc_hz;
    if (said++)
        return;
    if (cfg)
        ck_printf("screen: panel: display chip %04x:%04x seen; this build (CK_X86_PANEL_OFF=0) never switches the "
                  "panel; reversible panel off unavailable (rest uses ACPI S5)\n", vendor, device);
    else
        ck_puts("screen: panel: no display chip on the PCI bus owns the framebuffer; reversible panel off unavailable (rest uses ACPI S5)\n");
}
int ck_x86_panel_ready(void) { return 0; }
int ck_x86_panel_off(void) { return -1; }
int ck_x86_panel_on(void) { return -1; }
#endif
