/* Hosted verification oracle. No host execution substitutes for CPL3 gates. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../artifact/ck_artifact.h"
#include "../local_bundle.h"
#include "sha256.h"
#define CHECK(v) do { if (!(v)) { fprintf(stderr,"local admission failed at %d\n",__LINE__); return 1; } count++; } while (0)
int main(int argc, char **argv)
{
    unsigned count = 0;
    CHECK(argc == 4);
    struct cka_parsed p; struct cka_ident id; uint8_t fp[32], hash[32];
    for (unsigned i = 0; i < 3; i++) {
        const struct ck_local_artifact *a = &ck_local_artifacts[i];
        CHECK(!cka_verify_profile(a->bytes,a->length,ck_local_keys,1,&p,&id,fp,2,2));
        sha256_hash(a->bytes,a->length,hash); CHECK(!memcmp(hash,a->hash,32));
        CHECK(cka_parse(a->bytes,a->length,&p) == CKA_WRONG_TARGET);
        CHECK(!cka_parse_profile(a->bytes,a->length,&p,2,2));
        FILE *f = fopen(argv[i+1],"rb"); CHECK(f);
        uint8_t raw[65537]; size_t n = fread(raw,1,sizeof raw,f); CHECK(!ferror(f)); fclose(f);
        CHECK(n == p.code.flen && !memcmp(raw,a->bytes+p.payload_off,n));
        CHECK(p.ncaps == 0 && p.env.max_caps == 0 && p.env.code_pages == 16 &&
              p.env.data_pages == 32 && p.env.stack_pages == 16 && p.env.cpu == 100000 &&
              p.env.elapsed == 500000 && p.env.syscalls == 1);
        CHECK(cka_verify_profile(a->bytes,a->length,ck_local_keys,0,&p,&id,fp,2,2) == CKA_UNTRUSTED_SIGNER);
        CHECK(cka_verify_profile(a->bytes,a->length,ck_local_keys,1,&p,&id,fp,2,1) == CKA_WRONG_ABI);
        for (size_t short_len=0; short_len<a->length; short_len++)
            CHECK(cka_parse_profile(a->bytes,short_len,&p,2,2) != 0);
        uint8_t *bad = malloc(a->length); CHECK(bad);
        memcpy(bad,a->bytes,a->length); bad[a->length-1] ^= 1;
        CHECK(cka_verify_profile(bad,a->length,ck_local_keys,1,&p,&id,fp,2,2) == CKA_BAD_SIGNATURE);
        memcpy(bad,a->bytes,a->length); bad[242] ^= 1;
        CHECK(cka_verify_profile(bad,a->length,ck_local_keys,1,&p,&id,fp,2,2) != 0);
        free(bad);
    }
    printf("LOCAL_ADMISSION: PASS checks=%u compiled_payloads=3\n",count);
    return 0;
}
