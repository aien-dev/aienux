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

/// Stable fail-closed reasons for Binary Artifact v0 parsing and validation.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(u16)]
pub enum ArtifactError {
    BadMagic = 1,
    UnsupportedVersion = 2,
    WrongTarget = 3,
    WrongAbi = 4,
    LengthOverflow = 5,
    Truncated = 6,
    WrongLength = 7,
    ReservedNonZero = 8,
    UnsupportedFlags = 9,
    SectionOverlap = 10,
    BadSection = 11,
    BadEntryPoint = 12,
    MalformedCapability = 13,
    ResourceLimit = 14,
    BadSignatureFormat = 15,
    DigestMismatch = 16,
    UntrustedSigner = 17,
    BadSignature = 18,
    RightsEscalation = 19,
    /// Admission Receipt v0 record violates §7 (never a candidate reason).
    MalformedReceipt = 20,
}
