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

//! AIENOS Agent State ABI & Execution Incarnation Substrate (§9).
//!
//! Provides sovereign logical agent identities, deterministic branch derivation,
//! append-only token history logs with rolling cryptographic hash chains,
//! and complete decoupling of persistent semantic identity from ephemeral
//! physical compute incarnations.

pub mod abi;
pub mod history;
pub mod id;
pub mod incarnation;
pub mod lineage;
pub mod manager;

pub use abi::{AgentStateAbi, CheckpointHash, StateError};
pub use history::{TokenHistory, TokenRecord};
pub use id::{LogicalAgentId, LogicalBranchId};
pub use incarnation::{
    ComputeState, ExecutionIncarnation, IncarnationId, LogicalBranch, PhysicalInferenceState,
    SequenceId,
};
pub use lineage::{EpistemicRefs, Lineage};
pub use manager::{AgentSnapshot, AgentStateManager, CheckpointEnvelope};
