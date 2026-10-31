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

/* ck_compat.h -- what the kernel-build compat layer reports back. */
#ifndef AIENOS_CK_COMPAT_H
#define AIENOS_CK_COMPAT_H
/* No weak or timer source exists: NONE means the read was refused. */
enum { CK_ENTROPY_NONE = 0, CK_ENTROPY_RNDR = 1, CK_ENTROPY_HOST = 3 };
/* Source used for the last "/dev/urandom" read (kernel build). */
int ck_compat_entropy_source(void);
#endif
