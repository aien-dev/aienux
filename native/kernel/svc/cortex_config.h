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

/* cortex_config.h -- CX_CONFIG_H for the vendored Omega Cortex store
 * (third_party/omega/rx_cortex.c) inside AIENOS. The kernel has no general
 * heap: the store's allocations come from one bounded region handed over
 * by ck_cx_heap_init (svc/cortex_journal.c). */
#ifndef AIENOS_CORTEX_CONFIG_H
#define AIENOS_CORTEX_CONFIG_H
#include <stddef.h>
void *ck_cx_alloc(size_t n);
void *ck_cx_realloc(void *p, size_t n);
void ck_cx_free(void *p);
#define CX_MALLOC(n) ck_cx_alloc(n)
#define CX_REALLOC(p, n) ck_cx_realloc((p), (n))
#define CX_FREE(p) ck_cx_free(p)
#endif
