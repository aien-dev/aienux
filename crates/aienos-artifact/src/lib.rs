#![no_std]
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

//! Canonical, bounded parser and identity primitives for Binary Artifact v0.
//! Persistent data is decoded byte-by-byte; no input is reinterpreted as a
//! Rust struct and no parser path allocates.

#[cfg(test)]
extern crate std;

pub mod canonical;
pub mod capability;
pub mod error;
pub mod format;
pub mod id;
pub mod receipt;
pub mod resource;
pub mod signature;
pub mod verify;

pub use error::ArtifactError;
pub use format::{Artifact, Section};
pub use id::ArtifactId;
pub use signature::{
    ArtifactVerifier, ConfiguredArtifactVerifier, Ed25519Verifier, EmptyTrustAnchorSet,
    ProductionTrustAnchorSet, ReceiptSigner, SignatureVerifier, TrustAnchorSet, TrustTier,
    TrustedSigner, VerifiedArtifact,
};

#[cfg(test)]
mod tests;

#[cfg(test)]
mod receipt_tests;
