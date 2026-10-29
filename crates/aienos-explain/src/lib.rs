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

//! AIENOS explanation bridge: think in graphs, speak in paths.
//!
//! Machine artifacts (Cortex records, agent branches, RSI hypothesis contracts,
//! evaluation receipts) are expressed in the language-neutral evidence-graph contract
//! ([`contract`]). The builder ([`build`]) walks explicit support edges backward with
//! hard limits, cuts loops, keeps gaps visible and orders the result into a path: the
//! [`bundle::ExplanationBundle`], which is the authoritative explanation. Renderers
//! ([`render`], [`session`]) are pure projections of the bundle.
//!
//! Boundaries:
//! - host-side only; nothing here is part of the C kernel or the boot path;
//! - read-only: no function writes to Cortex or agent state;
//! - no hidden reasoning: inputs are records, never token histories or model text;
//! - analogies live only in the renderer and are never evidence;
//! - an explanation grants no authority and is not an approval of anything;
//! - no network, no model, no outside service.

pub mod adapters;
pub mod build;
pub mod bundle;
pub mod contract;
pub mod render;
pub mod session;

pub use build::build_bundle;
pub use bundle::{ExplanationBundle, Limits};
pub use contract::{EvidenceGraph, ExplainError};
pub use render::{render, Audience, ExplanationRequest, Mode};
pub use session::ExplanationSession;
