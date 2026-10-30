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

/* Constant-time helpers: volatile wipe and equality (same construction as
 * native/crypto/ct.c, separate symbols so both libraries can be linked). */
#include "sig_internal.h"

void aienos_sig_wipe(void *p, size_t n)
{
    volatile uint8_t *v = (volatile uint8_t *)p;
    for (size_t i = 0; i < n; i++)
        v[i] = 0;
    __asm__ volatile("" : : "r"(p) : "memory");
}

int aienos_sig_ct_equal(const uint8_t *a, const uint8_t *b, size_t n)
{
    uint32_t diff = 0;
    for (size_t i = 0; i < n; i++)
        diff |= (uint32_t)(a[i] ^ b[i]);
    AIENOS_SIG_BARRIER(diff);
    /* (diff - 1) >> 31 is 1 iff diff == 0 (diff <= 0xff). */
    return (int)((diff - 1u) >> 31);
}
