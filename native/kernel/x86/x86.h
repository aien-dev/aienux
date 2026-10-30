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

/* x86.h -- x86_64 machine access for the AIENOS-KERNEL-0 kernel
 * (native/kernel/x86): port and MSR I/O, CPUID, TSC, control registers,
 * the trap frame trap.S builds, and the entry points between the files.
 * Everything is compiled -mabi=ms (UEFI calling convention, like the stub),
 * so trap.S passes the first argument in rcx. */
#ifndef AIENOS_CK_X86_H
#define AIENOS_CK_X86_H
#include <stddef.h>
#include <stdint.h>
#include "disk.h"
#include "../svc/allen_reply.h"

/* Read-only firmware inventory; never establishes DMA protection. */
void ck_x86_dmar_inventory(uint64_t rsdp);

static inline void ck_outb(uint16_t p, uint8_t v) { __asm__ volatile("outb %0, %1" ::"a"(v), "Nd"(p)); }
static inline void ck_outw(uint16_t p, uint16_t v) { __asm__ volatile("outw %0, %1" ::"a"(v), "Nd"(p)); }
static inline uint8_t ck_inb(uint16_t p)
{
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(p));
    return v;
}
static inline uint16_t ck_inw(uint16_t p)
{
    uint16_t v;
    __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(p));
    return v;
}
static inline uint64_t ck_rdmsr(uint32_t m)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(m));
    return (uint64_t)hi << 32 | lo;
}
static inline void ck_wrmsr(uint32_t m, uint64_t v)
{
    __asm__ volatile("wrmsr" ::"c"(m), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)) : "memory");
}
static inline void ck_cpuid(uint32_t leaf, uint32_t sub, uint32_t r[4])
{
    __asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(sub));
}
static inline uint64_t ck_rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (uint64_t)hi << 32 | lo;
}
#define CK_RD_CR(n)                                                                                \
    static inline uint64_t ck_rd_cr##n(void)                                                       \
    {                                                                                              \
        uint64_t v;                                                                                \
        __asm__ volatile("mov %%cr" #n ", %0" : "=r"(v));                                         \
        return v;                                                                                  \
    }
CK_RD_CR(0)
CK_RD_CR(2)
CK_RD_CR(3)
CK_RD_CR(4)
#undef CK_RD_CR
static inline void ck_wr_cr3(uint64_t v) { __asm__ volatile("mov %0, %%cr3" ::"r"(v) : "memory"); }
static inline void ck_cli(void) { __asm__ volatile("cli" ::: "memory"); }
static inline void ck_sti(void) { __asm__ volatile("sti" ::: "memory"); }
static inline void ck_pause(void) { __asm__ volatile("pause"); }

/* What trap.S pushes, lowest address first: the general registers, the
 * vector and error code (0 for vectors without one), then the CPU frame. */
struct ck_x86_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8, rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error;
    uint64_t rip, cs, rflags, rsp, ss;
};
_Static_assert(sizeof(struct ck_x86_frame) == 22 * 8, "ck_x86_frame size");

/* trap.S */
extern char ck_x86_isr_stubs[]; /* 256 stubs, 16 bytes apart */
#define CK_X86_ISR_STRIDE 16u
void ck_x86_load_gdt(const void *gdtr, uint16_t code_sel, uint16_t data_sel, uint16_t tss_sel);
__attribute__((noreturn)) void ck_x86_call_on_stack(void (*fn)(void *), void *arg, uint64_t sp);
void ck_x86_trap(struct ck_x86_frame *f); /* kmain.c, called by every stub */

/* console_x86.c: COM1 if a 16550 answers there, the GOP framebuffer once
 * the kernel turned it on, and a RAM copy of everything written so far that
 * is replayed onto the screen when it comes on. */
int ck_x86_com1_probe(void);
/* Allen's courier on the second serial port (x86/courier_x86.c): NULL when no 16550 answers at COM2. */
struct ck_courier;
const struct ck_courier *ck_x86_courier_init(void);
int ck_x86_console_fb_on(volatile void *base, uint32_t width, uint32_t height, uint32_t pitch,
                         uint32_t format, uint32_t *scale, uint32_t *cols, uint32_t *rows);

/* console_x86.c: the RAM copy of everything written so far (the first
 * 64 KiB); in the lab image, the input the network report filters. The
 * second call counts the bytes written after that copy filled. */
const char *ck_x86_console_log(size_t *len);
size_t ck_x86_console_log_dropped(void);
/* console_x86.c: the screen for another drawer (Allen's window). get: its
 * framebuffer (pitch in pixels, format 0 RGBX / 1 BGRX), or -1 when there is
 * none; off: console text stops drawing on it and goes on to the RAM copy and
 * COM1 only. */
int ck_x86_console_fb_get(volatile uint32_t **px, uint32_t *width, uint32_t *height, uint32_t *pitch,
                          uint32_t *format);
void ck_x86_console_fb_off(void);
/* console_x86.c: the startup picture. hold, before fb_on: the screen is kept
 * as it is and text stays off it; tick is called after each console write
 * (never from inside itself) so the picture can move on. show: the text
 * comes back (screen cleared, everything so far replayed), for anything
 * that is not Allen's window: a panic, a fault, a machine without a
 * keyboard. Once Allen's window has the screen (off), show does nothing. */
void ck_x86_console_fb_hold(void (*tick)(void));
void ck_x86_console_fb_show(void);

/* pci_x86.c: PCI walk over the MCFG ECAM window; every AHCI and NVMe disk
 * read-only (identify, read, GPT check), a look at the Ethernet chip. */
struct ck_frames;
struct ck_x86_storage_result {
    unsigned pci_functions, ahci_ctrls, ahci_disks, nvme_ctrls, nvme_disks, gpt_valid;
    unsigned net_found, net_link; /* net_link: 0 not read, 1 up, 2 down, 3 not up yet (driver running) */
    unsigned net_driver;          /* 0 not tried, 1 running, 2 failed */
};
const struct ck_x86_storage_result *ck_x86_storage(uint64_t rsdp, struct ck_frames *fr, uint64_t tsc_hz,
                                                   uint64_t map_top);
/* The PCI functions that walk found, in order (at most 64): 0, or -1 past the last.
 * cls is class, subclass and interface (0x030000: a VGA display controller). */
int ck_x86_pci_fn(unsigned i, uint16_t *vendor, uint16_t *device, uint32_t *cls);
/* kmain_x86.c: maps [pa, pa + len) uncached when it lies above the identity map. */
int ck_x86_map_mmio(uint64_t pa, uint64_t len);
/* ahci_x86.c: one AHCI controller at abar, polled; read-only until a write
 * range is opened (only x86/memory_x86.c does). */
void ck_x86_ahci_start(unsigned ctrl, uint64_t abar, struct ck_frames *fr, uint64_t tsc_hz);
unsigned ck_x86_ahci_disks(void);
/* The disk_dev of AHCI disk index (NULL past the last) and its IDENTIFY model. */
const disk_dev *ck_x86_ahci_dev(unsigned index, const char **model);
/* Opens one more write range [first, last] (never LBA 0; at most 3): 0 or -1. */
int ck_x86_ahci_allow(unsigned index, uint64_t first, uint64_t last);
/* Closes every write range of the disk. */
void ck_x86_ahci_allow_none(unsigned index);
#ifdef CK_X86_SWAP_TEST_DISK
int ck_x86_ahci_read_lba(unsigned ctrl, unsigned disk, uint64_t lba, unsigned count, uint8_t *buf);
#endif

/* efi_x86.c: what the network loader handed this kernel (boot/netboot_proto.h),
 * or 0 when it was not started by the loader (the firmware started it from a
 * disk, or another loader). *object_dropped: the loader named an object that
 * was not in its own memory, so it was ignored. */
struct nb_pass;
const struct nb_pass *ck_x86_netldr_pass(int *object_dropped);

/* kmain_x86.c: marks the end of a start-up step (TSC now) for boot_time's
 * breakdown; name is a short static string. */
void ck_x86_step(const char *name);

/* memory_x86.c: Allen's memory and notes on a hard disk (svc/allen_memory.h).
 * open: finds the disk holding Allen's section (or adds one on an approved
 * disk, in unused space), lists its partitions, installs the network loader's
 * verified kernel on the boot section beside it (fat_boot.h), loads the newest copy into
 * *mem and the notes it counts into *notes. 1 when saves will reach the disk,
 * 0 when Allen remembers only until power off. save, notes_write (note index,
 * written before the save that counts it): 0 or -1 (then nothing more is
 * written this boot). */
int ck_x86_memory_open(ck_allen_memory *mem, ck_allen_notes *notes, uint64_t tsc_hz);
int ck_x86_memory_save(const ck_allen_memory *mem);
/* The AIENOS partition, for the setup wizard's records (svc/setup_store.h) at
 * units CK_SETUP_FIRST and +1, inside the open write range; NULL when Allen
 * keeps nothing this start or the section has no room for them. */
const disk_dev *ck_x86_memory_part(void);
int ck_x86_notes_write(const ck_allen_notes *notes, uint32_t index);

/* pci_x86.c: offers every xHCI controller the walk found to xhci_x86.c until
 * one has a USB boot keyboard; prints "keyboard: ready (...)" or why none.
 * Call after ck_x86_storage. */
void ck_x86_usb(struct ck_frames *fr, uint64_t tsc_hz);
/* pci_x86.c: hands every High Definition Audio controller the walk found to
 * hda_x86.c (Intel PCI settings first); the first with a speaker path plays
 * the startup chirp from the moment the night (begun at TSC night_t0; 0 for
 * no night) has reached. Call after ck_x86_storage. hda_x86.c start: stops
 * any DMA the firmware left running, resets the controller, maps each codec
 * onto the report, opens every output path; with play set, also runs the
 * chirp. bar = its registers (mapped), memory decode and bus master on. 1 when
 * playing, 0 when ready but not playing, -1 when not ready. step: stops the
 * stream once the chirp is over; cheap, call it often. */
void ck_x86_sound(struct ck_frames *fr, uint64_t tsc_hz, uint64_t night_t0);
int ck_x86_hda_start(unsigned ctrl, volatile uint8_t *bar, struct ck_frames *fr, uint64_t tsc_hz, uint64_t night_t0,
                     int play);
void ck_x86_hda_step(void);
/* pci_x86.c: hands the display chip the walk found (the one whose VRAM holds
 * the framebuffer at fb_pa, fb_bytes long) to panel_x86.c; with none, says so.
 * Call after ck_x86_storage. */
void ck_x86_display(uint64_t fb_pa, uint64_t fb_bytes, struct ck_frames *fr, uint64_t tsc_hz);

/* gpu/gpu_x86.c: AMD Polaris GPU driver entry point (CK_X86_GPU=1).
 * Called from pci_x86.c when a Polaris display chip is found.
 * bar0_pa = VRAM aperture PA; bar5_pa = register aperture PA (already mapped
 * by panel_x86.c).  fb_pa, fb_w, fb_h, fb_pitch describe the GOP framebuffer.
 * Stubs out to a log line when CK_X86_GPU=0. */
void ck_x86_gpu_start(volatile uint8_t *cfg, unsigned bus, unsigned dev, unsigned fn,
                      uint16_t vendor, uint16_t device,
                      uint64_t bar0_pa, uint64_t bar5_pa,
                      uint64_t fb_pa, uint32_t fb_w, uint32_t fb_h, uint32_t fb_pitch,
                      struct ck_frames *fr, uint64_t tsc_hz);
/* panel_x86.c: the built-in display panel, switched off for real while Allen
 * rests and on again when he wakes (built in with CK_X86_PANEL_OFF=1; otherwise,
 * or on a machine without the AMD chip it knows, every call says so once and
 * does nothing). start: the display chip's config space (NULL: none found) and
 * identity, plus the framebuffer; prints what it found and whether the panel
 * can be switched. ready: 1 when it can. off / on: 0 when every step took,
 * -1 when the panel was left alone (each step is logged). */
void ck_x86_panel_start(volatile uint8_t *cfg, unsigned bus, unsigned dev, unsigned fn, uint16_t vendor,
                        uint16_t device, uint32_t cls, uint64_t fb_pa, uint64_t fb_bytes, uint64_t tsc_hz);
int ck_x86_panel_ready(void);
int ck_x86_panel_off(void);
int ck_x86_panel_on(void);
/* xhci_x86.c: the USB keyboard, polled. start: bar0 = the controller's
 * registers (mapped), memory decode and bus master on; 0 when a keyboard is
 * running. hold_poll: handles waiting keys (echo, line editing, a
 * "keyboard_line: ..." line on Enter) and, while keys arrive, moves *end (a
 * TSC deadline) out to idle_s after the last key, capped. */
int ck_x86_xhci_start(unsigned ctrl, volatile uint8_t *bar0, struct ck_frames *fr, uint64_t tsc_hz);
int ck_x86_kbd_ready(void);
void ck_x86_kbd_hold_poll(uint64_t *end);
unsigned ck_x86_kbd_idle_s(void);
/* Ends the typing session at the end of the hold: "keyboard: done (lines=N)" on
 * its own line (once; nothing when no keyboard ran). */
void ck_x86_kbd_done(void);
/* Where typed text goes besides the console: edit(buf, len) after every key
 * that changed the line, line(text) after each Enter (after its
 * "keyboard_line:" line). Either may be NULL. */
void ck_x86_kbd_sink(void (*edit)(const char *buf, unsigned len), void (*line)(const char *text));
void ck_x86_kbd_discard(void); /* drop the line being typed (the key that woke Allen) */
/* Every key, before the line editor (enum ck_key_kind, c for a character): a
 * nonzero return takes it whole, so it is never echoed or logged (the setup
 * wizard's key entry). NULL: keys go to the line as before. */
void ck_x86_kbd_keys(int (*key)(int kind, char c));
/* fwcfg_x86.c, launcher image only (CK_X86_HOSTDESC=1): reads the QEMU fw_cfg file "name"
 * (read-only; only when the DSDT names the device):
 * 0 whole, 1 cut at cap, -1 none. */
int ck_x86_fwcfg_file(uint64_t rsdp, const char *name, char *buf, unsigned cap, unsigned *len);
/* setup_x86.c: Allen's setup wizard in his window. open: reads the saved record
 * and starts the wizard unless an earlier start finished it (again: start over
 * whatever is saved); 1 when it is up. key: every key while it is up (1:
 * taken). line: a courier line, 1 when it was the key test's answer. step:
 * the clock, in the hold loop. */
struct ck_courier;
struct ck_x86_setup_facts {
    uint64_t rsdp, ram_bytes;
    unsigned cores;
    uint32_t screen_w, screen_h;
    uint32_t starts; /* Allen's starts with this memory, this one counted (1: the first) */
};
int ck_x86_setup_open(const struct ck_x86_setup_facts *f, const struct ck_courier *c, uint64_t tsc_hz, int again);
int ck_x86_setup_on(void);
int ck_x86_setup_key(int kind, char c);
int ck_x86_setup_line(const char *line);
void ck_x86_setup_step(void);
/* kmain_x86.c: one step of the screen hold: waiting keys (which may move *end
 * out) and the window's animation. Called in every hold loop. */
void ck_x86_hold_step(uint64_t *end);

/* tg3_x86.c: the Broadcom BCM57766 Ethernet chip, transmit only, polled.
 * start: regs = BAR0 (mapped), cfg = its ECAM config space; 0 when the chip
 * is up (the link may still be negotiating). The rest report on or use it. */
int ck_x86_tg3_start(volatile uint8_t *regs, volatile uint8_t *cfg, struct ck_frames *fr, uint64_t tsc_hz);
int ck_x86_tg3_ready(void);
int ck_x86_tg3_link(void);
void ck_x86_tg3_mac(uint8_t mac[6]);
#if defined(CK_X86_REPORT) && CK_X86_REPORT
/* Lab image only (make imac-kernel CK_X86_REPORT=1); absent from the default
 * and any resident or service build. */
int ck_x86_tg3_send(const uint8_t *frame, size_t len);
int ck_x86_tg3_drain(unsigned ms);
/* The next received frame (lab image only): bytes copied to out, 0 when none waits. */
size_t ck_x86_tg3_recv(uint8_t *out, size_t cap);
/* kmain_x86.c (lab image only): Allen's courier over Ethernet (courier_udp.h)
 * bound to the chip; NULL when the chip is not up. */
const struct ck_courier *ck_x86_courier_udp(uint64_t tsc_hz);
/* report_x86.c (lab only): holds `seconds`, sending the allowlisted console
 * lines as unauthenticated UDP broadcasts every 2 s while the link is up;
 * returns the rounds sent. */
unsigned ck_x86_report_hold(uint64_t tsc_hz, unsigned seconds);
/* The GPU bench's receive (lab only): 0 with *len the UDP payload of a datagram for port 47035 (headers
 * stripped) and src_mac its sender, 0 with *len 0 when none waits, -1 when the chip is not up. Frames for
 * the other reader (ck_x86_tg3_recv) are kept for it, and the other way round. */
int  ck_x86_tg3_recv_bench(uint8_t *pkt, size_t cap, size_t *len, uint8_t src_mac[6]);
/* UDP broadcast send helper. */
void ck_x86_udp_send(const uint8_t dst_mac[6], const uint8_t *pkt, size_t len,
                      uint16_t src_port, uint16_t dst_port);
/* Live swap entry: the trampoline (swap_tramp.S) calls the new image's PE entry, _start, with this
 * value where UEFI puts the ImageHandle (RCX) and the handoff where it puts the SystemTable (RDX).
 * It is not a canonical address, so no firmware handle can equal it; efi_main then skips the
 * firmware and enters the kernel (efi_x86.c). */
#define CK_X86_SWAP_ENTRY_MAGIC 0x5357415041454E49ull /* "INEAPAWS" */
/* Set by efi_main when this kernel was entered by a live swap, not by the firmware. */
extern int ck_x86_swapped_in;
/* Swap helpers (swap_x86.c, exposed to kmain_x86.c). */
void ck_x86_swap_stop_aps(void);
uint64_t ck_x86_tsc_hz(void);
void ck_x86_xhci_stop_all(void);
void ck_x86_tg3_stop(void);
void ck_x86_hda_stop(void);
#endif

/* smp_x86.S: the secondary-core trampoline, copied below 1 MiB and patched. */
extern char ck_x86_tramp_start[], ck_x86_tramp_end[], ck_x86_tramp_gdt[], ck_x86_tramp_pm32[],
    ck_x86_tramp_lm64[], ck_x86_tramp_gdtr[], ck_x86_tramp_far32[], ck_x86_tramp_far64[], ck_x86_tramp_cr3[],
    ck_x86_tramp_stack[], ck_x86_tramp_entry[], ck_x86_tramp_arg[];

#endif
