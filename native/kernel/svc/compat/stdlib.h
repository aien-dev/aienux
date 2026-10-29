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

/* Kernel-build stand-in, see README. */
#ifndef CK_COMPAT_STDLIB_H
#define CK_COMPAT_STDLIB_H
#include <stddef.h>
void *ck_compat_malloc(size_t n);
void *ck_compat_calloc(size_t n, size_t sz);
void ck_compat_free(void *p);
#define malloc(n) ck_compat_malloc(n)
#define calloc(n, s) ck_compat_calloc((n), (s))
#define free(p) ck_compat_free(p)
#endif
