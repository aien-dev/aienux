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

//! Structural check that a file is an AArch64 PE32+ EFI application.
//! This verifies the artifact format only, not that the image boots.

fn u16_at(data: &[u8], at: usize) -> Option<u16> {
    Some(u16::from_le_bytes(data.get(at..at + 2)?.try_into().ok()?))
}

fn u32_at(data: &[u8], at: usize) -> Option<u32> {
    Some(u32::from_le_bytes(data.get(at..at + 4)?.try_into().ok()?))
}

const MACHINE_ARM64: u16 = 0xAA64;
const PE32_PLUS: u16 = 0x20B;
const SUBSYSTEM_EFI_APPLICATION: u16 = 10;

pub fn verify_efi_application(data: &[u8]) -> Result<(), String> {
    if data.len() < 0x100 || &data[..2] != b"MZ" {
        return Err("missing DOS image header".into());
    }
    let pe = u32_at(data, 0x3C).ok_or("truncated DOS header")? as usize;
    if data.get(pe..pe + 4) != Some(b"PE\0\0".as_slice()) {
        return Err("missing PE header".into());
    }
    let optional = pe + 4 + 20;
    let machine = u16_at(data, pe + 4).ok_or("truncated COFF header")?;
    let magic = u16_at(data, optional).ok_or("truncated optional header")?;
    let subsystem = u16_at(data, optional + 68).ok_or("truncated optional header")?;
    if (machine, magic, subsystem) != (MACHINE_ARM64, PE32_PLUS, SUBSYSTEM_EFI_APPLICATION) {
        return Err(format!(
            "unexpected machine/PE/subsystem: {machine:#x}/{magic:#x}/{subsystem}"
        ));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn image(machine: u16, subsystem: u16) -> Vec<u8> {
        let mut d = vec![0u8; 0x200];
        d[..2].copy_from_slice(b"MZ");
        d[0x3C..0x40].copy_from_slice(&0x80u32.to_le_bytes());
        d[0x80..0x84].copy_from_slice(b"PE\0\0");
        d[0x84..0x86].copy_from_slice(&machine.to_le_bytes());
        let optional = 0x80 + 24;
        d[optional..optional + 2].copy_from_slice(&PE32_PLUS.to_le_bytes());
        d[optional + 68..optional + 70].copy_from_slice(&subsystem.to_le_bytes());
        d
    }

    #[test]
    fn accepts_arm64_efi_application() {
        assert!(verify_efi_application(&image(MACHINE_ARM64, 10)).is_ok());
    }

    #[test]
    fn rejects_wrong_machine_subsystem_and_garbage() {
        assert!(verify_efi_application(&image(0x8664, 10)).is_err());
        assert!(verify_efi_application(&image(MACHINE_ARM64, 3)).is_err());
        assert!(verify_efi_application(b"not an image").is_err());
    }
}
