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

/* slot_control.c -- see slot_control.h. */
#include "slot_control.h"

static struct ck_slot_control g_back;

int ck_slot_control_commit(ts_region *r, const struct ck_slot_control *c, uint64_t *seq_out)
{
    uint64_t seq = 0;
    int rc = ts_commit(r, (const uint8_t *)c);
    if (rc == TS_OK)
        rc = ts_recover(r, (uint8_t *)&g_back, &seq, 0);
    if (rc == TS_OK) {
        const uint8_t *a = (const uint8_t *)&g_back, *b = (const uint8_t *)c;
        uint8_t d = 0;
        for (unsigned i = 0; i < sizeof g_back; i++)
            d |= (uint8_t)(a[i] ^ b[i]);
        if (d)
            rc = TS_CORRUPT;
    }
    if (seq_out)
        *seq_out = rc == TS_OK ? seq : 0;
    return rc;
}
