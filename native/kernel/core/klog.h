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

/* klog.h -- RAM copy of the console output, so a board with no serial line
 * attached (the hub Raspberry Pi 5) can write it to its card before the
 * reset (core/klog_slot.c). The first CK_KLOG_HEAD bytes are kept in order;
 * after that the newest CK_KLOG_TAIL bytes are kept in a ring, so both the
 * start of the boot and the lines right before a failure survive. Pure:
 * no device access, no allocation. Host test: tests/test_klog.c. */
#ifndef AIENOS_CK_KLOG_H
#define AIENOS_CK_KLOG_H
#include <stddef.h>
#include <stdint.h>

#define CK_KLOG_HEAD (192u * 1024u)
#define CK_KLOG_TAIL (64u * 1024u)
/* Largest ck_klog_read output: head + the dropped-bytes line + tail. */
#define CK_KLOG_MAX (CK_KLOG_HEAD + 64u + CK_KLOG_TAIL)

void ck_klog_put(const char *s, size_t n);
/* Copies the log into out: the head, then "\n[klog: N bytes dropped]\n" when
 * the ring overwrote anything, then the ring oldest first. Stops at cap.
 * Returns the bytes written. */
size_t ck_klog_read(char *out, size_t cap);
/* Every byte ever put, kept or not. */
uint64_t ck_klog_total(void);
/* Empties the log (tests). */
void ck_klog_clear(void);
#endif
