/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Drake Stapleton (AIEN / aien-dev)
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* kmain_x86.c -- AIENOS-KERNEL-0: the C kernel core on x86_64, first
 * cut, for the 2017 iMac (iMac18,3, Core i5-7500) and QEMU q35.
 *
 * After ExitBootServices (efi_x86.c) on the firmware's page tables:
 * interrupts off, own GDT + TSS (IST1 stack for #DF, NMI, #MC) and IDT (256
 * stubs, trap.S), the CHANDOF3 record checked (core/handoff_check.c), a frame
 * allocator over the memory map (mm/frames.c), own 4-level page tables that
 * identity map [0, top) with 2 MiB pages (RAM write-back, the rest uncached,
 * the GOP framebuffer write-combining through PAT entry 1) and a 4 KiB guard
 * page under the kernel stack, CR3 switched and the stack moved. W^X (#409):
 * EFER.NXE and CR0.WP on every core; only the kernel's code is executable
 * (read-only), every other leaf is no-execute, read-only data is read-only,
 * proven by probes and an audit of every leaf. Then: the
 * screen console (core/fbcon.c), heap (mm/heap.c), the guard page proven to
 * fault and the fault contained, the TSC measured against the HPET, the
 * local APIC (xAPIC) with its timer at 100 Hz for 20 ticks, the MADT CPU
 * count, the secondary cores started and parked (smp_x86.S), the AHCI and
 * NVMe disks read-only and a look at the Ethernet chip (pci_x86.c,
 * ahci_x86.c), and the final report. With a keyboard, Allen's window, whose
 * memory lives in its own partition of the approved hard disk (memory_x86.c,
 * the only disk writes). Goodbye, a tap of the power button (PM1 status,
 * polled) or CK_X86_IDLE_S without a key rests the computer: the picture goes
 * to black (ui/allen_ui.c), then the known display controller is asked to
 * switch the panel off (panel_x86.c). Only a register-confirmed off permits
 * reversible rest with any-key wake. Unsupported or failed panel control
 * uses the existing ACPI S5 power-off instead, with no automatic restart.
 * Without a keyboard it holds the screen CK_X86_HOLD_S seconds and
 * powers off through ACPI S5 (FADT PM1 control + the DSDT's \_S5 package); the lab image
 * (CK_X86_REPORT=1) without a keyboard restarts through the FADT reset register instead, back
 * into its network loader (native/boot/net_loader.c).
 * Scheduling on the secondary cores, the network driver and VT-d
 * are later cuts. No runtime service
 * is called. QEMU runs of this path qualify nothing physical. */
#include "../core/ck_internal.h"
#include "../core/handoff_check.h"
#include "../mm/frames.h"
#include "../mm/heap.h"
#include "../svc/allen_code.h"
#include "../dev/usb_hid.h"
#include "../svc/allen_intent.h"
#include "../svc/allen_reply.h"
#include "../svc/courier.h"
#include "courier_udp.h"
#include "../ui/allen_ui.h"
#include "x86.h"
#include "../../boot/netboot_proto.h"
#include "swap_x86.h"
#include "bench_x86.h"
#include "local_tools.h"
#include "local_wire.h"

#ifndef AIENOS_COMMIT
#define AIENOS_COMMIT "unknown"
#endif
#ifndef CK_X86_REPORT
#define CK_X86_REPORT 0 /* 1 only in the opt-in lab image: the network report */
#endif
#ifndef CK_X86_HOLD_S
#define CK_X86_HOLD_S 60u
#endif
#ifndef CK_X86_IDLE_S
#define CK_X86_IDLE_S 600u /* Allen's window: the computer rests after this long without a key (0: never) */
#endif

#define SEL_CODE 0x08
#define SEL_DATA 0x10
#define SEL_TSS 0x18
#define VEC_TIMER 0x40
#define VEC_SPURIOUS 0xff
#define STACK_BYTES (64u << 10)
#define IST_BYTES (16u << 10)
#define HEAP_PAGES 256u /* 1 MiB */
#define MAP_TOP_MAX (512ull << 30) /* one PML4 entry */

#define PTE_P 0x1ull
#define PTE_W 0x2ull
#define PTE_PWT 0x8ull
#define PTE_PCD 0x10ull
#define PTE_PS 0x80ull
#define PTE_NX (1ull << 63)
#define PTE_ADDR 0x000ffffffffff000ull
#define MSR_EFER 0xc0000080u
#define EFER_NXE (1ull << 11)
#define CR0_WP (1ull << 16)

/* CK_X86_WX_MUTATE (negative test builds only): 1 leaves RAM executable,
 * 2 leaves the kernel's code writable. The W^X probes must then FAIL. */
#ifndef CK_X86_WX_MUTATE
#define CK_X86_WX_MUTATE 0
#endif

/* Image regions (image_x86_64.ld), runtime addresses after self-relocation. */
extern char __image_base[], __text_start[], __text_end[], __ro_start[], __ro_end[], __image_end[];

const char *ck_commit(void) { return AIENOS_COMMIT; }

static struct ck_handoff *hand;
static struct ck_frames frames;
static struct ck_heap heap;
static uint64_t tramp_pa; /* secondary-core trampoline page, below 1 MiB; 0 = none */

/* ---- descriptor tables ---- */
struct __attribute__((packed)) tss64 {
    uint32_t r0;
    uint64_t rsp[3];
    uint64_t r1;
    uint64_t ist[7];
    uint64_t r2;
    uint16_t r3, iomap;
};
_Static_assert(sizeof(struct tss64) == 104, "tss64 size");
struct __attribute__((packed)) dtr {
    uint16_t limit;
    uint64_t base;
};
struct idt_gate {
    uint16_t off0, sel;
    uint8_t ist, attr;
    uint16_t off1;
    uint32_t off2, zero;
};
_Static_assert(sizeof(struct idt_gate) == 16, "idt gate size");

static uint64_t gdt[7] __attribute__((aligned(16)));
static struct tss64 tss __attribute__((aligned(16)));
static struct idt_gate idt[256] __attribute__((aligned(16)));
static uint8_t ist_stack[IST_BYTES] __attribute__((aligned(16)));
/* First page: the guard, unmapped once the kernel's tables are live. */
static uint8_t kstack[4096 + STACK_BYTES] __attribute__((aligned(4096)));

static void tables_load(void)
{
    gdt[0] = 0;
    gdt[1] = 0x00209a0000000000ull; /* 64-bit code, DPL 0 */
    gdt[2] = 0x0000920000000000ull; /* data */
    uint64_t tb = (uint64_t)(uintptr_t)&tss, tl = sizeof tss - 1;
    gdt[3] = (tl & 0xffff) | (tb & 0xffffff) << 16 | 0x89ull << 40 | ((tl >> 16) & 0xf) << 48 |
             ((tb >> 24) & 0xff) << 56;
    gdt[4] = tb >> 32;
    gdt[5] = 0x0020fa0000000000ull; /* CPL3 64-bit code, selector 0x2b */
    gdt[6] = 0x0000f20000000000ull; /* CPL3 data, selector 0x33 */
    tss.ist[0] = (uint64_t)(uintptr_t)(ist_stack + sizeof ist_stack);
    tss.iomap = sizeof tss;
    struct dtr g = {sizeof gdt - 1, (uint64_t)(uintptr_t)gdt};
    ck_x86_load_gdt(&g, SEL_CODE, SEL_DATA, SEL_TSS);

    for (unsigned v = 0; v < 256; v++) {
        uint64_t a = (uint64_t)(uintptr_t)ck_x86_isr_stubs + (uint64_t)v * CK_X86_ISR_STRIDE;
        idt[v].off0 = (uint16_t)a;
        idt[v].sel = SEL_CODE;
        idt[v].ist = (v == 2 || v == 8 || v == 18) ? 1 : 0;
        idt[v].attr = v == 0x80 ? 0xee : 0x8e; /* present, DPL 0, 64-bit interrupt gate */
        idt[v].off1 = (uint16_t)(a >> 16);
        idt[v].off2 = (uint32_t)(a >> 32);
        idt[v].zero = 0;
    }
    struct dtr i = {sizeof idt - 1, (uint64_t)(uintptr_t)idt};
    __asm__ volatile("lidt %0" ::"m"(i) : "memory");
}

/* ---- power ---- */
static uint64_t tsc_hz;
uint64_t ck_x86_tsc_hz(void) { return tsc_hz; }

static void delay_ms(uint64_t ms)
{
    if (!tsc_hz)
        return;
    uint64_t t0 = ck_rdtsc(), n = tsc_hz / 1000 * ms;
    while (ck_rdtsc() - t0 < n)
        ck_pause();
}

/* \_S5 sleep types from the DSDT: "_S5_" PackageOp PkgLength NumElements
 * then two integers (ZeroOp, OneOp, OnesOp or BytePrefix n). */
static int s5_types(const uint8_t *dsdt, uint32_t len, unsigned *a, unsigned *b)
{
    for (uint32_t i = 36; i + 12 < len; i++) {
        if (memcmp(dsdt + i, "_S5_", 4) != 0 || dsdt[i + 4] != 0x12)
            continue;
        uint32_t p = i + 5;
        p += 1u + (dsdt[p] >> 6); /* PkgLength: lead byte + follow bytes */
        p++;                      /* NumElements */
        unsigned v[2];
        for (unsigned k = 0; k < 2; k++) {
            if (p >= len)
                return -1;
            uint8_t op = dsdt[p++];
            if (op == 0x00 || op == 0x01)
                v[k] = op;
            else if (op == 0xff)
                v[k] = 0x7;
            else if (op == 0x0a && p < len)
                v[k] = dsdt[p++] & 7u;
            else
                return -1;
        }
        *a = v[0];
        *b = v[1];
        return 0;
    }
    return -1;
}

static uint32_t rd32(const void *p, unsigned off)
{
    uint32_t v;
    memcpy(&v, (const uint8_t *)p + off, 4);
    return v;
}
static uint64_t rd64(const void *p, unsigned off)
{
    uint64_t v;
    memcpy(&v, (const uint8_t *)p + off, 8);
    return v;
}

static void acpi_poweroff(void)
{
    const uint8_t *fadt = hand && hand->rsdp ? ck_acpi_lookup(hand->rsdp, "FACP") : 0;
    if (!fadt) {
        ck_puts("power: no FADT\n");
        return;
    }
    uint32_t flen = rd32(fadt, 4);
    uint64_t dsdt = flen >= 148 ? rd64(fadt, 140) : 0;
    if (!dsdt)
        dsdt = rd32(fadt, 40);
    uint32_t pm1a = rd32(fadt, 64), pm1b = rd32(fadt, 68), smi = rd32(fadt, 48);
    uint8_t enable = fadt[52];
    unsigned ta, tb;
    if (!dsdt || s5_types((const uint8_t *)(uintptr_t)dsdt, rd32((const void *)(uintptr_t)dsdt, 4), &ta, &tb)) {
        ck_puts("power: no \\_S5 package in the DSDT\n");
        return;
    }
    if (!pm1a || pm1a > 0xffff) {
        ck_printf("power: PM1a control block 0x%x is not an I/O port\n", pm1a);
        return;
    }
    if (!(ck_inw((uint16_t)pm1a) & 1) && smi && enable) { /* SCI_EN clear: ACPI mode off */
        ck_outb((uint16_t)smi, enable);
        for (unsigned i = 0; i < 300 && !(ck_inw((uint16_t)pm1a) & 1); i++)
            delay_ms(10);
    }
    ck_printf("power: off via ACPI S5 pm1a=0x%x pm1b=0x%x slp_typ=%u/%u\n", pm1a, pm1b, ta, tb);
    ck_outw((uint16_t)pm1a, (uint16_t)(ta << 10 | 1u << 13));
    if (pm1b && pm1b <= 0xffff)
        ck_outw((uint16_t)pm1b, (uint16_t)(tb << 10 | 1u << 13));
    delay_ms(2000);
    ck_puts("power: still running after the S5 write\n");
}

#if CK_X86_REPORT
/* Lab image only: restart instead of powering off, so the iMac comes back to
 * its network loader (NETLDR.EFI) for the next test without a hand on the
 * power button. The FADT reset register (ACPI 6.5 section 4.8.3.6; on the
 * iMac18,3: flags bit 10 RESET_REG_SUP set, SystemIO port 0xCF9, value 0x06,
 * read from its FACP table on 2026-10-17); without one, the same write to the
 * PCH reset control port 0xCF9 that the FADT names. */
static void acpi_reset(void)
{
    const uint8_t *fadt = hand && hand->rsdp ? ck_acpi_lookup(hand->rsdp, "FACP") : 0;
    uint16_t port = 0xCF9;
    uint8_t value = 0x06;
    if (fadt && rd32(fadt, 4) >= 129 && (rd32(fadt, 112) & (1u << 10)) && fadt[116] == 1 /* SystemIO */ &&
        rd64(fadt, 120) && rd64(fadt, 120) <= 0xffff) {
        port = (uint16_t)rd64(fadt, 120);
        value = fadt[128];
        ck_printf("power: restart via the ACPI reset register port=0x%x value=0x%x\n", port, value);
    } else {
        ck_puts("power: restart via port 0xcf9 (no usable ACPI reset register)\n");
    }
    ck_outb(port, value);
    for (volatile uint64_t i = 0; i < 100000000ull; i++)
        __asm__ volatile("pause");
    ck_puts("power: still running after the reset write\n");
}
#endif

/* Allen on the screen (ui/allen_ui.c) with its first local answers
 * (svc/allen_reply.c). Every Enter gets an answer on the console
 * ("allen: ..."); when a keyboard is ready and the RAM for drawing is there,
 * the screen switches from the boot report to Allen's window, which stays
 * up (no countdown to power-off). "goodbye", a tap of the power button or
 * CK_X86_IDLE_S without a key fades the picture to black, then tries the
 * existing guarded AMD panel-off sequence. Confirmed off rests the cores;
 * any key or another tap restores the panel before the picture. If the
 * controller cannot confirm off (or restore it), ACPI S5 is the fallback,
 * not black pixels with a lit backlight. No keyboard keeps the timed hold. */
static int allen_on;
static uint64_t allen_bye; /* TSC when Allen said goodbye; 0 = still talking */
static uint64_t allen_woke; /* TSC when Allen's window took the screen */
static uint64_t allen_key;  /* TSC of the last key, or of waking; the idle timer counts from it */
static int allen_asleep;  /* rest requested: fade pending, then confirmed panel off or S5 */
static uint64_t panel_due; /* TSC when the rest began: the panel goes off once the picture is black (0: not waiting) */
#define PANEL_WAIT_S 10u   /* the picture gets this long to go black before the panel goes off anyway */
static int panel_dark;     /* panel_off succeeded; waking must restore it */
static uint16_t pm1_sts;  /* PM1a status port (FADT), 0 = no power-button watch */
static uint64_t pwr_last; /* TSC of the last power-button tap taken */

/* The power button sets PWRBTN_STS (PM1 status bit 8, write 1 to clear) whether or
 * not it is enabled as an interrupt (chipset book 332691 p163). The enable bit is
 * cleared so no SMI or SCI goes to the firmware while we poll; holding the button
 * 4 s is still the chipset's own hard power-off. */
static void power_button_watch(void)
{
    const uint8_t *fadt = hand && hand->rsdp ? ck_acpi_lookup(hand->rsdp, "FACP") : 0;
    if (!fadt || rd32(fadt, 4) < 92) {
        ck_puts("power: no FADT; the power button is not watched\n");
        return;
    }
    uint32_t evt = rd32(fadt, 56); /* PM1a_EVT_BLK */
    uint8_t len = fadt[88];        /* PM1_EVT_LEN: status then enable, len/2 each */
    if (!evt || evt > 0xffff || len < 4) {
        ck_printf("power: PM1a event block 0x%x (%u bytes) is not a watchable I/O port\n", evt, len);
        return;
    }
    /* Real chipsets latch PWRBTN_STS whether or not PWRBTN_EN is set (ACPI 6.5
     * 4.8.2.2.1.2: "unconditionally set"), and with it clear a press raises no SMI#
     * or SCI (332691 PM1_EN). QEMU's model (hw/acpi/core.c acpi_pm1_evt_power_down)
     * latches only when PWRBTN_EN is set; interrupts are off here, so under a
     * hypervisor the bit is set instead and the SCI line it raises goes nowhere. */
    uint32_t r[4];
    ck_cpuid(1, 0, r);
    int hv = (r[2] >> 31) & 1;
    uint16_t en = (uint16_t)(evt + len / 2);
    uint16_t env = (uint16_t)(ck_inw(en) & ~0x100u);
    if (hv)
        env |= 0x100;
    ck_outw(en, env);
    ck_outw((uint16_t)evt, 0x100); /* a press from before now is forgotten */
    pm1_sts = (uint16_t)evt;
    ck_printf("power: button watched at PM1 status port 0x%x (tap: sleep or wake; hold 4 s: off%s)\n", evt,
              hv ? "; enable bit set for the hypervisor's chipset model" : "");
}
static int power_button_tapped(void)
{
    if (!pm1_sts || !(ck_inw(pm1_sts) & 0x100))
        return 0;
    ck_outw(pm1_sts, 0x100);
    uint64_t now = ck_rdtsc();
    if (now - pwr_last < tsc_hz / 2) /* bounce */
        return 0;
    pwr_last = now;
    return 1;
}
/* Resting while Allen sleeps. Nothing on the screen moves then, so the
 * drawing cores wait in MWAIT on park.gen until a write to it (Allen waking,
 * or draw_stop) instead of spinning; the boot core, which still polls the
 * keyboard and the power button, halts between polls until a one-shot LAPIC
 * timer interrupt REST_MS later. The MWAIT hint is the deepest of C6, C3 and
 * C1 that CPUID leaf 5 lists (an MWAIT C-state the firmware limits is entered
 * as a shallower one). Without MONITOR/MWAIT the drawing cores spin as before
 * and the boot core uses HLT. */
#define REST_MS 20u
static struct {
    volatile uint32_t gen;
    uint8_t pad[60]; /* alone on its 64-byte line: only a wake writes it */
} park __attribute__((aligned(64)));
static int rest_mwait;
static uint32_t rest_hint, lapic_per_ms;

static void rest_probe(uint32_t lapic_hz)
{
    uint32_t r[4];
    lapic_per_ms = lapic_hz / 1000u;
    ck_cpuid(0, 0, r);
    uint32_t max = r[0];
    ck_cpuid(1, 0, r);
    if (max < 5 || !(r[2] & 1u << 3))
        return;
    ck_cpuid(5, 0, r); /* EDX: 4 bits of sub-states per MWAIT C-state, C0 first; hint (n - 1) << 4 for Cn */
    rest_mwait = 1;
    rest_hint = (r[3] >> 12 & 0xfu) ? 0x20u : (r[3] >> 8 & 0xfu) ? 0x10u : 0x00u;
}

/* A drawing core while the screen is dark: waits until park.gen is no longer g. */
static void rest_wait(uint32_t g)
{
    if (!rest_mwait) {
        ck_pause();
        return;
    }
    __asm__ volatile("sfence" ::: "memory"); /* the last strips leave the write-combining buffers first */
    __asm__ volatile("monitor" ::"a"(&park.gen), "c"(0), "d"(0));
    if (__atomic_load_n(&park.gen, __ATOMIC_ACQUIRE) == g)
        __asm__ volatile("mwait" ::"a"(rest_hint), "c"(0) : "memory");
}
static void rest_wake(void) { __atomic_fetch_add(&park.gen, 1, __ATOMIC_RELEASE); }
static void rest_boot(void); /* after the LAPIC registers, below */

/* The processor's own energy counter (RAPL MSR_PKG_ENERGY_STATUS 0x611, units
 * in MSR_RAPL_POWER_UNIT 0x606 bits 12:8): read only on Intel client cores from
 * Skylake on (the iMac's i5-7500 is model 0x9e) and never under a hypervisor,
 * where the MSR may not exist and reading it would fault. 32 bits that wrap in
 * about an hour at 65 W, so sampled each second into microjoules. */
static struct {
    int on;
    uint32_t esu, last;
    uint64_t uj, t0, next;
} meter;

static const char *meter_start(void)
{
    static const uint8_t models[] = {0x4e, 0x5e, 0x8e, 0x9e, 0xa5, 0xa6};
    uint32_t r[4];
    ck_cpuid(1, 0, r);
    if (r[2] >> 31 & 1)
        return "none (under a hypervisor)";
    uint32_t sig = r[0];
    ck_cpuid(0, 0, r);
    if (r[1] != 0x756e6547u) /* "Genu" */
        return "none (not an Intel processor)";
    r[0] = sig;
    uint32_t family = r[0] >> 8 & 0xfu, model = (r[0] >> 4 & 0xfu) | (r[0] >> 12 & 0xf0u);
    int known = 0;
    for (unsigned i = 0; i < sizeof models; i++)
        known |= family == 6 && model == models[i];
    if (!known)
        return "none (not an Intel client core from Skylake on)";
    meter.esu = (uint32_t)(ck_rdmsr(0x606) >> 8 & 0x1fu);
    meter.last = (uint32_t)ck_rdmsr(0x611);
    meter.t0 = meter.next = ck_rdtsc();
    meter.on = 1;
    return "rapl package";
}
static void meter_step(uint64_t now)
{
    if (!meter.on || now < meter.next)
        return;
    uint32_t e = (uint32_t)ck_rdmsr(0x611);
    meter.uj += ((uint64_t)(uint32_t)(e - meter.last) * 1000000u) >> meter.esu;
    meter.last = e;
    meter.next = now + tsc_hz;
}
/* The processor's average since the last line, then the count starts again. */
static void meter_line(const char *when)
{
    if (!meter.on)
        return;
    uint64_t now = ck_rdtsc();
    meter.next = 0;
    meter_step(now);
    uint64_t ms = (now - meter.t0) * 1000u / tsc_hz;
    if (ms >= 1000) {
        uint64_t mw = meter.uj / ms;
        ck_printf("power: processor %llu.%llu W on average over %llu s %s\n", (unsigned long long)(mw / 1000),
                  (unsigned long long)(mw / 100 % 10), (unsigned long long)(ms / 1000), when);
    }
    meter.uj = 0;
    meter.t0 = now;
}

static void draw_stop(void);

/* A black GOP framebuffer proves nothing about the backlight. Never guess
 * Intel/AMD registers when the existing AMD path is unsupported or fails.
 * ACPI S5 is already hardware-documented for this iMac in
 * evidence/imac_kernel0_2026-10-16/README.md. Deliberately bypass the REPORT
 * build's no-key reboot policy: this is a requested display-off transition. */
static __attribute__((noreturn)) void panel_poweroff(const char *why)
{
    ck_printf("power: %s; ACPI S5 fallback (power button to start; no any-key wake)\n", why);
    ck_cli();
    draw_stop();
    ck_x86_kbd_done();
    acpi_poweroff();
    ck_puts("power: ACPI S5 failed; display-off not confirmed; halted (no restart); hold the power button to turn off\n");
    for (;;)
        __asm__ volatile("hlt");
}

static void allen_sleep(int on)
{
    if (allen_asleep == on)
        return;
    meter_line(on ? "awake" : "asleep");
    if (on) {
        ck_local_wire_begin(0);
        __atomic_store_n(&allen_asleep, 1, __ATOMIC_RELEASE);
        ck_allen_ui_sleep(1, ck_rdtsc()); /* the picture goes to black first */
        panel_due = ck_rdtsc() | 1;       /* then the panel (panel_step), then the cores rest */
        ck_puts("power: preparing display off; unconfirmed panel off uses ACPI S5\n");
        return;
    }
    panel_due = 0; /* waking during the fade needs no panel restore */
    if (panel_dark && ck_x86_panel_on())
        panel_poweroff("panel restore failed");
    panel_dark = 0;
    __atomic_store_n(&allen_asleep, 0, __ATOMIC_RELEASE);
    ck_allen_ui_sleep(0, ck_rdtsc()); /* then the picture comes back */
    rest_wake(); /* after the UI has the news: a woken core draws the sky again */
    allen_key = ck_rdtsc();
    ck_x86_kbd_discard();
    ck_puts("power: Allen awake\n");
}

/* Once the picture is black (or PANEL_WAIT_S has passed), require a confirmed
 * panel-off result. The old !ready return silently kept the backlight lit,
 * and the ignored off result did the same on a failed sequence. */
static void panel_step(void)
{
    if (!panel_due || !allen_asleep)
        return;
    uint64_t now = ck_rdtsc(), began = panel_due;
    int black = ck_allen_ui_asleep();
    if (!black && now - began < (uint64_t)PANEL_WAIT_S * tsc_hz)
        return;
    panel_due = 0;
    if (!black)
        ck_printf("screen: the picture did not go black within %u s; proceeding with display off\n", (unsigned)PANEL_WAIT_S);
    if (!ck_x86_panel_ready())
        panel_poweroff("panel off unsupported");
    ck_printf("screen: display-off transition %llu ms after the rest began; switching the panel off\n",
              (unsigned long long)((now - began) / (tsc_hz / 1000)));
    if (ck_x86_panel_off())
        panel_poweroff("panel off not confirmed");
    panel_dark = 1;
    ck_puts("power: Allen asleep (panel registers confirm off; any key or a tap of the power button wakes him)\n");
}

#define STEP_MAX 16
static struct {
    const char *name;
    uint64_t tsc;
} steps[STEP_MAX];
static unsigned nsteps;

void ck_x86_step(const char *name)
{
    if (nsteps < STEP_MAX)
        steps[nsteps].name = name, steps[nsteps++].tsc = ck_rdtsc();
}
static void night_stats(const char *when);
static ck_allen_facts allen_facts; /* filled at the summary line */
static ck_allen_mind allen_mind;
static const ck_courier *courier; /* to a far-off worker (NULL: no wire on this machine) */
static uint32_t job_seq;          /* the last job sent down it */
static uint64_t job_t;            /* TSC when that job went (0: nothing out) */
static uint64_t job_last;         /* TSC of the last word from the far side about it (the silence clock) */
static char job_buf[CK_COURIER_JOB_MAX];
static char job_asked[96];        /* the line the job was about, for Allen's note of the exchange */
/* Code asked of the Spark (svc/allen_code.h): one code job at a time, beside
 * no other job. The pieces gather in code_rx; the wait is on the TSC in ms. */
static ck_code_rx code_rx;
static ck_code_wait code_wait;
static uint32_t code_job;   /* the code job out (0: none) */
static uint64_t code_t;     /* TSC when it went */
static int code_again;      /* asked again once already */
static int code_lines_shown; /* lines of the code on the screen (0: none yet) */
static ck_allen_notes allen_notes; /* loaded from disk at the window, or empty */
static struct ck_x86_setup_facts setup_facts; /* what the setup wizard is told at the window */
static uint64_t ram_bytes;                     /* RAM in the memory map */

static void allen_edit(const char *buf, unsigned len)
{
    allen_key = ck_rdtsc();
    if (allen_asleep) { /* the key only wakes him */
        allen_sleep(0);
        return;
    }
    ck_allen_ui_input(buf, len, ck_rdtsc());
}

/* The notes map beside the window: the newest live notes (point 0 the newest)
 * and the links among them; every live note and link is counted. */
static void allen_map(void)
{
    static uint32_t idx[CK_ALLEN_UI_MAP_NODES];
    static uint8_t ea[CK_ALLEN_UI_MAP_LINKS], eb[CK_ALLEN_UI_MAP_LINKS];
    unsigned n = 0, ne = 0, total = 0, links = 0;
    int lit = -1;
    for (uint32_t i = allen_notes.n; i-- > 0;) {
        if (allen_notes.note[i].gone)
            continue;
        total++;
        for (uint32_t j = i + 1; j < allen_notes.n; j++)
            links += !allen_notes.note[j].gone && ck_allen_note_links(&allen_notes.note[i], &allen_notes.note[j]) > 0;
        if (n == CK_ALLEN_UI_MAP_NODES)
            continue;
        if (allen_mind.lit == (int)i + 1)
            lit = (int)n;
        for (unsigned k = 0; k < n && ne < CK_ALLEN_UI_MAP_LINKS; k++)
            if (ck_allen_note_links(&allen_notes.note[i], &allen_notes.note[idx[k]]) > 0) {
                ea[ne] = (uint8_t)k;
                eb[ne++] = (uint8_t)n;
            }
        idx[n++] = i;
    }
    ck_allen_ui_map(n, ea, eb, ne, lit, lit >= 0 ? allen_notes.note[idx[lit]].text : 0, total, links);
}

/* Keys while the setup wizard is up go to it whole (never echoed or logged). */
static int wizard_key(int kind, char c)
{
    if (!ck_x86_setup_on())
        return 0;
    allen_key = ck_rdtsc();
    if (allen_asleep) { /* the key only wakes him */
        allen_sleep(0);
        return 1;
    }
    return ck_x86_setup_key(kind, c);
}

/* Keys while code is on the screen: Up, Down, Page Up, Page Down, Home and End
 * move it (taken whole), Escape puts it away; anything else goes to the line. */
static int code_key(int kind, char c)
{
    (void)c;
    if (!ck_allen_ui_code_on())
        return 0;
    if (kind == CK_KEY_ESCAPE) {
        ck_allen_ui_code(0, 0, 0, ck_rdtsc());
        ck_printf("allen-code: code put away\n");
        return 1;
    }
    int d = kind == CK_KEY_UP     ? -1
            : kind == CK_KEY_DOWN ? 1
            : kind == CK_KEY_PGUP ? -CK_ALLEN_UI_CODE_PAGE
            : kind == CK_KEY_PGDN ? CK_ALLEN_UI_CODE_PAGE
            : kind == CK_KEY_HOME ? -(int)CK_CODE_MAX
            : kind == CK_KEY_END  ? (int)CK_CODE_MAX
                                  : 0;
    if (!d)
        return 0;
    allen_key = ck_rdtsc();
    unsigned top = 0, rows = 0, page = 0;
    if (ck_allen_ui_code_scroll(d, &top, &rows, &page) >= 0)
        ck_printf("allen-code: rows %u to %u of %u shown\n", top + 1, top + page < rows ? top + page : rows, rows);
    return 1;
}

static int line_key(int kind, char c) { return wizard_key(kind, c) || code_key(kind, c); }

static uint64_t now_ms(void) { return ck_rdtsc() / (tsc_hz / 1000 ? tsc_hz / 1000 : 1); }

/* A coding request: Allen says at once that he needs to code it and sends it
 * to the Spark (courier job "code", or "code-more" for a change to the code
 * on the screen), then goes on thinking until the code comes (courier_step).
 * With no Spark he says so plainly. 0: not a coding request. */
static int code_request(const char *text)
{
    char folded[CK_COURIER_LINE_MAX];
    ck_allen_fold(text, folded, sizeof folded);
    int more = ck_allen_ui_code_on() && ck_allen_code_more(folded);
    if (!more && !ck_allen_wants_code(folded))
        return 0;
    uint64_t t0 = allen_key;
    const char *say;
    unsigned n = 0;
    int sent = 0;
    if (!courier || !courier->ready())
        say = "I'd need to code that on the Spark, and I can't reach it right now. Is it switched on?";
    else if (code_job || job_t)
        say = "Give me a moment, the Spark is still on your last request.";
    else if (!(n = ck_allen_code_job(&allen_mind, job_seq + 1, text, more, job_buf, sizeof job_buf)) ||
             courier->send(job_buf, n) != 0)
        say = "I couldn't send that to the Spark. Try it once more?";
    else {
        code_job = ++job_seq;
        code_t = ck_rdtsc() | 1;
        code_again = 0;
        ck_code_begin(&code_rx, code_job);
        ck_code_wait_begin(&code_wait, now_ms());
        sent = 1;
        say = more ? "Let me take that change to the Spark." : "I need to code that. Let me send it off to the Spark.";
    }
    ck_printf("allen: %s\n", say);
    ck_allen_ui_said(text, say, ck_rdtsc());
    if (sent) {
        ck_allen_ui_working(1);
        ck_printf("allen-code: asked the Spark (%u chars)\n", (unsigned)strlen(text));
        ck_printf("allen-code: acknowledged in %llu ms (job %u, %s)\n",
                  (unsigned long long)((ck_rdtsc() - t0) * 1000 / tsc_hz), (unsigned)code_job, more ? "a change" : "new code");
    } else {
        ck_printf("allen-code: not asked (%s)\n", !courier || !courier->ready() ? "the Spark is not reachable" : "busy or not sent");
    }
    return 1;
}

/* The end of a code job: the code shown, asked again once, or given up out loud. */
static void code_done(const char *sentence)
{
    int st = ck_code_state(&code_rx);
    uint64_t now = ck_rdtsc();
    if ((st == CK_CODE_PART || st == CK_CODE_BAD) && !code_again && courier) {
        char again[32];
        unsigned n = (unsigned)ck_snprintf(again, sizeof again, "AGAIN %u", (unsigned)code_job);
        ck_printf("allen-code: %s, asked again\n", st == CK_CODE_BAD ? "the checksum did not match" : "a piece went missing");
        code_again = 1;
        ck_code_begin(&code_rx, code_job);
        ck_code_wait_heard(&code_wait, now_ms());
        if (courier->send(again, n) == 0)
            return;
    }
    const char *say = sentence && sentence[0] ? sentence : "Here it is.";
    if (st == CK_CODE_WHOLE) {
        ck_allen_ui_code(code_rx.text, code_rx.bytes, code_rx.lang, now);
        code_lines_shown = (int)ck_code_lines(code_rx.text, code_rx.bytes);
        ck_printf("allen-code: code shown, %u lines in %llu ms\n", (unsigned)code_lines_shown,
                  (unsigned long long)((now - code_t) * 1000 / tsc_hz));
        ck_printf("allen-code: %u bytes, crc32 %08x, %u pieces, language %s\n", (unsigned)code_rx.bytes,
                  (unsigned)code_rx.crc, (unsigned)code_rx.n, code_rx.lang[0] ? code_rx.lang : "none");
        /* the first lines on COM1 (the QEMU gate reads them back; never in the lab report) */
        unsigned line = 0;
        for (uint32_t i = 0, s = 0; i <= code_rx.bytes && line < 40; i++)
            if (i == code_rx.bytes || code_rx.text[i] == '\n') {
                if (i > s || i < code_rx.bytes) {
                    char row[200]; /* a longer line is echoed cut (the checksum covers the whole) */
                    uint32_t k = 0;
                    for (; k < i - s && k + 1 < sizeof row; k++)
                        row[k] = code_rx.text[s + k];
                    row[k] = 0;
                    ck_printf("code|%s\n", row);
                }
                s = i + 1;
                line++;
            }
    } else if (st != CK_CODE_NONE) {
        say = "The code got lost on the way from the Spark. Ask me again?";
        ck_printf("allen-code: lost (%u of %u pieces, %u refused)\n", (unsigned)code_rx.got, (unsigned)code_rx.n,
                  (unsigned)code_rx.refused);
    } else {
        ck_printf("allen-code: the Spark answered without code\n");
    }
    ck_printf("allen: %s\n", say);
    ck_allen_ui_said(0, say, now);
    ck_allen_ui_working(0);
    code_job = 0;
}
/* a is b (lower case), whatever a's letter case and the spaces around them */
static int same_words(const char *a, const char *b)
{
    while (*a == ' ')
        a++;
    for (; *b; a++, b++) {
        char x = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a;
        if (!*a || x != *b)
            return 0;
    }
    while (*a == ' ')
        a++;
    return *a == 0;
}

static void allen_line(const char *text)
{
    allen_key = ck_rdtsc();
    if (allen_asleep) {
        allen_sleep(0);
        return;
    }
    if (allen_on && same_words(text, "set up again") && ck_x86_setup_open(&setup_facts, courier, tsc_hz, 1))
        return; /* the setup wizard from its first screen; it says hello itself */
    if (allen_on && code_request(text))
        return;
    /* A far-off worker that has said hello answers first (svc/allen_reply.h far). */
    allen_mind.far = courier && courier->ready();
    const char *reply = ck_allen_answer(&allen_mind, text, ck_rdtsc() / tsc_hz);
    /* What was taught goes to disk before Allen says it will remember it: a
     * note first, then the memory copy that counts it; goodbye saves the line
     * count. A failed write is said out loud. */
    int save = allen_mind.changed || reply == ck_allen_goodbye;
    if (allen_mind.lasting && allen_mind.note_dirty &&
        ck_x86_notes_write(&allen_notes, (uint32_t)allen_mind.note_dirty - 1u)) {
        allen_mind.lasting = 0;
        reply = ck_allen_save_failed;
    }
    if (allen_mind.lasting && save && ck_x86_memory_save(&allen_mind.mem)) {
        allen_mind.lasting = 0;
        if (allen_mind.changed)
            reply = ck_allen_save_failed;
    }
    if (allen_mind.changed)
        ck_allen_ui_named(ck_allen_self(&allen_mind));
    allen_mind.changed = 0;
    if (allen_mind.sky_changed) {
        ck_allen_ui_sky_mode(allen_mind.mem.sky_mode, allen_mind.mem.sky_region,
                             allen_mind.mem.sky_lat_udeg, allen_mind.mem.sky_lon_udeg);
        allen_mind.sky_changed = 0;
    }
    if (allen_mind.map_changed || allen_mind.note_dirty)
        allen_map();
    allen_mind.map_changed = 0;
    allen_mind.note_dirty = 0;
    /* Nothing here could answer: when a courier is ready, Allen writes the job
     * himself and keeps thinking until the far side answers (courier_step). */
    if (reply == ck_allen_not_yet && courier && courier->ready() && !job_t && !code_job) {
        unsigned n = ck_allen_job(&allen_mind, job_seq + 1, text, job_buf, sizeof job_buf);
        if (n) /* his memory rides along: his name, the person's, the facts, the newest notes */
            n += ck_allen_job_memory(&allen_mind, 6, job_buf + n, sizeof job_buf - n);
        if (n && n + 16 < sizeof job_buf) {
            memcpy(job_buf + n, "\tlocal-tools=v1", 15); n += 15; job_buf[n] = 0;
        }
        if (n && courier->send(job_buf, n) == 0) {
            job_seq++;
            ck_local_wire_begin(job_seq);
            job_t = job_last = ck_rdtsc() | 1;
            ck_snprintf(job_asked, sizeof job_asked, "%s", text);
            ck_printf("allen: (asking the far side, job %u)\n", (unsigned)job_seq);
            ck_printf("screen: Allen asked the far side (%u characters in, %u in the job)\n", (unsigned)strlen(text), n);
            ck_allen_ui_think(text, ck_rdtsc());
            return;
        }
    }
    if (reply == ck_allen_not_yet && allen_mind.far) /* the far side is there but could not take it, or is still on the last line */
        reply = job_t || code_job ? "Give me a moment, I'm still on your last line." : ck_allen_lost_thread;
    ck_printf("allen: %s\n", reply);
    /* the only line about it the lab report may carry: lengths, no words */
    ck_printf("screen: Allen answered (%u characters in, %u out)\n", (unsigned)strlen(text), (unsigned)strlen(reply));
    ck_allen_ui_said(text, reply, ck_rdtsc());
    if (reply == ck_allen_goodbye && !allen_bye)
        allen_bye = ck_rdtsc() | 1;
    if (reply == ck_allen_goodbye)
        night_stats("by goodbye");
}

/* The startup screen (ui/allen_ui.c): the night sky from the moment the clock
 * is measured, Allen formed in it by the time the keyboard is ready. Until
 * the secondary cores are up the boot core draws a frame between console
 * writes (night_tick); then the first secondary core draws the frames
 * (draw_loop) and the next two draw strips of them (help_loop). draw_lock
 * keeps to one frame at a time whoever draws it. */
static uint32_t night_ms, night_mib;                     /* what is said about it once the console is up */
static uint64_t night_t0;                                /* when the night began (TSC); the chirp keeps time with it */
static uint64_t night_last, night_cost;                  /* the boot core's last frame and what it took */
static volatile uint32_t draw_run, draw_on, draw_helpers; /* drawing allowed; a core draws frames; cores helping */
static volatile uint8_t draw_lock;

static uint64_t night_clock(void) { return ck_rdtsc(); }

/* After each console write, while no secondary core draws: a frame when one
 * is due, with at least twice a frame's drawing time between frames, so
 * startup is not slowed by more than a third. */
static void night_tick(void)
{
    ck_x86_hda_step();
    uint64_t now = ck_rdtsc();
    if (__atomic_load_n(&draw_on, __ATOMIC_ACQUIRE) || now - night_last < night_cost * 2 ||
        __atomic_test_and_set(&draw_lock, __ATOMIC_ACQUIRE))
        return;
    if (ck_allen_ui_tick(now)) {
        night_last = ck_rdtsc();
        night_cost = night_last - now;
    }
    __atomic_clear(&draw_lock, __ATOMIC_RELEASE);
}

/* The secondary cores' work while the night is up; each returns to its halt
 * once draw_stop clears draw_run. While Allen sleeps they rest (rest_wait):
 * the frame core once its frame has put the dark screen up, the helpers at
 * once (the frame core draws any strip they leave). park.gen is read before
 * the state it guards, so a wake between the two is never missed. */
/* Every pass of a drawing core ends in a PAUSE, a frame drawn or not. When the
 * water came (f913a63) a frame under QEMU TCG took longer than FRAME_MS, so the
 * frame core drew frame after frame and never reached its PAUSE: a virtual CPU
 * that never leaves its translated code never passes an RCU quiescent point,
 * and QEMU's device_add waits for one (drain_call_rcu). The keyboard replug test
 * then hung inside device_add with the monitor blocked, and the keys it types
 * afterwards never arrived (L411, 2026-10-22, gdb stacks: main thread in
 * qmp_device_add > drain_call_rcu, vCPU 1 in code_gen_buffer). On the iMac a
 * PAUSE per frame or strip costs nothing. */
static void draw_loop(void)
{
    __atomic_store_n(&draw_on, 1, __ATOMIC_RELEASE);
    while (__atomic_load_n(&draw_run, __ATOMIC_ACQUIRE)) {
        uint32_t g = __atomic_load_n(&park.gen, __ATOMIC_ACQUIRE);
        while (__atomic_test_and_set(&draw_lock, __ATOMIC_ACQUIRE))
            ck_pause();
        (void)ck_allen_ui_tick(ck_rdtsc()); /* drawn or not, the pass ends in a PAUSE */
        int dark = ck_allen_ui_asleep();
        __atomic_clear(&draw_lock, __ATOMIC_RELEASE);
        if (dark)
            rest_wait(g);
        else
            ck_pause(); /* also after a frame: see above */
    }
    __atomic_store_n(&draw_on, 0, __ATOMIC_RELEASE);
}
static void help_loop(void)
{
    __atomic_fetch_add(&draw_helpers, 1, __ATOMIC_ACQ_REL);
    while (__atomic_load_n(&draw_run, __ATOMIC_ACQUIRE)) {
        uint32_t g = __atomic_load_n(&park.gen, __ATOMIC_ACQUIRE);
        if (ck_allen_ui_help()) {
            ck_pause(); /* one PAUSE per strip: see draw_loop */
            continue;
        }
        if (ck_allen_ui_asleep()) /* keep helping until the final black frame is presented */
            rest_wait(g);
        else
            ck_pause();
    }
    __atomic_fetch_sub(&draw_helpers, 1, __ATOMIC_ACQ_REL);
}

/* Before text goes back on the screen: the drawing cores finish the frame in
 * hand and stop. Waits at most 200 ms (a panic never hangs here); then the
 * frame lock is held for good, so nothing draws over the text. */
static void draw_stop(void)
{
    __atomic_store_n(&draw_run, 0, __ATOMIC_RELEASE);
    rest_wake(); /* resting cores see draw_run cleared */
    if (!tsc_hz)
        return;
    uint64_t t0 = ck_rdtsc();
    while ((draw_on || draw_helpers) && ck_rdtsc() - t0 < tsc_hz / 5)
        ck_pause();
    while (__atomic_test_and_set(&draw_lock, __ATOMIC_ACQUIRE) && ck_rdtsc() - t0 < tsc_hz / 5)
        ck_pause();
}

static void night_stats(const char *when)
{
    uint32_t n, slowest, average;
    ck_allen_ui_stats(&n, &slowest, &average);
    ck_printf("screen: night: %u frames %s, average %u.%u ms, slowest %u.%u ms (drawn by %s)\n", n, when,
              average / 1000, average / 100 % 10, slowest / 1000, slowest / 100 % 10,
              draw_on ? "a secondary core" : "the boot core");
    ck_allen_ui_dusk_stats(&n, &slowest, &average);
    if (n)
        ck_printf("screen: dusk: %u frames, average %u.%u ms, slowest %u.%u ms\n", n, average / 1000,
                  average / 100 % 10, slowest / 1000, slowest / 100 % 10);
}

/* Time since the power button for each step. The TSC counts from power-on
 * (and from every reset), so its value at a step is the time since then:
 * firmware until the loader (or this kernel) starts, the loader, this kernel
 * until Allen's window takes the screen and the keyboard. */
static void boot_times(void)
{
    const struct nb_pass *np = ck_x86_netldr_pass(0);
    uint64_t per_ms = tsc_hz / 1000u, entry = hand->uefi_entry_ticks;
    if (!per_ms)
        return;
    if (np)
        ck_printf("boot_time: since power on: firmware until the loader %llu ms, loader %llu ms, kernel %llu ms; "
                  "Allen's window at %llu ms\n", (unsigned long long)(np->loader_tsc / per_ms),
                  (unsigned long long)((entry - np->loader_tsc) / per_ms),
                  (unsigned long long)((allen_woke - entry) / per_ms), (unsigned long long)(allen_woke / per_ms));
    else
        ck_printf("boot_time: since power on: firmware until this kernel %llu ms (no loader), kernel %llu ms; "
                  "Allen's window at %llu ms\n", (unsigned long long)(entry / per_ms),
                  (unsigned long long)((allen_woke - entry) / per_ms), (unsigned long long)(allen_woke / per_ms));
    /* each step of this kernel, and when the night reached the screen */
    static char line[240]; /* the report keeps lines up to 240 characters */
    uint64_t prev = entry;
    int n = ck_snprintf(line, sizeof line, "boot_time: steps in ms:");
    for (unsigned k = 0; k < nsteps && n > 0 && (size_t)n < sizeof line; k++) {
        n += ck_snprintf(line + n, sizeof line - (size_t)n, " %s %llu", steps[k].name,
                         (unsigned long long)((steps[k].tsc - prev) / per_ms));
        prev = steps[k].tsc;
    }
    ck_printf("%s window %llu; the night on screen at %llu ms\n", line, (unsigned long long)((allen_woke - prev) / per_ms),
              night_t0 ? (unsigned long long)(night_t0 / per_ms + night_ms) : 0ull);
}

static int64_t rtc_unix(void); /* forward declaration; defined below */
static void allen_window(void)
{
    static char footer[64];
    volatile uint32_t *px;
    uint32_t w, h, pitch, format;
    uint64_t pa;
    ck_x86_step("keyboard");
    ck_allen_mind_init(&allen_mind, &allen_facts);
    allen_mind.notes = &allen_notes;
    if (ck_x86_memory_open(&allen_mind.mem, &allen_notes, tsc_hz)) {
        allen_mind.lasting = 1;
        allen_mind.mem.starts++;
        if (ck_x86_memory_save(&allen_mind.mem))
            allen_mind.lasting = 0;
    } else {
        allen_notes.n = 0; /* nothing half-loaded is kept */
        allen_mind.mem.nnotes = 0;
    }
    ck_x86_step("memory");
    courier = 0;
#if CK_X86_REPORT
    courier = ck_x86_courier_udp(tsc_hz); /* lab image: Ethernet to the far side when the chip is up (it says so itself) */
#endif
    if (!courier) {
        courier = ck_x86_courier_init();
        ck_printf(courier ? "courier: a second serial port answers; Allen can ask a far-off worker once it says hello\n"
                          : "courier: no second serial port; Allen answers from this computer alone\n");
    }
    ck_allen_ui_named(ck_allen_self(&allen_mind)); /* the bar says who to speak to */
    ck_x86_kbd_sink(0, allen_line);
#ifdef CK_X86_SWAP_TEST_DISK
    /* QEMU swap test only (never in a lab image that reaches the iMac): teach Allen
     * one fact through the real line path so kernel A writes Allen's memory to disk
     * before the swap, and the swapped-in kernel must load it back. */
    allen_line("my name is Ada");
#endif
    if (ck_x86_console_fb_get(&px, &w, &h, &pitch, &format)) {
        ck_puts("screen: text only (no framebuffer for Allen's window)\n");
        return;
    }
    allen_facts.screen_w = w;
    allen_facts.screen_h = h;
    allen_mind.facts = allen_facts;
    if (!ck_allen_ui_started()) { /* the night could not start at boot: Allen's home starts here */
        size_t need = ck_allen_ui_mem_bytes(w, h);
        uint64_t pages = (need + CK_PAGE - 1) / CK_PAGE;
        if (ck_frames_alloc(&frames, pages, CK_PAGE, &pa)) {
            ck_printf("screen: text only (no %llu MiB for Allen's window)\n", (unsigned long long)(need >> 20));
            return;
        }
        if (ck_allen_ui_start(px, w, h, pitch, format, (void *)(uintptr_t)pa, pages * CK_PAGE, tsc_hz, ck_rdtsc(),
                              night_clock)) {
            ck_puts("screen: text only (Allen's window refused the screen)\n");
            ck_frames_free(&frames, pa, pages);
            return;
        }
        night_mib = (uint32_t)(need >> 20);
    }
    ck_snprintf(footer, sizeof footer, ck_x86_panel_ready() ?
                "Goodbye: screen off; power off if panel control fails" :
                "Goodbye powers off; power button starts this computer");
    ck_printf("screen: Allen's window (%ux%u, %u MiB drawing memory)\n", w, h, night_mib);
    ck_x86_console_fb_off();
    ck_allen_ui_refresh(); /* whatever else was on the screen goes */
    ck_allen_ui_wake(footer, ck_rdtsc());
    /* Read the real-time clock (CMOS ports 0x70/0x71) for sidereal sky calculations */
    {
        int64_t rtc_s = rtc_unix();
        if (rtc_s > 0)
            ck_allen_ui_set_unix_time(rtc_s, ck_rdtsc());
    }
    /* Apply sky mode saved in Allen's memory */
    if (allen_mind.mem.sky_mode)
        ck_allen_ui_sky_mode(allen_mind.mem.sky_mode, allen_mind.mem.sky_region,
                             allen_mind.mem.sky_lat_udeg, allen_mind.mem.sky_lon_udeg);
    ck_x86_kbd_sink(allen_edit, allen_line);
    allen_on = 1;
    allen_woke = allen_key = ck_rdtsc();
    power_button_watch();
    const char *meter_how = meter_start();
    if (CK_X86_IDLE_S)
        ck_printf("power: this computer rests after %u s without a key\n", (unsigned)CK_X86_IDLE_S);
    else
        ck_puts("power: this computer does not rest by itself\n");
    if (rest_mwait)
        ck_printf("power: asleep, the drawing cores rest in mwait (hint 0x%x) and the boot core in mwait between "
                  "keyboard polls every %u ms; meter: %s\n", rest_hint, REST_MS, meter_how);
    else
        ck_printf("power: asleep, the drawing cores spin (no MONITOR/MWAIT) and the boot core halts between "
                  "keyboard polls every %u ms; meter: %s\n", REST_MS, meter_how);
    boot_times();
    night_stats("before Allen woke");
    allen_map(); /* the notes loaded from disk, before the first word */
    setup_facts = (struct ck_x86_setup_facts){hand->rsdp, ram_bytes, allen_facts.cores, w, h, allen_mind.mem.starts};
    ck_x86_kbd_keys(line_key);
    if (ck_x86_setup_open(&setup_facts, courier, tsc_hz, 0))
        return; /* the setup wizard says hello in its own words */
    const char *hello = ck_allen_welcome(&allen_mind);
    if (hello) {
        ck_printf("allen: %s\n", hello);
        ck_allen_ui_said(0, hello, ck_rdtsc());
    }
}

static int same_text(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

/* Allen's note of an exchange he had through the far side: what was asked,
 * what he said, one note on disk (svc/allen_reply.h ck_allen_note_exchange),
 * so his memory grows with every answer. The cost is printed in ms. */
static void allen_note_exchange(const char *said)
{
    uint64_t t0 = ck_rdtsc();
    if (ck_allen_note_exchange(&allen_mind, job_asked, said))
        return;
    int failed = 0;
    if (allen_mind.lasting && allen_mind.note_dirty &&
        ck_x86_notes_write(&allen_notes, (uint32_t)allen_mind.note_dirty - 1u))
        failed = 1;
    if (!failed && allen_mind.lasting && ck_x86_memory_save(&allen_mind.mem))
        failed = 1;
    if (failed)
        allen_mind.lasting = 0;
    allen_mind.changed = 0;
    allen_mind.note_dirty = 0;
    allen_map();
    allen_mind.map_changed = 0;
    ck_printf("allen_memory: note %u (the exchange) %s in %llu ms\n", (unsigned)allen_notes.n,
              !allen_mind.lasting ? "kept until power off" : "saved", (unsigned long long)((ck_rdtsc() - t0) * 1000 / tsc_hz));
}

/* Answers from the far side, taken in the hold loop: the parts of the one
 * Allen is waiting for are joined and shown as they come (the ink settles
 * into words); anything else is dropped. "WAIT" says the far side is held up
 * and starts the silence clock over; 30 s of silence gives the job up out
 * loud. The console gets Allen's words (COM1); the lab report carries lengths
 * and times only. */
static void courier_step(void)
{
    static int said_ready;
    static char text[160]; /* one part, folded */
    static char said[160]; /* the parts so far: what the window shows at once (ui TEXT_MAX) */
    static unsigned said_n, parts;
    char line[CK_COURIER_LINE_MAX];
    uint32_t seq;
    unsigned part, total;
    if (!courier)
        return;
    if (!said_ready && courier->ready()) {
        said_ready = 1;
        ck_printf("courier: ready, the far side said hello\n");
    }
    ck_local_snapshot(allen_facts.cores, ck_frames_free_bytes(&frames),
                      hand->fb_width, hand->fb_height,
                      ck_x86_tg3_ready() ? (ck_x86_tg3_link() ? 1 : 2) : 0);
    ck_local_wire_step(courier);
    while (courier->take(line, sizeof line)) {
        if (ck_local_wire_line(line)) continue;
        if (ck_x86_setup_line(line))
            continue; /* the setup wizard's key test answered */
        if (code_job) {
            int k = ck_code_take(&code_rx, line);
            if (k > 0)
                ck_code_wait_heard(&code_wait, now_ms());
            if (k)
                continue;
            if (ck_allen_say_line(line, &seq, &part, &total, text, sizeof text) && seq == code_job) {
                ck_code_wait_heard(&code_wait, now_ms());
                if (part && total && part >= total)
                    code_done(text);
                continue;
            }
        }
        if (!ck_allen_say_line(line, &seq, &part, &total, text, sizeof text) || !job_t || seq != job_seq)
            continue;
        job_last = ck_rdtsc() | 1;
        if (part == 0) { /* WAIT: the far side is held up (a busy model); the silence clock starts over */
            static uint32_t waited_job;
            if (waited_job != seq) {
                waited_job = seq;
                ck_printf("screen: the far side is busy, Allen waits for job %u\n", (unsigned)seq);
            }
            continue;
        }
        if (part == 1) {
            said_n = 0;
            parts = 0;
            ck_printf("screen: the far side began job %u after %llu ms\n", (unsigned)seq,
                      (unsigned long long)((job_last - job_t) * 1000 / tsc_hz));
        }
        parts++;
        if (text[0]) { /* the parts join with a space; what the window cannot hold is left off */
            if (said_n && said_n + 1 < sizeof said)
                said[said_n++] = ' ';
            for (unsigned i = 0; text[i] && said_n + 1 < sizeof said; i++)
                said[said_n++] = text[i];
            said[said_n] = 0;
            ck_allen_ui_said(0, said, ck_rdtsc());
        }
        if (total && part >= total) { /* the last part: the answer is whole */
            if (!said_n) {
                ck_snprintf(said, sizeof said, "%s", ck_allen_lost_thread);
                ck_allen_ui_said(0, said, ck_rdtsc());
            }
            ck_printf("allen: %s\n", said);
            ck_printf("screen: the far side answered job %u (%u characters out, %u parts, %llu ms)\n", (unsigned)seq,
                      (unsigned)strlen(said), parts, (unsigned long long)((ck_rdtsc() - job_t) * 1000 / tsc_hz));
            job_t = 0;
            ck_local_wire_begin(0);
            if (!same_text(said, ck_allen_lost_thread)) /* no note of an exchange that got no answer */
                allen_note_exchange(said);
        }
    }
    if (code_job) {
        int w = ck_code_wait_tick(&code_wait, now_ms());
        if (w == CK_CODE_WAIT_SLOW) {
            const char *say = "The Spark is still writing it. Longer code takes a little while.";
            ck_printf("allen: %s\n", say);
            ck_printf("allen-code: the Spark is slow (%u s so far)\n", CK_CODE_SLOW_MS / 1000u);
            ck_allen_ui_said(0, say, ck_rdtsc());
        } else if (w == CK_CODE_WAIT_GONE) {
            const char *say = "The Spark stopped answering, so I couldn't get the code. Try again in a minute.";
            ck_printf("allen: %s\n", say);
            ck_printf("allen-code: the Spark stopped answering job %u (%u s of silence)\n", (unsigned)code_job,
                      CK_CODE_GONE_MS / 1000u);
            ck_allen_ui_said(0, say, ck_rdtsc());
            ck_allen_ui_working(0);
            code_job = 0;
        }
    }
    if (job_t && ck_rdtsc() - job_last > 30 * tsc_hz) {
        job_t = 0;
        ck_local_wire_begin(0);
        ck_printf("allen: %s\n", ck_allen_lost_thread);
        ck_printf("screen: the far side did not answer job %u\n", (unsigned)job_seq);
        ck_allen_ui_said(0, ck_allen_lost_thread, ck_rdtsc());
    }
}

/* One step of the hold loop. The devices (keyboard controller, the courier's
 * serial port, the power button's PM1 port, the sound controller) are polled
 * at most once every POLL_US by the TSC; between polls the loop only spins
 * on the clock. Polling them on every pass hit them about a million times a
 * second, which under QEMU starved the emulator's main loop of its big lock
 * after a keyboard replug (handoff 2026-10-18: the replugged keyboard's keys
 * never arrived). 0.5 ms keeps a key under a millisecond late and the
 * courier's 16-byte FIFO safe (1.4 ms to fill at 115200 baud). The picture's
 * frame (when this core draws it) and the sleep bookkeeping run every pass. */
#define POLL_US 500u
static uint64_t poll_next; /* TSC of the next device poll */

void ck_x86_hold_step(uint64_t *end)
{
    uint64_t now = ck_rdtsc();
    int poll = (int64_t)(now - poll_next) >= 0;
    if (poll) {
        poll_next = now + tsc_hz / 1000000 * POLL_US;
        ck_x86_hda_step();
        ck_x86_kbd_hold_poll(end);
        courier_step(); /* before the window too: the wire's FIFO is 16 bytes, the far side's hello 14 */
    }
    /* The lab receivers (swap, bench) run whether or not Allen's window is up: a lab
     * iMac with no keyboard must still take a swap or a bench command. */
    ck_x86_swap_step(hand, tramp_pa);
    ck_x86_bench_step(tsc_hz);
    if (!allen_asleep)
        ck_local_step();
    if (!allen_on)
        return;
    if (!allen_asleep) {
        const char *sample = ck_local_model_demo_step();
        if (sample) ck_allen_ui_said(0, sample, ck_rdtsc());
    }
    ck_x86_setup_step();
    if (!__atomic_load_n(&draw_on, __ATOMIC_ACQUIRE) && !__atomic_test_and_set(&draw_lock, __ATOMIC_ACQUIRE)) {
        ck_allen_ui_tick(ck_rdtsc()); /* no secondary core draws: this one does */
        __atomic_clear(&draw_lock, __ATOMIC_RELEASE);
    }
    static int told;
    if (!told && ck_rdtsc() - allen_woke > 20 * tsc_hz) {
        told = 1;
        night_stats("by 20 s after Allen woke");
    }
    /* After goodbye, allow the existing answer delay, then fade to black.
     * Idle and a power-button tap use the same confirmed-off/S5 policy. */
    if (allen_bye && ck_rdtsc() - allen_bye > 4 * tsc_hz) {
        allen_bye = 0;
        allen_sleep(1);
    }
    if (CK_X86_IDLE_S && !allen_asleep && ck_rdtsc() - allen_key > (uint64_t)CK_X86_IDLE_S * tsc_hz) {
        ck_printf("power: no key for %u s; the computer rests\n", (unsigned)CK_X86_IDLE_S);
        allen_sleep(1);
    }
    if (poll && power_button_tapped())
        allen_sleep(!allen_asleep);
    panel_step();
    meter_step(ck_rdtsc());
    *end = ~0ull;
    if (allen_asleep)
        rest_boot();
}

static __attribute__((noreturn)) void finish(void)
{
    ck_cli();
    if (!tsc_hz) { /* no clock yet: keep the screen readable, never power off unseen */
#if CK_X86_REPORT
        ck_puts("power: no clock measured; lab image restarts\n");
        acpi_reset();
#endif
        ck_puts("power: halted before the clock was measured; hold the power button to turn the machine off\n");
        for (;;)
            __asm__ volatile("hlt");
    }
    if (CK_X86_HOLD_S && ck_x86_kbd_ready()) {
        allen_window();
        if (allen_on)
            ck_puts("keyboard: type a line and press Enter; goodbye switches the display off; any-key wake only after confirmed panel off, otherwise ACPI S5 (power button to start)\n");
        else
            ck_printf("keyboard: type a line and press Enter; the screen stays on until %u s after the last key\n",
                      ck_x86_kbd_idle_s());
    }
    if (!allen_on) { /* no Allen's window: the startup text, not the night */
        draw_stop();
        ck_x86_console_fb_show();
    }
    if (CK_X86_HOLD_S) {
        /* With Allen's window up the hold does not expire. Rest either
         * confirms panel off and permits wake, or powers off through S5. */
#if CK_X86_REPORT
        /* Lab build only (make imac-kernel CK_X86_REPORT=1): sends the
         * allowlisted, unauthenticated report if the network is up. */
        if (allen_on)
            ck_puts("power: Allen's window stays on; goodbye, idle or a power-button tap switches the display off; confirmed panel off permits any-key wake, otherwise ACPI S5\n");
        else
            ck_printf("power: holding the screen %u s, then restart\n", (unsigned)CK_X86_HOLD_S);
        ck_x86_report_hold(tsc_hz, CK_X86_HOLD_S);
        ck_x86_kbd_done();
#else
        /* Default and resident builds: no report code is linked; nothing is sent. */
        if (allen_on)
            ck_puts("power: Allen's window stays on; goodbye, idle or a power-button tap switches the display off; confirmed panel off permits any-key wake, otherwise ACPI S5\n");
        else
            ck_printf("power: holding the screen %u s, then ACPI S5\n", (unsigned)CK_X86_HOLD_S);
        uint64_t end = ck_rdtsc() + (uint64_t)CK_X86_HOLD_S * tsc_hz; /* typing moves it out */
        while (ck_rdtsc() < end) {
            ck_x86_hold_step(&end);
            ck_pause();
        }
        ck_x86_kbd_done();
#endif
    }
    if (allen_on) { /* not reached while the hold holds; if it ever ends, halt: never a restart after goodbye */
        ck_puts("power: Allen's window ended; halted (no restart); hold the power button 4 s to turn the computer off\n");
        for (;;)
            __asm__ volatile("hlt");
    }
#if CK_X86_REPORT
    acpi_reset();
#endif
    acpi_poweroff();
    ck_puts("power: halted; hold the power button to turn the machine off\n");
    for (;;)
        __asm__ volatile("hlt");
}

void ck_panic(const char *fmt, ...)
{
    ck_cli();
    draw_stop();
    ck_x86_console_fb_show(); /* a panic is always readable */
    va_list ap;
    va_start(ap, fmt);
    ck_puts("panic: ");
    ck_vprintf(fmt, ap);
    va_end(ap);
    ck_puts("\n");
    finish();
}

/* ---- traps ---- */
static struct {
    volatile int armed, hit;
    volatile uint64_t resume, cr2, err;
} probe;
static volatile uint64_t ticks, tick_tsc[32];
static volatile uint64_t stray_irqs;
static volatile uint32_t *xapic;

static const char *const exc_name[32] = {
    "#DE", "#DB", "NMI", "#BP", "#OF", "#BR", "#UD", "#NM", "#DF", "?", "#TS", "#NP", "#SS", "#GP", "#PF", "?",
    "#MF", "#AC", "#MC", "#XM", "#VE", "#CP", "?", "?", "?", "?", "?", "?", "#HV", "#VC", "#SX", "?"};

/* The local APIC in xAPIC mode (MMIO, 4 KiB at IA32_APIC_BASE): the mode
 * the iMac's firmware leaves it in and the one QEMU TCG emulates. x2APIC
 * (MSR access) waits for a test that can run it. */
static void apic_wr(unsigned reg, uint32_t v)
{
    xapic[reg / 4] = v;
}
static uint32_t apic_rd(unsigned reg)
{
    return xapic[reg / 4];
}
#define APIC_ID 0x20
#define APIC_VER 0x30
#define APIC_TPR 0x80
#define APIC_EOI 0xb0
#define APIC_SVR 0xf0
#define APIC_LVT_TIMER 0x320
#define APIC_TIMER_INIT 0x380
#define APIC_TIMER_CUR 0x390
#define APIC_TIMER_DIV 0x3e0

/* The boot core between keyboard polls while Allen sleeps: a one-shot LAPIC
 * timer REST_MS out, then STI;MWAIT (or STI;HLT) until its interrupt, which
 * STI's one-instruction shadow cannot let slip in before the wait begins. The
 * timer was proven at boot (apic_start: 20 ticks or a panic), every other
 * interrupt line is masked, and a stray vector is only counted. */
static void rest_boot(void)
{
    if (!lapic_per_ms)
        return;
    apic_wr(APIC_LVT_TIMER, VEC_TIMER); /* one shot, unmasked; divide 16 as measured */
    apic_wr(APIC_TIMER_INIT, lapic_per_ms * REST_MS);
    if (rest_mwait) {
        __asm__ volatile("monitor" ::"a"(&park.gen), "c"(0), "d"(0));
        __asm__ volatile("sti; mwait; cli" ::"a"(rest_hint), "c"(0) : "memory");
    } else
        __asm__ volatile("sti; hlt; cli" ::: "memory");
    apic_wr(APIC_TIMER_INIT, 0);
    apic_wr(APIC_LVT_TIMER, 1u << 16 | VEC_TIMER);
}

void ck_local_machine_stack(uint64_t top) { tss.rsp[0] = top; }
int ck_local_machine_timer(int enable)
{
    if (!xapic || !lapic_per_ms)
        return -1;
    apic_wr(APIC_TIMER_INIT, 0);
    apic_wr(APIC_LVT_TIMER, (enable ? 0 : 1u << 16) | VEC_TIMER);
    if (enable)
        apic_wr(APIC_TIMER_INIT, lapic_per_ms);
    return 0;
}
void ck_local_machine_eoi(uint64_t v)
{
    if (xapic && v >= 32 && v != 0x80 && v != VEC_SPURIOUS)
        apic_wr(APIC_EOI, 0);
}

void ck_x86_trap(struct ck_x86_frame *f)
{
    if (f->vector == 14 && probe.armed) {
        probe.armed = 0;
        probe.hit = 1;
        probe.cr2 = ck_rd_cr2();
        probe.err = f->error;
        f->rip = probe.resume;
        return;
    }
    if (f->vector == VEC_TIMER) {
        uint64_t n = ticks;
        if (n < 32)
            tick_tsc[n] = ck_rdtsc();
        ticks = n + 1;
        apic_wr(APIC_EOI, 0);
        return;
    }
    if (f->vector == VEC_SPURIOUS)
        return;
    if (f->vector >= 32) {
        stray_irqs++;
        if (xapic)
            apic_wr(APIC_EOI, 0);
        return;
    }
    ck_panic("exception: vector %llu %s error=0x%llx rip=0x%llx rsp=0x%llx cr2=0x%llx rflags=0x%llx",
             (unsigned long long)f->vector, exc_name[f->vector], (unsigned long long)f->error,
             (unsigned long long)f->rip, (unsigned long long)f->rsp, (unsigned long long)ck_rd_cr2(),
             (unsigned long long)f->rflags);
}

/* ---- memory ---- */
struct mm {
    uint64_t root, top, pages_wb, pages_wc, pages_uc, pt_frames, fb_lo, fb_hi, pages_4k;
    int fb_ok;
};
static struct mm mm;

static uint64_t *table_new(void)
{
    uint64_t pa;
    if (ck_frames_alloc(&frames, 1, CK_PAGE, &pa))
        ck_panic("paging: out of frames for page tables");
    mm.pt_frames++;
    memset((void *)(uintptr_t)pa, 0, CK_PAGE);
    return (uint64_t *)(uintptr_t)pa;
}

/* EFI types that are RAM (write-back): everything but reserved (0), unusable
 * (8), MMIO (11, 12) and persistent memory left to later cuts. */
static int efi_ram(uint32_t type)
{
    return type <= 10 && type != 0 && type != 8;
}

static uint64_t *pds[MAP_TOP_MAX >> 30];

static int in(uint64_t p, const char *lo, const char *hi)
{
    return p >= (uint64_t)(uintptr_t)lo && p < (uint64_t)(uintptr_t)hi;
}

/* The permission of one 4 KiB page inside a split 2 MiB page: the stack
 * guard unmapped, code read-only executable, the PE header and read-only data
 * read-only no-execute, the trampoline writable executable until
 * wx_retire_trampoline(), everything else writable no-execute. */
static uint64_t leaf4k(uint64_t p, uint64_t cache, uint64_t nx)
{
    if (p == (uint64_t)(uintptr_t)kstack)
        return 0;
    if (in(p, __text_start, __text_end))
        return p | PTE_P | cache | (CK_X86_WX_MUTATE == 2 ? PTE_W : 0);
    if (in(p, __image_base, __text_start) || in(p, __ro_start, __ro_end))
        return p | PTE_P | cache | nx;
    if (tramp_pa && p == tramp_pa)
        return p | PTE_P | PTE_W | cache;
    return p | PTE_P | PTE_W | cache | nx;
}

static void split_2m(uint64_t a, uint64_t nx)
{
    uint64_t *e = &pds[a >> 30][(a >> 21) & 511];
    if (!(*e & PTE_PS))
        return; /* already split */
    uint64_t cache = *e & (PTE_PCD | PTE_PWT), *pt = table_new();
    for (uint64_t j = 0; j < 512; j++)
        pt[j] = leaf4k(a | j << 12, cache, nx);
    *e = (uint64_t)(uintptr_t)pt | PTE_P | PTE_W;
    mm.pages_4k += 512;
}

/* The 4 KiB PTE mapping `a`, or 0 when `a` sits in a 2 MiB page. */
static uint64_t *pte4k(uint64_t a)
{
    uint64_t e = pds[a >> 30][(a >> 21) & 511];
    if (!(e & PTE_P) || (e & PTE_PS))
        return 0;
    return (uint64_t *)(uintptr_t)(e & PTE_ADDR) + ((a >> 12) & 511);
}

/* The secondary cores are up (or will never start): the trampoline page
 * becomes read-only no-execute data. */
static void wx_retire_trampoline(void)
{
    uint64_t *e = tramp_pa ? pte4k(tramp_pa) : 0;
    if (!e)
        return;
    *e = (*e & ~PTE_W) | (CK_X86_WX_MUTATE == 1 ? 0 : PTE_NX);
    __asm__ volatile("invlpg (%0)" ::"r"(tramp_pa) : "memory");
}

/* Lab swap only, called once after a verified payload and before the jump: the
 * trampoline page becomes writable and executable again, and so does the running image,
 * which the trampoline overwrites with the new kernel's sections (text is read-only and
 * data no-execute under W^X; the new kernel runs on these tables until its own mm_build).
 * The application processors are halted by then (quiesce_aps), so the BSP's CR3 reload
 * is the only TLB flush needed. */
void ck_x86_swap_tramp_open(void)
{
    uint64_t *e = tramp_pa ? pte4k(tramp_pa) : 0;
    if (e)
        *e = (*e | PTE_W) & ~PTE_NX;
    for (uint64_t a = (uint64_t)(uintptr_t)__image_base; a < (uint64_t)(uintptr_t)__image_end; a += CK_PAGE) {
        uint64_t *pd = &pds[a >> 30][(a >> 21) & 511];
        if (!(*pd & PTE_P))
            continue;
        if (*pd & PTE_PS) {
            *pd = (*pd | PTE_W) & ~PTE_NX;
            continue;
        }
        /* The stack guard (kstack's first page) is unmapped: it is image memory the copy
         * writes too, mapped back identity, write-back, like the image pages around it. */
        uint64_t *pte = pte4k(a);
        if (pte)
            *pte = (*pte & PTE_P) ? (*pte | PTE_W) & ~PTE_NX : a | PTE_P | PTE_W;
    }
    ck_wr_cr3(ck_rd_cr3());
}

/* Every present leaf of the identity map: writable+executable ones, and
 * executable 4 KiB units (must equal the code pages). */
static void wx_audit(uint64_t *wx, uint64_t *exec4k)
{
    *wx = *exec4k = 0;
    for (uint64_t g = 0; g < mm.top >> 30; g++)
        for (uint64_t i = 0; i < 512; i++) {
            uint64_t e = pds[g][i];
            if (!(e & PTE_P))
                continue;
            if (e & PTE_PS) {
                if (!(e & PTE_NX)) {
                    *exec4k += 512;
                    *wx += !!(e & PTE_W);
                }
                continue;
            }
            const uint64_t *pt = (const uint64_t *)(uintptr_t)(e & PTE_ADDR);
            for (uint64_t j = 0; j < 512; j++)
                if ((pt[j] & PTE_P) && !(pt[j] & PTE_NX)) {
                    (*exec4k)++;
                    *wx += !!(pt[j] & PTE_W);
                }
        }
}

/* Memory this kernel is standing on when it starts, kept out of the frame pool. After the
 * firmware it all sits outside conventional memory and the reserves change nothing. After a live
 * swap (swap_x86.c) the old kernel's frames hold it: the handoff record (in the old kernel's
 * trampoline page), the memory map copy, the stack this code runs on, and the page tables CR3
 * still points at. A frame handed out from any of them would be overwritten while in use. */
#define LIVE_STACK_BELOW (16u << 10) /* stack mm_build may still grow into below the current rsp */
#define LIVE_STACK_ABOVE (64u << 10) /* the callers' frames above it (the swap stack is 64 KiB) */
static void reserve_live(const struct ck_handoff *h)
{
    uint64_t hp = (uint64_t)(uintptr_t)h, rsp;
    ck_frames_reserve(&frames, hp, hp + sizeof *h);
    ck_frames_reserve(&frames, h->memory_map, h->memory_map + h->map_size);
    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp));
    ck_frames_reserve(&frames, rsp > LIVE_STACK_BELOW ? rsp - LIVE_STACK_BELOW : 0, rsp + LIVE_STACK_ABOVE);
    /* The firmware's tables are its own memory; any other CR3 is a previous kernel's, built from
     * frames: keep every table it reaches (leaves of 1 GiB and 2 MiB are not tables). */
    uint64_t cr3 = ck_rd_cr3() & PTE_ADDR;
    if (cr3 == (h->firmware_ttbr0 & PTE_ADDR))
        return;
    ck_frames_reserve(&frames, cr3, cr3 + CK_PAGE);
    const uint64_t *l4 = (const uint64_t *)(uintptr_t)cr3;
    for (unsigned a = 0; a < 512; a++) {
        if (!(l4[a] & PTE_P))
            continue;
        const uint64_t *l3 = (const uint64_t *)(uintptr_t)(l4[a] & PTE_ADDR);
        ck_frames_reserve(&frames, (uint64_t)(uintptr_t)l3, (uint64_t)(uintptr_t)l3 + CK_PAGE);
        for (unsigned b = 0; b < 512; b++) {
            if (!(l3[b] & PTE_P) || (l3[b] & PTE_PS))
                continue;
            const uint64_t *l2 = (const uint64_t *)(uintptr_t)(l3[b] & PTE_ADDR);
            ck_frames_reserve(&frames, (uint64_t)(uintptr_t)l2, (uint64_t)(uintptr_t)l2 + CK_PAGE);
            for (unsigned c = 0; c < 512; c++)
                if ((l2[c] & PTE_P) && !(l2[c] & PTE_PS))
                    ck_frames_reserve(&frames, l2[c] & PTE_ADDR, (l2[c] & PTE_ADDR) + CK_PAGE);
        }
    }
}

static void mm_build(void)
{
    const struct ck_handoff *h = hand;
    ck_frames_init(&frames);
    if (ck_frames_from_efi(&frames, (const void *)(uintptr_t)h->memory_map, h->map_size, h->desc_size) < 0)
        ck_panic("memory: bad UEFI memory map");
    /* Page 0 stays out: a null pointer must never be a usable frame. */
    ck_frames_reserve(&frames, 0, CK_PAGE);
    /* The running kernel's own image is never a frame. Its pages can be typed conventional in the
     * map, and then a frame taken from them (the swap stack did) is inside the image that a live
     * swap copies over. Reserving it keeps every frame the kernel hands out outside its image. */
    ck_frames_reserve(&frames, (uint64_t)(uintptr_t)__image_base, (uint64_t)(uintptr_t)__image_end);
    reserve_live(h);
    /* The secondary-core trampoline needs a page below 1 MiB; take the
     * lowest free page before anything else does. */
    if (ck_frames_alloc(&frames, 1, CK_PAGE, &tramp_pa) || tramp_pa >= 0x100000)
        tramp_pa = 0;

    uint64_t top = 4ull << 30;
    for (uint64_t off = 0; off + h->desc_size <= h->map_size; off += h->desc_size) {
        const struct ck_efi_desc *d = (const void *)(uintptr_t)(h->memory_map + off);
        uint64_t end = d->phys + d->pages * CK_PAGE;
        if (end > top)
            top = end;
    }
    top = (top + (1ull << 30) - 1) & ~((1ull << 30) - 1);
    if (top > MAP_TOP_MAX)
        top = MAP_TOP_MAX;
    mm.top = top;

    /* Framebuffer: write-combining 2 MiB pages, unless one overlaps RAM. */
    if (h->fb_status == CK_HANDOFF_FB_PRESENT) {
        mm.fb_lo = h->fb_base & ~((2ull << 20) - 1);
        mm.fb_hi = (h->fb_base + h->fb_size + (2ull << 20) - 1) & ~((2ull << 20) - 1);
        mm.fb_ok = mm.fb_hi <= top;
        for (uint64_t off = 0; mm.fb_ok && off + h->desc_size <= h->map_size; off += h->desc_size) {
            const struct ck_efi_desc *d = (const void *)(uintptr_t)(h->memory_map + off);
            uint64_t lo = d->phys, hi = d->phys + d->pages * CK_PAGE;
            if (efi_ram(d->type) && lo < mm.fb_hi && hi > mm.fb_lo)
                mm.fb_ok = 0;
        }
    }

    /* Every 2 MiB page starts uncached (PCD|PWT, PAT index 3): device memory
     * (local APIC, I/O APIC, HPET, PCI BARs) whatever the MTRRs say. Pages
     * that hold RAM from the memory map become write-back (index 0), the
     * framebuffer write-combining (PWT alone, index 1, set to WC below).
     * W^X (#409): every leaf is no-execute except the kernel's code (and the
     * secondary-core trampoline while the cores start); the 2 MiB pages that
     * hold the image or the trampoline are split into 4 KiB pages below.
     * W^X separates code from data inside one privilege level; it is not
     * process isolation. Learned or generated code must never run here. */
    if (ck_rd_cr4() & (1ull << 12))
        ck_panic("paging: firmware runs 5-level paging (CR4.LA57); this kernel builds 4-level tables");
    uint32_t xr[4];
    ck_cpuid(0x80000000, 0, xr);
    if (xr[0] < 0x80000001)
        ck_panic("paging: CPU has no extended CPUID leaf for NX");
    ck_cpuid(0x80000001, 0, xr);
    if (!(xr[3] & (1u << 20)))
        ck_panic("paging: CPU has no NX (execute disable)");
    uint64_t nx = CK_X86_WX_MUTATE == 1 ? 0 : PTE_NX;
    uint64_t *pml4 = table_new(), *pdpt = table_new();
    pml4[0] = (uint64_t)(uintptr_t)pdpt | PTE_P | PTE_W;
    for (uint64_t g = 0; g < top >> 30; g++) {
        uint64_t *pd = pds[g] = table_new();
        pdpt[g] = (uint64_t)(uintptr_t)pd | PTE_P | PTE_W;
        for (uint64_t i = 0; i < 512; i++)
            pd[i] = (g << 30 | i << 21) | PTE_P | PTE_W | PTE_PS | PTE_PCD | PTE_PWT | nx;
    }
    for (uint64_t off = 0; off + h->desc_size <= h->map_size; off += h->desc_size) {
        const struct ck_efi_desc *d = (const void *)(uintptr_t)(h->memory_map + off);
        if (!efi_ram(d->type))
            continue;
        uint64_t hi = d->phys + d->pages * CK_PAGE;
        for (uint64_t a = d->phys & ~((2ull << 20) - 1); a < hi && a < top; a += 2ull << 20) {
            uint64_t *e = &pds[a >> 30][(a >> 21) & 511];
            if (*e & PTE_PS)
                *e &= ~(PTE_PCD | PTE_PWT);
        }
    }
    for (uint64_t a = mm.fb_lo; mm.fb_ok && a < mm.fb_hi; a += 2ull << 20) {
        uint64_t *e = &pds[a >> 30][(a >> 21) & 511];
        if (*e & PTE_PS)
            *e = (*e & ~PTE_PCD) | PTE_PWT;
    }
    /* 4 KiB pages where the image and the trampoline live, keeping the
     * 2 MiB page's cache type (PCD/PWT sit at the same bits in a 4 KiB PTE). */
    uint64_t img_lo = (uint64_t)(uintptr_t)__image_base, img_hi = (uint64_t)(uintptr_t)__image_end;
    for (uint64_t a = img_lo & ~((2ull << 20) - 1); a < img_hi; a += 2ull << 20)
        split_2m(a, nx);
    if (tramp_pa)
        split_2m(tramp_pa & ~((2ull << 20) - 1), nx);
    for (uint64_t g = 0; g < top >> 30; g++)
        for (uint64_t i = 0; i < 512; i++) {
            uint64_t e = pds[g][i];
            if (!(e & PTE_PS))
                continue;
            if (e & PTE_PCD)
                mm.pages_uc++;
            else if (e & PTE_PWT)
                mm.pages_wc++;
            else
                mm.pages_wb++;
        }
    mm.root = (uint64_t)(uintptr_t)pml4;

    /* IA32_PAT: the power-on value with entry 1 (PWT) changed from WT to WC. */
    uint32_t r[4];
    ck_cpuid(1, 0, r);
    if (!(r[3] & (1u << 16)))
        ck_panic("paging: CPU has no PAT");
    __asm__ volatile("wbinvd" ::: "memory");
    ck_wrmsr(0x277, 0x0007040600070106ull);
    /* NX bits are reserved (a #PF on every access) until EFER.NXE is set. */
    ck_wrmsr(MSR_EFER, ck_rdmsr(MSR_EFER) | EFER_NXE);
    ck_wr_cr3(mm.root);
    uint64_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile("mov %0, %%cr0" ::"r"(cr0 | CR0_WP) : "memory"); /* read-only binds the kernel too */
    /* Toggling CR4.PGE drops global TLB entries the firmware's tables left. */
    uint64_t cr4 = ck_rd_cr4();
    if (cr4 & 0x80) {
        __asm__ volatile("mov %0, %%cr4" ::"r"(cr4 & ~0x80ull) : "memory");
        __asm__ volatile("mov %0, %%cr4" ::"r"(cr4) : "memory");
    }
    __asm__ volatile("wbinvd" ::: "memory");
}

/* Device registers above the identity-mapped range (a 64-bit BAR the firmware
 * placed high, as OVMF does with NVMe): 2 MiB uncached pages added to the live
 * tables on demand. 0 ok, -1 refused (wraps or beyond 46 address bits). */
static uint64_t *sub_table(uint64_t *t, unsigned i)
{
    if (!(t[i] & PTE_P))
        t[i] = (uint64_t)(uintptr_t)table_new() | PTE_P | PTE_W;
    return (uint64_t *)(uintptr_t)(t[i] & PTE_ADDR);
}

int ck_x86_map_mmio(uint64_t pa, uint64_t len)
{
    if (!len || pa + len < pa || pa + len > (1ull << 46))
        return -1;
    uint64_t *pml4 = (uint64_t *)(uintptr_t)mm.root;
    for (uint64_t a = pa & ~((2ull << 20) - 1); a < pa + len; a += 2ull << 20) {
        if (a < mm.top)
            continue;
        uint64_t *pd = sub_table(sub_table(pml4, (unsigned)(a >> 39) & 511), (unsigned)(a >> 30) & 511);
        uint64_t *e = &pd[(a >> 21) & 511];
        if (!(*e & PTE_P)) {
            *e = a | PTE_P | PTE_W | PTE_PS | PTE_PCD | PTE_PWT | PTE_NX;
            mm.pages_uc++;
        }
        __asm__ volatile("invlpg (%0)" ::"r"(a) : "memory");
    }
    return 0;
}

/* ---- time ---- */
static const char *timebase = "none";
static uint64_t hpet_period_fs;

static void tsc_measure(void)
{
    const uint8_t *t = ck_acpi_lookup(hand->rsdp, "HPET");
    if (t && rd32(t, 4) >= 56 && t[40] == 0) {
        volatile uint64_t *hp = (volatile uint64_t *)(uintptr_t)rd64(t, 44);
        uint64_t period = hp[0] >> 32; /* GCAP_ID: counter period in fs */
        if (period && period <= 100000000ull) {
            hp[2] |= 1; /* GEN_CONF.ENABLE_CNF */
            uint64_t want = 50000000000000ull / period; /* 50 ms in HPET ticks */
            /* Bounded by 2^34 TSC ticks (about 5 s at 3.4 GHz): an HPET that
             * does not count falls through to CPUID instead of hanging. */
            uint64_t c0 = hp[30], s0 = ck_rdtsc(), c1;
            do
                c1 = hp[30];
            while (c1 - c0 < want && ck_rdtsc() - s0 < (1ull << 34));
            if (c1 - c0 < want)
                goto no_hpet;
            uint64_t s1 = ck_rdtsc(), ns = (c1 - c0) * period / 1000000ull;
            if (ns) {
                tsc_hz = (s1 - s0) * 1000000000ull / ns;
                hpet_period_fs = period;
                timebase = "hpet";
                return;
            }
        }
    }
no_hpet:;
    uint32_t r[4];
    ck_cpuid(0, 0, r);
    if (r[0] >= 0x16) {
        ck_cpuid(0x16, 0, r);
        if (r[0]) {
            tsc_hz = (uint64_t)r[0] * 1000000ull;
            timebase = "cpuid16";
        }
    }
}

/* ---- interrupts ---- */
struct apic_report {
    uint32_t id, version, lapic_hz, cpus, ioapics, ioapic_lines;
    uint64_t ticks, ms, min_us, max_us, avg_us;
};

static void ioapic_mask_all(uint64_t base, uint32_t *lines)
{
    volatile uint32_t *io = (volatile uint32_t *)(uintptr_t)base;
    io[0] = 1;
    uint32_t n = ((io[4] >> 16) & 0xff) + 1;
    for (uint32_t i = 0; i < n; i++) {
        io[0] = 0x10 + 2 * i;
        io[4] |= 1u << 16;
    }
    *lines += n;
}

static void madt_walk(struct apic_report *ar)
{
    const uint8_t *m = ck_acpi_lookup(hand->rsdp, "APIC");
    if (!m)
        ck_panic("madt: no ACPI MADT");
    uint32_t len = rd32(m, 4);
    for (uint32_t off = 44; off + 2 <= len;) {
        uint8_t type = m[off], el = m[off + 1];
        if (el < 2 || off + el > len)
            ck_panic("madt: bad entry at %u", off);
        if (type == 0 && el >= 8 && (rd32(m, off + 4) & 3))
            ar->cpus++;
        else if (type == 9 && el >= 16 && (rd32(m, off + 8) & 3))
            ar->cpus++;
        else if (type == 1 && el >= 12) {
            ar->ioapics++;
            ioapic_mask_all(rd32(m, off + 4), &ar->ioapic_lines);
        }
        off += el;
    }
}

static void apic_start(struct apic_report *ar)
{
    /* Legacy 8259 PICs: every line masked. */
    ck_outb(0x21, 0xff);
    ck_outb(0xa1, 0xff);
    uint64_t base = ck_rdmsr(0x1b);
    if (base & 1u << 10) /* x2APIC on: leaving it needs a pass through disabled */
        ck_wrmsr(0x1b, base & ~(3ull << 10));
    ck_wrmsr(0x1b, (base & ~(1ull << 10)) | 1u << 11);
    xapic = (volatile uint32_t *)(uintptr_t)(base & 0xffffff000ull);
    ar->id = apic_rd(APIC_ID) >> 24;
    ar->version = apic_rd(APIC_VER);
    apic_wr(APIC_TPR, 0);
    apic_wr(APIC_SVR, 0x100 | VEC_SPURIOUS);

    /* LAPIC timer rate: divide by 16, one shot, 10 ms of TSC. */
    apic_wr(APIC_TIMER_DIV, 0x3);
    apic_wr(APIC_LVT_TIMER, 1u << 16 | VEC_TIMER);
    apic_wr(APIC_TIMER_INIT, 0xffffffffu);
    delay_ms(10);
    uint32_t left = apic_rd(APIC_TIMER_CUR);
    apic_wr(APIC_TIMER_INIT, 0);
    ar->lapic_hz = (0xffffffffu - left) * 100u;
    if (!ar->lapic_hz)
        ck_panic("apic: timer did not count");

    /* 100 Hz periodic for 20 ticks, bounded by 1 s. */
    ticks = 0;
    apic_wr(APIC_LVT_TIMER, 1u << 17 | VEC_TIMER);
    apic_wr(APIC_TIMER_INIT, ar->lapic_hz / 100u);
    uint64_t t0 = ck_rdtsc();
    ck_sti();
    while (ticks < 21 && ck_rdtsc() - t0 < tsc_hz)
        ck_pause(); /* not hlt: a timer that never fires must not hang the boot */
    ck_cli();
    apic_wr(APIC_LVT_TIMER, 1u << 16 | VEC_TIMER);
    apic_wr(APIC_TIMER_INIT, 0);
    uint64_t n = ticks < 21 ? ticks : 21;
    ar->ticks = n ? n - 1 : 0; /* intervals between the first and the last tick seen */
    ar->min_us = ~0ull;
    for (uint64_t i = 1; i < n; i++) {
        uint64_t us = (tick_tsc[i] - tick_tsc[i - 1]) * 1000000ull / tsc_hz;
        if (us < ar->min_us)
            ar->min_us = us;
        if (us > ar->max_us)
            ar->max_us = us;
    }
    if (ar->ticks) {
        uint64_t span = (tick_tsc[n - 1] - tick_tsc[0]) * 1000000ull / tsc_hz;
        ar->avg_us = span / ar->ticks;
        ar->ms = span / 1000;
    } else {
        ar->min_us = 0;
    }
}

/* ---- secondary cores ----
 * Each enabled local APIC the MADT lists (other than this core's) gets INIT,
 * SIPI, SIPI pointing at the trampoline (smp_x86.S). A started core loads the
 * kernel's GDT (no task register) and IDT, sets the same PAT, enables its
 * local APIC, records its APIC ID, checks in, and halts with interrupts off.
 * Nothing is scheduled on it yet. */
#define SMP_MAX 16u
#define AP_STACK_PAGES 4u
#define APIC_ICR_LO 0x300
#define APIC_ICR_HI 0x310
#define PAT_VALUE 0x0007040600070106ull

struct smp_report {
    unsigned listed, started, failed;
    uint32_t ids[SMP_MAX], up_ids[SMP_MAX];
    const char *why;
};
static volatile uint32_t ap_up, ap_ids[SMP_MAX];

static void delay_us(uint64_t us)
{
    uint64_t t0 = ck_rdtsc(), n = tsc_hz / 1000000 * us;
    while (ck_rdtsc() - t0 < n)
        ck_pause();
}

static void ap_entry(void *arg)
{
    uint64_t idx = (uint64_t)(uintptr_t)arg;
    struct dtr g = {sizeof gdt - 1, (uint64_t)(uintptr_t)gdt};
    ck_x86_load_gdt(&g, SEL_CODE, SEL_DATA, 0);
    struct dtr i = {sizeof idt - 1, (uint64_t)(uintptr_t)idt};
    __asm__ volatile("lidt %0" ::"m"(i) : "memory");
    ck_wrmsr(0x277, PAT_VALUE);
    uint64_t base = ck_rdmsr(0x1b);
    if (base & 1u << 10)
        ck_wrmsr(0x1b, base & ~(3ull << 10));
    ck_wrmsr(0x1b, (base & ~(1ull << 10)) | 1u << 11);
    apic_wr(APIC_TPR, 0);
    apic_wr(APIC_SVR, 0x100 | VEC_SPURIOUS);
    ap_ids[idx] = apic_rd(APIC_ID) >> 24;
    __atomic_fetch_add(&ap_up, 1, __ATOMIC_SEQ_CST);
    if (idx == 0) /* the first one draws the night's frames, the next two help */
        draw_loop();
    else if (idx < 3)
        help_loop();
    for (;;)
        __asm__ volatile("cli; hlt");
}

static int icr_send(uint32_t id, uint32_t lo)
{
    apic_wr(APIC_ICR_HI, id << 24);
    apic_wr(APIC_ICR_LO, lo);
    for (unsigned n = 0; n < 1000; n++) {
        if (!(apic_rd(APIC_ICR_LO) & 1u << 12))
            return 0;
        delay_us(1);
    }
    return -1;
}

/* Lab swap only (swap_x86.c), before the trampoline overwrites this image: every other core
 * gets INIT, all excluding self (SDM vol.3 13.4.7.3, broadcast 000C4500H as in 11.4.4.1), and
 * waits for a start-up IPI with no code of ours running. A drawing core left in draw_loop would
 * run the new kernel's bytes mid-copy. The new kernel starts them again with INIT, SIPI, SIPI. */
void ck_x86_swap_stop_aps(void)
{
    if (!ap_up)
        return;
    if (icr_send(0, 0xc4500))
        ck_puts("swap: INIT to the other cores not accepted by the local APIC\n");
    delay_ms(10);
    ck_printf("swap: %u other cores stopped (INIT, waiting for start-up)\n", (unsigned)ap_up);
}

static void tramp_put32(const char *sym, uint32_t v)
{
    uint64_t off = (uint64_t)(uintptr_t)(sym - ck_x86_tramp_start);
    memcpy((uint8_t *)(uintptr_t)tramp_pa + off, &v, 4);
}
static void tramp_put64(const char *sym, uint64_t v)
{
    uint64_t off = (uint64_t)(uintptr_t)(sym - ck_x86_tramp_start);
    memcpy((uint8_t *)(uintptr_t)tramp_pa + off, &v, 8);
}
static uint32_t tramp_at(const char *sym)
{
    return (uint32_t)(tramp_pa + (uint64_t)(uintptr_t)(sym - ck_x86_tramp_start));
}

static void smp_start(const struct apic_report *ar, struct smp_report *sr)
{
    const uint8_t *m = ck_acpi_lookup(hand->rsdp, "APIC");
    uint32_t len = rd32(m, 4);
    for (uint32_t off = 44; off + 2 <= len; off += m[off + 1])
        if (m[off] == 0 && m[off + 1] >= 8 && (rd32(m, off + 4) & 3) && sr->listed < SMP_MAX)
            sr->ids[sr->listed++] = m[off + 3];
    uint64_t size = (uint64_t)(uintptr_t)(ck_x86_tramp_end - ck_x86_tramp_start);
    if (!tramp_pa)
        sr->why = "no free page below 1 MiB";
    else if (mm.root >= (1ull << 32))
        sr->why = "page tables above 4 GiB";
    else if (size > CK_PAGE)
        sr->why = "trampoline larger than a page";
    if (sr->why)
        return;
    memcpy((void *)(uintptr_t)tramp_pa, ck_x86_tramp_start, size);
    tramp_put32(ck_x86_tramp_gdtr + 2, tramp_at(ck_x86_tramp_gdt));
    tramp_put32(ck_x86_tramp_far32, tramp_at(ck_x86_tramp_pm32));
    tramp_put32(ck_x86_tramp_far64, tramp_at(ck_x86_tramp_lm64));
    tramp_put32(ck_x86_tramp_cr3, (uint32_t)mm.root);
    tramp_put64(ck_x86_tramp_entry, (uint64_t)(uintptr_t)ap_entry);
    for (unsigned k = 0; k < sr->listed; k++) {
        uint32_t id = sr->ids[k];
        if (id == ar->id)
            continue;
        uint64_t st;
        if (ck_frames_alloc(&frames, AP_STACK_PAGES, CK_PAGE, &st)) {
            sr->failed++;
            continue;
        }
        unsigned before = ap_up;
        tramp_put64(ck_x86_tramp_stack, st + AP_STACK_PAGES * CK_PAGE);
        tramp_put64(ck_x86_tramp_arg, before);
        __asm__ volatile("mfence" ::: "memory");
        int bad = icr_send(id, 0x4500); /* INIT, assert */
        delay_ms(10);
        for (unsigned s = 0; s < 2 && ap_up == before; s++) {
            bad |= icr_send(id, 0x4600 | (uint32_t)(tramp_pa >> 12)); /* start-up */
            delay_us(200);
        }
        uint64_t t0 = ck_rdtsc();
        while (ap_up == before && ck_rdtsc() - t0 < tsc_hz / 10)
            ck_pause();
        if (ap_up != before && !bad)
            sr->up_ids[sr->started++] = ap_ids[before];
        else
            sr->failed++;
        if (ap_up > before + 1)
            break; /* a core checked in twice or out of turn: stop starting more */
    }
}

/* ---- W^X probes (#409) ----
 * Each probe arms the #PF catcher (ck_x86_trap), touches one address and
 * resumes at the label after the access. A store writes back the byte it
 * read, so a mutated (writable) build changes nothing. An execute probe
 * jumps to two bytes `jmp *%rax` with rax = the resume label, so code that
 * does run comes straight back and the probe sees no fault. */
static int probe_store(uint64_t a)
{
    probe.hit = 0;
    probe.armed = 1;
    __asm__ volatile("lea 1f(%%rip), %%rax\n\t"
                     "mov %%rax, %0\n\t"
                     "movb (%1), %%cl\n\t"
                     "movb %%cl, (%1)\n"
                     "1:"
                     : "=m"(probe.resume)
                     : "r"(a)
                     : "rax", "rcx", "memory");
    probe.armed = 0;
    return probe.hit && probe.cr2 == a && (probe.err & 3) == 3; /* present + write */
}

static int probe_exec(uint8_t *buf)
{
    buf[0] = 0xff; /* jmp *%rax */
    buf[1] = 0xe0;
    uint64_t a = (uint64_t)(uintptr_t)buf;
    probe.hit = 0;
    probe.armed = 1;
    __asm__ volatile("lea 1f(%%rip), %%rax\n\t"
                     "mov %%rax, %0\n\t"
                     "jmp *%1\n"
                     "1:"
                     : "=m"(probe.resume)
                     : "r"(a)
                     : "rax", "memory");
    probe.armed = 0;
    return probe.hit && probe.cr2 == a && (probe.err & 0x11) == 0x11; /* present + instruction fetch */
}

static void wx_probes(void)
{
    uint64_t efer = ck_rdmsr(MSR_EFER), cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    uint8_t *hb = ck_heap_alloc(&heap, 16);
    volatile uint8_t sb[16];
    if (!hb)
        ck_panic("wx: no heap for the execute probe");
    int tw = probe_store((uint64_t)(uintptr_t)__text_start);
    int rw = probe_store((uint64_t)(uintptr_t)__ro_start);
    int hx = probe_exec(hb);
    int sx = probe_exec((uint8_t *)sb);
    ck_heap_free(&heap, hb);
    const char *f = "fault", *n = "ALLOWED";
    if (!(efer & EFER_NXE) || !(cr0 & CR0_WP) || !tw || !rw || !hx || !sx)
        ck_panic("wx: FAIL nx=%s wp=%s text_write=%s ro_write=%s heap_exec=%s stack_exec=%s",
                 efer & EFER_NXE ? "on" : "off", cr0 & CR0_WP ? "on" : "off", tw ? f : n, rw ? f : n,
                 hx ? f : n, sx ? f : n);
    ck_puts("wx: ok nx=on wp=on text=rx ro=r data=rw-nx text_write=fault ro_write=fault heap_exec=fault "
            "stack_exec=fault\n");
}

/* ---- the kernel ---- */
static void cpu_line(void)
{
    uint32_t r[4];
    char vendor[13], brand[49];
    ck_cpuid(0, 0, r);
    memcpy(vendor, &r[1], 4);
    memcpy(vendor + 4, &r[3], 4);
    memcpy(vendor + 8, &r[2], 4);
    vendor[12] = 0;
    brand[0] = 0;
    ck_cpuid(0x80000000, 0, r);
    if (r[0] >= 0x80000004) {
        for (uint32_t i = 0; i < 3; i++) {
            ck_cpuid(0x80000002 + i, 0, r);
            memcpy(brand + 16 * i, r, 16);
        }
        brand[48] = 0;
    }
    const char *b = brand;
    while (*b == ' ')
        b++;
    ck_printf("cpu: %s %s\n", vendor, b);
}

/* The night begins before anything is said on the screen; the text stays
 * off it (still in the RAM copy and on COM1) until Allen's window takes
 * over, or comes back if anything goes wrong. */
/* ---- real-time clock (CMOS RTC, I/O ports 0x70/0x71) ----
 * Port 0x70: write the register index (bit 7 set disables NMI during read);
 * port 0x71: read the value. Reference: Intel 82801 ICH7 datasheet, section 5.7;
 * PC/AT BIOS Technical Reference, IBM 1984. */
static uint8_t cmos_read(uint8_t reg)
{
    ck_outb(0x70, (uint8_t)(0x80u | reg));
    return ck_inb(0x71);
}
static uint8_t bcd2bin(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 0xf)); }
/* Read the CMOS RTC and return seconds since 1970-01-01 00:00:00 UTC.
 * The iMac's EFI sets the RTC to UTC. Returns 0 if the RTC is unreadable. */
static int64_t rtc_unix(void)
{
    for (int n = 0; n < 100000; n++) /* wait for UIP bit (reg 0x0A bit 7) to clear */
        if (!(cmos_read(0x0a) & 0x80))
            break;
    uint8_t s  = cmos_read(0x00); uint8_t mi = cmos_read(0x02); uint8_t hr = cmos_read(0x04);
    uint8_t dm = cmos_read(0x07); uint8_t mo = cmos_read(0x08); uint8_t yr = cmos_read(0x09);
    uint8_t sb = cmos_read(0x0b); uint8_t ce = cmos_read(0x32);
    if (s == 0xff && mi == 0xff && hr == 0xff) return 0;
    if (!(sb & 0x04)) { /* BCD mode */
        s=bcd2bin(s); mi=bcd2bin(mi); hr=bcd2bin(hr);
        dm=bcd2bin(dm); mo=bcd2bin(mo); yr=bcd2bin(yr); ce=bcd2bin(ce);
    }
    if (!(sb & 0x02) && (hr & 0x80)) { hr = (uint8_t)((hr & 0x7f) + 12); if (hr >= 24) hr -= 12; }
    uint32_t year = yr + ((ce >= 19 && ce <= 22) ? (uint32_t)ce * 100u : 2000u);
    if (year < 2000) year += 2000;
    ck_printf("rtc: %04u-%02u-%02u %02u:%02u:%02u UTC\n",
              (unsigned)year, (unsigned)mo, (unsigned)dm, (unsigned)hr, (unsigned)mi, (unsigned)s);
    static const uint32_t dbm[13] = {0,0,31,59,90,120,151,181,212,243,273,304,334};
    int leap = (year%4==0 && (year%100!=0 || year%400==0));
    uint32_t doy = dbm[mo] + (uint32_t)dm - 1u + (mo > 2 && leap ? 1u : 0u);
    uint32_t y = year - 1;
    uint32_t de = (year-1970)*365u + y/4 - y/100 + y/400 - (1969/4 - 1969/100 + 1969/400) + doy;
    return (int64_t)de * 86400ll + hr*3600 + mi*60 + s;
}

static void night_start(const struct ck_handoff *h)
{
    if (h->fb_status != CK_HANDOFF_FB_PRESENT || !mm.fb_ok || !tsc_hz)
        return;
    size_t need = ck_allen_ui_mem_bytes(h->fb_width, h->fb_height);
    uint64_t pages = (need + CK_PAGE - 1) / CK_PAGE, pa;
    if (ck_frames_alloc(&frames, pages, CK_PAGE, &pa))
        return;
    uint64_t t0 = ck_rdtsc();
    if (ck_allen_ui_start((volatile uint32_t *)(uintptr_t)h->fb_base, h->fb_width, h->fb_height, h->fb_pitch,
                          h->fb_format, (void *)(uintptr_t)pa, pages * CK_PAGE, tsc_hz, t0, night_clock)) {
        ck_frames_free(&frames, pa, pages);
        return;
    }
    night_t0 = t0;
    night_ms = (uint32_t)((ck_rdtsc() - t0) * 1000 / tsc_hz);
    night_mib = (uint32_t)(need >> 20);
    draw_run = 1;
    ck_x86_console_fb_hold(night_tick);
}

static __attribute__((noreturn)) void kernel_main(void *arg)
{
    (void)arg;
    const struct ck_handoff *h = hand;
    tsc_measure(); /* first: the night runs on this clock */
    ck_x86_step("clock");
    night_start(h);
    ck_x86_step("night");
    if (h->fb_status == CK_HANDOFF_FB_PRESENT && mm.fb_ok) {
        uint32_t scale, cols, rows;
        if (ck_x86_console_fb_on((volatile void *)(uintptr_t)h->fb_base, h->fb_width, h->fb_height,
                                 h->fb_pitch, h->fb_format, &scale, &cols, &rows) == 0)
            ck_printf("screen: gop %ux%u pitch=%u format=%s base=0x%llx map=wc scale=%u grid=%ux%u\n",
                      h->fb_width, h->fb_height, h->fb_pitch, h->fb_format ? "bgrx" : "rgbx",
                      (unsigned long long)h->fb_base, scale, cols, rows);
        else
            ck_puts("screen: refused (unusable geometry)\n");
        if (draw_run)
            ck_printf("screen: night: the sky made in %u ms (%u MiB drawing memory); stars, black holes, then Allen\n",
                      night_ms, night_mib);
    } else if (h->fb_status == CK_HANDOFF_FB_PRESENT) {
        ck_puts("screen: refused (framebuffer overlaps RAM or lies above the mapped range)\n");
    } else {
        ck_puts("screen: none\n");
    }
    uint64_t cr3 = ck_rd_cr3() & PTE_ADDR;
    ck_printf("paging: root=0x%llx firmware_cr3=0x%llx switched=%s top_gib=%llu pages_2m_wb=%llu "
              "pages_2m_wc=%llu pages_2m_uc=%llu pt_frames=%llu fb=%s pat1=wc\n",
              (unsigned long long)cr3, (unsigned long long)(h->firmware_ttbr0 & PTE_ADDR),
              cr3 == mm.root && cr3 != (h->firmware_ttbr0 & PTE_ADDR) ? "yes" : "no",
              (unsigned long long)(mm.top >> 30), (unsigned long long)mm.pages_wb, (unsigned long long)mm.pages_wc, (unsigned long long)mm.pages_uc,
              (unsigned long long)mm.pt_frames, mm.fb_ok ? "wc" : "unmapped");

    uint64_t hp;
    if (ck_frames_alloc(&frames, HEAP_PAGES, CK_PAGE, &hp) ||
        ck_heap_init(&heap, (void *)(uintptr_t)hp, HEAP_PAGES * CK_PAGE))
        ck_panic("heap: no region");
    void *a = ck_heap_alloc(&heap, 100), *b = ck_heap_alloc(&heap, 5000);
    if (!a || !b || ck_heap_free(&heap, a) || ck_heap_free(&heap, b) || ck_heap_check(&heap))
        ck_panic("heap: smoke test failed");
    ck_printf("memory: frame_ranges=%u free_bytes=%llu heap=0x%llx+%u heap_test=ok\n", frames.n,
              (unsigned long long)ck_frames_free_bytes(&frames), (unsigned long long)hp,
              (unsigned)(HEAP_PAGES * CK_PAGE));
    uint64_t ram = 0;
    for (uint64_t off = 0; off + hand->desc_size <= hand->map_size; off += hand->desc_size) {
        const struct ck_efi_desc *d = (const void *)(uintptr_t)(hand->memory_map + off);
        if (efi_ram(d->type))
            ram += d->pages * CK_PAGE;
    }
    ram_bytes = ram;
    /* the firmware keeps a little for itself, so whole GiB rounded up is what is fitted */
    ck_printf("ram: %llu MiB in the memory map, about %llu GiB fitted\n", (unsigned long long)(ram >> 20),
              (unsigned long long)((ram + (1ull << 30) - 1) >> 30));
    ck_printf("stack: 0x%llx-0x%llx guard=0x%llx ist1=0x%llx\n",
              (unsigned long long)(uintptr_t)(kstack + 4096),
              (unsigned long long)(uintptr_t)(kstack + sizeof kstack), (unsigned long long)(uintptr_t)kstack,
              (unsigned long long)(uintptr_t)(ist_stack + sizeof ist_stack));

    /* The guard page faults, and the fault is contained. */
    uint64_t g = (uint64_t)(uintptr_t)kstack;
    probe.hit = 0;
    probe.armed = 1;
    __asm__ volatile("lea 1f(%%rip), %%rax\n\t"
                     "mov %%rax, %0\n\t"
                     "movb (%1), %%al\n"
                     "1:"
                     : "=m"(probe.resume)
                     : "r"(g)
                     : "rax", "memory");
    probe.armed = 0;
    if (probe.hit && probe.cr2 == g && !(probe.err & 1))
        ck_printf("guard_page: ok fault=contained (#PF cr2=0x%llx error=0x%llx, not present)\n",
                  (unsigned long long)probe.cr2, (unsigned long long)probe.err);
    else
        ck_panic("guard_page: FAIL hit=%d cr2=0x%llx error=0x%llx", probe.hit, (unsigned long long)probe.cr2,
                 (unsigned long long)probe.err);

    wx_probes();

    if (!tsc_hz)
        ck_panic("timebase: no HPET and no CPUID leaf 0x16 frequency");
    ck_printf("timebase: %s period_fs=%llu tsc_hz=%llu\n", timebase, (unsigned long long)hpet_period_fs,
              (unsigned long long)tsc_hz);

    struct apic_report ar = {0};
    madt_walk(&ar);
    apic_start(&ar);
    ck_x86_step("timer");
    ck_printf("apic: mode=%s id=%u version=0x%x timer_hz=%u\n", "xapic", ar.id,
              ar.version, ar.lapic_hz);
    ck_printf("madt: cpus=%u ioapics=%u ioapic_lines_masked=%u\n", ar.cpus, ar.ioapics, ar.ioapic_lines);
    ck_printf("timer_irq: %llu ticks in %llu ms\n", (unsigned long long)ar.ticks, (unsigned long long)ar.ms);
    ck_printf("timer_stats: vector=0x%x period_us=10000 min_us=%llu avg_us=%llu max_us=%llu stray_irqs=%llu\n",
              VEC_TIMER, (unsigned long long)ar.min_us, (unsigned long long)ar.avg_us,
              (unsigned long long)ar.max_us, (unsigned long long)stray_irqs);
    if (ar.ticks < 20)
        ck_panic("timer: only %llu ticks", (unsigned long long)ar.ticks);
    rest_probe(ar.lapic_hz);

    struct smp_report sr = {0};
    smp_start(&ar, &sr);
    ck_x86_step("cores");
    wx_retire_trampoline();
    uint64_t wx, exec4k, text4k = (uint64_t)(__text_end - __text_start) >> 12;
    wx_audit(&wx, &exec4k);
    if (wx || exec4k != text4k)
        ck_panic("wx_audit: FAIL wx_leaves=%llu exec_pages=%llu text_pages=%llu", (unsigned long long)wx,
                 (unsigned long long)exec4k, (unsigned long long)text4k);
    ck_printf("wx_audit: ok wx_leaves=0 exec_pages=%llu text_pages=%llu pages_4k=%llu trampoline=retired\n",
              (unsigned long long)exec4k, (unsigned long long)text4k, (unsigned long long)mm.pages_4k);
    if (sr.why) {
        ck_printf("smp: cpus=%u started=0 (%s)\n", sr.listed, sr.why);
    } else {
        char ids[SMP_MAX * 4 + 1];
        unsigned n = 0;
        for (unsigned k = 0; k < sr.started; k++)
            n += (unsigned)ck_snprintf(ids + n, sizeof ids - n, k ? ",%u" : "%u", sr.up_ids[k]);
        ids[n] = 0;
        ck_printf("smp: cpus=%u started=%u failed=%u bsp_id=%u ap_ids=%s trampoline=0x%llx state=%s helpers=%u\n",
                  sr.listed, sr.started, sr.failed, ar.id, sr.started ? ids : "none",
                  (unsigned long long)tramp_pa, draw_on ? "drawing" : "halted", draw_helpers);
    }

    const struct ck_x86_storage_result *st = ck_x86_storage(h->rsdp, &frames, tsc_hz, mm.top);
    ck_x86_sound(&frames, tsc_hz, night_t0); /* before the keyboard: the chirp is already under way */
    ck_x86_step("sound");
    ck_x86_usb(&frames, tsc_hz);
    ck_x86_step("usb");
    { /* the display chip whose VRAM holds the framebuffer: panel_x86.c may switch its panel while Allen rests */
        volatile uint32_t *px;
        uint32_t w, h, pitch, format;
        if (ck_x86_console_fb_get(&px, &w, &h, &pitch, &format) == 0)
            ck_x86_display((uint64_t)(uintptr_t)px, (uint64_t)pitch * 4u * h, &frames, tsc_hz);
        else
            ck_x86_display(0, 0, &frames, tsc_hz);
    }
    ck_x86_swap_init(&frames);
    ck_local_init(mm.root, tsc_hz);
    ck_local_snapshot(sr.started + 1, ck_frames_free_bytes(&frames), h->fb_width, h->fb_height, st->net_link);
    ck_local_selftest();
    allen_facts.cores = sr.started + 1;
    allen_facts.sata_disks = st->ahci_disks;
    allen_facts.nvme_disks = st->nvme_disks;
    ck_printf("AIENOS-KERNEL-0 commit=%s\n", ck_commit());
    ck_puts("kernel: alive\n");
    ck_printf("summary: cores=%u/%u timer=ok sata_disks=%u nvme_disks=%u gpt_valid=%u writes=none net=%s\n",
              sr.started + 1, sr.listed, st->ahci_disks, st->nvme_disks, st->gpt_valid,
              !st->net_found          ? "none"
              : st->net_driver == 2   ? "driver-failed"
              : st->net_link == 1     ? "link-up"
              : st->net_link == 2     ? "link-down"
              : st->net_link == 3     ? "link-pending"
                                      : "seen");
    ck_puts("AIENOS-KERNEL-0 DONE\n");
    ck_puts("note: QEMU qualifies nothing physical\n");
    finish();
}

void ck_x86_kernel_entry(struct ck_handoff *h)
{
    ck_cli();
    hand = h;
    tables_load();
    ck_puts("\nreport_kind: kernel\n");
    if (CK_X86_WX_MUTATE)
        ck_printf("wx: MUTATED TEST BUILD (CK_X86_WX_MUTATE=%u): W^X deliberately broken, never deploy\n",
                  (unsigned)CK_X86_WX_MUTATE);
    ck_printf("exit_boot_services: ok attempts=%u\n", h->exit_attempts);
    cpu_line();
    ck_puts("descriptors: gdt=7 tss=yes ist1=df,nmi,mc idt=256\n");
    const char *why;
    if (ck_handoff_check(h, &why) != 0)
        ck_panic("handoff: %s", why);
    ck_puts("handoff: CHANDOF3 ok\n");
    mm_build();
    ck_x86_step("paging");
    ck_x86_call_on_stack(kernel_main, 0, (uint64_t)(uintptr_t)(kstack + sizeof kstack));
}
