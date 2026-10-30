//! allen-scene: asks the model server for scenes, the way Allen's canvas will
//! (native/kernel/svc/allen_scene.h, docs/ALLEN_CANVAS.md), and keeps every raw
//! reply for the host test (scripts/ck_allen_canvas_test.sh --live).
//!
//! For each question (one per line of --questions; blank lines and lines
//! starting with '#' are skipped) it sends the system prompt
//! (crates/allen-courier/scene_prompt.txt, the same file the relay can
//! include) and the question to `POST /v1/chat/completions`, and writes
//! `NN-slug.txt` (the reply exactly as the model wrote it), `ask-NN-slug.txt`
//! (the question) and one line of `log.tsv` (status, time, tokens, finish).
//!
//! The server makes one answer at a time and says 503 while it is busy with
//! someone else's: this waits exactly as its Retry-After asks (1 to 15 s; 2 s
//! without one) for at most --wait-max seconds per question, and never sends
//! two requests at once. A reply already kept is not asked again, so a
//! round that was cut off resumes where it stopped. A server that is not
//! listening (restarting) is waited for the same way.
//! Standard library only: hand-built HTTP and JSON, as in the relay.
use std::io::{Read, Write};
use std::net::TcpStream;
use std::path::PathBuf;
use std::time::{Duration, Instant};

/// The system prompt, built in so the relay and this tool can never disagree.
pub const SCENE_PROMPT: &str = include_str!("../../scene_prompt.txt");

struct Args {
    model: String,
    questions: PathBuf,
    out: PathBuf,
    prompt: Option<PathBuf>,
    max_tokens: u32,
    wait_max: u64,
    temperature: String,
}

fn usage() -> ! {
    eprintln!(
        "usage: allen-scene --questions FILE --out DIR [--model HOST:PORT] [--prompt FILE] [--max-tokens N] \
         [--wait-max SECS] [--temperature T]"
    );
    std::process::exit(2)
}

fn parse_args() -> Args {
    let mut a = Args {
        model: "127.0.0.1:18091".into(),
        questions: PathBuf::new(),
        out: PathBuf::new(),
        prompt: None,
        max_tokens: 400,
        wait_max: 1800,
        temperature: "0.3".into(),
    };
    let mut it = std::env::args().skip(1);
    while let Some(k) = it.next() {
        let v = it.next().unwrap_or_else(|| usage());
        match k.as_str() {
            "--model" => a.model = v,
            "--questions" => a.questions = PathBuf::from(v),
            "--out" => a.out = PathBuf::from(v),
            "--prompt" => a.prompt = Some(PathBuf::from(v)),
            "--max-tokens" => a.max_tokens = v.parse().unwrap_or_else(|_| usage()),
            "--wait-max" => a.wait_max = v.parse().unwrap_or_else(|_| usage()),
            "--temperature" => a.temperature = v,
            _ => usage(),
        }
    }
    if a.questions.as_os_str().is_empty() || a.out.as_os_str().is_empty() {
        usage()
    }
    a
}

fn json_str(s: &str) -> String {
    let mut o = String::with_capacity(s.len() + 2);
    o.push('"');
    for c in s.chars() {
        match c {
            '"' => o.push_str("\\\""),
            '\\' => o.push_str("\\\\"),
            '\n' => o.push_str("\\n"),
            '\r' => o.push_str("\\r"),
            '\t' => o.push_str("\\t"),
            c if (c as u32) < 0x20 => o.push_str(&format!("\\u{:04x}", c as u32)),
            c => o.push(c),
        }
    }
    o.push('"');
    o
}

fn json_read_str(s: &[u8], mut i: usize) -> Option<(String, usize)> {
    if s.get(i) != Some(&b'"') {
        return None;
    }
    i += 1;
    let mut out: Vec<u8> = Vec::new();
    while i < s.len() {
        match s[i] {
            b'"' => return String::from_utf8(out).ok().map(|t| (t, i + 1)),
            b'\\' => {
                let e = *s.get(i + 1)?;
                i += 2;
                match e {
                    b'n' => out.push(b'\n'),
                    b't' => out.push(b'\t'),
                    b'r' => out.push(b'\r'),
                    b'b' | b'f' => out.push(b' '),
                    b'u' => {
                        let hex = std::str::from_utf8(s.get(i..i + 4)?).ok()?;
                        let mut cp = u32::from_str_radix(hex, 16).ok()?;
                        i += 4;
                        if (0xd800..0xdc00).contains(&cp) && s.get(i..i + 2) == Some(b"\\u") {
                            let lo = u32::from_str_radix(
                                std::str::from_utf8(s.get(i + 2..i + 6)?).ok()?,
                                16,
                            )
                            .ok()?;
                            if (0xdc00..0xe000).contains(&lo) {
                                cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                                i += 6;
                            }
                        }
                        let c = char::from_u32(cp).unwrap_or('?');
                        let mut b = [0u8; 4];
                        out.extend_from_slice(c.encode_utf8(&mut b).as_bytes());
                    }
                    c => out.push(c),
                }
            }
            c => {
                out.push(c);
                i += 1;
            }
        }
    }
    None
}

fn find(h: &[u8], n: &[u8], from: usize) -> Option<usize> {
    if n.is_empty() || h.len() < n.len() || from > h.len() - n.len() {
        return None;
    }
    (from..=h.len() - n.len()).find(|&i| &h[i..i + n.len()] == n)
}

/// The first JSON string value after `"key"` in body.
fn string_after(body: &[u8], key: &str) -> Option<String> {
    let needle = format!("\"{}\"", key);
    let mut from = 0;
    while let Some(p) = find(body, needle.as_bytes(), from) {
        let mut i = p + needle.len();
        while i < body.len() && matches!(body[i], b' ' | b':' | b'\n' | b'\r' | b'\t') {
            i += 1;
        }
        if let Some((s, _)) = json_read_str(body, i) {
            return Some(s);
        }
        from = p + 1;
    }
    None
}

/// The first number after `"key"` in body.
fn number_after(body: &[u8], key: &str) -> Option<u64> {
    let needle = format!("\"{}\"", key);
    let p = find(body, needle.as_bytes(), 0)? + needle.len();
    let digits: String = body[p..]
        .iter()
        .skip_while(|c| matches!(c, b' ' | b':'))
        .take_while(|c| c.is_ascii_digit())
        .map(|&c| c as char)
        .collect();
    digits.parse().ok()
}

fn dechunk(b: &[u8]) -> Vec<u8> {
    let mut out = Vec::new();
    let mut i = 0;
    while let Some(e) = find(b, b"\r\n", i) {
        let n = usize::from_str_radix(String::from_utf8_lossy(&b[i..e]).trim(), 16).unwrap_or(0);
        if n == 0 {
            break;
        }
        let s = e + 2;
        if s + n > b.len() {
            break;
        }
        out.extend_from_slice(&b[s..s + n]);
        i = s + n + 2;
    }
    out
}

/// One HTTP exchange: (status, head, body).
fn http(
    host: &str,
    method: &str,
    path: &str,
    body: &str,
) -> std::io::Result<(u16, String, Vec<u8>)> {
    let mut s = TcpStream::connect(host)?;
    s.set_read_timeout(Some(Duration::from_secs(300)))?;
    write!(
        s,
        "{} {} HTTP/1.1\r\nHost: {}\r\nContent-Type: application/json\r\nContent-Length: {}\r\n\
         Connection: close\r\n\r\n{}",
        method,
        path,
        host,
        body.len(),
        body
    )?;
    let mut raw = Vec::new();
    s.read_to_end(&mut raw)?;
    let end = find(&raw, b"\r\n\r\n", 0).ok_or_else(|| std::io::Error::other("no http head"))?;
    let head = String::from_utf8_lossy(&raw[..end]).to_string();
    let status = head
        .split_whitespace()
        .nth(1)
        .and_then(|s| s.parse().ok())
        .unwrap_or(0);
    let mut b = raw[end + 4..].to_vec();
    if head
        .to_ascii_lowercase()
        .contains("transfer-encoding: chunked")
    {
        b = dechunk(&b);
    }
    Ok((status, head, b))
}

fn retry_after(head: &str) -> Option<u64> {
    head.lines()
        .find_map(|l| {
            l.to_ascii_lowercase()
                .strip_prefix("retry-after:")
                .map(|v| v.trim().to_string())
        })
        .and_then(|v| v.parse().ok())
}

/// "what's 17% of 240" -> "whats-17-of-240" (at most five words).
fn slug(q: &str) -> String {
    let words: Vec<String> = q
        .split_whitespace()
        .map(|w| {
            w.chars()
                .filter(|c| c.is_ascii_alphanumeric())
                .collect::<String>()
                .to_ascii_lowercase()
        })
        .filter(|w| !w.is_empty())
        .take(5)
        .collect();
    words.join("-")
}

fn main() {
    let a = parse_args();
    let prompt = match &a.prompt {
        Some(p) => std::fs::read_to_string(p).unwrap_or_else(|e| {
            eprintln!("allen-scene: prompt {}: {}", p.display(), e);
            std::process::exit(1)
        }),
        None => SCENE_PROMPT.to_string(),
    };
    let questions = std::fs::read_to_string(&a.questions).unwrap_or_else(|e| {
        eprintln!("allen-scene: questions {}: {}", a.questions.display(), e);
        std::process::exit(1)
    });
    std::fs::create_dir_all(&a.out).ok();
    // the served model's id, sent back exactly (the server refuses any other)
    let model_id = http(&a.model, "GET", "/v1/models", "")
        .ok()
        .filter(|(s, _, _)| *s == 200)
        .and_then(|(_, _, b)| string_after(&b, "id"));
    println!(
        "allen-scene: model {} at {}",
        model_id.as_deref().unwrap_or("(unnamed)"),
        a.model
    );
    let mut log = std::fs::OpenOptions::new()
        .create(true)
        .append(true)
        .open(a.out.join("log.tsv"))
        .expect("log.tsv");
    let mut n = 0;
    let mut answered = 0;
    for q in questions
        .lines()
        .map(str::trim)
        .filter(|l| !l.is_empty() && !l.starts_with('#'))
    {
        n += 1;
        let name = format!("{:02}-{}", n, slug(q));
        let model_field = model_id
            .as_ref()
            .map(|m| format!("\"model\":{},", json_str(m)))
            .unwrap_or_default();
        let body = format!(
            "{{{}\"messages\":[{{\"role\":\"system\",\"content\":{}}},{{\"role\":\"user\",\"content\":{}}}],\
             \"max_tokens\":{},\"temperature\":{},\"stream\":false}}",
            model_field,
            json_str(&prompt),
            json_str(q),
            a.max_tokens,
            a.temperature
        );
        if a.out.join(format!("{}.txt", name)).exists() {
            println!("allen-scene: {} already answered, kept", name);
            answered += 1;
            continue; // a round cut off part way resumes where it stopped
        }
        let t0 = Instant::now();
        let mut wait = 5u64;
        let mut waited = 0u64;
        let mut busy = 0u32;
        let result = loop {
            match http(&a.model, "POST", "/v1/chat/completions", &body) {
                Ok((503, head, _)) if waited < a.wait_max => {
                    busy += 1;
                    // exactly as the server asks (its Retry-After, 1 s today; 2 s without one; at most 15 s): a busy
                    // answer costs it nothing, and a slower poll loses every free moment to faster callers
                    let w = retry_after(&head).unwrap_or(2).clamp(1, 15);
                    std::thread::sleep(Duration::from_secs(w));
                    waited += w;
                }
                Err(_) if waited < a.wait_max => {
                    // the server is not there (restarting): wait for it, 5 s doubling to 60 s
                    busy += 1;
                    std::thread::sleep(Duration::from_secs(wait));
                    waited += wait;
                    wait = (wait * 2).min(60);
                }
                other => break other,
            }
        };
        let ms = t0.elapsed().as_millis();
        match result {
            Ok((200, _, b)) => {
                let reply = string_after(&b, "content").unwrap_or_default();
                let finish = string_after(&b, "finish_reason").unwrap_or_default();
                let toks = number_after(&b, "completion_tokens").unwrap_or(0);
                std::fs::write(a.out.join(format!("{}.txt", name)), &reply).expect("write reply");
                std::fs::write(a.out.join(format!("ask-{}.txt", name)), format!("{}\n", q))
                    .expect("write question");
                let _ = writeln!(
                    log,
                    "{}\t200\t{} ms\t{} tokens\tfinish {}\t{} busy answers waited out\t{}",
                    name, ms, toks, finish, busy, q
                );
                println!(
                    "allen-scene: {} -> {} chars, {} tokens, {} ms ({} busy)",
                    name,
                    reply.len(),
                    toks,
                    ms,
                    busy
                );
                answered += 1;
            }
            Ok((code, _, b)) => {
                let _ = writeln!(
                    log,
                    "{}\t{}\t{} ms\t\t\t{} busy answers waited out\t{}",
                    name, code, ms, busy, q
                );
                println!(
                    "allen-scene: {} -> status {} ({})",
                    name,
                    code,
                    String::from_utf8_lossy(&b)
                        .chars()
                        .take(160)
                        .collect::<String>()
                );
            }
            Err(e) => {
                let _ = writeln!(log, "{}\terror\t{} ms\t\t\t{}\t{}", name, ms, e, q);
                println!("allen-scene: {} -> {}", name, e);
            }
        }
        std::thread::sleep(Duration::from_secs(1)); // gentle: never back to back
    }
    println!("allen-scene: {} of {} questions answered", answered, n);
    if answered == 0 {
        std::process::exit(1)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_prompt_is_plain_and_names_every_kind() {
        assert!(SCENE_PROMPT.is_ascii());
        for k in [
            "say",
            "mood",
            "note:",
            "list:",
            "number:",
            "quote:",
            "person to:",
            "timer:",
            "picture:",
            "video:",
        ] {
            assert!(SCENE_PROMPT.contains(k), "{k}");
        }
    }

    #[test]
    fn replies_and_numbers_are_read_from_a_completion() {
        let b = br#"{"choices":[{"message":{"role":"assistant","content":"say hi\nnumber: 2"},"finish_reason":"stop"}],"usage":{"completion_tokens": 7}}"#;
        assert_eq!(string_after(b, "content").unwrap(), "say hi\nnumber: 2");
        assert_eq!(string_after(b, "finish_reason").unwrap(), "stop");
        assert_eq!(number_after(b, "completion_tokens"), Some(7));
    }

    #[test]
    fn slugs_and_waits() {
        assert_eq!(slug("what's 17% of 240"), "whats-17-of-240");
        assert_eq!(retry_after("HTTP/1.1 503 Busy\r\nRetry-After: 4"), Some(4));
        assert_eq!(retry_after("HTTP/1.1 503 Busy"), None);
    }
}
