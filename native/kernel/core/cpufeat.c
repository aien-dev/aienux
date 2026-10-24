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

/* cpufeat.c -- see cpufeat.h for the register field positions. */
#include "cpufeat.h"
#include "fmt.h"

static unsigned ufield(uint64_t v, unsigned hi, unsigned lo)
{
    return (unsigned)((v >> lo) & ((1u << (hi - lo + 1u)) - 1u));
}

/* Signed 4-bit field: 0b1000..0b1111 are negative (0xf = -1 = not implemented). */
static int sfield(uint64_t v, unsigned hi, unsigned lo)
{
    int x = (int)ufield(v, hi, lo);
    return x >= 8 ? x - 16 : x;
}

void ck_cpufeat_decode(uint64_t isar0, uint64_t isar1, uint64_t pfr0, struct ck_cpu_features *out)
{
    out->dotprod = ufield(isar0, 47, 44) >= 1u;
    out->fhm = ufield(isar0, 51, 48) >= 1u;
    out->rndr = ufield(isar0, 63, 60) >= 1u;
    out->i8mm = ufield(isar1, 55, 52) >= 1u;
    out->bf16 = ufield(isar1, 47, 44) >= 1u;
    out->sve = ufield(pfr0, 35, 32) >= 1u;
    out->fp16 = sfield(pfr0, 19, 16) >= 1 && sfield(pfr0, 23, 20) >= 1;
}

int ck_cpufeat_format(const struct ck_cpu_features *f, char *buf, size_t n)
{
    return ck_snprintf(buf, n, "cpu_features: dotprod=%u i8mm=%u bf16=%u fhm=%u fp16=%u sve=%u rndr=%u", f->dotprod,
                       f->i8mm, f->bf16, f->fhm, f->fp16, f->sve, f->rndr);
}

/* The boot core's decode, kept for consumers that select code by feature
 * (core/infer.c passes dotprod to the Rust inference unit). All zero until
 * kmain records it, so nothing is ever assumed present. */
static struct ck_cpu_features g_boot_features;

void ck_cpufeat_set_boot(const struct ck_cpu_features *f)
{
    g_boot_features = *f;
}

const struct ck_cpu_features *ck_cpufeat_boot(void)
{
    return &g_boot_features;
}
