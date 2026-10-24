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

//! Host-side evidence tooling for the M0 reference freeze.
//!
//! Replaces the former Python capture and verification scripts. Everything
//! here reads the Linux reference machine directly (procfs, sysfs, git, and a
//! local inference endpoint) and hashes with the kernel's own SHA-256, so the
//! evidence path has no interpreter and no vendor userspace tools.

pub mod canon;
pub mod capture;
pub mod http;
pub mod pe;
pub mod rollback;
pub mod time;
pub mod verify;

/// Evidence schema written by `capture` and accepted by `verify`.
pub const SCHEMA_VERSION: &str = "3.0.0";
