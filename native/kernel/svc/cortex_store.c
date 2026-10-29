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

/* cortex_store.c -- the canonical Omega Cortex store (CxStore) built
 * freestanding inside AIENOS: no POSIX journal, no libc allocation. The
 * source is vendored byte for byte from the omega.lock commit
 * (third_party/omega/rx_cortex.c, checked by scripts/omega_lock_check.sh);
 * this wrapper only sets the build switches so the vendored file stays
 * identical to Omega's. The journal backend is svc/cortex_journal.c. */
#define CX_NO_POSIX 1
#define CX_CONFIG_H "cortex_config.h"
#include "../third_party/omega/rx_cortex.c"
