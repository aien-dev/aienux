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

//! Memory metrics for C1 Copy-On-Write Prefix Tree verification.

use serde::{Deserialize, Serialize};

/// Performance and memory metrics proving sub-linear scaling and zero-copy branching.
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct C1MemoryMetrics {
    pub physical_pages_allocated: usize,
    pub physical_memory_bytes: usize,
    pub total_virtual_tokens: usize,
    pub total_unique_physical_tokens: usize,
    pub sharing_ratio: f64,
}
