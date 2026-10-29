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

/* klog_boot.h -- what the UEFI stub found about the log slot
 * (native/boot/efi_klog.c), read by the kernel after ExitBootServices
 * (core/klog_flush.c). Raspberry Pi 5 build only (CK_KLOG). */
#ifndef AIENOS_CK_KLOG_BOOT_H
#define AIENOS_CK_KLOG_BOOT_H
#include <stdint.h>
#include "klog_slot.h"

enum { CK_KLOG_BOOT_NONE = 0, CK_KLOG_BOOT_ABSENT = 1, CK_KLOG_BOOT_REFUSED = 2, CK_KLOG_BOOT_FOUND = 3 };

struct ck_klog_boot {
    int state;                /* CK_KLOG_BOOT_* */
    struct ck_klog_map map;   /* checked map (state FOUND) */
    uint64_t disk_last_block; /* firmware LastBlock of the disk that carries the slot */
};
extern struct ck_klog_boot ck_klog_boot;
#endif
