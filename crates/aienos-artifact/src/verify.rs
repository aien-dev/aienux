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

//! Structural parsing and identity entry points shared by host tooling and
//! the kernel. Authentication is added in P2-3.

use crate::canonical::payload_digest;
use crate::error::ArtifactError;
use crate::format::{Artifact, MAX_ARTIFACT_SIZE};
use crate::id::{compute_artifact_id, ArtifactId};
use aienos_crypto::sha256::Digest;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct IdentifiedArtifact<'a> {
    pub artifact: Artifact<'a>,
    pub artifact_id: ArtifactId,
    pub payload_digest: Digest,
}

pub fn parse_and_identify(bytes: &[u8]) -> Result<IdentifiedArtifact<'_>, ArtifactError> {
    if bytes.len() > MAX_ARTIFACT_SIZE {
        return Err(ArtifactError::ResourceLimit);
    }
    let artifact = Artifact::parse(bytes)?;
    Ok(IdentifiedArtifact {
        artifact_id: compute_artifact_id(&artifact),
        payload_digest: payload_digest(&artifact),
        artifact,
    })
}
