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

#ifndef ARGUS_CONTAIN_H
#define ARGUS_CONTAIN_H

#include "argus_abi.h"

/* Bounded, caller-owned containment state. No allocation, clocks, authority, or I/O. */
typedef struct ArgusContain ArgusContain;

size_t argus_contain_footprint(void);
int argus_contain_init(ArgusContain **state, void *memory, size_t bytes);

/* Observe a validated event after core ingestion. Findings are codes 17-21. */
int argus_contain_observe(ArgusContain *state, const ArgusCore *core,
                          const ArgusEvent *event, ArgusFinding *findings,
                          size_t finding_cap, size_t *finding_count);

/* Convert eligible detector findings to requests and kind-90 events. The caller
 * ingests returned events directly into the core, then hands requests to AEGIS. */
int argus_contain_propose(ArgusContain *state, const ArgusCore *core,
                          const ArgusFinding *findings, size_t finding_count,
                          const ArgusEvent *trigger_event,
                          ArgusContainmentRequest *requests, size_t request_cap,
                          ArgusEvent *proposed_events, size_t event_cap,
                          size_t *proposal_count);

void argus_contain_health(const ArgusContain *state, ArgusContainHealth *out);
void argus_contain_state_digest(const ArgusContain *state,
                                uint8_t out[ARGUS_DIGEST_LEN]);

/* Pending-table overflow is reported once per lifetime. Drain the resulting
 * CRITICAL telemetry-loss event after propose; ARGUS_ERR_STATE means none waits. */
int argus_contain_take_overflow_event(ArgusContain *state, ArgusEvent *out);

#endif
