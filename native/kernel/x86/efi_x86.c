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

/* efi_x86.c -- UEFI stub of AIENOS-KERNEL-0 (x86_64): the same job as
 * native/boot/efi_main.c does for the AArch64 kernel, on the same CHANDOF3
 * record (handoff.h), so core/handoff_check.c checks it unchanged.
 *
 * It finds the ACPI 2.0 RSDP and the GOP framebuffer (boot/efi_gop.c), takes
 * the final memory map, leaves boot services (retrying when the map key goes
 * stale) and jumps to ck_x86_kernel_entry. It never calls a runtime service.
 * When the network loader started it, it keeps the loader's struct nb_pass
 * (boot/netboot_proto.h, from LoadOptions) and drops its object unless that
 * lies wholly in EfiLoaderData of the final memory map.
 * x86 meaning of the AArch64-named fields: firmware_el = CPL (0),
 * firmware_ttbr0 = CR3, firmware_sctlr = CR0, uefi_entry_ticks = TSC,
 * counter_freq_hz = 0 (the kernel measures the TSC itself). */
#include "../../boot/efi.h"
#include "../../boot/netboot_proto.h"
#include "../core/ck_internal.h"
#include "x86.h"

#define MAP_BYTES (64u << 10)
#define EXIT_TRIES 8

extern char __image_base[], __image_end[];
__attribute__((noreturn)) void ck_x86_kernel_entry(struct ck_handoff *h);
void ck_boot_gop_find(EFI_SYSTEM_TABLE *st, struct ck_handoff *h);

static uint64_t map_buf[MAP_BYTES / 8];
static struct ck_handoff handoff;
static struct nb_pass pass; /* from the network loader, when it started this kernel */
static int pass_state;      /* 0 none, 1 taken, 2 taken but its object was not in loader memory (dropped) */

const struct nb_pass *ck_x86_netldr_pass(int *object_dropped)
{
    if (object_dropped)
        *object_dropped = pass_state == 2;
    return pass_state ? &pass : 0;
}

/* The loader's object must lie wholly inside one EfiLoaderData range of the
 * final memory map (pages nothing in this kernel allocates from). */
static int in_loader_data(const struct ck_handoff *h, uint64_t a, uint64_t n)
{
    for (uint64_t off = 0; off + h->desc_size <= h->map_size; off += h->desc_size) {
        const EFI_MEMORY_DESCRIPTOR *m = (const void *)((const uint8_t *)map_buf + off);
        uint64_t bytes = m->NumberOfPages * 4096u;
        if (m->Type == 2 && a >= m->PhysicalStart && n <= bytes && a - m->PhysicalStart <= bytes - n)
            return 1;
    }
    return 0;
}

static int guid_eq(const EFI_GUID *a, const EFI_GUID *b)
{
    return memcmp(a, b, sizeof *a) == 0;
}

/* The night's empty sky (ui/night.c, no stars yet: about 2, 2, 6) over the
 * whole screen, before ExitBootServices: the firmware's own shutdown takes
 * about 1.2 s on the real machine, and the firmware's picture should not be
 * what is seen meanwhile. Eight bytes at a time, two pixels per store. */
static void night_ground(const struct ck_handoff *h)
{
    if (h->fb_status != CK_HANDOFF_FB_PRESENT || !h->fb_base || h->fb_pitch < h->fb_width ||
        (uint64_t)h->fb_pitch * h->fb_height * 4u > h->fb_size)
        return;
    uint64_t px = h->fb_format == CK_HANDOFF_FB_BGRX ? 0x00020206u : 0x00060202u;
    px |= px << 32;
    for (uint32_t y = 0; y < h->fb_height; y++) {
        volatile uint64_t *row = (volatile uint64_t *)(uintptr_t)(h->fb_base + (uint64_t)y * h->fb_pitch * 4u);
        for (uint32_t x = 0; x + 1u < h->fb_width; x += 2u)
            row[x / 2u] = px;
        if (h->fb_width & 1u)
            ((volatile uint32_t *)row)[h->fb_width - 1u] = (uint32_t)px;
    }
}

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
    struct ck_handoff *h = &handoff;
    uint16_t cs;
    __asm__ volatile("mov %%cs, %0" : "=r"(cs));
    h->magic = CK_HANDOFF_MAGIC;
    h->firmware_el = cs & 3u;
    h->uefi_entry_ticks = ck_rdtsc();
    h->firmware_ttbr0 = ck_rd_cr3();
    h->firmware_sctlr = ck_rd_cr0();
    h->image_base = (uint64_t)(uintptr_t)__image_base;
    h->image_end = (uint64_t)(uintptr_t)__image_end;
    h->commit = ck_commit();
    h->memory_map = (uint64_t)(uintptr_t)map_buf;

    ck_x86_com1_probe();
    if (st->BootServices->SetWatchdogTimer)
        st->BootServices->SetWatchdogTimer(0, 0, 0, 0);
    static const EFI_GUID li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID_INIT;
    EFI_LOADED_IMAGE_PROTOCOL *li = 0;
    if (st->BootServices->HandleProtocol(image, &li_guid, (void **)&li) == EFI_SUCCESS && li && li->LoadOptions &&
        li->LoadOptionsSize == sizeof pass && !memcmp(li->LoadOptions, NB_PASS_MAGIC, 8)) {
        memcpy(&pass, li->LoadOptions, sizeof pass);
        pass_state = 1;
    }
    static const EFI_GUID acpi20 = EFI_ACPI_20_TABLE_GUID_INIT;
    for (uint64_t i = 0; i < st->NumberOfTableEntries; i++)
        if (guid_eq(&st->ConfigurationTable[i].VendorGuid, &acpi20))
            h->rsdp = (uint64_t)(uintptr_t)st->ConfigurationTable[i].VendorTable;
    ck_x86_step("start");

    ck_puts("AIENOS-KERNEL-0\n");
    ck_printf("report_kind: pre_exit\naienos_commit: %s\n", ck_commit());
    ck_puts("kernel_impl: c (native/kernel/x86, x86_64)\n");
    char v[80];
    size_t n = 0;
    for (; st->FirmwareVendor && n < sizeof v - 1 && st->FirmwareVendor[n]; n++)
        v[n] = st->FirmwareVendor[n] < 0x80 ? (char)st->FirmwareVendor[n] : '?';
    v[n] = 0;
    ck_printf("firmware_vendor: %s\nfirmware_revision: 0x%x\n", v, st->FirmwareRevision);
    ck_printf("firmware_cpl: %u\n", h->firmware_el);
    ck_boot_gop_find(st, h);
    ck_x86_step("gop");
    night_ground(h);
    ck_x86_step("paint");
    ck_printf("acpi_rsdp: 0x%llx\n", (unsigned long long)h->rsdp);
    ck_printf("image: 0x%llx-0x%llx\n", (unsigned long long)h->image_base, (unsigned long long)h->image_end);
    ck_puts("exiting firmware boot services\n");

    EFI_BOOT_SERVICES *bs = st->BootServices;
    EFI_STATUS s = EFI_INVALID_PARAMETER;
    for (unsigned tries = 1; tries <= EXIT_TRIES; tries++) {
        uint64_t size = sizeof map_buf, key = 0, dsize = 0;
        uint32_t dver = 0;
        s = bs->GetMemoryMap(&size, (EFI_MEMORY_DESCRIPTOR *)map_buf, &key, &dsize, &dver);
        if (s != EFI_SUCCESS) {
            ck_printf("exit_boot_services: GetMemoryMap status=0x%llx size=%llu\n", (unsigned long long)s,
                      (unsigned long long)size);
            return s;
        }
        h->map_size = size;
        h->desc_size = dsize;
        h->desc_version = dver;
        h->exit_attempts = tries;
        s = bs->ExitBootServices(image, key);
        if (s == EFI_SUCCESS)
            break;
    }
    if (s != EFI_SUCCESS) {
        ck_printf("exit_boot_services: failed status=0x%llx after %u tries\n", (unsigned long long)s,
                  EXIT_TRIES);
        return s;
    }
    ck_x86_step("exit");
    if (pass_state && pass.object && !in_loader_data(h, pass.object, pass.object_len)) {
        pass.object = pass.object_len = 0;
        pass_state = 2;
    }
    ck_x86_kernel_entry(h);
}
