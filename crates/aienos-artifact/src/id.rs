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

use aienos_crypto::sha256::{Digest, Sha256};

use crate::format::Artifact;

pub const ARTIFACT_ID_DOMAIN: &[u8] = b"AIENOS-ARTIFACT-V1\0";

#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub struct ArtifactId(pub Digest);

impl ArtifactId {
    pub const fn as_bytes(&self) -> &Digest {
        &self.0
    }
}

pub fn compute_artifact_id(artifact: &Artifact<'_>) -> ArtifactId {
    let mut hash = Sha256::new();
    hash.update(ARTIFACT_ID_DOMAIN);
    hash.update(artifact.unsigned_bytes);
    ArtifactId(hash.finalize())
}

pub fn artifact_id_from_unsigned_bytes(bytes: &[u8]) -> ArtifactId {
    let mut hash = Sha256::new();
    hash.update(ARTIFACT_ID_DOMAIN);
    hash.update(bytes);
    ArtifactId(hash.finalize())
}
