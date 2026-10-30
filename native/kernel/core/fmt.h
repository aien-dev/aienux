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

/* fmt.h -- freestanding printf-style formatting shared by the console and
 * the host tests. Pure: no I/O, no globals. */
#ifndef AIENOS_CK_FMT_H
#define AIENOS_CK_FMT_H

#include <stdarg.h>
#include <stddef.h>

typedef void (*ck_emit_fn)(void *ctx, char c);

/* Formats into emit(); returns the number of characters produced.
 * Supports %s %c %d %i %u %x %X %p %% with flags '-' '0', width (digits or
 * '*') and length hh h l ll z. %p prints 0x followed by lowercase hex. */
int ck_vformat(ck_emit_fn emit, void *ctx, const char *fmt, va_list ap);
int ck_vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
int ck_snprintf(char *buf, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

#endif
