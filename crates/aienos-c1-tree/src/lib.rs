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

//! AIENOS C1 Copy-On-Write Prefix Tree (§10, §28).
//!
//! Provides zero-duplication prefix sharing across reasoning branches,
//! deterministic Copy-On-Write divergence, and complete eviction independence.

pub mod metrics;
pub mod page;
pub mod pool;
pub mod tree;

pub use metrics::C1MemoryMetrics;
pub use page::{PageId, PhysicalKvPage, PAGE_CAPACITY_TOKENS, PAGE_KV_BYTES};
pub use pool::PhysicalPagePool;
pub use tree::{BranchDescriptor, C1Error, C1PrefixTree, ReservationReceipt};
