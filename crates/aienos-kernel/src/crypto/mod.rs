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

//! Sovereign cryptographic primitives for AIENOS.

pub mod envelope;
pub mod hkdf;
pub mod hmac;
pub mod sha256;

pub use envelope::{
    decrypt_envelope, derive_chunk_nonce, encrypt_envelope, EnvelopeError, EnvelopeHeader,
    DEFAULT_CHUNK_SIZE, ENVELOPE_HEADER_LEN, ENVELOPE_MAGIC, ENVELOPE_VERSION_1,
};
pub use hkdf::{hkdf_expand, hkdf_extract, HkdfError};
pub use hmac::{constant_time_eq, hmac_sha256, HmacSha256};
pub use sha256::{hash, Digest, Sha256};
