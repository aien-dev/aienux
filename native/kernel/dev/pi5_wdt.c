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

/* pi5_wdt.c -- see pi5_wdt.h. Pi 5 build only. */
#ifndef CK_PI5
#error "pi5_wdt.c is Raspberry Pi 5 only (CK_PI5); never build it for another board"
#endif
#include "pi5_wdt.h"
#include "ck.h"

static volatile uint8_t *pm;

static uint32_t rd(uint32_t off) { return *(volatile uint32_t *)(pm + off); }
static void wr(uint32_t off, uint32_t v) { *(volatile uint32_t *)(pm + off) = v; }

int ck_pi5_wdt_arm(void)
{
    pm = ck_mmio_map(CK_PI5_PM_PHYS, 0x1000);
    if (!pm)
        return -1;
    ck_pi5_wdt_pet();
    wr(CK_PI5_PM_RSTC, ck_pi5_rstc_arm(rd(CK_PI5_PM_RSTC)));
    return 0;
}

void ck_pi5_wdt_pet(void)
{
    if (pm)
        wr(CK_PI5_PM_WDOG, ck_pi5_wdog_value(CK_PI5_WDT_S));
}

void ck_pi5_wdt_stop(void)
{
    if (pm)
        wr(CK_PI5_PM_RSTC, CK_PI5_PM_PASSWORD | CK_PI5_PM_RSTC_RESET);
}
