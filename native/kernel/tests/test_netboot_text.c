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

/* test_netboot_text.c -- nb_text_len (native/boot/netboot_proto.h), the
 * length of the DONE text NETLDR.EFI sends. Negative fixture for the CodeQL
 * alert "array offset used before range check" (net_loader.c done()): a text
 * of exactly NB_TEXT_MAX bytes with no NUL sits at the end of a heap block,
 * so reading text[NB_TEXT_MAX] is a heap overflow that the sanitize build
 * (make sanitize, ASan) reports. */
#include <stdlib.h>
#include <string.h>

#include "ck_test.h"
#include "netboot_proto.h"

static char *block(size_t n, size_t nul_at)
{
    char *p = malloc(n);
    if (!p)
        abort();
    memset(p, 'A', n);
    if (nul_at < n)
        p[nul_at] = 0;
    return p;
}

int main(void)
{
    char *p = block(NB_TEXT_MAX, (size_t)-1); /* unterminated, exactly at the cap */
    CHECK(nb_text_len(p) == NB_TEXT_MAX);
    free(p);
    p = block(NB_TEXT_MAX + 40, (size_t)-1); /* unterminated, longer than the cap */
    CHECK(nb_text_len(p) == NB_TEXT_MAX);
    free(p);
    p = block(NB_TEXT_MAX, NB_TEXT_MAX - 1); /* NUL in the last byte */
    CHECK(nb_text_len(p) == NB_TEXT_MAX - 1);
    free(p);
    p = block(1, 0);
    CHECK(nb_text_len(p) == 0);
    free(p);
    CHECK(nb_text_len("verified, starting") == 18);
    return ck_t_verdict("test_netboot_text");
}
