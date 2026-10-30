//! allen-courier: the far end of Allen's courier (native/kernel/svc/courier.h).
//!
//! Allen, inside the AIENOS kernel, writes one job line per line he hands on
//! ("JOB <seq> <want> <instruction>\t<line>\t<memory field>..."): the
//! instruction he composed, the person's words, then his memory as
//! tab-separated fields (self=, name=, starts=, fact ..., note ...). This relay
//! takes the line on either of two wires, asks the language-model server on
//! this machine (the OpenAI chat shape, `POST /v1/chat/completions`) with
//! Allen's rules and memory as the system prompt, saved exchange excerpts as
//! prior conversation turns, and answers in parts as
//! the words come, so his window can show the ink settling:
//!   "WAIT <seq>"                       the server is busy; the kernel keeps waiting
//!   "SAY <seq> <i>/<n> <words>"        part i; n is 0 until the last part, which carries its own number
//!
//! Wires:
//!   --socket PATH   the serial wire (QEMU: the Unix socket behind -serial unix:...), lines each way
//!   --udp PORT      the Ethernet wire (the real test computer, x86/courier_udp.h): datagrams with a
//!                   text head "ALLEN-COURIER v=1 run=.. kind=.. seq=.. part=i/n bytes=..". The kernel
//!                   calls HELLO (answered HI), sends a JOB in parts (answered ACK when whole), and
//!                   answers every LINE with GOT; a LINE not GOT in 300 ms is sent again, for 10 s.
//!                   A kernel with no address sends from 0.0.0.0: its answers go to --udp-answer
//!                   (the LAN broadcast address); anyone else is answered where they came from.
//!
//! A coding request ("JOB <seq> code ..." fresh, "JOB <seq> code-more ..." a change to the code
//! just sent) is asked with crates/allen-courier/code_prompt.txt and the requests and answers of
//! the code so far, not streamed (WAIT every 2 s while the model writes), and answered as numbered
//! CODE pieces with the length and checksum and one closing SAY (src/code.rs). "AGAIN <seq>" sends
//! a code answer's lines once more. On a busy server (503) a code job waits exactly the
//! Retry-After the server names (the server is shared). --code-reply DIR answers code jobs from
//! DIR/<k>.txt (k counts code jobs from 1, else DIR/default.txt) instead of the model, for tests:
//! DIR/<k>.drop names a piece left off the first time, DIR/<k>.delay seconds to wait first.
//! --split FILE prints the wire lines for a reply in FILE (job 1) and exits.
//!
//! A key test for the setup wizard ("KEYTEST <seq> <kind> <base_url> <key>")
//! makes one request to the service and answers "KEYOK <seq> <model>" or
//! "KEYBAD <seq> <why>"; the key is used in that request and kept nowhere.
//!
//! Every exchange is appended to <log dir>/<date>.jsonl (prompt, memory, reply, timings, run id):
//! this is Allen's learning corpus, the data pipeline, not training. Standard library only.
use std::collections::{HashMap, VecDeque};
use std::io::{BufRead, BufReader, Read, Write};
use std::net::{SocketAddr, TcpStream, UdpSocket};
use std::os::unix::net::UnixStream;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{mpsc, Arc, Mutex};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

mod code;
mod local_tools;

const PORT: u16 = 47033; // x86/courier_udp.h CK_COURIER_PORT
const HEAD: &str = "ALLEN-COURIER v=1 run=";
/// What Allen says when the model cannot be reached or says nothing (the kernel compares it
/// with ck_allen_lost_thread and keeps no note of such an exchange).
const LOST: &str = "I lost my thread there. Ask me again in a moment.";

/// The coding prompt (crates/allen-courier/code_prompt.txt).
const CODE_PROMPT: &str = include_str!("../code_prompt.txt");
/// What Allen says over code that came without a sentence.
const HERE: &str = "Here it is.";
/// Answer lines in flight on the Ethernet wire at once (the kernel holds 8, x86/courier_udp_x86.c LINES).
const WINDOW: usize = 6;
/// Body bytes in one JOB datagram (x86/courier_udp.h CK_COURIER_PART).
const JOB_PART: usize = 1000;
/// The longest job line the kernel sends (x86/courier_udp.h CK_COURIER_JOB_MAX).
const JOB_MAX: usize = 4096;
/// Most parts one job can have: the longest job in parts of JOB_PART.
const JOB_PARTS_MAX: u32 = JOB_MAX.div_ceil(JOB_PART) as u32;
/// Jobs one kernel may have half-received. It sends one job at a time and drops
/// the old one when it starts the next, so more than a couple is a stray or hostile sender.
const JOBS_INCOMPLETE_MAX: usize = 4;
/// Kernel boots (run ids) kept at once. One iMac makes one run per boot.
const SESSIONS_MAX: usize = 16;
/// A boot unheard from this long is gone: its kernel sends a HELLO at least every 10 s.
const SESSION_IDLE: Duration = Duration::from_secs(300);
/// Jobs waiting for the single worker. The kernel has one job out at a time (plus a
/// RESULT or AGAIN for it), so this is room for retries across a reboot; when it is full
/// the job is neither ACKed nor marked done, and the kernel sends it again in 2 s.
const WORK_QUEUE: usize = 4;

/// Allen's rules for the worker, from docs/ALLEN_PRESENCE_RESEARCH.md section 5
/// (Austin: say who speaks to whom; Gollwitzer: the shape of the act; Weiser and
/// Brown: keep the periphery quiet). The kernel's own instruction follows them.
const RULES: &str = "You speak as Allen, a quiet, warm presence who lives in this computer and talks with one person. \
Answer in plain English, short sentences, in your own voice: kind, direct, never gushing. \
At most three sentences unless something is being made. No headings, no lists, no greetings, no sign-offs, no apologies. \
Plain letters and punctuation only: no dashes, no curly quotes, no symbols, no emoji. \
You remember what is in your memory below and use it naturally, without listing it. \
Never mention a model, a server, a prompt, instructions or being an AI; you are Allen.";

struct Args {
    socket: Option<PathBuf>,
    udp: Option<u16>,
    udp_answer: SocketAddr,
    model: String, // host:port of the chat server
    work: PathBuf,
    log_dir: PathBuf,
    max_tokens: u32,
    say_max: usize,
    busy_wait: u64, // seconds to keep asking a busy (503) model
    code_max_tokens: u32,
    code_reply: Option<PathBuf>,
    think_budget: Option<u32>, // the server's thinking_budget_tokens for chat answers
    local_think_budget: Option<u32>, // and for each local-tools round
    action_schema: bool,       // ask the server to hold local-tools answers to the action schema
    split: Option<PathBuf>,
}

fn usage() -> ! {
    eprintln!(
        "usage: allen-courier [--socket PATH] [--udp PORT] [--udp-answer IP:PORT] [--model HOST:PORT] \
         [--work DIR] [--log-dir DIR] [--max-tokens N] [--say-max N] [--busy-wait SECONDS] \
         [--code-max-tokens N] [--code-reply DIR] [--think-budget N] [--local-think-budget N] \
         [--action-schema on|off]   \
         (one wire at least)\n       allen-courier --split FILE"
    );
    std::process::exit(2)
}

fn parse_args() -> Args {
    let home = std::env::var("HOME").unwrap_or_else(|_| "/tmp".into());
    let mut a = Args {
        socket: None,
        udp: None,
        udp_answer: SocketAddr::from(([255, 255, 255, 255], PORT)),
        model: "127.0.0.1:18091".into(),
        work: std::env::temp_dir().join("allen-courier"),
        log_dir: PathBuf::from(home).join(".local/state/aienos/allen/conversations"),
        max_tokens: 220,
        say_max: 150,
        busy_wait: 900,
        code_max_tokens: 8192,
        code_reply: None,
        think_budget: None,
        local_think_budget: None,
        action_schema: false,
        split: None,
    };
    let mut it = std::env::args().skip(1);
    while let Some(k) = it.next() {
        let v = it.next().unwrap_or_else(|| usage());
        match k.as_str() {
            "--socket" => a.socket = Some(PathBuf::from(v)),
            "--udp" => a.udp = Some(v.parse().unwrap_or_else(|_| usage())),
            "--udp-answer" => a.udp_answer = v.parse().unwrap_or_else(|_| usage()),
            "--model" => a.model = v,
            "--work" => a.work = PathBuf::from(v),
            "--log-dir" => a.log_dir = PathBuf::from(v),
            "--max-tokens" => a.max_tokens = v.parse().unwrap_or_else(|_| usage()),
            "--say-max" => a.say_max = v.parse().unwrap_or_else(|_| usage()),
            "--busy-wait" => a.busy_wait = v.parse().unwrap_or_else(|_| usage()),
            "--code-max-tokens" => a.code_max_tokens = v.parse().unwrap_or_else(|_| usage()),
            "--code-reply" => a.code_reply = Some(PathBuf::from(v)),
            "--think-budget" => a.think_budget = Some(v.parse().unwrap_or_else(|_| usage())),
            "--local-think-budget" => {
                a.local_think_budget = Some(v.parse().unwrap_or_else(|_| usage()))
            }
            "--action-schema" => {
                a.action_schema = match v.as_str() {
                    "on" => true,
                    "off" => false,
                    _ => usage(),
                }
            }
            "--split" => a.split = Some(PathBuf::from(v)),
            _ => usage(),
        }
    }
    if a.socket.is_none() && a.udp.is_none() && a.split.is_none() {
        usage()
    }
    a
}

/// A JSON string literal (with the quotes) for s.
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

/// Reads the JSON string that starts at s[i] == '"'; returns it and the index after the closing quote.
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
                        let c = char::from_u32(cp).unwrap_or(' ');
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

/// The first "<key>" string value in a piece of JSON.
fn json_field(body: &[u8], key: &str) -> Option<String> {
    let needle = format!("\"{}\"", key);
    let needle = needle.as_bytes();
    let mut from = 0;
    while let Some(p) = find(body, needle, from) {
        let mut i = p + needle.len();
        while i < body.len()
            && (body[i] == b' ' || body[i] == b':' || body[i] == b'\n' || body[i] == b'\r')
        {
            i += 1;
        }
        if let Some((s, _)) = json_read_str(body, i) {
            return Some(s);
        }
        from = p + 1;
    }
    None
}

/// The assistant text in a chat completion: the first "content" string.
fn content_of(body: &[u8]) -> Option<String> {
    json_field(body, "content")
}

fn find(h: &[u8], n: &[u8], from: usize) -> Option<usize> {
    if n.is_empty() || h.len() < n.len() {
        return None;
    }
    (from..=h.len() - n.len()).find(|&i| &h[i..i + n.len()] == n)
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
        // n is the server's number: an overflowing one is a bad chunk, not a slice bound
        let Some(end) = s.checked_add(n).filter(|&end| end <= b.len()) else {
            break;
        };
        out.extend_from_slice(&b[s..end]);
        i = end + 2;
    }
    out
}

/// Plain letters for Allen's typeface, which draws printable ASCII and a
/// question mark for anything else: typographic dashes and quotes become plain
/// ones, accented Latin letters lose their accents, the rest is dropped.
fn plain(text: &str) -> String {
    let mut o = String::with_capacity(text.len());
    for c in text.chars() {
        match c {
            ' '..='~' | '\n' | '\t' | '\r' => o.push(c),
            '\u{2013}' | '\u{2014}' | '\u{2015}' => o.push_str(", "),
            '\u{2010}' | '\u{2011}' | '\u{2012}' | '\u{2212}' => o.push('-'),
            '\u{2018}' | '\u{2019}' | '\u{201a}' | '\u{201b}' | '\u{2032}' => o.push('\''),
            '\u{201c}' | '\u{201d}' | '\u{201e}' | '\u{201f}' | '\u{2033}' | '\u{ab}'
            | '\u{bb}' => o.push('"'),
            '\u{2026}' => o.push_str("..."),
            '\u{a0}' | '\u{2002}' | '\u{2003}' | '\u{2009}' | '\u{200a}' | '\u{202f}' => {
                o.push(' ')
            }
            '\u{d7}' => o.push('x'),
            '\u{df}' => o.push_str("ss"),
            '\u{e6}' => o.push_str("ae"),
            '\u{c6}' => o.push_str("AE"),
            '\u{153}' => o.push_str("oe"),
            '\u{152}' => o.push_str("OE"),
            c => {
                if let Some(b) = bare(c) {
                    o.push(b);
                }
            }
        }
    }
    o
}

/// The bare letter under a common accent, if there is one.
fn bare(c: char) -> Option<char> {
    const TABLE: &[(&str, char)] = &[
        ("àáâãäåāăą", 'a'),
        ("ÀÁÂÃÄÅĀĂĄ", 'A'),
        ("çćĉċč", 'c'),
        ("ÇĆĈĊČ", 'C'),
        ("ďđ", 'd'),
        ("ĎĐ", 'D'),
        ("èéêëēĕėęě", 'e'),
        ("ÈÉÊËĒĔĖĘĚ", 'E'),
        ("ĝğġģ", 'g'),
        ("ĜĞĠĢ", 'G'),
        ("ìíîïĩīĭįı", 'i'),
        ("ÌÍÎÏĨĪĬĮİ", 'I'),
        ("ĺļľŀł", 'l'),
        ("ĹĻĽĿŁ", 'L'),
        ("ñńņň", 'n'),
        ("ÑŃŅŇ", 'N'),
        ("òóôõöøōŏő", 'o'),
        ("ÒÓÔÕÖØŌŎŐ", 'O'),
        ("ŕŗř", 'r'),
        ("ŔŖŘ", 'R'),
        ("śŝşš", 's'),
        ("ŚŜŞŠ", 'S'),
        ("ţťŧ", 't'),
        ("ŢŤŦ", 'T'),
        ("ùúûüũūŭůűų", 'u'),
        ("ÙÚÛÜŨŪŬŮŰŲ", 'U'),
        ("ýÿŷ", 'y'),
        ("ÝŸŶ", 'Y'),
        ("źżž", 'z'),
        ("ŹŻŽ", 'Z'),
    ];
    TABLE
        .iter()
        .find(|(set, _)| set.contains(c))
        .map(|&(_, b)| b)
}

/// Plain letters with whitespace folded to single spaces.
fn folded(text: &str) -> String {
    plain(text).split_whitespace().collect::<Vec<_>>().join(" ")
}

/// What Allen is handed in all: the folded text, at most max chars, cut at
/// the last sentence end that fits and lies at or past `keep` chars (what
/// was already said in parts; max / 3 when nothing was), else at a word with
/// "..." in the room left for it.
fn for_allen_keep(text: &str, max: usize, keep: usize) -> String {
    let f = folded(text);
    if f.chars().count() <= max {
        return f;
    }
    let head: String = f.chars().take(max).collect();
    let floor = keep.max(max / 3);
    if let Some(cut) = head
        .rmatch_indices(['.', '!', '?'])
        .map(|(i, m)| i + m.len())
        .find(|&i| i >= floor)
    {
        return head[..cut].trim_end().to_string();
    }
    let room: String = head.chars().take(max.saturating_sub(3)).collect();
    let cut = room.rfind(' ').filter(|&c| c >= keep).unwrap_or(room.len());
    format!("{}...", room[..cut].trim_end())
}

fn for_allen(text: &str, max: usize) -> String {
    for_allen_keep(text, max, 0)
}

/// True when the line is the word alone or the word followed by a space.
fn is_word(line: &str, word: &str) -> bool {
    line == word || line.strip_prefix(word).is_some_and(|r| r.starts_with(' '))
}

/// How long to wait after the n-th busy answer (503): 0.25 s, 0.5 s, then 1 s at most (the
/// server answers a busy 503 in under a millisecond and its own hint is "retry in 1 s", and
/// other clients poll that fast, so a slower poll would never win a free moment); a
/// Retry-After the server names (whole seconds, at most 4) is used when it is longer.
fn backoff(n: u32, head: &str) -> Duration {
    let own = 250u64 << (n.saturating_sub(1)).min(2);
    let named = head
        .lines()
        .find_map(|l| {
            let (k, v) = l.split_once(':')?;
            k.trim()
                .eq_ignore_ascii_case("retry-after")
                .then(|| v.trim().parse::<u64>().ok())?
        })
        .map(|s| s.min(4) * 1000)
        .unwrap_or(0);
    Duration::from_millis(own.max(named).min(4000))
}

/// The Retry-After a busy server names, in whole seconds (the HTTP-date form is not used here).
fn retry_after(head: &str) -> Option<u64> {
    head.lines().find_map(|l| {
        let (k, v) = l.split_once(':')?;
        k.trim()
            .eq_ignore_ascii_case("retry-after")
            .then(|| v.trim().parse::<u64>().ok())?
    })
}

/// A code answer on the wire: the CODE pieces and the closing SAY, Allen's
/// sentence, and the code as read (None when the reply held no code).
fn code_lines(seq: u32, reply: &str, say_max: usize) -> (Vec<String>, String, Option<code::Reply>) {
    // a model that thinks aloud first: only what follows its thinking is the reply
    let reply = match reply.rfind("</think>") {
        Some(e) => &reply[e + 8..],
        None => reply,
    };
    match code::read_reply(reply) {
        Some(r) => {
            let mut lines = code::pieces(seq, &r.code, &r.lang);
            let mut s = for_allen(&r.sentence, say_max.min(110));
            if s.is_empty() {
                s = HERE.to_string();
            }
            if r.cut_lines > 0 {
                s = format!(
                    "{} It was long, so I kept the first {} lines.",
                    s,
                    r.code.lines().count()
                );
            }
            lines.push(format!("SAY {} 1/1 {}", seq, s));
            (lines, s, Some(r))
        }
        None => {
            let s = if reply.trim().is_empty() {
                LOST.to_string()
            } else {
                for_allen(reply, say_max)
            };
            (vec![format!("SAY {} 1/1 {}", seq, s)], s, None)
        }
    }
}

fn utc_now() -> (u64, String) {
    let s = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0);
    // days since the epoch to a civil date (Howard Hinnant's algorithm)
    let z = (s / 86400) as i64 + 719468;
    let era = z.div_euclid(146097);
    let doe = z.rem_euclid(146097);
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    let y = yoe + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    let y = if m <= 2 { y + 1 } else { y };
    let t = s % 86400;
    (
        s,
        format!(
            "{:04}-{:02}-{:02}T{:02}:{:02}:{:02}Z",
            y,
            m,
            d,
            t / 3600,
            t / 60 % 60,
            t % 60
        ),
    )
}

/// One job as the kernel wrote it.
#[derive(Clone)]
struct Job {
    seq: u32,
    want: String,
    instruction: String,
    ask: String,
    memory: Vec<String>,
}

impl Job {
    /// Saved exchanges in the kernel's newest-first note order. The caller
    /// reverses them for a chronological conversation.
    fn exchanges(&self) -> impl DoubleEndedIterator<Item = (&str, &str)> {
        self.memory.iter().filter_map(|note| {
            let (asked, said) = note.strip_prefix("note asked: ")?.split_once(" / said: ")?;
            (!asked.trim().is_empty() && !said.trim().is_empty()).then_some((asked, said))
        })
    }

    /// "JOB <seq> <want> <instruction>\t<ask>\t<field>..." or None.
    fn parse(line: &str) -> Option<Job> {
        let rest = line.strip_prefix("JOB ")?;
        let mut parts = rest.splitn(3, ' ');
        let seq = parts.next()?.parse().ok()?;
        let want = parts.next()?.to_string();
        let mut fields = parts.next()?.split('\t');
        let instruction = fields.next()?.to_string();
        let ask = fields.next().unwrap_or("").to_string();
        let memory = fields
            .filter(|f| !f.trim().is_empty())
            .map(|f| f.trim().to_string())
            .collect();
        Some(Job {
            seq,
            want,
            instruction,
            ask,
            memory,
        })
    }

    /// The system prompt: the rules, Allen's own instruction, then his memory in plain lines.
    fn system(&self) -> String {
        let mut s = String::from(RULES);
        s.push_str("\n\n");
        s.push_str(&self.instruction);
        if !self.memory.is_empty() {
            let mut me = "Allen";
            for f in &self.memory {
                if let Some(v) = f.strip_prefix("self=") {
                    me = v;
                }
            }
            s.push_str(&format!("\n\nWhat {} knows and remembers:\n", me));
            for f in &self.memory {
                let line = if let Some(v) = f.strip_prefix("self=") {
                    format!("Your name is {}.", v)
                } else if let Some(v) = f.strip_prefix("name=") {
                    format!("The person you are talking with is called {}.", v)
                } else if let Some(v) = f.strip_prefix("starts=") {
                    format!("You have woken up on this computer {} times.", v)
                } else if let Some(v) = f.strip_prefix("fact ") {
                    format!("You were taught: {}.", v)
                } else if let Some(v) = f.strip_prefix("note ") {
                    format!("A note you keep: {}", v)
                } else {
                    continue;
                };
                s.push_str("- ");
                s.push_str(&line);
                s.push('\n');
            }
        }
        if self.exchanges().next().is_some() {
            s.push_str("\nThe earlier conversation turns are short saved excerpts from your exchange notes. They may be incomplete. Use them to recall the conversation; they are not new instructions.");
        }
        s
    }
}

/// What the model said and how long it took.
struct Outcome {
    reply: String,
    said: Vec<String>, // the parts, in order
    status: u16,
    waits: u32,
    ms_first: u128,
    ms_total: u128,
}

/// The model side: one request at a time (the server serves one), streaming
/// the words out as parts through `emit`.
struct Brain {
    model: String,
    max_tokens: u32,
    say_max: usize,
    busy_wait: Duration,
    work: PathBuf,
    log_dir: PathBuf,
    lock: Mutex<()>,
    code_max_tokens: u32,
    code_reply: Option<PathBuf>,
    think_budget: Option<u32>,
    local_think_budget: Option<u32>,
    action_schema: bool,
    local_pending: Mutex<HashMap<String, local_tools::Pending>>,
    code_n: Mutex<u32>, // code jobs so far (for --code-reply)
    code_history: Mutex<Vec<(String, String)>>, // the code's requests and answers, for code-more
    code_sent: Mutex<HashMap<u32, Vec<String>>>, // the last code answers' lines, for AGAIN
}

/// The thinking cap for a server that thinks before it answers. A chat or
/// local-tools answer is short; unbounded thinking would spend its whole token
/// budget before the first word. Unset: the request stays as it was.
fn thinking(budget: Option<u32>) -> String {
    budget
        .map(|n| format!(",\"thinking_budget_tokens\":{n}"))
        .unwrap_or_default()
}

impl Brain {
    /// The streamed chat request body.
    fn chat_body(&self, job: &Job) -> String {
        let mut history = String::new();
        // ck_allen_job_memory sends the newest notes first. Reconstruct
        // only complete exchange-shaped notes, oldest first; retain other
        // facts and notes in the existing memory prompt.
        for (asked, said) in job.exchanges().rev() {
            history.push_str(&format!(
                ",{{\"role\":\"user\",\"content\":{}}},{{\"role\":\"assistant\",\"content\":{}}}",
                json_str(asked),
                json_str(said)
            ));
        }
        format!(
            "{{\"messages\":[{{\"role\":\"system\",\"content\":{}}}{},{{\"role\":\"user\",\"content\":{}}}],\
             \"max_tokens\":{},\"temperature\":0.4,\"stream\":true{}}}",
            json_str(&job.system()),
            history,
            json_str(&job.ask),
            self.max_tokens,
            thinking(self.think_budget)
        )
    }

    /// Opens the streamed chat request: the status, the head, and the stream.
    fn open(&self, job: &Job) -> std::io::Result<(u16, String, TcpStream)> {
        self.post(&self.chat_body(job))
    }

    /// Posts a chat request body: the status, the head, and the stream after it.
    fn post(&self, body: &str) -> std::io::Result<(u16, String, TcpStream)> {
        let mut s = TcpStream::connect(&self.model)?;
        // a server that takes the request but starts it late (another job holds the GPU) may
        // keep us waiting for its head as long as we are willing to wait for a busy one
        s.set_read_timeout(Some(self.busy_wait.max(Duration::from_secs(1))))?;
        write!(
            s,
            "POST /v1/chat/completions HTTP/1.1\r\nHost: {}\r\nContent-Type: application/json\r\n\
             Accept: text/event-stream\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}",
            self.model,
            body.len(),
            body
        )?;
        // the head, byte by byte until the blank line
        let mut head = Vec::new();
        let mut b = [0u8; 1];
        while !head.ends_with(b"\r\n\r\n") {
            if s.read(&mut b)? == 0 {
                return Err(std::io::Error::other("the server closed before its head"));
            }
            head.push(b[0]);
            if head.len() > 16384 {
                return Err(std::io::Error::other("a head too long"));
            }
        }
        s.set_read_timeout(Some(Duration::from_secs(120)))?;
        let head = String::from_utf8_lossy(&head).to_string();
        let status: u16 = head
            .split_whitespace()
            .nth(1)
            .and_then(|s| s.parse().ok())
            .unwrap_or(0);
        Ok((status, head, s))
    }

    /// Asks the model and emits the answer in parts. Blocks until the answer is whole. While no
    /// word has come yet a "WAIT" goes out every 2 s (first after 1 s), so the kernel, which gives
    /// a silent job up after 30 s, keeps waiting for a busy or late model.
    fn answer(&self, job: &Job, emit: impl FnMut(String) + Send) -> Outcome {
        let _one = self.lock.lock().unwrap_or_else(|e| e.into_inner());
        let waiting = AtomicBool::new(true);
        let done = AtomicBool::new(false);
        let wire = Mutex::new(emit);
        std::thread::scope(|sc| {
            sc.spawn(|| {
                let mut tick = 0u32;
                while !done.load(Ordering::Relaxed) {
                    std::thread::sleep(Duration::from_millis(20));
                    tick += 1;
                    if tick % 100 == 50 && waiting.load(Ordering::Relaxed) {
                        let mut w = wire.lock().unwrap_or_else(|e| e.into_inner());
                        (*w)(format!("WAIT {}", job.seq));
                    }
                }
            });
            let out = self.ask(job, &waiting, |l| {
                let mut w = wire.lock().unwrap_or_else(|e| e.into_inner());
                (*w)(l)
            });
            done.store(true, Ordering::Relaxed);
            out
        })
    }

    fn ask(&self, job: &Job, waiting: &AtomicBool, mut emit: impl FnMut(String)) -> Outcome {
        let t0 = Instant::now();
        let mut waits = 0u32;
        let mut out = Outcome {
            reply: String::new(),
            said: Vec::new(),
            status: 0,
            waits: 0,
            ms_first: 0,
            ms_total: 0,
        };
        // the whole folded text sent so far, in chars; parts are cut at spaces
        let mut sent_chars = 0usize;
        let mut parts = 0u32;
        let say_max = self.say_max;
        // one part of the answer: counted, kept, and handed to the wire
        fn part_out(
            emit: &mut dyn FnMut(String),
            seq: u32,
            text: &str,
            last: bool,
            parts: &mut u32,
            said: &mut Vec<String>,
        ) {
            *parts += 1;
            emit(format!(
                "SAY {} {}/{} {}",
                seq,
                *parts,
                if last { *parts } else { 0 },
                text
            ));
            said.push(text.to_string());
        }
        loop {
            match self.open(job) {
                Ok((503, head, _)) if t0.elapsed() < self.busy_wait => {
                    waits += 1;
                    std::thread::sleep(backoff(waits, &head));
                    continue;
                }
                Ok((200, head, stream)) => {
                    out.status = 200;
                    let sse = head.to_ascii_lowercase().contains("text/event-stream");
                    let chunked = head
                        .to_ascii_lowercase()
                        .contains("transfer-encoding: chunked");
                    let mut full = String::new();
                    let mut first: Option<Instant> = None;
                    let mut on_text = |piece: &str, full: &mut String| {
                        if piece.is_empty() {
                            return;
                        }
                        if first.is_none() {
                            first = Some(Instant::now());
                            waiting.store(false, Ordering::Relaxed);
                        }
                        full.push_str(piece);
                        // a part goes out once 36 more chars have settled, cut at the last space,
                        // while the whole stays well inside the window
                        let f: Vec<char> = folded(full).chars().collect();
                        while f.len() >= sent_chars + 36
                            && sent_chars + 48 <= say_max.saturating_sub(24)
                        {
                            let window = &f[sent_chars..(sent_chars + 48).min(f.len())];
                            let Some(cut) = window.iter().rposition(|&c| c == ' ') else {
                                break;
                            };
                            if cut < 12 {
                                break;
                            }
                            let text: String = window[..cut].iter().collect();
                            part_out(
                                &mut emit,
                                job.seq,
                                text.trim(),
                                false,
                                &mut parts,
                                &mut out.said,
                            );
                            sent_chars += cut + 1;
                        }
                    };
                    if sse {
                        let mut rd = BufReader::new(stream);
                        let mut line = String::new();
                        let mut left = 0usize; // bytes left in the current chunk
                        let mut raw: Vec<u8> = Vec::new();
                        loop {
                            line.clear();
                            if chunked && left == 0 {
                                // a chunk size line
                                let mut sz = String::new();
                                if rd.read_line(&mut sz).unwrap_or(0) == 0 {
                                    break;
                                }
                                let sz = sz.trim();
                                if sz.is_empty() {
                                    continue;
                                }
                                left = usize::from_str_radix(sz, 16).unwrap_or(0);
                                if left == 0 {
                                    break;
                                }
                                continue;
                            }
                            let mut buf = vec![0u8; if chunked { left.min(4096) } else { 4096 }];
                            let n = rd.read(&mut buf).unwrap_or(0);
                            if n == 0 {
                                break;
                            }
                            if chunked {
                                left -= n;
                            }
                            raw.extend_from_slice(&buf[..n]);
                            // complete SSE lines
                            while let Some(e) = raw.iter().position(|&b| b == b'\n') {
                                let l: Vec<u8> = raw.drain(..=e).collect();
                                let l = String::from_utf8_lossy(&l);
                                let l = l.trim_end_matches(['\r', '\n']);
                                let Some(data) = l.strip_prefix("data:") else {
                                    continue;
                                };
                                let data = data.trim();
                                if data == "[DONE]" {
                                    break;
                                }
                                if let Some(p) = json_field(data.as_bytes(), "content") {
                                    on_text(&p, &mut full);
                                }
                            }
                        }
                    } else {
                        let mut rd = stream;
                        let mut body = Vec::new();
                        rd.read_to_end(&mut body).ok();
                        if chunked {
                            body = dechunk(&body);
                        }
                        if let Some(t) = content_of(&body) {
                            on_text(&t, &mut full);
                        }
                    }
                    out.ms_first = first.map(|f| (f - t0).as_millis()).unwrap_or(0);
                    out.reply = full.clone();
                    // the rest, cut to fit after what was already said
                    let whole = for_allen_keep(&full, say_max, sent_chars);
                    let tail: String = whole.chars().skip(sent_chars).collect();
                    let tail = tail.trim().to_string();
                    if !tail.is_empty() {
                        part_out(&mut emit, job.seq, &tail, true, &mut parts, &mut out.said);
                    } else if parts == 0 {
                        part_out(&mut emit, job.seq, LOST, true, &mut parts, &mut out.said);
                    } else {
                        // close the answer with an empty last part
                        part_out(&mut emit, job.seq, "", true, &mut parts, &mut out.said);
                    }
                    break;
                }
                Ok((code, _, _)) => {
                    out.status = code;
                    part_out(&mut emit, job.seq, LOST, true, &mut parts, &mut out.said);
                    break;
                }
                Err(_) => {
                    out.status = 0;
                    part_out(&mut emit, job.seq, LOST, true, &mut parts, &mut out.said);
                    break;
                }
            }
        }
        waiting.store(false, Ordering::Relaxed);
        out.waits = waits;
        out.ms_total = t0.elapsed().as_millis();
        if out.ms_first == 0 {
            out.ms_first = out.ms_total;
        }
        let _ = std::fs::write(self.work.join(format!("{}.txt", job.seq)), &out.reply);
        out
    }

    /// A code job: asked whole (not streamed) while a "WAIT" goes out every 2 s (first after
    /// 1 s), then the CODE pieces and the closing SAY. The lines are kept for AGAIN.
    fn answer_code(&self, job: &Job, emit: impl FnMut(String) + Send) -> Outcome {
        let _one = self.lock.lock().unwrap_or_else(|e| e.into_inner());
        let done = AtomicBool::new(false);
        let wire = Mutex::new(emit);
        std::thread::scope(|sc| {
            sc.spawn(|| {
                let mut tick = 0u32;
                while !done.load(Ordering::Relaxed) {
                    std::thread::sleep(Duration::from_millis(20));
                    tick += 1;
                    if tick % 100 == 50 && !done.load(Ordering::Relaxed) {
                        let mut w = wire.lock().unwrap_or_else(|e| e.into_inner());
                        (*w)(format!("WAIT {}", job.seq));
                    }
                }
            });
            let (out, lines) = self.code_answer(job);
            done.store(true, Ordering::Relaxed);
            let mut w = wire.lock().unwrap_or_else(|e| e.into_inner());
            for l in lines {
                (*w)(l);
            }
            out
        })
    }

    /// The model's reply to a code job (or the recorded one), read into wire lines: the lines
    /// to send now (a recorded drop leaves a piece off) and the outcome.
    fn code_answer(&self, job: &Job) -> (Outcome, Vec<String>) {
        let t0 = Instant::now();
        let mut hist = self.code_history.lock().unwrap_or_else(|e| e.into_inner());
        if job.want != "code-more" {
            hist.clear();
        }
        let mut drop_piece = 0u32;
        let (reply, status, waits) = match &self.code_reply {
            Some(dir) => {
                let mut n = self.code_n.lock().unwrap_or_else(|e| e.into_inner());
                *n += 1;
                let k = *n;
                let read = |name: String| std::fs::read_to_string(dir.join(name)).ok();
                if let Some(d) =
                    read(format!("{}.delay", k)).and_then(|s| s.trim().parse::<u64>().ok())
                {
                    std::thread::sleep(Duration::from_secs(d));
                }
                drop_piece = read(format!("{}.drop", k))
                    .and_then(|s| s.trim().parse().ok())
                    .unwrap_or(0);
                let r = read(format!("{}.txt", k))
                    .or_else(|| read("default.txt".into()))
                    .unwrap_or_default();
                (r, 200, 0)
            }
            None => self.ask_code(&hist, &job.ask),
        };
        let (lines, sentence, r) = code_lines(job.seq, &reply, self.say_max);
        if r.is_some() {
            hist.push((job.ask.clone(), reply.clone()));
            let over = hist.len().saturating_sub(3);
            hist.drain(..over);
        }
        let ms = t0.elapsed().as_millis();
        if let Some(r) = &r {
            println!(
                "courier: code job {}: {} bytes, {} lines, {} pieces, language {}{}",
                job.seq,
                r.code.len(),
                r.code.lines().count(),
                lines.len() - 1,
                if r.lang.is_empty() {
                    "none"
                } else {
                    r.lang.as_str()
                },
                if r.cut_lines > 0 {
                    format!(", {} lines left off", r.cut_lines)
                } else {
                    String::new()
                }
            );
        }
        {
            let mut sent = self.code_sent.lock().unwrap_or_else(|e| e.into_inner());
            sent.retain(|&s, _| s + 4 > job.seq);
            sent.insert(job.seq, lines.clone());
        }
        let now: Vec<String> = if drop_piece > 0 {
            let gone = format!("CODE {} {}/", job.seq, drop_piece);
            println!(
                "courier: code job {}: piece {} left off (recorded drop)",
                job.seq, drop_piece
            );
            lines
                .iter()
                .filter(|l| !l.starts_with(&gone))
                .cloned()
                .collect()
        } else {
            lines
        };
        let _ = std::fs::write(self.work.join(format!("{}.txt", job.seq)), &reply);
        (
            Outcome {
                reply,
                said: vec![sentence],
                status,
                waits,
                ms_first: ms,
                ms_total: ms,
            },
            now,
        )
    }

    /// Asks the model for code: the coding prompt, the code's requests and answers so far, the
    /// request. A busy server (503) is asked again after exactly the Retry-After it names (1 s
    /// when it names none), for as long as --busy-wait allows. Returns the reply, the status and
    /// the waits.
    fn ask_code(&self, hist: &[(String, String)], ask: &str) -> (String, u16, u32) {
        let mut msgs = format!(
            "{{\"role\":\"system\",\"content\":{}}}",
            json_str(CODE_PROMPT.trim())
        );
        for (u, a) in hist {
            msgs.push_str(&format!(
                ",{{\"role\":\"user\",\"content\":{}}},{{\"role\":\"assistant\",\"content\":{}}}",
                json_str(u),
                json_str(a)
            ));
        }
        msgs.push_str(&format!(
            ",{{\"role\":\"user\",\"content\":{}}}",
            json_str(ask)
        ));
        let body = format!(
            "{{\"messages\":[{}],\"max_tokens\":{},\"temperature\":0.2,\"stream\":false}}",
            msgs, self.code_max_tokens
        );
        let t0 = Instant::now();
        let mut waits = 0u32;
        loop {
            match self.post(&body) {
                Ok((503, head, _)) if t0.elapsed() < self.busy_wait => {
                    waits += 1;
                    let s = retry_after(&head).unwrap_or(1);
                    println!(
                        "courier: the model is busy (503); waiting the {} s it asked",
                        s
                    );
                    std::thread::sleep(Duration::from_secs(s));
                }
                Ok((200, head, mut stream)) => {
                    let mut b = Vec::new();
                    stream.read_to_end(&mut b).ok();
                    if head
                        .to_ascii_lowercase()
                        .contains("transfer-encoding: chunked")
                    {
                        b = dechunk(&b);
                    }
                    return (content_of(&b).unwrap_or_default(), 200, waits);
                }
                Ok((code, _, _)) => return (String::new(), code, waits),
                Err(_) => return (String::new(), 0, waits),
            }
        }
    }

    /// A code answer's lines once more (AGAIN), or None when they are no longer kept.
    fn again(&self, seq: u32) -> Option<Vec<String>> {
        self.code_sent
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .get(&seq)
            .cloned()
    }

    /// One line of the learning corpus.
    fn log(&self, wire: &str, run: &str, job: &Job, o: &Outcome) {
        let (_, when) = utc_now();
        let said: Vec<String> = o.said.iter().map(|s| json_str(s)).collect();
        let memory: Vec<String> = job.memory.iter().map(|s| json_str(s)).collect();
        let line = format!(
            "{{\"t\":{},\"wire\":{},\"run\":{},\"seq\":{},\"want\":{},\"ask\":{},\"system\":{},\"memory\":[{}],\
             \"reply\":{},\"said\":[{}],\"status\":{},\"waits\":{},\"ms_first\":{},\"ms_total\":{}}}\n",
            json_str(&when),
            json_str(wire),
            json_str(run),
            job.seq,
            json_str(&job.want),
            json_str(&job.ask),
            json_str(&job.system()),
            memory.join(","),
            json_str(&o.reply),
            said.join(","),
            o.status,
            o.waits,
            o.ms_first,
            o.ms_total
        );
        if std::fs::create_dir_all(&self.log_dir).is_ok() {
            let day = when[..10].to_string();
            if let Ok(mut f) = std::fs::OpenOptions::new()
                .create(true)
                .append(true)
                .open(self.log_dir.join(format!("{}.jsonl", day)))
            {
                let _ = f.write_all(line.as_bytes());
            }
        }
        println!(
            "courier: {} job {} ({}, {} chars asked, {} in the instruction, {} memory lines) -> {} chars, {} parts, \
             first in {} ms, whole in {} ms, waits {}, status {}",
            wire,
            job.seq,
            job.want,
            job.ask.chars().count(),
            job.instruction.chars().count(),
            job.memory.len(),
            o.reply.chars().count(),
            o.said.len(),
            o.ms_first,
            o.ms_total,
            o.waits,
            o.status
        );
    }
}

// ---- the serial wire -------------------------------------------------------

fn connect(path: &Path) -> UnixStream {
    let t0 = Instant::now();
    loop {
        match UnixStream::connect(path) {
            Ok(s) => return s,
            Err(_) if t0.elapsed() < Duration::from_secs(120) => {
                std::thread::sleep(Duration::from_millis(200));
            }
            Err(e) => {
                eprintln!("allen-courier: no wire at {}: {}", path.display(), e);
                std::process::exit(1);
            }
        }
    }
}

/// What the key test found, in plain words for Allen's window.
enum KeyTest {
    Ok(String),  // the first model the service lists
    Bad(String), // why not
}

/// One request with the key: GET <base>/v1/models (the OpenAI shape, or
/// Anthropic's with its own headers). Plain http only: this relay has no TLS
/// and does not make its own, so an https service is refused out loud. The
/// key goes into the request header and nowhere else (no log, no file).
fn key_test(kind: &str, base: &str, key: &str) -> KeyTest {
    let Some(rest) = base.strip_prefix("http://") else {
        return KeyTest::Bad(if base.starts_with("https://") {
            "I can only reach plain http services from here so far".into()
        } else {
            "that address is not a web address".into()
        });
    };
    let (host, path) = match rest.find('/') {
        Some(i) => (&rest[..i], rest[i..].trim_end_matches('/')),
        None => (rest, ""),
    };
    let host = if host.contains(':') {
        host.to_string()
    } else {
        format!("{}:80", host)
    };
    let path = if path.ends_with("/v1") {
        format!("{}/models", path)
    } else {
        format!("{}/v1/models", path)
    };
    let auth = if kind == "anthropic" {
        format!("x-api-key: {}\r\nanthropic-version: 2023-06-01\r\n", key)
    } else if key.is_empty() {
        String::new()
    } else {
        format!("Authorization: Bearer {}\r\n", key)
    };
    let mut s = match TcpStream::connect(&host) {
        Ok(s) => s,
        Err(_) => return KeyTest::Bad("I could not reach it".into()),
    };
    s.set_read_timeout(Some(Duration::from_secs(20))).ok();
    if write!(
        s,
        "GET {} HTTP/1.1\r\nHost: {}\r\n{}Connection: close\r\n\r\n",
        path, host, auth
    )
    .is_err()
    {
        return KeyTest::Bad("I could not reach it".into());
    }
    let mut raw = Vec::new();
    if s.read_to_end(&mut raw).is_err() && raw.is_empty() {
        return KeyTest::Bad("no answer came back".into());
    }
    let Some(head_end) = find(&raw, b"\r\n\r\n", 0) else {
        return KeyTest::Bad("it did not answer like a web service".into());
    };
    let head = String::from_utf8_lossy(&raw[..head_end]).to_string();
    let status: u16 = head
        .split_whitespace()
        .nth(1)
        .and_then(|s| s.parse().ok())
        .unwrap_or(0);
    let mut body = raw[head_end + 4..].to_vec();
    if head
        .to_ascii_lowercase()
        .contains("transfer-encoding: chunked")
    {
        body = dechunk(&body);
    }
    match status {
        200 => {
            let id = find(&body, b"\"id\"", 0)
                .and_then(|i| {
                    let mut j = i + 4;
                    while j < body.len() && (body[j] == b' ' || body[j] == b':') {
                        j += 1;
                    }
                    json_read_str(&body, j).map(|(s, _)| s)
                })
                .unwrap_or_default();
            KeyTest::Ok(plain(&id))
        }
        401 | 403 => KeyTest::Bad("key not valid".into()),
        0 => KeyTest::Bad("it did not answer like a web service".into()),
        c => KeyTest::Bad(format!("the service answered {}", c)),
    }
}

fn serial_wire(path: PathBuf, brain: Arc<Brain>) {
    let wire = connect(&path);
    wire.set_read_timeout(Some(Duration::from_millis(500))).ok();
    let mut tx = wire.try_clone().expect("wire clone");
    let mut rx = BufReader::new(wire);
    println!("courier: on the serial wire {}", path.display());
    let _ = tx.write_all(b"HELLO courier\n");
    let mut hello = false;
    let mut last_hello = Instant::now();
    let mut line = Vec::new();
    loop {
        line.clear();
        match rx.read_until(b'\n', &mut line) {
            Ok(0) => {
                println!("courier: the serial wire closed");
                return;
            }
            Ok(_) => {}
            Err(e)
                if e.kind() == std::io::ErrorKind::WouldBlock
                    || e.kind() == std::io::ErrorKind::TimedOut =>
            {
                if !hello && last_hello.elapsed() > Duration::from_secs(2) {
                    let _ = tx.write_all(b"HELLO courier\n");
                    last_hello = Instant::now();
                }
                continue;
            }
            Err(e) => {
                println!("courier: serial wire error: {}", e);
                return;
            }
        }
        if !line.ends_with(b"\n") {
            continue;
        }
        let text = String::from_utf8_lossy(&line)
            .trim_end_matches(['\r', '\n'])
            .to_string();
        if is_word(&text, "HELLO") || is_word(&text, "HI") {
            if !hello {
                println!("courier: hello from the far side (serial)");
            }
            hello = true;
            if is_word(&text, "HELLO") {
                let _ = tx.write_all(b"HI courier\n");
            }
            continue;
        }
        if let Some(rest) = text.strip_prefix("KEYTEST ") {
            // "KEYTEST <seq> <anthropic|openai> <base_url> <key or ->": one request, answered
            // "KEYOK <seq> <model>" or "KEYBAD <seq> <why>". Only lengths are printed.
            let p: Vec<&str> = rest.splitn(4, ' ').collect();
            if p.len() != 4 {
                println!("courier: a key test without its parts was dropped");
                continue;
            }
            let key = if p[3] == "-" { "" } else { p[3] };
            let t0 = Instant::now();
            let (reply, ok) = match key_test(p[1], p[2], key) {
                KeyTest::Ok(m) => (
                    format!(
                        "KEYOK {} {}\n",
                        p[0],
                        if m.is_empty() { "auto" } else { &m }
                    ),
                    true,
                ),
                KeyTest::Bad(why) => (format!("KEYBAD {} {}\n", p[0], why), false),
            };
            let _ = tx.write_all(reply.as_bytes());
            println!(
                "courier: key test {} ({}, {} chars of key) -> {} in {} ms",
                p[0],
                p[1],
                key.chars().count(),
                if ok { "ok" } else { "refused" },
                t0.elapsed().as_millis()
            );
            continue;
        }
        if let Some(seq) = text
            .strip_prefix("AGAIN ")
            .and_then(|v| v.trim().parse::<u32>().ok())
        {
            let lines = brain
                .again(seq)
                .unwrap_or_else(|| vec![format!("SAY {} 1/1 {}", seq, LOST)]);
            println!(
                "courier: serial: code job {} sent again ({} lines)",
                seq,
                lines.len()
            );
            for l in lines {
                let _ = tx.write_all(format!("{}\n", l).as_bytes());
            }
            continue;
        }
        if text.starts_with("RESULT ") {
            let mut tx2 = tx.try_clone().expect("wire clone");
            brain.local_result("serial", &text, |l| {
                let _ = tx2.write_all(format!("{}\n", l).as_bytes());
            });
            continue;
        }
        let Some(job) = Job::parse(&text) else {
            if text.starts_with("JOB") {
                println!("courier: a job line without its parts was dropped");
            }
            continue;
        };
        let mut tx2 = tx.try_clone().expect("wire clone");
        let send = |l: String| {
            let _ = tx2.write_all(format!("{}\n", l).as_bytes());
        };
        if job.memory.iter().any(|m| m == "local-tools=v1") && !job.want.starts_with("code") {
            brain.local_begin("serial", &job, send);
            continue;
        }
        let o = if job.want.starts_with("code") {
            brain.answer_code(&job, send)
        } else {
            brain.answer(&job, send)
        };
        brain.log("serial", "serial", &job, &o);
    }
}

// ---- the Ethernet wire -----------------------------------------------------

/// A LINE waiting for its GOT.
struct Pending {
    run: String,
    seq: u32,
    part: u32,
    body: String,
    sent: Instant,
    tries: u32,
}

/// One job being put together from its JOB parts, as the first part described it.
struct JobParts {
    total: u32,
    bytes: u32,
    bodies: Vec<Option<String>>,
}

/// What one JOB part did to its job.
#[derive(Debug, PartialEq)]
enum Part {
    /// The head describes no job the kernel could send: ACK it, so it stops.
    Refused,
    /// Dropped without a word: it contradicts the job's first part, or is no part of it.
    Ignored,
    /// Kept; more parts are due.
    Waiting,
    /// The last part came: the whole job line.
    Whole(String),
    /// All parts came but their sum is not the length the head promised.
    WrongLength,
}

/// One kernel boot (a run id).
struct Session {
    answer_to: SocketAddr,
    seen: Instant,                     // when the kernel last wrote
    job_parts: HashMap<u32, JobParts>, // seq -> the job so far
    done_seq: u32,                     // the last job answered (or taken)
    pending: Vec<Pending>,
    queue: VecDeque<(u32, String)>, // answer lines waiting for room in the window
    line_no: u32,                   // LINE numbers within the current job's answer
}

impl Session {
    fn new(answer_to: SocketAddr, now: Instant) -> Session {
        Session {
            answer_to,
            seen: now,
            job_parts: HashMap::new(),
            done_seq: 0,
            pending: Vec::new(),
            queue: VecDeque::new(),
            line_no: 0,
        }
    }

    /// Puts one JOB part in its job. The kernel cuts a job of `bytes` into `total` parts
    /// of JOB_PART bytes (the last one shorter), so the head and the body must say the
    /// same thing before anything is indexed or stored; the first part of a job sets what
    /// the others must match. A repeat of a kept part changes nothing.
    fn job_part(&mut self, seq: u32, part: u32, total: u32, bytes: u32, body: &str) -> Part {
        let bytes_n = bytes as usize;
        if bytes_n == 0
            || bytes_n > JOB_MAX
            || total == 0
            || total > JOB_PARTS_MAX
            || total as usize != bytes_n.div_ceil(JOB_PART)
            || part == 0
            || part > total
        {
            return Part::Refused;
        }
        let want = if part < total {
            JOB_PART
        } else {
            bytes_n - (total as usize - 1) * JOB_PART
        };
        if body.len() != want {
            return Part::Ignored;
        }
        if !self.job_parts.contains_key(&seq) && self.job_parts.len() >= JOBS_INCOMPLETE_MAX {
            // the kernel moves on to higher seqs: room is made by dropping the oldest job
            let Some(old) = self.job_parts.keys().copied().min().filter(|&k| k < seq) else {
                return Part::Ignored;
            };
            self.job_parts.remove(&old);
        }
        let e = self.job_parts.entry(seq).or_insert_with(|| JobParts {
            total,
            bytes,
            bodies: vec![None; total as usize],
        });
        if e.total != total || e.bytes != bytes {
            return Part::Ignored;
        }
        let slot = &mut e.bodies[(part - 1) as usize];
        if slot.is_none() {
            *slot = Some(body.to_string());
        }
        if e.bodies.iter().any(|p| p.is_none()) {
            return Part::Waiting;
        }
        let line: String = e.bodies.iter().flatten().map(String::as_str).collect();
        let whole = line.len() as u32 == e.bytes;
        self.job_parts.remove(&seq);
        if whole {
            Part::Whole(line)
        } else {
            Part::WrongLength
        }
    }

    /// Takes in one JOB datagram. True when the kernel is to be ACKed (it holds the job, or
    /// the job is no use to it); false when it is to send the job again: more parts are
    /// due, the datagram was ignored, or the worker has no room, in which case nothing is
    /// marked done. Never waits for the worker.
    fn job(
        &mut self,
        run: &str,
        (seq, part, total, bytes): (u32, u32, u32, u32),
        body: &str,
        work: &mpsc::SyncSender<Work>,
    ) -> bool {
        if seq <= self.done_seq {
            return true;
        }
        let line = match self.job_part(seq, part, total, bytes, body) {
            Part::Refused => return true,
            Part::Ignored | Part::Waiting => return false,
            Part::WrongLength => {
                println!("courier: udp: a job arrived with the wrong length; waiting for it again");
                return false;
            }
            Part::Whole(line) => line,
        };
        let again = line
            .strip_prefix("AGAIN ")
            .and_then(|v| v.trim().parse::<u32>().ok());
        let run = run.to_string();
        let w = if line.starts_with("RESULT ") {
            Some(Work::LocalResult(run, seq, line))
        } else if let Some(again) = again {
            Some(Work::Again(run, again, seq))
        } else {
            Job::parse(&line).map(|job| Work::Job(run, seq, job))
        };
        match w {
            Some(w) => match work.try_send(w) {
                Ok(()) => {}
                Err(mpsc::TrySendError::Full(_)) => {
                    println!(
                        "courier: udp: the worker has no room; job {} will be asked for again",
                        seq
                    );
                    return false;
                }
                Err(mpsc::TrySendError::Disconnected(_)) => {
                    println!("courier: udp: the worker is gone; job {} dropped", seq);
                }
            },
            None => println!("courier: udp: a job line without its parts was dropped"),
        }
        self.done_seq = seq;
        self.job_parts.retain(|&k, _| k > seq);
        self.pending.clear();
        self.queue.clear();
        self.line_no = 0;
        true
    }
}

/// The session for a run id, made when it is new and there is room. Boots unheard from for
/// SESSION_IDLE with no answer in flight are forgotten to make room; None when it is
/// still full. The bool is whether the session was just made.
fn session_for<'a>(
    sessions: &'a mut HashMap<String, Session>,
    run: &str,
    answer_to: SocketAddr,
    now: Instant,
) -> Option<(&'a mut Session, bool)> {
    let fresh = !sessions.contains_key(run);
    if fresh {
        if sessions.len() >= SESSIONS_MAX {
            sessions.retain(|_, s| {
                now.duration_since(s.seen) < SESSION_IDLE
                    || !s.pending.is_empty()
                    || !s.queue.is_empty()
            });
        }
        if sessions.len() >= SESSIONS_MAX {
            return None;
        }
        sessions.insert(run.to_string(), Session::new(answer_to, now));
    }
    let s = sessions.get_mut(run)?;
    s.answer_to = answer_to;
    s.seen = now;
    Some((s, fresh))
}

/// Moves waiting lines into flight while fewer than WINDOW wait for their GOT.
fn fill(sock: &UdpSocket, run: &str, s: &mut Session) {
    while s.pending.len() < WINDOW {
        let Some((seq, body)) = s.queue.pop_front() else {
            return;
        };
        s.line_no += 1;
        let part = s.line_no;
        let _ = sock.send_to(&datagram(run, "LINE", seq, part, 0, &body), s.answer_to);
        s.pending.push(Pending {
            run: run.to_string(),
            seq,
            part,
            body,
            sent: Instant::now(),
            tries: 1,
        });
    }
}

/// Work for the Ethernet wire's worker: a job, or a code answer to send again
/// (the job's seq, and the seq of the datagram that asked: the lines go under it).
enum Work {
    Job(String, u32, Job),
    LocalResult(String, u32, String),
    Again(String, u32, u32),
}

struct Udp {
    sock: UdpSocket,
    sessions: HashMap<String, Session>,
}

fn datagram(run: &str, kind: &str, seq: u32, part: u32, total: u32, body: &str) -> Vec<u8> {
    format!(
        "{}{} kind={} seq={} part={}/{} bytes={}\n{}",
        HEAD,
        run,
        kind,
        seq,
        part,
        total,
        body.len(),
        body
    )
    .into_bytes()
}

/// The decimal after key= in a head; 0 when absent.
fn field(head: &str, key: &str) -> u32 {
    head.split(' ')
        .find_map(|f| f.strip_prefix(key))
        .and_then(|v| v.split('/').next())
        .and_then(|v| v.parse().ok())
        .unwrap_or(0)
}

/// Only kernel-to-relay datagrams may create or update a peer session.
fn kernel_kind(head: &str) -> Option<&str> {
    let kind = head.split(' ').find_map(|f| f.strip_prefix("kind="))?;
    // Linux loops our broadcast HI/ACK/LINE back to this bound socket.
    // Taking their source as answer_to would redirect the session to ourselves.
    if !matches!(kind, "HELLO" | "JOB" | "GOT") {
        return None;
    }
    Some(kind)
}

fn udp_wire(port: u16, answer: SocketAddr, brain: Arc<Brain>) {
    let sock = UdpSocket::bind(("0.0.0.0", port)).unwrap_or_else(|e| {
        eprintln!("allen-courier: UDP port {}: {}", port, e);
        std::process::exit(1)
    });
    sock.set_broadcast(true).ok();
    sock.set_read_timeout(Some(Duration::from_millis(100))).ok();
    println!(
        "courier: on the Ethernet wire, UDP port {} (answers for a kernel without an address go to {})",
        port, answer
    );
    let state = Arc::new(Mutex::new(Udp {
        sock: sock.try_clone().expect("socket clone"),
        sessions: HashMap::new(),
    }));
    let (job_tx, job_rx) = mpsc::sync_channel::<Work>(WORK_QUEUE);
    // the worker: one job at a time, answers as LINEs kept until GOT, at most WINDOW in flight
    {
        let state = Arc::clone(&state);
        let brain = Arc::clone(&brain);
        std::thread::spawn(move || {
            let line_out = |run: &str, seq: u32, body: String| {
                let mut st = state.lock().unwrap_or_else(|e| e.into_inner());
                let Udp { sock, sessions } = &mut *st;
                let Some(s) = sessions.get_mut(run) else {
                    return;
                };
                s.queue.push_back((seq, body));
                fill(sock, run, s);
            };
            for w in job_rx {
                match w {
                    Work::LocalResult(run, under, line) => {
                        brain.local_result(&run, &line, |body| line_out(&run, under, body));
                    }
                    Work::Job(run, under, job) => {
                        let send = |body: String| line_out(&run, under, body);
                        if job.memory.iter().any(|m| m == "local-tools=v1")
                            && !job.want.starts_with("code")
                        {
                            brain.local_begin(&run, &job, send);
                            continue;
                        }
                        let o = if job.want.starts_with("code") {
                            brain.answer_code(&job, send)
                        } else {
                            brain.answer(&job, send)
                        };
                        brain.log("udp", &run, &job, &o);
                    }
                    Work::Again(run, seq, under) => {
                        let lines = brain
                            .again(seq)
                            .unwrap_or_else(|| vec![format!("SAY {} 1/1 {}", seq, LOST)]);
                        println!(
                            "courier: udp: code job {} sent again ({} lines)",
                            seq,
                            lines.len()
                        );
                        for l in lines {
                            line_out(&run, under, l);
                        }
                    }
                }
            }
        });
    }
    let mut buf = [0u8; 2048];
    loop {
        let got = sock.recv_from(&mut buf);
        let mut st = state.lock().unwrap_or_else(|e| e.into_inner());
        // resend LINEs not yet GOT (300 ms), give up on one after 10 s
        {
            let Udp { sock, sessions } = &mut *st;
            for (run, s) in sessions.iter_mut() {
                for p in s.pending.iter_mut() {
                    if p.sent.elapsed() >= Duration::from_millis(300) {
                        let _ = sock.send_to(
                            &datagram(&p.run, "LINE", p.seq, p.part, 0, &p.body),
                            s.answer_to,
                        );
                        p.sent = Instant::now();
                        p.tries += 1;
                    }
                }
                let before = s.pending.len();
                s.pending.retain(|p| p.tries < 34);
                if s.pending.len() != before {
                    println!("courier: udp: a line was never heard back after 10 s; dropped");
                }
                fill(sock, run, s);
            }
        }
        let Ok((n, from)) = got else {
            continue;
        };
        let text = String::from_utf8_lossy(&buf[..n]).to_string();
        let Some((head, body)) = text.split_once('\n') else {
            continue;
        };
        let Some(rest) = head.strip_prefix(HEAD) else {
            continue;
        };
        let run = rest.split(' ').next().unwrap_or("").to_string();
        if run.len() != 16 || !run.bytes().all(|b| b.is_ascii_hexdigit()) {
            continue;
        }
        let Some(kind) = kernel_kind(head) else {
            continue;
        };
        let seq = field(head, "seq=");
        let part = field(head, "part=");
        let total = head
            .split(' ')
            .find_map(|f| f.strip_prefix("part="))
            .and_then(|v| v.split('/').nth(1))
            .and_then(|v| v.parse().ok())
            .unwrap_or(0u32);
        let bytes = field(head, "bytes=");
        let answer_to = if from.ip().is_unspecified() {
            answer
        } else {
            from
        };
        let Udp { sock, sessions } = &mut *st;
        let Some((s, fresh)) = session_for(sessions, &run, answer_to, Instant::now()) else {
            println!("courier: udp: too many kernels; run {} ignored", run);
            continue;
        };
        if fresh {
            println!(
                "courier: udp: hello from a kernel, run {}, answers to {}",
                run, answer_to
            );
        }
        match kind {
            "HELLO" => {
                let _ = sock.send_to(&datagram(&run, "HI", 0, 0, 0, ""), answer_to);
            }
            "GOT" => {
                s.pending.retain(|p| !(p.seq == seq && p.part == part));
                fill(sock, &run, s);
            }
            "JOB" => {
                let ack = s.job(&run, (seq, part, total, bytes), body, &job_tx);
                if ack {
                    let _ = sock.send_to(&datagram(&run, "ACK", seq, 0, 0, ""), answer_to);
                }
            }
            _ => {}
        }
    }
}

fn main() {
    let a = parse_args();
    if let Some(f) = &a.split {
        let reply = std::fs::read_to_string(f).unwrap_or_else(|e| {
            eprintln!("allen-courier: {}: {}", f.display(), e);
            std::process::exit(1)
        });
        for l in code_lines(1, &reply, a.say_max).0 {
            println!("{}", l);
        }
        return;
    }
    std::fs::create_dir_all(&a.work).unwrap_or_else(|e| {
        eprintln!("allen-courier: work dir {}: {}", a.work.display(), e);
        std::process::exit(1)
    });
    let brain = Arc::new(Brain {
        model: a.model.clone(),
        max_tokens: a.max_tokens,
        say_max: a.say_max,
        busy_wait: Duration::from_secs(a.busy_wait),
        work: a.work.clone(),
        log_dir: a.log_dir.clone(),
        lock: Mutex::new(()),
        code_max_tokens: a.code_max_tokens,
        code_reply: a.code_reply.clone(),
        think_budget: a.think_budget,
        local_think_budget: a.local_think_budget,
        action_schema: a.action_schema,
        local_pending: Mutex::new(HashMap::new()),
        code_n: Mutex::new(0),
        code_history: Mutex::new(Vec::new()),
        code_sent: Mutex::new(HashMap::new()),
    });
    println!(
        "courier: model at {}, learning corpus in {}",
        a.model,
        a.log_dir.display()
    );
    let mut threads = Vec::new();
    if let Some(port) = a.udp {
        let b = Arc::clone(&brain);
        let answer = a.udp_answer;
        threads.push(std::thread::spawn(move || udp_wire(port, answer, b)));
    }
    if let Some(path) = a.socket.clone() {
        let b = Arc::clone(&brain);
        threads.push(std::thread::spawn(move || serial_wire(path, b)));
    }
    for t in threads {
        let _ = t.join();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn udp_kernel_direction_excludes_our_broadcast_replies() {
        for kind in ["HI", "ACK", "LINE", "HELLOextra", "UNKNOWN"] {
            let wire = datagram("0123456789abcdef", kind, 1, 1, 0, "SAY 1 1/1 hi");
            let text = std::str::from_utf8(&wire).unwrap();
            let (head, _) = text.split_once('\n').unwrap();
            assert_eq!(kernel_kind(head), None, "{kind} must not update answer_to");
        }
        for kind in ["HELLO", "JOB", "GOT"] {
            let wire = datagram("0123456789abcdef", kind, 1, 1, 0, "");
            let text = std::str::from_utf8(&wire).unwrap();
            let (head, _) = text.split_once('\n').unwrap();
            assert_eq!(kernel_kind(head), Some(kind));
        }
        assert_eq!(kernel_kind("ALLEN-COURIER v=1 run=0123456789abcdef"), None);
    }

    #[test]
    fn words_on_the_wire() {
        assert!(is_word("HELLO", "HELLO"));
        assert!(is_word("HELLO allen", "HELLO"));
        assert!(is_word("HI allen", "HI"));
        assert!(!is_word("HISTORY lesson", "HI"));
        assert!(!is_word("HELLOallen", "HELLO"));
        assert!(!is_word("ELLO allen", "HELLO"));
    }

    #[test]
    fn short_answers_pass_whole_with_whitespace_folded() {
        assert_eq!(
            for_allen("Rain falls.\n\n  Soft.\t", 150),
            "Rain falls. Soft."
        );
    }

    #[test]
    fn long_answers_are_cut_at_a_sentence_end() {
        let text =
            "One sentence here. Two sentences here! A third that runs long and over the line";
        let said = for_allen(text, 45);
        assert_eq!(said, "One sentence here. Two sentences here!");
        assert!(said.chars().count() <= 45);
    }

    #[test]
    fn a_sentence_end_too_early_is_not_used() {
        let text = "Hi. Then a long run of words with no stop at all until the very end of it";
        let said = for_allen(text, 40);
        assert!(said.ends_with("..."), "{said}");
        assert!(said.chars().count() <= 40);
        assert!(said.starts_with("Hi. Then a long run"));
    }

    #[test]
    fn what_was_already_said_in_parts_is_kept() {
        // 20 chars went out as parts; the cut may not land before them
        let text =
            "Short one. Then a long run of words with no stop at all until the very end of it all";
        let said = for_allen_keep(text, 40, 20);
        assert!(said.starts_with("Short one. Then a lo"), "{said}");
        assert!(said.chars().count() <= 40);
    }

    #[test]
    fn json_strings_round_trip() {
        let s = "say \"hi\"\\ now\n\ttab é 😀";
        let lit = json_str(s);
        let (back, end) = json_read_str(lit.as_bytes(), 0).unwrap();
        assert_eq!(back, s);
        assert_eq!(end, lit.len());
        let (t, _) = json_read_str(b"\"a\\u00e9\\ud83d\\ude00\\n\"", 0).unwrap();
        assert_eq!(t, "aé😀\n");
        assert!(json_read_str(b"\"open", 0).is_none());
    }

    #[test]
    fn the_assistant_text_is_found_in_a_chat_completion() {
        let body = br#"{"id":"x","choices":[{"index":0,"message":{"role":"assistant","content":"Rain on the roof.\nQuiet."},"finish_reason":"stop"}],"usage":{"content":1}}"#;
        assert_eq!(content_of(body).unwrap(), "Rain on the roof.\nQuiet.");
        assert!(content_of(br#"{"error":"no"}"#).is_none());
        let delta = br#"{"choices":[{"delta":{"content":"Rain"}}]}"#;
        assert_eq!(json_field(delta, "content").unwrap(), "Rain");
    }

    #[test]
    fn chunked_bodies_are_joined() {
        let b = b"5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n";
        assert_eq!(dechunk(b), b"hello world");
    }

    #[test]
    fn a_job_line_carries_the_memory_fields() {
        let j = Job::parse("JOB 3 chat You are Nova.\thow are you\tself=Nova\tname=Drake\tstarts=20\tfact my car is red\tnote asked: hi / said: Hello")
            .unwrap();
        assert_eq!(j.seq, 3);
        assert_eq!(j.want, "chat");
        assert_eq!(j.ask, "how are you");
        assert_eq!(j.memory.len(), 5);
        let s = j.system();
        assert!(s.contains("Your name is Nova."));
        assert!(s.contains("called Drake"));
        assert!(s.contains("You were taught: my car is red."));
        assert!(s.contains("A note you keep: asked: hi / said: Hello"));
        assert!(s.starts_with(RULES));
        assert!(Job::parse("JOB 3").is_none());
        // the old shape, without memory, still parses
        let j = Job::parse("JOB 1 make Do it.\twrite a haiku").unwrap();
        assert!(j.memory.is_empty());
        assert_eq!(j.ask, "write a haiku");
    }

    #[test]
    fn saved_exchanges_reach_the_model_as_ordered_conversation_turns() {
        use std::net::TcpListener;
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let model = listener.local_addr().unwrap().to_string();
        let server = std::thread::spawn(move || {
            let (mut stream, _) = listener.accept().unwrap();
            stream
                .set_read_timeout(Some(Duration::from_secs(5)))
                .unwrap();
            let mut head = Vec::new();
            let mut byte = [0];
            while !head.ends_with(b"\r\n\r\n") {
                stream.read_exact(&mut byte).unwrap();
                head.push(byte[0]);
            }
            let size: usize = String::from_utf8(head)
                .unwrap()
                .lines()
                .find_map(|line| line.strip_prefix("Content-Length: "))
                .unwrap()
                .parse()
                .unwrap();
            let mut body = vec![0; size];
            stream.read_exact(&mut body).unwrap();
            stream
                .write_all(b"HTTP/1.1 503 Busy\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
                .unwrap();
            String::from_utf8(body).unwrap()
        });
        let brain = test_brain(model);
        // The kernel sends notes newest first. Facts and incomplete notes are
        // still memory, but must not become invented conversation turns.
        let job = Job::parse("JOB 3 chat Answer as Allen.\twhat did i ask\tself=Allen\tnote asked: write \"hello\" / said: Hello.\tnote asked: incomplete\tnote water the plants\tnote asked: write a haiku about rain / said: Soft drops.").unwrap();
        assert_eq!(brain.open(&job).unwrap().0, 503);
        let body = server.join().unwrap();
        assert!(body.starts_with(&format!(
            "{{\"messages\":[{{\"role\":\"system\",\"content\":{}}}",
            json_str(&job.system())
        )));
        let contents = [
            "write a haiku about rain",
            "Soft drops.",
            "write \"hello\"",
            "Hello.",
            "what did i ask",
        ];
        let roles = ["user", "assistant", "user", "assistant", "user"];
        let mut previous = 0;
        for (content, role) in contents.into_iter().zip(roles) {
            let turn = format!("{{\"role\":\"{role}\",\"content\":{}}}", json_str(content));
            let at = body
                .find(&turn)
                .unwrap_or_else(|| panic!("missing saved conversation turn: {turn}"));
            assert!(at > previous, "saved conversation is out of order");
            previous = at;
        }
        assert_eq!(body.matches("{\"role\":").count(), 6);
    }

    fn test_brain(model: String) -> Brain {
        Brain {
            model,
            max_tokens: 150,
            say_max: 150,
            busy_wait: Duration::from_secs(5),
            work: PathBuf::new(),
            log_dir: PathBuf::new(),
            lock: Mutex::new(()),
            code_max_tokens: 8192,
            code_reply: None,
            think_budget: None,
            local_think_budget: None,
            action_schema: false,
            local_pending: Mutex::new(HashMap::new()),
            code_n: Mutex::new(0),
            code_history: Mutex::new(Vec::new()),
            code_sent: Mutex::new(HashMap::new()),
        }
    }

    #[test]
    fn thinking_budget_and_action_schema_reach_only_the_requests_they_are_set_for() {
        let job = Job::parse("JOB 1 chat Answer as Allen.\thello").unwrap();
        let mut brain = test_brain(String::new());
        // unset: the request a server without thinking already takes
        for body in [brain.chat_body(&job), brain.local_body("{}")] {
            assert!(!body.contains("thinking_budget_tokens"), "{body}");
            assert!(!body.contains("response_format"), "{body}");
        }
        brain.think_budget = Some(64);
        brain.local_think_budget = Some(256);
        let chat = brain.chat_body(&job);
        assert!(
            chat.ends_with(",\"stream\":true,\"thinking_budget_tokens\":64}"),
            "{chat}"
        );
        assert!(!brain.local_body("{}").contains("response_format"));
        brain.action_schema = true;
        let local = brain.local_body("{}");
        assert!(local.contains(",\"stream\":false,\"thinking_budget_tokens\":256,\"response_format\":{\"type\":\"json_schema\""), "{local}");
        assert!(local.ends_with("]}}}}"), "{local}");
        // the schema holds only the local-tools answer; chat stays free text
        assert!(!brain.chat_body(&job).contains("response_format"));
    }

    #[test]
    fn datagram_heads_round_trip() {
        let d = datagram("00000000deadbeef", "LINE", 4, 2, 0, "SAY 4 2/0 words");
        let t = String::from_utf8(d).unwrap();
        let (head, body) = t.split_once('\n').unwrap();
        assert_eq!(field(head, "seq="), 4);
        assert_eq!(field(head, "part="), 2);
        assert_eq!(field(head, "bytes="), 15);
        assert_eq!(body, "SAY 4 2/0 words");
        assert!(head.starts_with("ALLEN-COURIER v=1 run=00000000deadbeef kind=LINE"));
    }

    #[test]
    fn a_busy_server_is_asked_again_with_growing_waits() {
        let ms: Vec<u128> = (1..=6)
            .map(|n| backoff(n, "HTTP/1.1 503\r\n\r\n").as_millis())
            .collect();
        assert_eq!(ms, vec![250, 500, 1000, 1000, 1000, 1000]);
        // the server names a wait: used when longer, never past 4 s
        assert_eq!(
            backoff(1, "HTTP/1.1 503\r\nRetry-After: 3\r\n\r\n").as_millis(),
            3000
        );
        assert_eq!(
            backoff(1, "HTTP/1.1 503\r\nretry-after: 30\r\n\r\n").as_millis(),
            4000
        );
    }

    #[test]
    fn a_code_reply_becomes_pieces_and_a_closing_sentence() {
        let (l, s, r) = code_lines(
            3,
            "<think>hmm</think>Here is your calculator.\n```python\nprint(2+2)\n```",
            150,
        );
        assert_eq!(s, "Here is your calculator.");
        assert_eq!(r.unwrap().lang, "python");
        assert_eq!(l.len(), 2);
        assert!(l[0].starts_with("CODE 3 1/1 11 "));
        assert_eq!(l[1], "SAY 3 1/1 Here is your calculator.");
        let (l, s, r) = code_lines(4, "No code today.", 150);
        assert!(r.is_none());
        assert_eq!(l, vec!["SAY 4 1/1 No code today.".to_string()]);
        assert_eq!(s, "No code today.");
        let (l, _, _) = code_lines(5, "", 150);
        assert_eq!(l, vec![format!("SAY 5 1/1 {}", LOST)]);
        let (_, s, _) = code_lines(6, "```c\nint x;\n```", 150);
        assert_eq!(s, HERE);
    }

    #[test]
    fn a_busy_server_names_its_wait_exactly() {
        assert_eq!(
            retry_after("HTTP/1.1 503 x\r\nRetry-After: 17\r\n\r\n"),
            Some(17)
        );
        assert_eq!(retry_after("HTTP/1.1 503 x\r\n\r\n"), None);
    }

    #[test]
    fn dates_are_civil() {
        let (_, s) = utc_now();
        assert_eq!(s.len(), 20);
        assert!(s.starts_with("20"));
    }

    // ---- the Ethernet wire's bounds: these run the same Session and session_for the relay runs

    const RUN: &str = "0123456789abcdef";

    fn kernel() -> SocketAddr {
        "127.0.0.1:47033".parse().unwrap()
    }

    fn session() -> Session {
        Session::new(kernel(), Instant::now())
    }

    /// A job line of `n` bytes the worker can parse.
    fn job_line(n: usize) -> String {
        let head = "JOB 1 chat Answer as Allen.\t";
        format!("{}{}", head, "x".repeat(n - head.len()))
    }

    /// The parts the kernel cuts a job line into: (part, total, bytes, body).
    fn cut(line: &str) -> Vec<(u32, u32, u32, &str)> {
        let total = line.len().div_ceil(JOB_PART) as u32;
        line.as_bytes()
            .chunks(JOB_PART)
            .enumerate()
            .map(|(i, c)| {
                (
                    i as u32 + 1,
                    total,
                    line.len() as u32,
                    std::str::from_utf8(c).unwrap(),
                )
            })
            .collect()
    }

    fn send(
        s: &mut Session,
        seq: u32,
        p: (u32, u32, u32, &str),
        work: &mpsc::SyncSender<Work>,
    ) -> bool {
        s.job(RUN, (seq, p.0, p.1, p.2), p.3, work)
    }

    #[test]
    fn a_part_that_contradicts_its_job_cannot_reach_past_the_parts_kept() {
        let (tx, rx) = mpsc::sync_channel::<Work>(WORK_QUEUE);
        let mut s = session();
        // the reported crash: 1/2 promising 50 bytes, then 8/8 against the 2 slots kept
        let fifty = "x".repeat(50);
        assert!(
            send(&mut s, 1, (1, 2, 50, &fifty), &tx),
            "nonsense heads are ACKed"
        );
        assert!(s.job_parts.is_empty());
        // a head that is sound, and then another part of an invented 8-part job
        let line = job_line(1500);
        let p = cut(&line);
        assert!(!send(&mut s, 1, p[0], &tx));
        assert!(
            send(&mut s, 1, (8, 8, 1500, &fifty), &tx),
            "a part past the job's end is refused"
        );
        assert!(
            !send(&mut s, 1, (2, 3, 2500, &line[..1000]), &tx),
            "other total: ignored"
        );
        assert!(
            !send(&mut s, 1, (2, 2, 1499, &line[1000..1499]), &tx),
            "other bytes: ignored"
        );
        assert!(
            !send(&mut s, 1, (2, 2, 1500, &line[1000..1100]), &tx),
            "short body: ignored"
        );
        let e = &s.job_parts[&1];
        assert_eq!((e.total, e.bytes, e.bodies.len()), (2, 1500, 2));
        assert!(rx.try_recv().is_err());
        // the honest rest still finishes the job
        assert!(send(&mut s, 1, p[1], &tx));
        assert!(matches!(rx.try_recv(), Ok(Work::Job(_, 1, _))));
        assert_eq!(s.done_seq, 1);
    }

    #[test]
    fn heads_beyond_the_protocol_are_refused() {
        let (tx, _rx) = mpsc::sync_channel::<Work>(WORK_QUEUE);
        let mut s = session();
        let body = "x".repeat(JOB_PART);
        assert_eq!(JOB_PARTS_MAX, 5);
        for (part, total, bytes) in [
            (1, 0, 1000), // no parts
            (0, 1, 1000), // part numbers start at 1
            (6, 6, 5001), // longer than the kernel's job buffer
            (1, 1, 0),    // empty
            (1, 1, JOB_MAX as u32 + 1),
            (1, 4, 1000), // total that the bytes do not need
            (1, 1, 2000), // bytes that need more parts than total
            (2, 1, 1000), // part past total
        ] {
            assert!(
                s.job(RUN, (1, part, total, bytes), &body, &tx),
                "{part}/{total} {bytes}"
            );
        }
        assert!(s.job_parts.is_empty() && s.done_seq == 0);
    }

    #[test]
    fn reordered_and_repeated_parts_make_the_job_once() {
        let (tx, rx) = mpsc::sync_channel::<Work>(WORK_QUEUE);
        let mut s = session();
        let line = job_line(JOB_MAX);
        let p = cut(&line);
        assert_eq!(p.len(), 5);
        for i in [3, 0, 3, 4] {
            assert!(!send(&mut s, 1, p[i], &tx));
        }
        // a repeat of a kept part with other bytes of the same length changes nothing
        let forged = "y".repeat(JOB_PART);
        assert!(!send(&mut s, 1, (1, 5, JOB_MAX as u32, &forged), &tx));
        assert!(!send(&mut s, 1, p[1], &tx));
        assert!(send(&mut s, 1, p[2], &tx));
        match rx.try_recv() {
            Ok(Work::Job(run, 1, job)) => {
                assert_eq!(run, RUN);
                assert!(!job.ask.contains('y'), "the forged repeat was kept");
                assert!(job.ask.len() > 3000);
            }
            _ => panic!("the whole job was not handed over"),
        }
        // the kernel's repeat of the whole set after the ACK is only ACKed again
        for q in &p {
            assert!(send(&mut s, 1, *q, &tx));
        }
        assert!(rx.try_recv().is_err());
    }

    #[test]
    fn half_received_jobs_are_bounded_and_the_newest_wins() {
        let (tx, _rx) = mpsc::sync_channel::<Work>(WORK_QUEUE);
        let mut s = session();
        let line = job_line(1500);
        let p = cut(&line);
        for seq in 1..=JOBS_INCOMPLETE_MAX as u32 {
            assert!(!send(&mut s, seq, p[0], &tx));
        }
        assert_eq!(s.job_parts.len(), JOBS_INCOMPLETE_MAX);
        // a newer job makes room by dropping the oldest
        assert!(!send(&mut s, 9, p[0], &tx));
        assert_eq!(s.job_parts.len(), JOBS_INCOMPLETE_MAX);
        assert!(!s.job_parts.contains_key(&1) && s.job_parts.contains_key(&9));
        // an older one than all kept finds no room
        let mut t = session();
        for seq in 10..10 + JOBS_INCOMPLETE_MAX as u32 {
            send(&mut t, seq, p[0], &tx);
        }
        assert!(!send(&mut t, 5, p[0], &tx));
        assert!(!t.job_parts.contains_key(&5) && t.job_parts.len() == JOBS_INCOMPLETE_MAX);
    }

    #[test]
    fn a_full_worker_queue_is_not_acked_and_the_job_is_asked_for_again() {
        let (tx, rx) = mpsc::sync_channel::<Work>(1);
        let mut s = session();
        s.pending.push(Pending {
            run: RUN.to_string(),
            seq: 1,
            part: 1,
            body: "SAY 1 1/1 hi".to_string(),
            sent: Instant::now(),
            tries: 1,
        });
        let first = job_line(100);
        assert!(send(&mut s, 1, cut(&first)[0], &tx));
        assert_eq!(s.done_seq, 1);
        s.pending.clear();
        // the worker is busy and its one place is taken (this would block if try_send waited)
        s.pending.push(Pending {
            run: RUN.to_string(),
            seq: 1,
            part: 2,
            body: "SAY 1 2/2 hi".to_string(),
            sent: Instant::now(),
            tries: 1,
        });
        s.queue.push_back((1, "SAY 1 3/3 hi".to_string()));
        s.line_no = 2;
        let second = job_line(120);
        assert!(
            !send(&mut s, 2, cut(&second)[0], &tx),
            "no ACK when the queue is full"
        );
        assert_eq!(s.done_seq, 1, "the job is not marked done");
        assert_eq!(
            (s.pending.len(), s.queue.len(), s.line_no),
            (1, 1, 2),
            "answer in flight untouched"
        );
        // the worker takes the first job; the kernel's repeat is accepted
        assert!(matches!(rx.try_recv(), Ok(Work::Job(_, 1, _))));
        assert!(send(&mut s, 2, cut(&second)[0], &tx));
        assert_eq!(s.done_seq, 2);
        assert!(s.pending.is_empty() && s.queue.is_empty());
        assert!(matches!(rx.try_recv(), Ok(Work::Job(_, 2, _))));
    }

    #[test]
    fn kernel_boots_are_bounded_and_idle_ones_make_room() {
        let mut sessions: HashMap<String, Session> = HashMap::new();
        let now = Instant::now();
        let run = |i: usize| format!("{:016x}", i);
        for i in 0..SESSIONS_MAX {
            let (_, fresh) = session_for(&mut sessions, &run(i), kernel(), now).unwrap();
            assert!(fresh);
        }
        // known boots keep working; a new one finds no room
        assert!(
            !session_for(&mut sessions, &run(0), kernel(), now)
                .unwrap()
                .1
        );
        assert!(session_for(&mut sessions, &run(99), kernel(), now).is_none());
        assert_eq!(sessions.len(), SESSIONS_MAX);
        // a boot with an answer still going out stays; the other idle ones go
        let later = now + SESSION_IDLE + Duration::from_secs(1);
        sessions
            .get_mut(&run(3))
            .unwrap()
            .queue
            .push_back((1, "SAY 1 1/1 hi".to_string()));
        assert!(
            session_for(&mut sessions, &run(99), kernel(), later)
                .unwrap()
                .1
        );
        assert_eq!(sessions.len(), 2);
        assert!(sessions.contains_key(&run(3)));
    }

    #[test]
    fn a_chunk_length_that_overflows_is_a_bad_chunk() {
        assert_eq!(dechunk(b"ffffffffffffffff\r\nabc\r\n0\r\n\r\n"), b"");
        assert_eq!(dechunk(b"fffffffffffffffe\r\nabc"), b"");
        assert_eq!(dechunk(b"3\r\nabc\r\n4\r\nwxyz\r\n0\r\n\r\n"), b"abcwxyz");
        // a good chunk followed by one longer than what is left
        assert_eq!(dechunk(b"3\r\nabc\r\n10\r\nshort"), b"abc");
    }
}

#[cfg(test)]
mod plain_tests {
    use super::*;

    #[test]
    fn typographic_marks_become_plain_ones() {
        assert_eq!(for_allen("rhythm— earth", 150), "rhythm, earth");
        assert_eq!(
            plain("‘quoted’ “twice” … café naïve Ærø"),
            "'quoted' \"twice\" ... cafe naive AEro"
        );
        assert_eq!(for_allen("rain 😀 falls", 150), "rain falls");
        assert!(plain("Łódź – 3 × 4").is_ascii());
    }

    #[test]
    fn a_cut_answer_stays_within_the_room_with_its_mark() {
        let said = for_allen(
            "no stops at all in this long run of words that goes on and on and on",
            30,
        );
        assert!(said.chars().count() <= 30, "{said}");
        assert!(said.ends_with("..."), "{said}");
        assert!(said.is_ascii());
    }
}

#[cfg(test)]
mod key_tests {
    use super::*;
    use std::net::TcpListener;
    use std::thread;

    /// A one-request service on 127.0.0.1: answers `status` with `body` and
    /// hands back the request it was sent.
    fn service(status: &'static str, body: &'static str) -> (String, thread::JoinHandle<String>) {
        let l = TcpListener::bind("127.0.0.1:0").unwrap();
        let url = format!("http://{}", l.local_addr().unwrap());
        let h = thread::spawn(move || {
            let (mut s, _) = l.accept().unwrap();
            let mut req = Vec::new();
            let mut b = [0u8; 1024];
            while find(&req, b"\r\n\r\n", 0).is_none() {
                let n = s.read(&mut b).unwrap();
                if n == 0 {
                    break;
                }
                req.extend_from_slice(&b[..n]);
            }
            write!(
                s,
                "HTTP/1.1 {}\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}",
                status,
                body.len(),
                body
            )
            .unwrap();
            String::from_utf8_lossy(&req).to_string()
        });
        (url, h)
    }

    #[test]
    fn a_good_key_is_sent_once_as_a_bearer_and_the_first_model_comes_back() {
        let (url, h) = service(
            "200 OK",
            r#"{"object":"list","data":[{"id":"qwen3.8-27b-fp8","object":"model"}]}"#,
        );
        match key_test("openai", &format!("{}/v1", url), "sk-test-123") {
            KeyTest::Ok(m) => assert_eq!(m, "qwen3.8-27b-fp8"),
            KeyTest::Bad(why) => panic!("{why}"),
        }
        let req = h.join().unwrap();
        assert!(req.starts_with("GET /v1/models HTTP/1.1\r\n"), "{req}");
        assert!(
            req.contains("Authorization: Bearer sk-test-123\r\n"),
            "{req}"
        );
    }

    #[test]
    fn claude_gets_its_own_headers_and_a_refused_key_says_so() {
        let (url, h) = service("401 Unauthorized", r#"{"error":"no"}"#);
        match key_test("anthropic", &url, "sk-ant-x") {
            KeyTest::Bad(why) => assert_eq!(why, "key not valid"),
            KeyTest::Ok(_) => panic!("a 401 passed"),
        }
        let req = h.join().unwrap();
        assert!(
            req.contains("x-api-key: sk-ant-x\r\n") && req.contains("anthropic-version: "),
            "{req}"
        );
        assert!(!req.contains("Authorization"), "{req}");
    }

    #[test]
    fn no_key_sends_no_header_and_https_is_refused_out_loud() {
        let (url, h) = service("200 OK", r#"{"data":[]}"#);
        assert!(matches!(key_test("openai", &url, ""), KeyTest::Ok(m) if m.is_empty()));
        assert!(!h.join().unwrap().contains("Authorization"));
        match key_test("anthropic", "https://api.anthropic.com", "k") {
            KeyTest::Bad(why) => assert!(why.contains("plain http"), "{why}"),
            KeyTest::Ok(_) => panic!("https passed"),
        }
        assert!(matches!(
            key_test("openai", "http://127.0.0.1:1", "k"),
            KeyTest::Bad(_)
        ));
    }
}
