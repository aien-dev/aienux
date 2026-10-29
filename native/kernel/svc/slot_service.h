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

/* slot_service.h -- the kernel side of the AIENOS image slots (native v1
 * plan P11 cut 2b, gate N7; docs/NATIVE_IMAGE_SLOTS.md "Kernel promotion").
 *
 * Linked only into a CK_SERVICE=1 image. SLOTLDR.EFI hands the kernel one
 * boot record (native/boot/slot_format.h, struct ck_slot_boot_record) in a
 * page of memory type CK_SLOT_BOOT_MEMTYPE; ck_slot_boot_capture copies it in
 * ck_kernel_entry, before the kernel builds its own page tables. The
 * lifecycle then calls:
 *   ck_slot_service_start(healthy)   once, after the lifecycle reached its
 *                                     first state: abandons an exhausted or
 *                                     invalid candidate, promotes a healthy one;
 *   ck_slot_service_health(healthy)  after a successful `unlock`: promotes a
 *                                     candidate that was waiting for health;
 *   ck_slot_service_stop()           at the stop order: says when a candidate
 *                                     was never promoted (its tries keep
 *                                     counting down in the loader).
 * healthy = the service health milestone: Store open + continuity committed
 * this boot (PROVISIONED or RESUMED: the self-test record) + lifecycle READY.
 *
 * Every write is one control record commit through the torn-slot protocol
 * (ck_slot_control_commit, the same routine as the loader) over the slots
 * partition view (dev/devices.c ck_dev_slots_disk); the decision is the pure
 * ck_slot_kernel_action (native/boot/slot_select.c). No boot record, an
 * invalid one, no slots partition, no control record, a manifest on disk
 * that differs from the record, or a control record whose sequence moved
 * since the loader read it: the kernel writes nothing and says why. */
#ifndef CK_SLOT_SERVICE_H
#define CK_SLOT_SERVICE_H
void ck_slot_service_start(int healthy);
void ck_slot_service_health(int healthy);
void ck_slot_service_stop(void);
/* P11 cut 3: operator verb `update` (lifecycle READY only): write the signed
 * image in the update inbox partition into the inactive slot and stage it as
 * the candidate (native/boot/slot_update.h). Prints one "slot: update ..."
 * outcome line. */
#include <stddef.h>
void ck_slot_service_update(const char *arg, size_t len);
#endif
