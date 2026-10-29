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

/* test_common_d.h -- tiny check harness + event builder for lane D tests. */
#ifndef TEST_COMMON_D_H
#define TEST_COMMON_D_H
#include "../argus_core.h"
#include "stub_lanes_d.h"
#include <stdio.h>
#include <string.h>

static int t_fail = 0, t_pass = 0;
#define CHECK(c) do { if (c) t_pass++; else { t_fail++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static inline void mid(uint8_t id[ARGUS_MACHINE_ID_LEN], uint32_t n)
{
    memset(id, 0, ARGUS_MACHINE_ID_LEN);
    id[0] = 0xA5; id[1] = (uint8_t)n; id[2] = (uint8_t)(n >> 8);
}

static inline void dg(uint8_t d[ARGUS_DIGEST_LEN], uint32_t n)
{
    memset(d, 0, ARGUS_DIGEST_LEN);
    d[31] = 0x5A; d[0] = (uint8_t)n; d[1] = (uint8_t)(n >> 8);
}

static inline ArgusEvent ev_make(uint16_t kind, uint64_t seq)
{
    ArgusEvent e;
    memset(&e, 0, sizeof e);
    e.version = ARGUS_ABI_VERSION;
    e.class_ = ARGUS_CLASS_SECURITY;
    e.kind = kind;
    e.outcome = ARGUS_OUTCOME_OK;
    e.flags = ARGUS_FLAG_SYNTHETIC;
    e.sequence = seq;
    mid(e.machine_id, 1);
    return e;
}
#endif
