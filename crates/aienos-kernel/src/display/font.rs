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

//! Built-in 5x7 bitmap font for the early boot screen.
//!
//! Glyphs are drawn as text so a reviewer can check them by eye. Lowercase
//! letters render as uppercase; anything without a glyph renders as `?`.

pub const GLYPH_WIDTH: usize = 5;
pub const GLYPH_HEIGHT: usize = 7;

type Art = [&'static str; GLYPH_HEIGHT];

const GLYPHS: &[(char, Art)] = &[
    (
        ' ',
        [
            "     ", "     ", "     ", "     ", "     ", "     ", "     ",
        ],
    ),
    (
        '0',
        [
            " ### ", "#   #", "#  ##", "# # #", "##  #", "#   #", " ### ",
        ],
    ),
    (
        '1',
        [
            "  #  ", " ##  ", "  #  ", "  #  ", "  #  ", "  #  ", " ### ",
        ],
    ),
    (
        '2',
        [
            " ### ", "#   #", "    #", "   # ", "  #  ", " #   ", "#####",
        ],
    ),
    (
        '3',
        [
            "#####", "   # ", "  #  ", "   # ", "    #", "#   #", " ### ",
        ],
    ),
    (
        '4',
        [
            "   # ", "  ## ", " # # ", "#  # ", "#####", "   # ", "   # ",
        ],
    ),
    (
        '5',
        [
            "#####", "#    ", "#### ", "    #", "    #", "#   #", " ### ",
        ],
    ),
    (
        '6',
        [
            "  ## ", " #   ", "#    ", "#### ", "#   #", "#   #", " ### ",
        ],
    ),
    (
        '7',
        [
            "#####", "    #", "   # ", "  #  ", " #   ", " #   ", " #   ",
        ],
    ),
    (
        '8',
        [
            " ### ", "#   #", "#   #", " ### ", "#   #", "#   #", " ### ",
        ],
    ),
    (
        '9',
        [
            " ### ", "#   #", "#   #", " ####", "    #", "   # ", " ##  ",
        ],
    ),
    (
        'A',
        [
            " ### ", "#   #", "#   #", "#####", "#   #", "#   #", "#   #",
        ],
    ),
    (
        'B',
        [
            "#### ", "#   #", "#   #", "#### ", "#   #", "#   #", "#### ",
        ],
    ),
    (
        'C',
        [
            " ### ", "#   #", "#    ", "#    ", "#    ", "#   #", " ### ",
        ],
    ),
    (
        'D',
        [
            "#### ", "#   #", "#   #", "#   #", "#   #", "#   #", "#### ",
        ],
    ),
    (
        'E',
        [
            "#####", "#    ", "#    ", "#### ", "#    ", "#    ", "#####",
        ],
    ),
    (
        'F',
        [
            "#####", "#    ", "#    ", "#### ", "#    ", "#    ", "#    ",
        ],
    ),
    (
        'G',
        [
            " ### ", "#   #", "#    ", "# ###", "#   #", "#   #", " ####",
        ],
    ),
    (
        'H',
        [
            "#   #", "#   #", "#   #", "#####", "#   #", "#   #", "#   #",
        ],
    ),
    (
        'I',
        [
            " ### ", "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", " ### ",
        ],
    ),
    (
        'J',
        [
            "  ###", "   # ", "   # ", "   # ", "   # ", "#  # ", " ##  ",
        ],
    ),
    (
        'K',
        [
            "#   #", "#  # ", "# #  ", "##   ", "# #  ", "#  # ", "#   #",
        ],
    ),
    (
        'L',
        [
            "#    ", "#    ", "#    ", "#    ", "#    ", "#    ", "#####",
        ],
    ),
    (
        'M',
        [
            "#   #", "## ##", "# # #", "# # #", "#   #", "#   #", "#   #",
        ],
    ),
    (
        'N',
        [
            "#   #", "#   #", "##  #", "# # #", "#  ##", "#   #", "#   #",
        ],
    ),
    (
        'O',
        [
            " ### ", "#   #", "#   #", "#   #", "#   #", "#   #", " ### ",
        ],
    ),
    (
        'P',
        [
            "#### ", "#   #", "#   #", "#### ", "#    ", "#    ", "#    ",
        ],
    ),
    (
        'Q',
        [
            " ### ", "#   #", "#   #", "#   #", "# # #", "#  # ", " ## #",
        ],
    ),
    (
        'R',
        [
            "#### ", "#   #", "#   #", "#### ", "# #  ", "#  # ", "#   #",
        ],
    ),
    (
        'S',
        [
            " ####", "#    ", "#    ", " ### ", "    #", "    #", "#### ",
        ],
    ),
    (
        'T',
        [
            "#####", "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", "  #  ",
        ],
    ),
    (
        'U',
        [
            "#   #", "#   #", "#   #", "#   #", "#   #", "#   #", " ### ",
        ],
    ),
    (
        'V',
        [
            "#   #", "#   #", "#   #", "#   #", "#   #", " # # ", "  #  ",
        ],
    ),
    (
        'W',
        [
            "#   #", "#   #", "#   #", "# # #", "# # #", "# # #", " # # ",
        ],
    ),
    (
        'X',
        [
            "#   #", "#   #", " # # ", "  #  ", " # # ", "#   #", "#   #",
        ],
    ),
    (
        'Y',
        [
            "#   #", "#   #", " # # ", "  #  ", "  #  ", "  #  ", "  #  ",
        ],
    ),
    (
        'Z',
        [
            "#####", "    #", "   # ", "  #  ", " #   ", "#    ", "#####",
        ],
    ),
    (
        ':',
        [
            "     ", "  #  ", "  #  ", "     ", "  #  ", "  #  ", "     ",
        ],
    ),
    (
        '_',
        [
            "     ", "     ", "     ", "     ", "     ", "     ", "#####",
        ],
    ),
    (
        '.',
        [
            "     ", "     ", "     ", "     ", "     ", " ##  ", " ##  ",
        ],
    ),
    (
        ',',
        [
            "     ", "     ", "     ", "     ", " ##  ", "  #  ", " #   ",
        ],
    ),
    (
        '-',
        [
            "     ", "     ", "     ", "#####", "     ", "     ", "     ",
        ],
    ),
    (
        '/',
        [
            "    #", "    #", "   # ", "  #  ", " #   ", "#    ", "#    ",
        ],
    ),
    (
        '=',
        [
            "     ", "     ", "#####", "     ", "#####", "     ", "     ",
        ],
    ),
    (
        '(',
        [
            "   # ", "  #  ", " #   ", " #   ", " #   ", "  #  ", "   # ",
        ],
    ),
    (
        ')',
        [
            " #   ", "  #  ", "   # ", "   # ", "   # ", "  #  ", " #   ",
        ],
    ),
    (
        '#',
        [
            " # # ", "#####", " # # ", " # # ", " # # ", "#####", " # # ",
        ],
    ),
    (
        '?',
        [
            " ### ", "#   #", "    #", "   # ", "  #  ", "     ", "  #  ",
        ],
    ),
];

fn art(c: char) -> &'static Art {
    let c = c.to_ascii_uppercase();
    GLYPHS
        .iter()
        .find(|(g, _)| *g == c)
        .or_else(|| GLYPHS.iter().find(|(g, _)| *g == '?'))
        .map(|(_, a)| a)
        .expect("font contains '?'")
}

/// Whether the pixel at column `x`, row `y` of `c`'s glyph is lit.
pub fn lit(c: char, x: usize, y: usize) -> bool {
    art(c)
        .get(y)
        .and_then(|row| row.as_bytes().get(x))
        .is_some_and(|b| *b == b'#')
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn every_glyph_is_five_by_seven_hashes_and_spaces() {
        for (c, rows) in GLYPHS {
            for row in rows {
                assert_eq!(row.len(), GLYPH_WIDTH, "glyph {c:?}");
                assert!(row.bytes().all(|b| b == b' ' || b == b'#'), "glyph {c:?}");
            }
        }
    }

    #[test]
    fn digits_and_letters_are_distinct() {
        let chars: std::vec::Vec<char> = ('0'..='9').chain('A'..='Z').collect();
        for (i, a) in chars.iter().enumerate() {
            for b in &chars[i + 1..] {
                assert_ne!(art(*a), art(*b), "{a} and {b} share a glyph");
            }
        }
    }

    #[test]
    fn lowercase_maps_to_uppercase_and_unknown_to_question_mark() {
        assert_eq!(art('k'), art('K'));
        assert_eq!(art('~'), art('?'));
        assert!(lit('L', 0, 6) && lit('L', 4, 6) && !lit('L', 4, 0));
    }
}
