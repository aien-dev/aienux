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

/* diag.h -- shared by the UEFI diagnostics (diag_main.c and its x86_64 part,
 * diag_x86.c). diag_say formats one report line onto ConOut and into the
 * report file buffer, exactly like diag_main.c's own say(). */
#ifndef AIENOS_DIAG_H
#define AIENOS_DIAG_H
#include "efi.h"

void diag_say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#if defined(__x86_64__)
/* Switch an Apple firmware console to text mode so ConOut shows (no-op elsewhere). */
void diag_x86_console(EFI_SYSTEM_TABLE *st);
/* CPUID identity, features and the mode the firmware left the CPU in. */
void diag_x86_cpu(void);
/* SMBIOS identity, PCI functions, GOP modes and whole-disk Block I/O devices. */
void diag_x86_devices(EFI_SYSTEM_TABLE *st);
#endif

#endif
