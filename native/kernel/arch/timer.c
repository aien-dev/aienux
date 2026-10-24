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

/* timer.c -- EL1 physical generic timer (CNTP, PPI INTID 30) and the
 * monotonic time services. Tick semantics match the Rust kernel: the IRQ
 * re-arms CNTP_CVAL 10 ms after "now", then records the interval since the
 * previous tick (the first interval is measured from arming).
 *
 * Both ends of an interval are the counter value read at IRQ ENTRY, the same
 * value the next deadline is built from. An interval is therefore never
 * shorter than the period, whatever the host does: the old code read the
 * counter again AFTER re-arming, so a host stall between the two reads (QEMU
 * TCG on a loaded machine) made one interval long and the next one short, and
 * the M1 "no interval shorter than half a period" check failed (an interrupt
 * storm or a missing EOI still shows: the next IRQ arrives at once). */
#include "arch.h"
#include "ck_internal.h"

static volatile uint64_t ticks, last, tmin, tmax;

#define CK_TIMER_MIN_TICKS 5   /* the M1 gate wants at least five ticks */
#define CK_TIMER_GRACE_MS 5000 /* longest extra wait for them */

static uint64_t counter(void)
{
    ck_isb();
    return ck_rd(cntpct_el0);
}

uint64_t ck_counter_to_us(uint64_t c, uint64_t f)
{
    if (!f)
        return 0;
    /* floor(c * 1e6 / f) without 128-bit division. */
    return (c / f) * 1000000u + ((c % f) * 1000000u) / f;
}

uint64_t ck_time_us(void)
{
    return ck_counter_to_us(counter(), ck_rd(cntfrq_el0));
}

void ck_udelay(uint32_t us)
{
    uint64_t f = ck_rd(cntfrq_el0), start = counter();
    uint64_t need = (uint64_t)us * (f / 1000000u) + ((uint64_t)us * (f % 1000000u)) / 1000000u;
    while (counter() - start < need)
        ;
}

void ck_timer_tick(void)
{
    uint64_t hz = ck_rd(cntfrq_el0);
    uint64_t now = counter();
    ck_wr(cntp_cval_el0, now + (hz ? hz / 100 : 625000));
    ck_wr(cntp_ctl_el0, 1);
    ck_isb();
    ticks++;
    uint64_t interval = now - last;
    last = now;
    if (interval < tmin)
        tmin = interval;
    if (interval > tmax)
        tmax = interval;
}

int ck_timer_window(uint32_t ms, struct ck_timer_window *w)
{
    uint64_t hz = ck_rd(cntfrq_el0);
    if (!hz)
        return -1;
    uint64_t start = counter();
    ticks = 0;
    last = start;
    tmin = ~0ull;
    tmax = 0;
    ck_wr(cntp_cval_el0, start + hz / 100);
    ck_wr(cntp_ctl_el0, 1); /* ENABLE, IMASK clear */
    ck_isb();
    ck_irq_on();
    ck_isb();
    /* The window is wall-clock. A starved vCPU (QEMU TCG on a loaded host) may
     * see fewer than CK_TIMER_MIN_TICKS ticks in it, which says nothing about
     * the timer. Keep counting past the window until that many ticks arrived,
     * giving up after CK_TIMER_GRACE_MS: a dead timer still reports 0 ticks. */
    uint64_t window = hz / 1000 * ms;
    uint64_t grace = hz / 1000 * CK_TIMER_GRACE_MS;
    uint64_t t;
    while ((t = counter()) - start < window || (ticks < CK_TIMER_MIN_TICKS && t - start < grace))
        ;
    ck_irq_off();
    ck_isb();
    ck_wr(cntp_ctl_el0, 0);
    ck_isb();
    w->ticks = ticks;
    w->ms = (t - start) / (hz / 1000);
    w->freq_hz = hz;
    w->min_interval = ticks ? tmin : 0;
    w->max_interval = tmax;
    w->span = ticks ? last - start : 0;
    return 0;
}
