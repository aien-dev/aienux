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

/* Internal hooks shared by the library objects and the test. Not public API. */
#ifndef AIENOS_CRYPTO_INTERNAL_H
#define AIENOS_CRYPTO_INTERNAL_H

#include <stdint.h>

/* Constant-time AES S-box on all 16 bytes of s, in place. */
void aienos_aes_sub_bytes16(uint8_t s[16]);

/* Optimization barrier: the compiler must treat v as unknown, so a 0/1
 * value cannot be turned back into a branch. */
#define AIENOS_CT_BARRIER(v) __asm__ volatile("" : "+r"(v))

#endif
