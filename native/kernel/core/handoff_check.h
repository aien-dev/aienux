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

/* handoff_check.h -- pure validator of the loader -> kernel handoff record
 * (docs/BOOT_HANDOFF_CONTRACT.md section 6, CHANDOF3). No I/O, no
 * globals the caller can see; host tested by tests/test_handoff.c. */
#ifndef AIENOS_CK_HANDOFF_CHECK_H
#define AIENOS_CK_HANDOFF_CHECK_H

#include "handoff.h"

/* Largest memory map copy a loader may hand over (efi_main.c MAP_BYTES). */
#define CK_HANDOFF_MAP_MAX (64u << 10)
/* Smallest UEFI descriptor stride the kernel parses (struct ck_efi_desc,
 * mm/frames.h). */
#define CK_HANDOFF_DESC_MIN 40u
/* UEFI EFI_MEMORY_DESCRIPTOR_VERSION. */
#define CK_HANDOFF_DESC_VERSION 1u

/* Returns 0 when h is an acceptable CHANDOF3 record. Otherwise returns -1
 * and sets *why to the refusal reason without the "handoff: " prefix
 * (for example "bad magic"); the kernel panics with "handoff: <why>".
 * *why may point at a static buffer overwritten by the next call. */
int ck_handoff_check(const struct ck_handoff *h, const char **why);

#endif
