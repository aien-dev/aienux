/* boot_verify_keys.c -- the owner key table memory_x86.c reads (its extern
 * ck_slot_loader_keys / ck_slot_loader_nkeys), held here so boot_verify_test.c
 * can change it between runs. A separate file: memory_x86.c declares both const. */
#include <stdint.h>
#include <string.h>

uint8_t ck_slot_loader_keys[4][32];
unsigned ck_slot_loader_nkeys;

void bv_set_keys(const uint8_t (*keys)[32], unsigned n)
{
    memset(ck_slot_loader_keys, 0, sizeof ck_slot_loader_keys);
    for (unsigned i = 0; i < n && i < 4; i++)
        memcpy(ck_slot_loader_keys[i], keys[i], 32);
    ck_slot_loader_nkeys = n;
}
