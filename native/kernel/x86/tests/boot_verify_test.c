/* boot_verify_test.c -- the iMac kernel's boot-section install (memory_x86.c boot_section)
 * must check the owner's signature before any disk write.
 *
 * The real memory_x86.c is compiled into this file, with the disk, the GPT, the FAT
 * installer and the loader hand-over mocked, and the real ck_slot_verify, Ed25519 and
 * SHA-512 linked. The mocks count: write windows opened (ck_x86_ahci_allow), GPT
 * partitions added (ck_gpt_add) and installs (ck_fatb_install). Every object the owner
 * did not sign must leave all three at zero. Built for x86-64 (the kernel uses rdtsc).
 * Keys: the RFC 8032 test vectors 1 and 2, public. No real key, no disk.
 *
 * Prints "BOOT_VERIFY: PASS" or "BOOT_VERIFY: FAIL <why>". */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include MEMORY_X86_C

#include "../../../argus/sha256.h"

extern void bv_set_keys(const uint8_t (*keys)[32], unsigned n);

/* RFC 8032 section 7.1, tests 1 and 2 */
static const uint8_t SK1[32] = {0x9d, 0x61, 0xb1, 0x9d, 0xef, 0xfd, 0x5a, 0x60, 0xba, 0x84, 0x4a, 0xf4, 0x92, 0xec, 0x2c, 0xc4,
                                0x44, 0x49, 0xc5, 0x69, 0x7b, 0x32, 0x69, 0x19, 0x70, 0x3b, 0xac, 0x03, 0x1c, 0xae, 0x7f, 0x60};
static const uint8_t SK2[32] = {0x4c, 0xcd, 0x08, 0x9b, 0x28, 0xff, 0x96, 0xda, 0x9d, 0xb6, 0xc3, 0x46, 0xec, 0x11, 0x4e, 0x0f,
                                0x5b, 0x8a, 0x31, 0x9f, 0x35, 0xab, 0xa6, 0x24, 0xda, 0x8c, 0xf6, 0xed, 0x4f, 0xb8, 0xa6, 0xfb};

/* ---- mocks --------------------------------------------------------------- */
static unsigned n_allow, n_allow_none, n_add, n_install;
static const uint8_t *inst_man, *inst_img;
static uint32_t inst_len;
static struct nb_pass pass;
static int have_pass;
static int part_rc; /* what ck_gpt_find_type answers: CK_GPT_OK (a boot partition is there) or CK_GPT_ENOPART */
static disk_dev fake_disk;
const uint8_t ck_esp_part_type[16] = {0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11, 0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b};

void ck_printf(const char *fmt, ...) { (void)fmt; }
void ck_puts(const char *s) { (void)s; }
const struct nb_pass *ck_x86_netldr_pass(int *dropped)
{
    *dropped = 0;
    return have_pass ? &pass : 0;
}
int ck_x86_ahci_allow(unsigned index, uint64_t first, uint64_t last)
{
    (void)index, (void)first, (void)last;
    n_allow++;
    return 0;
}
void ck_x86_ahci_allow_none(unsigned index)
{
    (void)index;
    n_allow_none++;
}
int ck_gpt_find_type(const disk_dev *d, const uint8_t type[16], ck_part *p, ck_gpt_info *info)
{
    (void)d, (void)type;
    memset(p, 0, sizeof *p);
    memset(info, 0, sizeof *info);
    info->first_usable = 34;
    info->last_usable = 1000000;
    info->last_lba = 1000033;
    if (part_rc == CK_GPT_OK || n_add) { /* the disk's own boot partition, or the one just added */
        p->first_lba = 4096;
        p->blocks = 524288;
        p->valid = 1;
        return CK_GPT_OK;
    }
    return part_rc;
}
int ck_gpt_is_ours(const disk_dev *d, const ck_gpt_new *spec, uint32_t index)
{
    (void)d, (void)spec, (void)index;
    return 1;
}
const char *ck_gpt_add_strerror(int rc)
{
    (void)rc;
    return "mock";
}
int ck_gpt_add(const disk_dev *d, const ck_gpt_new *spec, uint64_t blocks, uint64_t align, ck_gpt_added *out)
{
    (void)d, (void)spec, (void)blocks, (void)align;
    n_add++;
    memset(out, 0, sizeof *out);
    return CK_GPT_OK;
}
int ck_fatb_state_read(const disk_dev *p, uint32_t serial, ck_fatb_state *st)
{
    (void)p, (void)serial, (void)st;
    return CK_FATB_EIO;
}
const char *ck_fatb_strerror(int rc)
{
    (void)rc;
    return "mock";
}
void ck_fatb_dos_time(uint64_t unix_s, uint16_t *date, uint16_t *time)
{
    (void)unix_s;
    *date = *time = 0;
}
int ck_fatb_install(const disk_dev *p, uint32_t serial, int format_ok, const uint8_t *man, const uint8_t *img,
                    uint32_t img_len, uint16_t dd, uint16_t tt, ck_fatb_result *out)
{
    (void)p, (void)serial, (void)format_ok, (void)dd, (void)tt;
    n_install++;
    inst_man = man, inst_img = img, inst_len = img_len;
    memset(out, 0, sizeof *out);
    out->written = 1;
    return CK_FATB_OK;
}

/* ---- objects ------------------------------------------------------------- */
#define IMG_LEN 3000u
static uint8_t object[CK_SLOT_UNIT + IMG_LEN];

/* A well-formed manifest and image. sign_with: the secret key that signs it (0: leave the
 * signature zero). claims: the public key whose id goes in signer_id. */
static void build(const uint8_t *sign_with, const uint8_t claims_pk[32])
{
    struct ck_slot_manifest *m = (struct ck_slot_manifest *)object;
    uint8_t *img = object + CK_SLOT_UNIT;
    memset(object, 0, sizeof object);
    for (unsigned i = 0; i < IMG_LEN; i++)
        img[i] = (uint8_t)(i * 7u + 1u);
    memcpy(m->magic, CK_SLOT_MANIFEST_MAGIC, 8);
    m->version = CK_SLOT_MANIFEST_VERSION;
    m->payload_kind = CK_SLOT_PAYLOAD_BOOTAA64;
    m->loader_abi = CK_SLOT_LOADER_ABI;
    m->kernel_abi = CK_SLOT_KERNEL_ABI;
    m->generation = 5;
    m->policy_generation = 5;
    m->min_rollback_generation = 1;
    m->image_len = IMG_LEN;
    aienos_sha512(img, IMG_LEN, m->image_sha512);
    memcpy(m->kernel_sha512, m->image_sha512, 64);
    sha256_hash(claims_pk, 32, m->signer_id);
    memset(m->commit, 'a', sizeof m->commit);
    memcpy(m->provenance, "boot_verify_test", 16);
    m->timestamp = 1;
    if (sign_with && aienos_ed25519_sign(m->signature, object, CK_SLOT_TBS_BYTES, sign_with) != AIENOS_SIG_OK) {
        printf("BOOT_VERIFY: FAIL the test signer failed\n");
        exit(1);
    }
    pass.loader_tsc = 1, pass.start_tsc = 2;
    pass.object = (uint64_t)(uintptr_t)object;
    pass.object_len = sizeof object;
    have_pass = 1;
}

static unsigned failures;
#define EXPECT(cond, ...)                                                                                          \
    do {                                                                                                           \
        if (!(cond)) {                                                                                             \
            failures++;                                                                                            \
            printf("BOOT_VERIFY: FAIL ");                                                                          \
            printf(__VA_ARGS__);                                                                                   \
            printf("\n");                                                                                          \
        }                                                                                                          \
    } while (0)

static void run(const char *label, int rc_part, int expect_install)
{
    n_allow = n_allow_none = n_add = n_install = 0;
    inst_man = inst_img = 0;
    part_rc = rc_part;
    boot_section(&fake_disk);
    const char *where = rc_part == CK_GPT_OK ? "existing boot partition" : "no boot partition yet";
    if (!expect_install) {
        EXPECT(n_allow == 0, "%s (%s): %u write window(s) opened, want 0", label, where, n_allow);
        EXPECT(n_add == 0, "%s (%s): %u partition(s) added, want 0", label, where, n_add);
        EXPECT(n_install == 0, "%s (%s): installed, want refused", label, where);
    } else {
        EXPECT(n_install == 1, "%s (%s): %u installs, want exactly 1", label, where, n_install);
        EXPECT(n_allow >= 1 && n_allow_none >= 1, "%s (%s): window opened %u / closed %u", label, where, n_allow,
               n_allow_none);
        EXPECT(inst_man == object && inst_img == object + CK_SLOT_UNIT && inst_len == IMG_LEN,
               "%s (%s): the installer got other bytes than the verified object", label, where);
    }
}

int main(void)
{
    uint8_t pk1[32], pk2[32];
    uint8_t both[2][32];
    aienos_ed25519_public_key(pk1, SK1);
    aienos_ed25519_public_key(pk2, SK2);
    memcpy(both[0], pk1, 32);
    memcpy(both[1], pk2, 32);
    fake_disk.block_size = 512;
    fake_disk.block_count = 2000000;

    const int modes[2] = {CK_GPT_OK, CK_GPT_ENOPART};
    for (unsigned k = 0; k < 2; k++) {
        int pm = modes[k];
        /* the key set holds the owner key 1 for every refusal below */
        bv_set_keys(both, 1);

        build(0, pk1); /* self-consistent, well formed, SHA-512 matches, never signed */
        run("unsigned self-consistent manifest", pm, 0);

        build(SK1, pk1);
        ((uint8_t *)object)[CK_SLOT_UNIT + 17] ^= 0x40; /* image changed after signing */
        run("tampered image", pm, 0);

        build(SK1, pk1);
        ((struct ck_slot_manifest *)object)->generation = 6; /* manifest field changed after signing */
        run("tampered manifest field", pm, 0);

        build(SK2, pk2); /* signed by a key that is not the owner's */
        run("signed by the wrong owner", pm, 0);

        build(SK2, pk1); /* claims the owner's id, signed by someone else */
        run("wrong signature under the owner's id", pm, 0);

        bv_set_keys(both, 0); /* an image built without owner keys */
        build(SK1, pk1);
        run("valid signature, empty key set", pm, 0);

        bv_set_keys(both, 1);
        build(SK1, pk1);
        run("owner-signed", pm, 1);
        if (pm == CK_GPT_ENOPART)
            EXPECT(n_add == 1, "owner-signed with no boot partition yet: %u partitions added, want 1", n_add);
    }
    if (failures)
        return 1;
    printf("BOOT_VERIFY: PASS\n");
    return 0;
}
