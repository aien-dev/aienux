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

#![deny(unsafe_code)]
// Block loops mirror ggml index arithmetic; as_chunks/is_multiple_of are too
// new for this workspace to rely on.
#![allow(
    clippy::needless_range_loop,
    clippy::chunks_exact_to_as_chunks,
    clippy::manual_is_multiple_of
)]

//! Native inference ingestion (aienos#34 lane 1): a bounded GGUF v3 reader
//! and the ggml block dequantizers (Q4_K, Q6_K and the plain float types)
//! needed to load Llama-3.2-1B-Instruct-Q4_K_M.
//!
//! Everything works on a `&[u8]` slice. Every read is length checked and
//! hostile input yields a [`GgufError`], never a panic. Dequantizers are
//! scalar and written for correctness first.

extern crate alloc;
#[cfg(test)]
extern crate std;

pub mod decode;
#[allow(unsafe_code)] // the only unsafe in this crate: SDOT intrinsics behind a capability token
pub mod dotprod;
pub mod gguf;
pub mod half;
pub mod math;
pub mod model;
pub mod quant;
pub mod tokenizer;

pub use decode::{argmax, generate_greedy, DecodeState};
pub use dotprod::{cpu_dotprod, set_cpu_dotprod, DotProd, Q8Act};
pub use gguf::{ArrayView, GgmlType, Gguf, GgufError, TensorInfo, Value, ValueType};
pub use model::{InferError, Model};
pub use tokenizer::Tokenizer;
