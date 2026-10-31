/* ck_gsp_sysinfo_encode -- host tool: the 936-byte GspSystemInfo payload of GSP_SET_SYSTEM_INFO (function 72), the
 * first message GSP-RM reads from the command queue (L332 rung 2 window 1; L192 gb10_gsp_boot.h takes these bytes as
 * gsp_boot_in.system_info).
 *
 * The layout is NVIDIA's (open-gpu-kernel-modules 580.173.02, MIT): GspSystemInfo
 * src/nvidia/inc/kernel/gpu/gsp/gsp_static_config.h:172-227 with GSP_VF_INFO :45-55 and GSP_PCIE_CONFIG_REG :58-62,
 * BUSINFO src/nvidia/generated/g_chipset_nvoc.h:220-227, ACPI_METHOD_DATA src/nvidia/inc/kernel/gpu/gpu_acpi_data.h:72-115
 * (NV0073_CTRL_SYSTEM_ACPI_ID_MAP_MAX_DISPLAYS 16, ctrl0073system.h:747). Natural LP64 layout, no pack pragma.
 * The mirror below is our own declaration of the same fields; `make gsp-sysinfo-nvhdr-test` compiles it against those
 * NVIDIA lines (cut out of the pinned tree at build time, never vendored) and compares every offset and size.
 *
 * The values are data, not code: rpcGspSetSystemInfo_v17_00 (src/nvidia/src/kernel/vgpu/rpc.c:10493) fills them from
 * the bus, the chipset object and the OS. They come in as a text file, one "field value" per line ("#" comments; field
 * names are the C names, nested ones with a dot, e.g. FHBBusInfo.deviceID; values decimal or 0x hex). Every field not
 * named is 0. The window-1 values and their sources are in evidence/own-gpu-driver/rung2/system_info.txt.
 *
 *   ck_gsp_sysinfo_encode encode IN.txt OUT.bin   writes 936 bytes, prints each field set; exit 2 + REFUSED on an
 *                                                 unknown field, a repeated field, a value that does not fit, bad syntax
 *   ck_gsp_sysinfo_encode fields                  prints every field with its offset and size
 *   ck_gsp_sysinfo_encode selftest                layout anchors, a known encoding, every refusal
 * Hosted C, no outside libraries. Never part of an image. */
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SI_ACPI_MAX_DISPLAYS 16

typedef struct { uint16_t deviceID, vendorID, subdeviceID, subvendorID; uint8_t revisionID; } si_businfo;
typedef struct { uint32_t status, acpiIdListLen, acpiIdList[SI_ACPI_MAX_DISPLAYS]; } si_dod;
typedef struct { uint32_t status, jtCaps; uint16_t jtRevId; uint8_t bSBIOSCaps; } si_jt;
typedef struct { uint32_t acpiId, mode, status; } si_mux_elem;
typedef struct {
    uint32_t tableLen;
    si_mux_elem acpiIdMuxModeTable[SI_ACPI_MAX_DISPLAYS], acpiIdMuxPartTable[SI_ACPI_MAX_DISPLAYS],
        acpiIdMuxStateTable[SI_ACPI_MAX_DISPLAYS];
} si_mux;
typedef struct { uint32_t status, optimusCaps; } si_caps;
typedef struct { uint8_t bValid; si_dod dodMethodData; si_jt jtMethodData; si_mux muxMethodData; si_caps capsMethodData; } si_acpi;
typedef struct {
    uint32_t totalVFs, firstVFOffset;
    uint64_t FirstVFBar0Address, FirstVFBar1Address, FirstVFBar2Address;
    uint8_t b64bitBar0, b64bitBar1, b64bitBar2;
} si_vf;
typedef struct { uint32_t linkCap; } si_pcie;

typedef struct {
    uint64_t gpuPhysAddr, gpuPhysFbAddr, gpuPhysInstAddr, gpuPhysIoAddr, nvDomainBusDeviceFunc, simAccessBufPhysAddr,
        notifyOpSharedSurfacePhysAddr, pcieAtomicsOpMask, consoleMemSize, maxUserVa;
    uint32_t pciConfigMirrorBase, pciConfigMirrorSize, PCIDeviceID, PCISubDeviceID, PCIRevisionID,
        pcieAtomicsCplDeviceCapMask;
    uint8_t oorArch;
    uint64_t clPdbProperties;
    uint32_t Chipset;
    uint8_t bGpuBehindBridge, bFlrSupported, b64bBar0Supported, bMnocAvailable;
    uint32_t chipsetL1ssEnable;
    uint8_t bUpstreamL0sUnsupported, bUpstreamL1Unsupported, bUpstreamL1PorSupported, bUpstreamL1PorMobileOnly,
        bSystemHasMux, upstreamAddressValid;
    si_businfo FHBBusInfo, chipsetIDInfo;
    si_acpi acpiMethodData;
    uint32_t hypervisorType;
    uint16_t virtualConfigBits;
    uint8_t bIsPassthru;
    uint64_t sysTimerOffsetNs;
    si_vf gspVFInfo;
    uint8_t bIsPrimary, bIsUnixHdmiFrlComplianceEnabled, isGridBuild;
    si_pcie pcieConfigReg;
    uint32_t gridBuildCsp;
    uint8_t bPreserveVideoMemoryAllocations, bTdrEventSupported, bFeatureStretchVblankCapable,
        bEnableDynamicGranularityPageArrays, bClockBoostSupported;
    uint64_t hostPageSize;
    uint8_t bIsCmcBasedHws, bGspNocatEnabled, bS0ixSupport, bWindowChannelAlwaysMapped;
} si_mirror;

/* Anchors also recorded by ck_gsp_rpc_encode (inventory section 13) from compiling the NVIDIA struct text. */
_Static_assert(sizeof(si_mirror) == 936, "GspSystemInfo is 936 bytes");
_Static_assert(sizeof(si_businfo) == 10 && sizeof(si_acpi) == 676 && sizeof(si_vf) == 40, "nested sizes");
_Static_assert(offsetof(si_mirror, FHBBusInfo) == 138 && offsetof(si_mirror, chipsetIDInfo) == 148 &&
               offsetof(si_mirror, acpiMethodData) == 160 && offsetof(si_mirror, hypervisorType) == 836 &&
               offsetof(si_mirror, gspVFInfo) == 856 && offsetof(si_mirror, hostPageSize) == 920 &&
               offsetof(si_mirror, bWindowChannelAlwaysMapped) == 931, "GspSystemInfo anchors");

#define F(f) {#f, offsetof(si_mirror, f), sizeof(((si_mirror *)0)->f)}
#define FN(s, f) {#s "." #f, offsetof(si_mirror, s) + offsetof(__typeof__(((si_mirror *)0)->s), f), \
                  sizeof(((si_mirror *)0)->s.f)}
static const struct field { const char *name; uint32_t off, size; } fields[] = {
    F(gpuPhysAddr), F(gpuPhysFbAddr), F(gpuPhysInstAddr), F(gpuPhysIoAddr), F(nvDomainBusDeviceFunc),
    F(simAccessBufPhysAddr), F(notifyOpSharedSurfacePhysAddr), F(pcieAtomicsOpMask), F(consoleMemSize), F(maxUserVa),
    F(pciConfigMirrorBase), F(pciConfigMirrorSize), F(PCIDeviceID), F(PCISubDeviceID), F(PCIRevisionID),
    F(pcieAtomicsCplDeviceCapMask), F(oorArch), F(clPdbProperties), F(Chipset), F(bGpuBehindBridge), F(bFlrSupported),
    F(b64bBar0Supported), F(bMnocAvailable), F(chipsetL1ssEnable), F(bUpstreamL0sUnsupported),
    F(bUpstreamL1Unsupported), F(bUpstreamL1PorSupported), F(bUpstreamL1PorMobileOnly), F(bSystemHasMux),
    F(upstreamAddressValid),
    FN(FHBBusInfo, deviceID), FN(FHBBusInfo, vendorID), FN(FHBBusInfo, subdeviceID), FN(FHBBusInfo, subvendorID),
    FN(FHBBusInfo, revisionID),
    FN(chipsetIDInfo, deviceID), FN(chipsetIDInfo, vendorID), FN(chipsetIDInfo, subdeviceID),
    FN(chipsetIDInfo, subvendorID), FN(chipsetIDInfo, revisionID),
    FN(acpiMethodData, bValid), FN(acpiMethodData, dodMethodData.status), FN(acpiMethodData, dodMethodData.acpiIdListLen),
    FN(acpiMethodData, jtMethodData.status), FN(acpiMethodData, capsMethodData.status),
    F(hypervisorType), F(virtualConfigBits), F(bIsPassthru), F(sysTimerOffsetNs),
    FN(gspVFInfo, totalVFs), FN(gspVFInfo, firstVFOffset), FN(gspVFInfo, FirstVFBar0Address),
    FN(gspVFInfo, FirstVFBar1Address), FN(gspVFInfo, FirstVFBar2Address), FN(gspVFInfo, b64bitBar0),
    FN(gspVFInfo, b64bitBar1), FN(gspVFInfo, b64bitBar2),
    F(bIsPrimary), F(bIsUnixHdmiFrlComplianceEnabled), F(isGridBuild), FN(pcieConfigReg, linkCap), F(gridBuildCsp),
    F(bPreserveVideoMemoryAllocations), F(bTdrEventSupported), F(bFeatureStretchVblankCapable),
    F(bEnableDynamicGranularityPageArrays), F(bClockBoostSupported), F(hostPageSize), F(bIsCmcBasedHws),
    F(bGspNocatEnabled), F(bS0ixSupport), F(bWindowChannelAlwaysMapped),
};
#define NFIELDS (sizeof(fields) / sizeof(fields[0]))

/* Encode the text in src into out[936]. Returns 0, or -1 with a reason in why. */
static int encode(const char *src, uint8_t out[936], char *why, size_t whylen, FILE *echo)
{
    uint8_t seen[NFIELDS] = {0};
    memset(out, 0, 936);
    unsigned line = 0;
    for (const char *p = src; *p;) {
        const char *eol = strchr(p, '\n');
        size_t n = eol ? (size_t)(eol - p) : strlen(p);
        char buf[512];
        line++;
        if (n >= sizeof buf) { snprintf(why, whylen, "line %u too long", line); return -1; }
        memcpy(buf, p, n);
        buf[n] = 0;
        p += n + (eol ? 1 : 0);
        char *h = strchr(buf, '#');
        if (h) *h = 0;
        char name[128], val[128], extra[8];
        int k = sscanf(buf, "%127s %127s %7s", name, val, extra);
        if (k <= 0) continue;
        if (k != 2) { snprintf(why, whylen, "line %u: want \"field value\"", line); return -1; }
        size_t i = 0;
        while (i < NFIELDS && strcmp(fields[i].name, name)) i++;
        if (i == NFIELDS) { snprintf(why, whylen, "line %u: unknown field %s", line, name); return -1; }
        if (seen[i]) { snprintf(why, whylen, "line %u: field %s repeated", line, name); return -1; }
        seen[i] = 1;
        char *end;
        errno = 0;
        unsigned long long v = strtoull(val, &end, 0);
        if (errno || *end || val[0] == '-') { snprintf(why, whylen, "line %u: %s is not a number", line, val); return -1; }
        if (fields[i].size < 8 && v >> (8 * fields[i].size)) {
            snprintf(why, whylen, "line %u: %.40s does not fit %.60s (%u bytes)", line, val, name, fields[i].size);
            return -1;
        }
        for (uint32_t b = 0; b < fields[i].size; b++) out[fields[i].off + b] = (uint8_t)(v >> (8 * b));   /* little endian */
        if (echo) fprintf(echo, "set %-36s off=%3u size=%u value=%#llx\n", name, fields[i].off, fields[i].size, v);
    }
    return 0;
}

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *b = NULL;
    size_t n = 0, cap = 0, r;
    char tmp[4096];
    while ((r = fread(tmp, 1, sizeof tmp, f)) > 0) {
        if (n + r + 1 > cap) { cap = (n + r + 1) * 2; b = realloc(b, cap); if (!b) { fclose(f); return NULL; } }
        memcpy(b + n, tmp, r);
        n += r;
    }
    fclose(f);
    if (!b) b = calloc(1, 1);
    else b[n] = 0;
    return b;
}

static int selftest(void)
{
    int fail = 0, n = 0;
    uint8_t out[936];
    char why[256];
#define CHECK(c, msg) do { n++; if (!(c)) { printf("FAIL %s\n", msg); fail = 1; } else printf("PASS %s\n", msg); } while (0)
    CHECK(encode("gpuPhysAddr 0x24000000\nPCIDeviceID 0x2e1210de # GB10\n\nFHBBusInfo.deviceID 0x22d1\n"
                 "hostPageSize 4096\nbWindowChannelAlwaysMapped 1\n", out, why, sizeof why, NULL) == 0,
          "valid text accepted");
    uint8_t want[936] = {0};
    want[3] = 0x24;
    want[88] = 0xde; want[89] = 0x10; want[90] = 0x12; want[91] = 0x2e;
    want[138] = 0xd1; want[139] = 0x22;
    want[921] = 0x10;
    want[931] = 1;
    CHECK(memcmp(out, want, 936) == 0, "bytes land at the NVIDIA offsets, little endian");
    static const struct { const char *text, *want; } bad[] = {
        {"gpuPhysAdr 1\n", "unknown field"},
        {"Chipset 1\nChipset 2\n", "repeated"},
        {"oorArch 256\n", "does not fit"},
        {"FHBBusInfo.deviceID 0x10000\n", "does not fit"},
        {"PCIRevisionID 0x100000000\n", "does not fit"},
        {"maxUserVa 0x10000000000000000\n", "not a number"},
        {"bIsPrimary -1\n", "not a number"},
        {"bIsPrimary 1x\n", "not a number"},
        {"bIsPrimary\n", "want"},
        {"bIsPrimary 1 2\n", "want"},
        {"acpiMethodData.dodMethodData 1\n", "unknown field"},
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        char msg[300];
        int rc = encode(bad[i].text, out, why, sizeof why, NULL);
        snprintf(msg, sizeof msg, "refused \"%.*s\": %s", (int)strcspn(bad[i].text, "\n"), bad[i].text, rc ? why : "ACCEPTED");
        CHECK(rc == -1 && strstr(why, bad[i].want), msg);
    }
    /* every field distinct and inside the struct */
    int overlap = 0;
    for (size_t i = 0; i < NFIELDS; i++)
        for (size_t j = i + 1; j < NFIELDS; j++)
            if (fields[i].off < fields[j].off + fields[j].size && fields[j].off < fields[i].off + fields[i].size) overlap = 1;
    CHECK(!overlap, "no two fields overlap");
    /* ACPI statuses: dod at 160+4, jt after dod's 16-entry list, caps right before hypervisorType (836) */
    CHECK(encode("acpiMethodData.dodMethodData.status 0xffff\nacpiMethodData.jtMethodData.status 0x56\n"
                 "acpiMethodData.capsMethodData.status 0x56\n", out, why, sizeof why, NULL) == 0 &&
              out[164] == 0xff && out[165] == 0xff && out[236] == 0x56 && out[828] == 0x56,
          "ACPI status fields land at 164, 236 and 828");
    printf("CK_GSP_SYSINFO_ENCODE_SELFTEST: %s (%d checks, %zu fields)\n", fail ? "FAIL" : "PASS", n, NFIELDS);
    return fail;
}

#ifdef CK_SYSINFO_NVHDR
/* make gsp-sysinfo-nvhdr-test: the NVIDIA struct text, cut out of the pinned tree by the Makefile. */
#include "nvtypes.h"
#include "nvstatus.h"
#include CK_SYSINFO_NVHDR
#define SAME(f) (offsetof(si_mirror, f) == offsetof(GspSystemInfo, f) && \
                 sizeof(((si_mirror *)0)->f) == sizeof(((GspSystemInfo *)0)->f))
static int nvhdr(void)
{
    int fail = sizeof(si_mirror) != sizeof(GspSystemInfo);
    int n = 1;
#define S(f) do { n++; if (!SAME(f)) { printf("FAIL %s\n", #f); fail = 1; } } while (0)
    S(gpuPhysAddr); S(gpuPhysFbAddr); S(gpuPhysInstAddr); S(gpuPhysIoAddr); S(nvDomainBusDeviceFunc);
    S(simAccessBufPhysAddr); S(notifyOpSharedSurfacePhysAddr); S(pcieAtomicsOpMask); S(consoleMemSize); S(maxUserVa);
    S(pciConfigMirrorBase); S(pciConfigMirrorSize); S(PCIDeviceID); S(PCISubDeviceID); S(PCIRevisionID);
    S(pcieAtomicsCplDeviceCapMask); S(oorArch); S(clPdbProperties); S(Chipset); S(bGpuBehindBridge); S(bFlrSupported);
    S(b64bBar0Supported); S(bMnocAvailable); S(chipsetL1ssEnable); S(bUpstreamL0sUnsupported);
    S(bUpstreamL1Unsupported); S(bUpstreamL1PorSupported); S(bUpstreamL1PorMobileOnly); S(bSystemHasMux);
    S(upstreamAddressValid); S(FHBBusInfo); S(FHBBusInfo.deviceID); S(FHBBusInfo.vendorID); S(FHBBusInfo.subdeviceID);
    S(FHBBusInfo.subvendorID); S(FHBBusInfo.revisionID); S(chipsetIDInfo); S(chipsetIDInfo.revisionID);
    S(acpiMethodData); S(acpiMethodData.bValid); S(acpiMethodData.muxMethodData); S(acpiMethodData.capsMethodData);
    S(acpiMethodData.dodMethodData.status); S(acpiMethodData.dodMethodData.acpiIdListLen);
    S(acpiMethodData.jtMethodData.status); S(acpiMethodData.capsMethodData.status);
    S(hypervisorType); S(virtualConfigBits); S(bIsPassthru); S(sysTimerOffsetNs); S(gspVFInfo);
    S(gspVFInfo.FirstVFBar2Address); S(gspVFInfo.b64bitBar2); S(bIsPrimary); S(bIsUnixHdmiFrlComplianceEnabled);
    S(isGridBuild); S(pcieConfigReg); S(pcieConfigReg.linkCap); S(gridBuildCsp); S(bPreserveVideoMemoryAllocations);
    S(bTdrEventSupported); S(bFeatureStretchVblankCapable); S(bEnableDynamicGranularityPageArrays);
    S(bClockBoostSupported); S(hostPageSize); S(bIsCmcBasedHws); S(bGspNocatEnabled); S(bS0ixSupport);
    S(bWindowChannelAlwaysMapped);
    printf("GSP_SYSINFO_NVHDR: %s (%d comparisons, sizeof GspSystemInfo %zu)\n", fail ? "FAIL" : "PASS", n,
           sizeof(GspSystemInfo));
    return fail;
}
#endif

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "selftest")) return selftest();
#ifdef CK_SYSINFO_NVHDR
    if (argc == 2 && !strcmp(argv[1], "nvhdr")) return nvhdr();
#endif
    if (argc == 2 && !strcmp(argv[1], "fields")) {
        for (size_t i = 0; i < NFIELDS; i++) printf("%-36s off=%3u size=%u\n", fields[i].name, fields[i].off, fields[i].size);
        return 0;
    }
    if (argc == 4 && !strcmp(argv[1], "encode")) {
        char *src = slurp(argv[2]), why[256];
        uint8_t out[936];
        if (!src) { printf("REFUSED: cannot read %s\n", argv[2]); return 2; }
        if (encode(src, out, why, sizeof why, stdout)) { printf("REFUSED: %s\n", why); return 2; }
        /* 0644, not fopen's 0666 & ~umask: the payload goes into the window-1 package and must not be world-writable. */
        int fd = open(argv[3], O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        FILE *f = fd < 0 ? NULL : fdopen(fd, "wb");
        if (fd >= 0 && !f) close(fd);
        if (!f || fwrite(out, 1, sizeof out, f) != sizeof out || fclose(f)) { printf("REFUSED: cannot write %s\n", argv[3]); return 2; }
        printf("RESULT OK %s 936 bytes\n", argv[3]);
        return 0;
    }
    fprintf(stderr, "usage: ck_gsp_sysinfo_encode encode IN.txt OUT.bin | fields | selftest\n");
    return 1;
}
