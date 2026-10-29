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

//! Epistemic status classifications conforming to AIENOS Blueprint §11 and Specs §2.3.

use serde::{Deserialize, Serialize};

/// Epistemic status distinctions preserving provenance and preventing self-reinforcing hallucination.
#[repr(u8)]
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash, Serialize, Deserialize)]
pub enum EpistemicStatus {
    /// Raw sensor, hardware register, or direct file input (ground truth).
    DirectObservation = 1,
    /// Mathematically, cryptographically, or deterministically verified fact.
    VerifiedFact = 2,
    /// Model deduction derived from observations or verified facts.
    Inference = 3,
    /// Tentative exploratory branch awaiting validation.
    Hypothesis = 4,
    /// Detected epistemic conflict between opposing assertions.
    Contradiction = 5,
    /// Canonical directive originating directly from the human operator.
    OperatorDecision = 6,
}

impl EpistemicStatus {
    /// Check if this status constitutes an immutable grounding root.
    pub fn is_ground_truth(&self) -> bool {
        matches!(
            self,
            Self::DirectObservation | Self::VerifiedFact | Self::OperatorDecision
        )
    }

    /// Check if this record is a derived or tentative assertion requiring grounding.
    pub fn requires_grounding(&self) -> bool {
        matches!(self, Self::Inference | Self::Hypothesis)
    }
}
