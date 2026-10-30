/* fwcfg_x86.c -- one named file from QEMU's firmware configuration device
 * (fw_cfg), read-only: how a launcher (aienos-run, or a QEMU test) hands the
 * kernel text about the computer around it (svc/setup_inv.h).
 *
 * Nothing is assumed: the device is used only when the ACPI DSDT names it
 * (_HID "QEMU0002") with an I/O port descriptor, and its signature reads
 * "QEMU". Registers per QEMU docs/specs/fw_cfg.rst: on x86 the selector is
 * I/O port 0x510 (16 bit, little endian; a selector below 0x4000 only reads)
 * and the data port is the next one (8 bit, offset advances by one per
 * read); key 0x0000 is the signature, key 0x0019 the file directory: a
 * big-endian count, then 64-byte entries {size BE32, select BE16, reserved,
 * name[56]}. On the iMac there is no such device and nothing is touched. */
#include "x86.h"
#include "../core/acpi.h"

static uint16_t sel_port;

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t be32(const uint8_t *b) { return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3]; }

/* The DSDT's QEMU0002 device and the base of its I/O port descriptor
 * (small resource 0x47: info, min lo/hi, max lo/hi, align, length); 0 if none. */
static uint16_t port_from_dsdt(uint64_t rsdp)
{
    const uint8_t *fadt = rsdp ? ck_acpi_lookup(rsdp, "FACP") : 0;
    if (!fadt)
        return 0;
    uint32_t flen = rd32(fadt + 4);
    uint64_t dsdt = 0;
    if (flen >= 148)
        dsdt = (uint64_t)rd32(fadt + 140) | (uint64_t)rd32(fadt + 144) << 32;
    if (!dsdt && flen >= 44)
        dsdt = rd32(fadt + 40);
    if (!dsdt)
        return 0;
    const uint8_t *d = (const uint8_t *)(uintptr_t)dsdt;
    if (d[0] != 'D' || d[1] != 'S' || d[2] != 'D' || d[3] != 'T')
        return 0;
    uint32_t len = rd32(d + 4);
    static const char hid[] = "QEMU0002";
    for (uint32_t i = 36; i + 8 <= len; i++) {
        unsigned k = 0;
        while (k < 8 && d[i + k] == (uint8_t)hid[k])
            k++;
        if (k < 8)
            continue;
        for (uint32_t j = i + 8; j + 8 <= len && j < i + 8 + 96; j++)
            if (d[j] == 0x47 && d[j + 7] >= 2) {
                uint16_t lo = (uint16_t)(d[j + 2] | d[j + 3] << 8), hi = (uint16_t)(d[j + 4] | d[j + 5] << 8);
                if (lo && lo == hi)
                    return lo;
            }
    }
    return 0;
}

static void fw_select(uint16_t key) { ck_outw(sel_port, key); }
static void fw_read(uint8_t *out, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        out[i] = ck_inb((uint16_t)(sel_port + 1));
}

int ck_x86_fwcfg_file(uint64_t rsdp, const char *name, char *buf, unsigned cap, unsigned *len)
{
    *len = 0;
    if (!sel_port)
        sel_port = port_from_dsdt(rsdp);
    if (!sel_port)
        return -1;
    uint8_t b[64];
    fw_select(0x0000);
    fw_read(b, 4);
    if (b[0] != 'Q' || b[1] != 'E' || b[2] != 'M' || b[3] != 'U') {
        sel_port = 0;
        return -1;
    }
    fw_select(0x0019);
    fw_read(b, 4);
    uint32_t n = be32(b);
    unsigned nl = 0;
    while (name[nl])
        nl++;
    for (uint32_t i = 0; i < n && i < 4096; i++) {
        fw_read(b, 64);
        b[63] = 0;
        unsigned k = 0;
        while (k < nl && b[8 + k] == (uint8_t)name[k])
            k++;
        if (k < nl || b[8 + nl])
            continue;
        uint32_t size = be32(b);
        uint16_t key = (uint16_t)(b[4] << 8 | b[5]);
        if (key >= 0x4000)
            return -1; /* only ever a read */
        fw_select(key);
        uint32_t take = size < cap ? size : cap;
        fw_read((uint8_t *)buf, take);
        *len = take;
        return size > cap ? 1 : 0;
    }
    return -1;
}
