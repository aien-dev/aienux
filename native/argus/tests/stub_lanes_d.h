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

/* stub_lanes_d.h -- TEST-ONLY controls for the lane B/E stand-ins in stub_lanes_d.c. */
#ifndef STUB_LANES_D_H
#define STUB_LANES_D_H
#include "../argus_abi.h"
extern int stub_detect_mode;
extern int stub_probe_rc;
extern ArgusCapShadow stub_probe_cap;
extern uint64_t stub_detect_calls;
#endif
