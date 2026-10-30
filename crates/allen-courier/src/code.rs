//! Code from the Spark (native/kernel/svc/allen_code.h): a coding request's
//! answer as the kernel takes it. The model answers with a short sentence and
//! one fenced code block; the code is made plain (tabs to spaces, anything
//! outside printable ASCII to '?'), held to the stated limits, and cut into
//! numbered pieces with its length and checksum:
//!   "CODE <seq> <i>/<n> <bytes> <crc32> <lang|-> <piece>"   piece: 128 bytes of code, "\n" and "\\" escaped
//!   "SAY <seq> 1/1 <sentence>"                              closes the answer
//! Pure functions only; the wire and the model are in main.rs.

/// Bytes of code at most (allen_code.h CK_CODE_MAX).
pub const CODE_MAX: usize = 32768;
/// Lines of code at most (CK_CODE_LINES).
pub const CODE_LINES: usize = 800;
/// Code bytes per piece (CK_CODE_PIECE).
pub const PIECE: usize = 128;
/// Language name bytes at most (CK_CODE_LANG - 1).
pub const LANG_MAX: usize = 15;

/// CRC-32 (IEEE 802.3, reflected, as zlib's crc32 and the kernel's ck_code_crc32).
pub fn crc32(b: &[u8]) -> u32 {
    let mut c = 0xffff_ffffu32;
    for &x in b {
        c ^= x as u32;
        for _ in 0..8 {
            c = if c & 1 != 0 {
                (c >> 1) ^ 0xedb8_8320
            } else {
                c >> 1
            };
        }
    }
    !c
}

/// What the model's reply holds once read: Allen's sentence, the language, the code.
#[derive(Debug, PartialEq)]
pub struct Reply {
    pub sentence: String,
    pub lang: String,
    pub code: String,
    /// lines left off to keep within the limits (0: the code is whole)
    pub cut_lines: usize,
}

/// The code made plain: CR dropped, tabs to four spaces, other control bytes
/// and anything outside printable ASCII to '?'; one newline at the end.
pub fn plain_code(s: &str) -> String {
    let mut o = String::with_capacity(s.len());
    for ch in s.chars() {
        match ch {
            '\r' => {}
            '\n' => o.push('\n'),
            '\t' => o.push_str("    "),
            ' '..='~' => o.push(ch),
            _ => o.push('?'),
        }
    }
    let t = o.trim_end_matches('\n').trim_start_matches('\n');
    let mut t = t.to_string();
    if !t.is_empty() {
        t.push('\n');
    }
    t
}

/// The language named after the opening fence, as one word of [a-z0-9+#._-].
fn lang_of(word: &str) -> String {
    let w = word.trim().to_ascii_lowercase();
    let w = match w.as_str() {
        "py" | "python3" => "python".to_string(),
        "js" => "javascript".to_string(),
        "ts" => "typescript".to_string(),
        "sh" | "shell" | "zsh" => "bash".to_string(),
        "c++" => "cpp".to_string(),
        "rs" => "rust".to_string(),
        _ => w,
    };
    w.chars()
        .take_while(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || "+#._-".contains(*c))
        .take(LANG_MAX)
        .collect()
}

/// Keeps code within CODE_MAX bytes and CODE_LINES lines, cut at a line end.
/// Returns the code kept and the lines left off.
pub fn within_limits(code: &str) -> (String, usize) {
    let all = code.lines().count();
    let mut kept = String::new();
    let mut n = 0;
    for line in code.lines() {
        if n == CODE_LINES || kept.len() + line.len() + 1 > CODE_MAX {
            break;
        }
        kept.push_str(line);
        kept.push('\n');
        n += 1;
    }
    (kept, all - n)
}

/// Reads the model's reply: the words before the first fence are the
/// sentence, the first fenced block the code (to its closing fence, or to the
/// end when the model stopped short). None when there is no code at all.
pub fn read_reply(reply: &str) -> Option<Reply> {
    let open = reply.find("```")?;
    let before = reply[..open].trim();
    let after = &reply[open + 3..];
    let (first, body) = match after.find('\n') {
        Some(e) => (&after[..e], &after[e + 1..]),
        None => (after, ""),
    };
    let lang = lang_of(first.split_whitespace().next().unwrap_or(""));
    // the closing fence: "```" at the start of a line
    let mut end = body.len();
    let mut at = 0;
    for line in body.split_inclusive('\n') {
        if line.trim_start().starts_with("```") {
            end = at;
            break;
        }
        at += line.len();
    }
    let code = plain_code(&body[..end]);
    if code.trim().is_empty() {
        return None;
    }
    let (code, cut_lines) = within_limits(&code);
    Some(Reply {
        sentence: before.to_string(),
        lang,
        code,
        cut_lines,
    })
}

/// "\n" and "\\" escaped, as the kernel's ck_code_take reads them.
fn escape(piece: &[u8]) -> String {
    let mut o = String::with_capacity(piece.len() + 8);
    for &b in piece {
        match b {
            b'\n' => o.push_str("\\n"),
            b'\\' => o.push_str("\\\\"),
            _ => o.push(b as char),
        }
    }
    o
}

/// The CODE lines for job seq: every piece CODE_PIECE bytes but the last.
pub fn pieces(seq: u32, code: &str, lang: &str) -> Vec<String> {
    let b = code.as_bytes();
    if b.is_empty() {
        return Vec::new();
    }
    let n = b.len().div_ceil(PIECE);
    let crc = crc32(b);
    let lang = if lang.is_empty() { "-" } else { lang };
    b.chunks(PIECE)
        .enumerate()
        .map(|(i, p)| {
            format!(
                "CODE {} {}/{} {} {:08x} {} {}",
                seq,
                i + 1,
                n,
                b.len(),
                crc,
                lang,
                escape(p)
            )
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn crc_is_zlibs() {
        assert_eq!(crc32(b"123456789"), 0xcbf4_3926);
        assert_eq!(crc32(b""), 0);
    }

    #[test]
    fn a_sentence_and_a_fenced_block_are_read() {
        let r = read_reply(
            "Here is a small calculator.\n\n```python\nprint(1 + 1)\n\tx = 2\n```\nEnjoy.",
        )
        .unwrap();
        assert_eq!(r.sentence, "Here is a small calculator.");
        assert_eq!(r.lang, "python");
        assert_eq!(r.code, "print(1 + 1)\n    x = 2\n");
        assert_eq!(r.cut_lines, 0);
    }

    #[test]
    fn short_names_and_odd_bytes_are_made_plain() {
        let r = read_reply("```py\nprint(\"\u{201c}hi\u{201d}\")\r\n```").unwrap();
        assert_eq!(r.lang, "python");
        assert_eq!(r.code, "print(\"?hi?\")\n");
        assert_eq!(r.sentence, "");
        assert_eq!(read_reply("```c++\nint main(){}\n```").unwrap().lang, "cpp");
    }

    #[test]
    fn an_unclosed_block_runs_to_the_end_and_no_block_is_none() {
        assert_eq!(
            read_reply("Sure.\n```c\nint main(void) {\n").unwrap().code,
            "int main(void) {\n"
        );
        assert!(read_reply("I would rather not.").is_none());
        assert!(read_reply("```\n\n```").is_none());
    }

    #[test]
    fn long_code_is_cut_at_a_line_within_the_limits() {
        let long: String = (0..1000).map(|i| format!("x = {}\n", i)).collect();
        let r = read_reply(&format!("```python\n{}```", long)).unwrap();
        assert_eq!(r.code.lines().count(), CODE_LINES);
        assert_eq!(r.cut_lines, 200);
        let wide: String = (0..400).map(|_| format!("{}\n", "y".repeat(100))).collect();
        let (k, cut) = within_limits(&wide);
        assert!(k.len() <= CODE_MAX && k.ends_with('\n'));
        assert_eq!(k.lines().count() + cut, 400);
    }

    #[test]
    fn pieces_carry_length_checksum_and_escapes() {
        let code = "a\\b\n".repeat(40); // 160 bytes: two pieces
        let p = pieces(7, &code, "c");
        assert_eq!(p.len(), 2);
        let crc = format!("{:08x}", crc32(code.as_bytes()));
        assert!(p[0].starts_with(&format!("CODE 7 1/2 160 {} c ", crc)));
        assert!(p[1].starts_with(&format!("CODE 7 2/2 160 {} c ", crc)));
        assert!(p[0].contains("a\\\\b\\n"));
        // every line fits the kernel's line (CK_COURIER_LINE_MAX 320, with its NUL)
        let worst = "\\".repeat(CODE_MAX);
        for l in pieces(4_000_000_000, &worst, "objective-c++") {
            assert!(l.len() < 320, "{}", l.len());
        }
        assert!(pieces(1, "", "c").is_empty());
        assert!(pieces(1, "x\n", "").pop().unwrap().contains(" - x\\n"));
    }
}
