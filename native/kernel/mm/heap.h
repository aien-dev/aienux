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

/* heap.h -- first-fit kernel heap over one contiguous region. Blocks carry a
 * 16-byte header (size, magic); payloads are 16-byte aligned and zeroed on
 * allocation; free coalesces with free neighbours. Pure, host testable. */
#ifndef AIENOS_CK_HEAP_H
#define AIENOS_CK_HEAP_H

#include <stddef.h>
#include <stdint.h>

struct ck_heap {
    uint8_t *base;
    size_t size;
};

int ck_heap_init(struct ck_heap *h, void *base, size_t size);
void *ck_heap_alloc(struct ck_heap *h, size_t bytes);
/* 0 on success; -1 for a pointer that is not a live block (double free,
 * foreign pointer, corrupted header). */
int ck_heap_free(struct ck_heap *h, void *p);
size_t ck_heap_free_bytes(const struct ck_heap *h);
/* Walks every block; 0 if all headers are intact and sizes add up. */
int ck_heap_check(const struct ck_heap *h);

#endif
