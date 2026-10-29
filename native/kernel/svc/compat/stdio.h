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

/* Kernel-build stand-in, see README. Only "/dev/urandom" opens. */
#ifndef CK_COMPAT_STDIO_H
#define CK_COMPAT_STDIO_H
#include <stddef.h>
typedef struct ck_compat_file FILE;
FILE *ck_compat_fopen(const char *path, const char *mode);
size_t ck_compat_fread(void *buf, size_t sz, size_t n, FILE *f);
int ck_compat_fclose(FILE *f);
#define fopen(p, m) ck_compat_fopen((p), (m))
#define fread(b, s, n, f) ck_compat_fread((b), (s), (n), (f))
#define fclose(f) ck_compat_fclose(f)
/* snprintf for the TEST-ONLY continuity wiring (svc/continuity_recovery.c): the kernel's own formatter, core/fmt.c. */
#include <stdarg.h>
int ck_snprintf(char *buf, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
#define snprintf ck_snprintf
#endif
