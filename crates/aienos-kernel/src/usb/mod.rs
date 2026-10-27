// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Drake Stapleton (AIEN / aien-dev)
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

//! USB support for the early console.
//!
//! After `ExitBootServices` the firmware's USB keyboard support is gone, so
//! AIENOS must drive the keyboard itself. This module holds the parts that
//! need no hardware: decoding HID boot-protocol keyboard reports into key
//! events and text, parsing the descriptors that locate a boot keyboard, and
//! the xHCI data structures (TRBs, rings, contexts) a polled controller
//! driver builds on (ADR 0012, SEED-0A experiment #1 `input.keyboard.usb`).

pub mod audio;
pub mod descriptor;
pub mod hid;
pub mod xhci;
