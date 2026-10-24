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

/* Pure parsing of virtio 1.x PCI vendor capabilities from a PCI configuration
 * image. Port of crates/aienos-kernel/src/virtio_net.rs (the spec). No device
 * access, no heap. Error order matches the Rust reference. */
#ifndef AIENOS_VIRTIO_PCI_H
#define AIENOS_VIRTIO_PCI_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    VIRTIO_PCI_OK = 0,
    VIRTIO_PCI_INVALID_CONFIG_LENGTH = 1,
    VIRTIO_PCI_CAPABILITIES_NOT_PRESENT = 2,
    VIRTIO_PCI_INVALID_POINTER = 3,
    VIRTIO_PCI_CAPABILITY_LOOP = 4,
    VIRTIO_PCI_SHORT_CAPABILITY = 5,
    VIRTIO_PCI_INVALID_REGION = 6,
    VIRTIO_PCI_DUPLICATE_CAPABILITY = 7,
} virtio_pci_err;

typedef struct {
    uint8_t present;
    uint8_t bar;
    uint32_t offset;
    uint32_t length;
} virtio_pci_region;

typedef struct {
    virtio_pci_region common_cfg;
    virtio_pci_region notify_cfg;
    uint8_t has_notify_off_multiplier;
    uint32_t notify_off_multiplier;
    virtio_pci_region isr_cfg;
    virtio_pci_region device_cfg;
} virtio_pci_caps;

/* config must be exactly 256 or 4096 bytes. *out is zeroed first and is only
 * meaningful when VIRTIO_PCI_OK is returned. */
virtio_pci_err virtio_pci_parse_caps(const uint8_t *config, size_t len, virtio_pci_caps *out);
const char *virtio_pci_err_name(virtio_pci_err e);

#endif
