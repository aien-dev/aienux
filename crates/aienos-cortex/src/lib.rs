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

//! AIENOS Cortex Epistemic Store (§11).
//!
//! Preserves durable epistemic memory with verifiable provenance:
//! DirectObservation, VerifiedFact, Inference, Hypothesis, Contradiction, OperatorDecision.
//! Rejects ungrounded model hallucinations and circular assertions.

pub mod record;
pub mod status;
pub mod store;
pub mod verification;

pub use record::EpistemicRecord;
pub use status::EpistemicStatus;
pub use store::{CortexStore, JournalEnvelope};
pub use verification::{verify_grounding, CortexError};
