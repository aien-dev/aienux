#![no_std]

//! Aien's anchor (aienix#5): an append-only, hash-chained log of checked
//! facts. Every entry is signed by a trusted admission key; the model never
//! holds one, so it cannot write. Queries answer `Known`, `Retracted` or
//! `Unknown`, never a guess. Entries are encoded and decoded byte-by-byte
//! with fixed bounds.

extern crate alloc;
#[cfg(test)]
extern crate std;

use aienos_crypto::sha256::{self, Digest};
use alloc::vec::Vec;
use ed25519_dalek::{Signature, VerifyingKey};

/// Domain prefix of every signed body; a signature over anything else is
/// never accepted as an anchor entry.
pub const DOMAIN: &[u8; 16] = b"AIENOS-ANCHOR-v0";
pub const MAX_KEY: usize = 256;
pub const MAX_VALUE: usize = 4096;
pub const MAX_SOURCE: usize = 256;
/// Digest a log starts from.
pub const GENESIS: Digest = [0u8; 32];

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Kind {
    Admit = 1,
    Retract = 2,
}

/// The signed content of one entry.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Body {
    pub seq: u64,
    pub prev: Digest,
    pub kind: Kind,
    pub time: u64,
    pub key: Vec<u8>,
    pub value: Vec<u8>,
    pub source: Vec<u8>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Entry {
    pub body: Body,
    pub signer: [u8; 32],
    pub signature: [u8; 64],
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Refusal {
    Malformed,
    TooLarge,
    OutOfOrder,
    BrokenChain,
    UntrustedSigner,
    BadSignature,
    RetractUnknown,
    RetractWithValue,
}

#[derive(Debug, PartialEq, Eq)]
pub enum Answer<'a> {
    Known { value: &'a [u8], source: &'a [u8], seq: u64, signer: [u8; 32] },
    Retracted { seq: u64 },
    Unknown,
}

impl Body {
    /// Canonical bytes: the exact message that is signed.
    pub fn encode(&self) -> Vec<u8> {
        let mut out = Vec::with_capacity(16 + 8 + 32 + 1 + 8 + 10 + self.key.len() + self.value.len() + self.source.len());
        out.extend_from_slice(DOMAIN);
        out.extend_from_slice(&self.seq.to_le_bytes());
        out.extend_from_slice(&self.prev);
        out.push(self.kind as u8);
        out.extend_from_slice(&self.time.to_le_bytes());
        out.extend_from_slice(&(self.key.len() as u16).to_le_bytes());
        out.extend_from_slice(&self.key);
        out.extend_from_slice(&(self.value.len() as u32).to_le_bytes());
        out.extend_from_slice(&self.value);
        out.extend_from_slice(&(self.source.len() as u16).to_le_bytes());
        out.extend_from_slice(&self.source);
        out
    }

    fn check_bounds(&self) -> Result<(), Refusal> {
        if self.key.is_empty() || self.key.len() > MAX_KEY || self.value.len() > MAX_VALUE || self.source.len() > MAX_SOURCE {
            return Err(Refusal::TooLarge);
        }
        if self.kind == Kind::Retract && !self.value.is_empty() {
            return Err(Refusal::RetractWithValue);
        }
        Ok(())
    }
}

impl Entry {
    pub fn encode(&self) -> Vec<u8> {
        let mut out = self.body.encode();
        out.extend_from_slice(&self.signer);
        out.extend_from_slice(&self.signature);
        out
    }

    /// Digest the next entry must name as `prev`.
    pub fn digest(&self) -> Digest {
        sha256::hash(&self.encode())
    }

    /// Decode one entry from the front of `bytes`; returns it and the bytes used.
    pub fn decode(bytes: &[u8]) -> Result<(Entry, usize), Refusal> {
        let mut r = Reader { bytes, at: 0 };
        if r.take(16)? != DOMAIN {
            return Err(Refusal::Malformed);
        }
        let seq = r.u64()?;
        let prev = r.array::<32>()?;
        let kind = match r.take(1)?[0] {
            1 => Kind::Admit,
            2 => Kind::Retract,
            _ => return Err(Refusal::Malformed),
        };
        let time = r.u64()?;
        let key_len = r.u16()? as usize;
        if key_len > MAX_KEY {
            return Err(Refusal::TooLarge);
        }
        let key = r.take(key_len)?.to_vec();
        let value_len = r.u32()? as usize;
        if value_len > MAX_VALUE {
            return Err(Refusal::TooLarge);
        }
        let value = r.take(value_len)?.to_vec();
        let source_len = r.u16()? as usize;
        if source_len > MAX_SOURCE {
            return Err(Refusal::TooLarge);
        }
        let source = r.take(source_len)?.to_vec();
        let signer = r.array::<32>()?;
        let signature = r.array::<64>()?;
        let body = Body { seq, prev, kind, time, key, value, source };
        Ok((Entry { body, signer, signature }, r.at))
    }
}

struct Reader<'a> {
    bytes: &'a [u8],
    at: usize,
}

impl<'a> Reader<'a> {
    fn take(&mut self, n: usize) -> Result<&'a [u8], Refusal> {
        let end = self.at.checked_add(n).ok_or(Refusal::Malformed)?;
        let out = self.bytes.get(self.at..end).ok_or(Refusal::Malformed)?;
        self.at = end;
        Ok(out)
    }
    fn array<const N: usize>(&mut self) -> Result<[u8; N], Refusal> {
        let mut out = [0u8; N];
        out.copy_from_slice(self.take(N)?);
        Ok(out)
    }
    fn u16(&mut self) -> Result<u16, Refusal> {
        Ok(u16::from_le_bytes(self.array()?))
    }
    fn u32(&mut self) -> Result<u32, Refusal> {
        Ok(u32::from_le_bytes(self.array()?))
    }
    fn u64(&mut self) -> Result<u64, Refusal> {
        Ok(u64::from_le_bytes(self.array()?))
    }
}

/// The anchor: trusted admission keys plus the verified log.
pub struct Anchor {
    trusted: Vec<[u8; 32]>,
    entries: Vec<Entry>,
    head: Digest,
}

impl Anchor {
    pub fn new(trusted: Vec<[u8; 32]>) -> Self {
        Anchor { trusted, entries: Vec::new(), head: GENESIS }
    }

    /// Rebuild from a stored log, re-verifying every entry. Any flaw refuses
    /// the whole log: a tampered history is never partly trusted.
    pub fn replay(trusted: Vec<[u8; 32]>, mut log: &[u8]) -> Result<Self, Refusal> {
        let mut anchor = Anchor::new(trusted);
        while !log.is_empty() {
            let (entry, used) = Entry::decode(log)?;
            anchor.append(entry)?;
            log = &log[used..];
        }
        Ok(anchor)
    }

    pub fn head(&self) -> Digest {
        self.head
    }

    pub fn len(&self) -> usize {
        self.entries.len()
    }

    pub fn is_empty(&self) -> bool {
        self.entries.is_empty()
    }

    /// The body the next admission must sign.
    pub fn next_body(&self, kind: Kind, time: u64, key: &[u8], value: &[u8], source: &[u8]) -> Body {
        Body { seq: self.entries.len() as u64, prev: self.head, kind, time, key: key.to_vec(), value: value.to_vec(), source: source.to_vec() }
    }

    /// Admit one signed entry, or refuse it and change nothing.
    pub fn append(&mut self, entry: Entry) -> Result<(), Refusal> {
        entry.body.check_bounds()?;
        if entry.body.seq != self.entries.len() as u64 {
            return Err(Refusal::OutOfOrder);
        }
        if entry.body.prev != self.head {
            return Err(Refusal::BrokenChain);
        }
        if !self.trusted.contains(&entry.signer) {
            return Err(Refusal::UntrustedSigner);
        }
        let key = VerifyingKey::from_bytes(&entry.signer).map_err(|_| Refusal::UntrustedSigner)?;
        key.verify_strict(&entry.body.encode(), &Signature::from_bytes(&entry.signature)).map_err(|_| Refusal::BadSignature)?;
        if entry.body.kind == Kind::Retract && !matches!(self.query(&entry.body.key), Answer::Known { .. }) {
            return Err(Refusal::RetractUnknown);
        }
        self.head = entry.digest();
        self.entries.push(entry);
        Ok(())
    }

    /// The latest word on `key`.
    pub fn query(&self, key: &[u8]) -> Answer<'_> {
        for e in self.entries.iter().rev() {
            if e.body.key == key {
                return match e.body.kind {
                    Kind::Admit => Answer::Known { value: &e.body.value, source: &e.body.source, seq: e.body.seq, signer: e.signer },
                    Kind::Retract => Answer::Retracted { seq: e.body.seq },
                };
            }
        }
        Answer::Unknown
    }

    /// The whole log as stored bytes.
    pub fn export(&self) -> Vec<u8> {
        self.entries.iter().flat_map(Entry::encode).collect()
    }
}

#[cfg(test)]
mod tests;
