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

/* slot_control.h -- commit one AIENOS slot control record (slot_format.h)
 * through the torn-slot protocol (native/store/torn_slot.c) and prove it
 * landed: ts_commit (body, flush, commit header, flush), then ts_recover and
 * a byte compare of what came back. The one control-commit routine shared by
 * SLOTLDR.EFI (native/boot/slot_loader.c, candidate tries), the kernel
 * (native/kernel/svc/slot_service.c, promote / abandon / stage) and the host
 * tool (ck_slot_tool control-commit). Freestanding, no allocation. */
#ifndef AIENOS_CK_SLOT_CONTROL_H
#define AIENOS_CK_SLOT_CONTROL_H
#include "slot_format.h"
#include "../store/torn_slot.h"

/* r must have been recovered (ts_recover) before the first commit. Returns
 * TS_OK with *seq_out = the committed sequence, or the torn-slot error, or
 * TS_CORRUPT when the record read back differs from the one committed. */
int ck_slot_control_commit(ts_region *r, const struct ck_slot_control *c, uint64_t *seq_out);

#endif
