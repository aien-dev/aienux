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

//! AIENOS AEGIS: Capability Graph, Effect Intent Pipeline, and J-Space Worlds (§13, §5, §12).
//!
//! Enforces unforgeable capability tokens, copy-on-write transactional worlds,
//! and strict boundary policies: intelligence proposes; deterministic authority disposes.

pub mod broker;
pub mod capability;
pub mod effect;
pub mod evaluator;
pub mod world;

pub use broker::{EffectAuditRecord, EffectBroker};
pub use capability::{AegisError, CapabilityGraph, CapabilityScope, CapabilityToken};
pub use effect::{EffectIntent, OperatorGrant};
pub use evaluator::{AegisDecision, AegisEvaluator};
pub use world::{JSpaceWorld, WorldStatus};
