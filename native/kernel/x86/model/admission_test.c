/* Hosted signature/profile oracle. Actual memory isolation is tested in QEMU. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../artifact/ck_artifact.h"
#include "../local_bundle.h"
#include "model_bundle.h"
#include "sha256.h"
#define CHECK(v) do { if (!(v)) { fprintf(stderr,"model admission failed at %d\n",__LINE__); return 1; } checks++; } while (0)
int main(int argc, char **argv)
{
    unsigned checks = 0;
    CHECK(argc == 4);
    const struct ck_local_artifact *a = &ck_model_artifact;
    struct cka_parsed p; struct cka_ident id; uint8_t fp[32], hash[32];
    CHECK(!memcmp(ck_model_key, ck_local_keys[0], 32));
    CHECK(!cka_verify_profile(a->bytes,a->length,ck_local_keys,1,&p,&id,fp,2,3));
    sha256_hash(a->bytes,a->length,hash); CHECK(!memcmp(hash,a->hash,32));
    CHECK(cka_env_validate(&p.env,0) == CKA_RESOURCE_LIMIT);
    CHECK(!cka_env_validate_profile(&p.env,0,2,3));
    struct cka_env bad_env = p.env; bad_env.data_pages++;
    CHECK(cka_env_validate_profile(&bad_env,0,2,3) == CKA_RESOURCE_LIMIT);
    CHECK(cka_env_validate_profile(&p.env,1,2,3) == CKA_RESOURCE_LIMIT);
    CHECK(cka_parse(a->bytes,a->length,&p) == CKA_WRONG_TARGET);
    CHECK(cka_parse_profile(a->bytes,a->length,&p,2,2) == CKA_WRONG_ABI);
    CHECK(!cka_parse_profile(a->bytes,a->length,&p,2,3));
    for (unsigned i = 0; i < 3; i++) {
        FILE *f = fopen(argv[i+1],"rb"); CHECK(f);
        CHECK(!fseek(f,0,SEEK_END)); long size=ftell(f); CHECK(size>0); rewind(f);
        uint8_t *bytes=malloc((size_t)size); CHECK(bytes);
        CHECK(fread(bytes,1,(size_t)size,f) == (size_t)size); fclose(f);
        if (!i) CHECK((size_t)size == p.code.flen && !memcmp(bytes,a->bytes+p.payload_off,size));
        else {
            sha256_hash(bytes,(size_t)size,hash);
            CHECK(!memcmp(hash,a->bytes+p.payload_off+p.data.rel+32*(i-1),32));
            bytes[size/2] ^= 1; sha256_hash(bytes,(size_t)size,hash);
            CHECK(memcmp(hash,a->bytes+p.payload_off+p.data.rel+32*(i-1),32));
        }
        free(bytes);
    }
    uint8_t *mutant=malloc(a->length); CHECK(mutant);
    size_t offsets[]={14,192,240,p.payload_off+p.data.rel,a->length-1};
    for (unsigned i=0;i<sizeof offsets/sizeof offsets[0];i++) {
        memcpy(mutant,a->bytes,a->length); mutant[offsets[i]] ^= 1;
        CHECK(cka_verify_profile(mutant,a->length,ck_local_keys,1,&p,&id,fp,2,3) != 0);
    }
    free(mutant);
    printf("MODEL_ADMISSION: PASS checks=%u compiled_payload=1 signed_assets=2\n",checks);
    return 0;
}
