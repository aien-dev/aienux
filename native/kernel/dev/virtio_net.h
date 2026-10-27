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

/* virtio_net.h -- virtio-net discovery stage: finds the virtio-net PCI
 * function and parses its virtio 1.x capabilities (native/net
 * aienos_virtio_pci.c). Binding and the data path are dev/net_bind.c. */
#ifndef AIENOS_CK_VIRTIO_NET_H
#define AIENOS_CK_VIRTIO_NET_H
#include "aienos_virtio_pci.h"
#include "pci.h"
/* Returns 0 when no virtio-net exists (*func_out = NULL) or its capabilities
 * parsed (*func_out = the function); the parse error code (> 0) otherwise.
 * Never enables the device. func_out may be NULL. */
int ck_virtio_net_probe(const pci_system *pci, virtio_pci_caps *caps_out, const pci_func **func_out);
#endif
