//! Allen's design tokens: one file in, three files out.
//!
//! `docs/design/DESIGN.md` (Google's DESIGN.md format) holds every colour, size, clock and curve of Allen's
//! look. Its YAML front matter carries the tokens the DESIGN.md tools know (colors, typography, rounded,
//! spacing, components); fenced blocks marked `allen-tokens` in its body carry the rest (motion, heartbeat,
//! boot, sound, contrast floors, reduced motion, keys). This crate reads both, resolves `{group.name}`
//! references, checks the contrast floors, and writes:
//!
//! - `allen_tokens.h` for the C kernel (integers only: colours packed as `CK_RGB`, durations in ms,
//!   ratios in Q8 / Q15 / Q16 fixed point, Bezier curves in Q16),
//! - `allen_tokens.rs` for the Rust runners (core types only, so `no_std` code can `include!` it),
//! - `allen-tokens.css` for web surfaces (custom properties, with a reduced-motion block).
//!
//! Only the YAML subset DESIGN.md uses is read: nested maps by two-space indent, `key: value` scalars,
//! optional double quotes. Anything else is an error, never a silent skip.

use std::fmt::Write as _;

/// One token: its dotted path (`colors.night`, `motion.letter`) and its value as written.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Token {
    pub path: String,
    pub raw: String,
}

/// A value, typed by its look.
#[derive(Debug, Clone, PartialEq)]
pub enum Value {
    Color(u8, u8, u8),
    /// A number and its unit (`px`, `ms`, `Hz`, `em`), as written.
    Dim(f64, String),
    /// A plain number, written quoted (`"0.030"`).
    Ratio(f64),
    /// `cubic-bezier(x1, y1, x2, y2)`.
    Bezier([f64; 4]),
    Text(String),
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Error(pub String);

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(&self.0)
    }
}

impl std::error::Error for Error {}

fn err<T>(msg: impl Into<String>) -> Result<T, Error> {
    Err(Error(msg.into()))
}

/// All tokens of one DESIGN.md, in the order written, references resolved.
#[derive(Debug, Clone, Default)]
pub struct Tokens {
    pub tokens: Vec<Token>,
}

impl Tokens {
    pub fn get(&self, path: &str) -> Option<&str> {
        self.tokens
            .iter()
            .find(|t| t.path == path)
            .map(|t| t.raw.as_str())
    }

    /// The tokens of one top-level group (`colors`), with the group prefix removed.
    pub fn group<'a>(&'a self, group: &'a str) -> impl Iterator<Item = (&'a str, &'a str)> + 'a {
        self.tokens.iter().filter_map(move |t| {
            t.path
                .strip_prefix(group)
                .and_then(|rest| rest.strip_prefix('.'))
                .map(|rest| (rest, t.raw.as_str()))
        })
    }

    pub fn groups(&self) -> Vec<&str> {
        let mut out: Vec<&str> = Vec::new();
        for t in &self.tokens {
            let g = t.path.split('.').next().unwrap_or("");
            if !out.contains(&g) {
                out.push(g);
            }
        }
        out
    }
}

/// Reads the YAML subset: returns (path, value) pairs for every scalar, in order.
fn parse_yaml(text: &str, first_line: usize) -> Result<Vec<Token>, Error> {
    let mut out = Vec::new();
    let mut stack: Vec<(usize, String)> = Vec::new();
    for (i, line) in text.lines().enumerate() {
        let n = first_line + i;
        let body = line.trim_end();
        if body.trim().is_empty() || body.trim_start().starts_with('#') {
            continue;
        }
        let indent = body.len() - body.trim_start().len();
        if !indent.is_multiple_of(2) {
            return err(format!("line {n}: indent must be a multiple of two spaces"));
        }
        let Some((key, value)) = body.trim_start().split_once(':') else {
            return err(format!("line {n}: expected `key: value`"));
        };
        let key = key.trim();
        if key.is_empty() || key.contains(' ') {
            return err(format!("line {n}: bad key `{key}`"));
        }
        while stack.last().is_some_and(|(d, _)| *d >= indent) {
            stack.pop();
        }
        if stack.len() * 2 != indent {
            return err(format!("line {n}: indent jumps a level"));
        }
        let path = stack
            .iter()
            .map(|(_, k)| k.as_str())
            .chain(std::iter::once(key))
            .collect::<Vec<_>>()
            .join(".");
        let value = value.trim();
        if value.is_empty() {
            stack.push((indent, key.to_string()));
            continue;
        }
        let raw = match value.strip_prefix('"') {
            Some(rest) => match rest.strip_suffix('"') {
                Some(inner) => inner.to_string(),
                None => return err(format!("line {n}: unclosed quote")),
            },
            None => value.to_string(),
        };
        out.push(Token { path, raw });
    }
    Ok(out)
}

/// Reads a DESIGN.md: the front matter and every ```allen-tokens block.
pub fn parse(design_md: &str) -> Result<Tokens, Error> {
    let Some(rest) = design_md.strip_prefix("---\n") else {
        return err("DESIGN.md must start with a `---` front matter line");
    };
    let Some(end) = rest.find("\n---\n") else {
        return err("front matter has no closing `---`");
    };
    let mut tokens = parse_yaml(&rest[..end], 2)?;
    let body_start = 4 + end + 5;
    let body = &design_md[body_start..];
    let body_first_line = design_md[..body_start].lines().count() + 1;
    let mut lines = body.lines().enumerate();
    while let Some((i, line)) = lines.next() {
        if line.trim_end() != "```allen-tokens" {
            continue;
        }
        let mut block = String::new();
        let mut closed = false;
        for (_, l) in lines.by_ref() {
            if l.trim_end() == "```" {
                closed = true;
                break;
            }
            block.push_str(l);
            block.push('\n');
        }
        if !closed {
            return err(format!(
                "allen-tokens block at line {} is not closed",
                body_first_line + i
            ));
        }
        tokens.extend(parse_yaml(&block, body_first_line + i + 1)?);
    }
    for (i, t) in tokens.iter().enumerate() {
        if tokens[..i].iter().any(|u| u.path == t.path) {
            return err(format!("token `{}` is defined twice", t.path));
        }
    }
    let mut out = Tokens { tokens };
    resolve(&mut out)?;
    Ok(out)
}

fn resolve(t: &mut Tokens) -> Result<(), Error> {
    for i in 0..t.tokens.len() {
        let mut raw = t.tokens[i].raw.clone();
        let mut hops = 0;
        while let Some(target) = raw.strip_prefix('{').and_then(|r| r.strip_suffix('}')) {
            hops += 1;
            if hops > 8 {
                return err(format!("`{}`: reference loop", t.tokens[i].path));
            }
            let found = match t.get(target) {
                Some(v) => v.to_string(),
                None if t.group(target).next().is_some() => {
                    // a whole group (a typography role): keep the reference, emitters expand it
                    break;
                }
                None => {
                    return err(format!(
                        "`{}` points at `{target}`, which does not exist",
                        t.tokens[i].path
                    ))
                }
            };
            raw = found;
        }
        t.tokens[i].raw = raw;
    }
    Ok(())
}

fn hex2(s: &str) -> Option<u8> {
    u8::from_str_radix(s, 16).ok()
}

/// Types a raw value by its look.
pub fn value(raw: &str) -> Value {
    if let Some(h) = raw.strip_prefix('#') {
        if h.len() == 6 {
            if let (Some(r), Some(g), Some(b)) = (hex2(&h[0..2]), hex2(&h[2..4]), hex2(&h[4..6])) {
                return Value::Color(r, g, b);
            }
        }
    }
    if let Some(args) = raw
        .strip_prefix("cubic-bezier(")
        .and_then(|r| r.strip_suffix(')'))
    {
        let v: Vec<f64> = args
            .split(',')
            .filter_map(|a| a.trim().parse().ok())
            .collect();
        if v.len() == 4 {
            return Value::Bezier([v[0], v[1], v[2], v[3]]);
        }
    }
    if let Ok(n) = raw.parse::<f64>() {
        return Value::Ratio(n);
    }
    let split = raw
        .find(|c: char| !(c.is_ascii_digit() || c == '.' || c == '-'))
        .unwrap_or(raw.len());
    if split > 0 {
        if let Ok(n) = raw[..split].parse::<f64>() {
            let unit = &raw[split..];
            if unit.chars().all(|c| c.is_ascii_alphabetic()) {
                return Value::Dim(n, unit.to_string());
            }
        }
    }
    Value::Text(raw.to_string())
}

/// WCAG 2.2 relative luminance of an sRGB colour.
pub fn luminance(r: u8, g: u8, b: u8) -> f64 {
    let lin = |c: u8| {
        let c = f64::from(c) / 255.0;
        if c <= 0.04045 {
            c / 12.92
        } else {
            ((c + 0.055) / 1.055).powf(2.4)
        }
    };
    0.2126 * lin(r) + 0.7152 * lin(g) + 0.0722 * lin(b)
}

/// WCAG contrast ratio between two colours (1.0 to 21.0).
pub fn contrast(a: (u8, u8, u8), b: (u8, u8, u8)) -> f64 {
    let (la, lb) = (luminance(a.0, a.1, a.2), luminance(b.0, b.1, b.2));
    let (hi, lo) = if la > lb { (la, lb) } else { (lb, la) };
    (hi + 0.05) / (lo + 0.05)
}

/// One measured pair from the `contrast` group.
#[derive(Debug, Clone)]
pub struct ContrastResult {
    pub fg: String,
    pub bg: String,
    /// Opacity the foreground is drawn at (1.0 opaque).
    pub alpha: f64,
    pub ratio: f64,
    pub floor: f64,
    /// Gated pairs fail the build below their floor; report-only pairs (the kernel today) do not.
    pub gated: bool,
}

impl ContrastResult {
    pub fn passes(&self) -> bool {
        self.ratio >= self.floor
    }
}

/// The foreground as it lands on the background when drawn at `alpha` (sRGB blend, as the kernel does).
pub fn blend(fg: (u8, u8, u8), bg: (u8, u8, u8), alpha: f64) -> (u8, u8, u8) {
    let mix = |f: u8, b: u8| (f64::from(b) + (f64::from(f) - f64::from(b)) * alpha).round() as u8;
    (mix(fg.0, bg.0), mix(fg.1, bg.1), mix(fg.2, bg.2))
}

fn alpha_of(t: &Tokens, group: &str, name: &str) -> Result<Option<f64>, Error> {
    match t.get(&format!("{group}.{name}")).map(value) {
        None => Ok(None),
        Some(Value::Ratio(a)) if (0.0..=1.0).contains(&a) => Ok(Some(a)),
        Some(_) => err(format!("{group}.{name}: must be a number from 0 to 1")),
    }
}

fn color_of(t: &Tokens, name: &str) -> Result<(u8, u8, u8), Error> {
    match t.get(&format!("colors.{name}")).map(value) {
        Some(Value::Color(r, g, b)) => Ok((r, g, b)),
        _ => err(format!(
            "contrast pair names `{name}`, which is not a colour"
        )),
    }
}

/// Measures every `contrast.<fg>-on-<bg>` pair against its floor.
pub fn check_contrast(t: &Tokens) -> Result<Vec<ContrastResult>, Error> {
    let mut out = Vec::new();
    for (pair, floor) in t.group("contrast") {
        let Some((fg, bg)) = pair.split_once("-on-") else {
            return err(format!("contrast.{pair}: name must be <fg>-on-<bg>"));
        };
        let Value::Ratio(floor) = value(floor) else {
            return err(format!("contrast.{pair}: floor must be a number"));
        };
        let (f, b) = (color_of(t, fg)?, color_of(t, bg)?);
        let alpha = alpha_of(t, "drawn-alpha", fg)?.unwrap_or(1.0);
        out.push(ContrastResult {
            fg: fg.to_string(),
            bg: bg.to_string(),
            alpha,
            ratio: contrast(blend(f, b, alpha), b),
            floor,
            gated: true,
        });
        if let Some(today) = alpha_of(t, "kernel-today-alpha", fg)? {
            out.push(ContrastResult {
                fg: fg.to_string(),
                bg: bg.to_string(),
                alpha: today,
                ratio: contrast(blend(f, b, today), b),
                floor,
                gated: false,
            });
        }
    }
    Ok(out)
}

/// The contrast report written next to the exports.
pub fn contrast_report(results: &[ContrastResult]) -> String {
    let mut s = String::from(
        "# Generated from docs/design/DESIGN.md by allen-design-tokens. Do not edit.\n\
         # WCAG 2.2 contrast of each declared pair (foreground on background), measured as drawn:\n\
         # the foreground blended over the background at its drawing opacity first.\n\
         # REPORT lines are the kernel as it draws today; they do not gate the build.\n",
    );
    for r in results {
        let _ = writeln!(
            s,
            "{:<10} on {:<6} at {:>5.3}  {:>6.2}:1  floor {:>4.1}  {}",
            r.fg,
            r.bg,
            r.alpha,
            r.ratio,
            r.floor,
            match (r.gated, r.passes()) {
                (true, true) => "PASS",
                (true, false) => "FAIL",
                (false, true) => "REPORT pass",
                (false, false) => "REPORT below floor",
            }
        );
    }
    s
}

fn ident(path: &str) -> String {
    path.chars()
        .map(|c| {
            if c.is_ascii_alphanumeric() {
                c.to_ascii_uppercase()
            } else {
                '_'
            }
        })
        .collect()
}

fn q(v: f64, one: f64) -> i64 {
    (v * one).round() as i64
}

const HEADER: &str = "Generated from docs/design/DESIGN.md by allen-design-tokens. Do not edit; change DESIGN.md and run\n   `cargo run -p allen-design-tokens -- generate`.";

/// The C header: integers only, for the kernel.
pub fn emit_c(t: &Tokens) -> String {
    let mut s = format!(
        "/* allen_tokens.h -- Allen's look as numbers.\n * {}\n * Colours are packed as CK_RGB (r << 16 | g << 8 | b). Durations in ms. Ratios in Q8 (/256), Q15 (/32767)\n * and Q16 (/65536). Bezier control points in Q16. */\n#ifndef AIENOS_ALLEN_TOKENS_H\n#define AIENOS_ALLEN_TOKENS_H\n",
        HEADER.replace('\n', "\n *")
    );
    for g in t.groups() {
        let _ = write!(s, "\n/* {g} */\n");
        for (name, raw) in t.group(g) {
            let id = format!("ALLEN_{}_{}", ident(g), ident(name));
            match value(raw) {
                Value::Color(r, gg, b) => {
                    let _ = writeln!(s, "#define {id} 0x{r:02X}{gg:02X}{b:02X}u");
                }
                Value::Dim(n, unit) => {
                    let unit_id = ident(&unit);
                    if n.fract() == 0.0 && n >= 0.0 {
                        let _ = writeln!(s, "#define {id}_{unit_id} {}u", n as i64);
                    } else {
                        let _ = writeln!(s, "#define {id}_{unit_id}_X1000 {}", q(n, 1000.0));
                    }
                }
                Value::Ratio(n) if n.fract() == 0.0 && n.abs() >= 2.0 => {
                    let _ = writeln!(s, "#define {id} {}", n as i64);
                }
                Value::Ratio(n) => {
                    let _ = writeln!(s, "#define {id}_Q8 {}", q(n, 256.0));
                    let _ = writeln!(s, "#define {id}_Q15 {}", q(n, 32767.0));
                    let _ = writeln!(s, "#define {id}_Q16 {}", q(n, 65536.0));
                }
                Value::Bezier(p) => {
                    for (k, v) in ["X1", "Y1", "X2", "Y2"].iter().zip(p) {
                        let _ = writeln!(s, "#define {id}_{k}_Q16 {}", q(v, 65536.0));
                    }
                }
                Value::Text(text) => {
                    if let Some(role) = text.strip_prefix('{').and_then(|r| r.strip_suffix('}')) {
                        let _ = writeln!(s, "/* {id}: {role} */");
                    } else {
                        let _ = writeln!(s, "#define {id} \"{}\"", text.replace('"', "\\\""));
                    }
                }
            }
        }
    }
    s.push_str("\n#endif\n");
    s
}

/// The Rust module: core types only.
pub fn emit_rust(t: &Tokens) -> String {
    let mut s = format!(
        "// allen_tokens.rs -- Allen's look as constants.\n// {}\n// Core types only, so no_std code can include!() this file.\n\n\
         /// An sRGB colour.\n#[derive(Debug, Clone, Copy, PartialEq, Eq)]\npub struct Rgb {{\n    pub r: u8,\n    pub g: u8,\n    pub b: u8,\n}}\n\n\
         impl Rgb {{\n    /// Packed as the kernel's CK_RGB.\n    pub const fn packed(self) -> u32 {{\n        (self.r as u32) << 16 | (self.g as u32) << 8 | self.b as u32\n    }}\n}}\n\n\
         /// A CSS cubic-bezier easing curve, control points in Q16 (65536 = 1.0).\n#[derive(Debug, Clone, Copy, PartialEq, Eq)]\npub struct Bezier {{\n    pub x1: i32,\n    pub y1: i32,\n    pub x2: i32,\n    pub y2: i32,\n}}\n",
        HEADER.replace('\n', "\n//")
    );
    for g in t.groups() {
        let module = g.replace('-', "_");
        let _ = write!(s, "\npub mod {module} {{\n    #[allow(unused_imports)]\n    use super::{{Bezier, Rgb}};\n");
        for (name, raw) in t.group(g) {
            let id = ident(name);
            let line = match value(raw) {
                Value::Color(r, gg, b) => {
                    format!("pub const {id}: Rgb = Rgb {{ r: {r}, g: {gg}, b: {b} }};")
                }
                Value::Dim(n, unit) => {
                    let unit_id = ident(&unit);
                    if n.fract() == 0.0 && n >= 0.0 {
                        format!("pub const {id}_{unit_id}: u32 = {};", n as i64)
                    } else {
                        format!("pub const {id}_{unit_id}: f32 = {n:?};")
                    }
                }
                Value::Ratio(n) if n.fract() == 0.0 && n.abs() >= 2.0 => {
                    format!("pub const {id}: u32 = {};", n as i64)
                }
                Value::Ratio(n) => format!(
                    "pub const {id}: f32 = {n:?};\n    pub const {id}_Q16: i32 = {};",
                    q(n, 65536.0)
                ),
                Value::Bezier(p) => format!(
                    "pub const {id}: Bezier = Bezier {{ x1: {}, y1: {}, x2: {}, y2: {} }};",
                    q(p[0], 65536.0),
                    q(p[1], 65536.0),
                    q(p[2], 65536.0),
                    q(p[3], 65536.0)
                ),
                Value::Text(text) => {
                    if let Some(role) = text.strip_prefix('{').and_then(|r| r.strip_suffix('}')) {
                        format!("// {id}: {role}")
                    } else {
                        format!("pub const {id}: &str = {text:?};")
                    }
                }
            };
            let _ = writeln!(s, "    {line}");
        }
        s.push_str("}\n");
    }
    s
}

/// The CSS custom properties.
pub fn emit_css(t: &Tokens) -> String {
    let var = |g: &str, name: &str| format!("--allen-{g}-{name}").replace('.', "-");
    let css_value = |raw: &str| -> String {
        if let Some(role) = raw.strip_prefix('{').and_then(|r| r.strip_suffix('}')) {
            return format!("var(--allen-{})", role.replace('.', "-"));
        }
        match value(raw) {
            Value::Text(text) if text.contains(' ') && !text.contains('(') => format!("\"{text}\""),
            _ => raw.to_string(),
        }
    };
    let mut s = format!(
        "/* allen-tokens.css -- Allen's look as CSS custom properties.\n   {HEADER} */\n:root {{\n"
    );
    for g in t.groups() {
        if g == "reduced-motion" {
            continue;
        }
        let _ = writeln!(s, "  /* {g} */");
        for (name, raw) in t.group(g) {
            let name_v = if g == "typography" && name.ends_with(".fontFamily") {
                let _ = writeln!(
                    s,
                    "  {}: \"{raw}\", ui-rounded, system-ui, sans-serif;",
                    var(g, name)
                );
                continue;
            } else {
                css_value(raw)
            };
            let _ = writeln!(s, "  {}: {name_v};", var(g, name));
            if let Value::Color(r, gg, b) = value(raw) {
                let _ = writeln!(s, "  {}-rgb: {r} {gg} {b};", var(g, name));
            }
        }
    }
    s.push_str("}\n\n@media (prefers-reduced-motion: reduce) {\n  :root {\n");
    for (name, raw) in t.group("reduced-motion") {
        let target = ["motion", "heartbeat", "boot"]
            .iter()
            .find(|g| t.get(&format!("{g}.{name}")).is_some())
            .map(|g| var(g, name))
            .unwrap_or_else(|| var("reduced-motion", name));
        let _ = writeln!(s, "    {target}: {};", css_value(raw));
    }
    s.push_str("  }\n}\n");
    s
}

/// Every file the generator writes, by name.
pub fn outputs(t: &Tokens) -> Result<Vec<(&'static str, String)>, Error> {
    let results = check_contrast(t)?;
    Ok(vec![
        ("allen_tokens.h", emit_c(t)),
        ("allen_tokens.rs", emit_rust(t)),
        ("allen-tokens.css", emit_css(t)),
        ("contrast.txt", contrast_report(&results)),
    ])
}

#[cfg(test)]
mod tests {
    use super::*;

    const MINI: &str = "---\nname: T\ncolors:\n  primary: \"#ECF0FC\"\n  night: \"#060812\"\ncomponents:\n  page:\n    backgroundColor: \"{colors.night}\"\n---\n\n## Motion\n\n```allen-tokens\nmotion:\n  letter: 35ms\n  ease-smooth: \"cubic-bezier(0.3333, 0, 0.6667, 1)\"\ncontrast:\n  primary-on-night: \"7.0\"\n```\n";

    #[test]
    fn reads_front_matter_and_blocks() {
        let t = parse(MINI).unwrap();
        assert_eq!(t.get("colors.night"), Some("#060812"));
        assert_eq!(t.get("components.page.backgroundColor"), Some("#060812"));
        assert_eq!(t.get("motion.letter"), Some("35ms"));
    }

    #[test]
    fn broken_reference_is_an_error() {
        let bad = MINI.replace("{colors.night}", "{colors.nope}");
        assert!(parse(&bad).is_err());
    }

    #[test]
    fn duplicate_token_is_an_error() {
        let bad = MINI.replace("  letter: 35ms\n", "  letter: 35ms\n  letter: 36ms\n");
        assert!(parse(&bad).is_err());
    }

    #[test]
    fn wcag_reference_values() {
        // black on white is 21:1 by definition; equal colours are 1:1
        assert!((contrast((0, 0, 0), (255, 255, 255)) - 21.0).abs() < 1e-9);
        assert!((contrast((6, 8, 18), (6, 8, 18)) - 1.0).abs() < 1e-9);
    }

    #[test]
    fn smoothstep_bezier_is_exact() {
        // x(s) = s for control x 1/3, 2/3, so y(s) = 3s^2 - 2s^3 = ck_smooth
        let y = |s: f64| 3.0 * (1.0 - s) * s * s + s * s * s;
        for i in 0..=10 {
            let s = f64::from(i) / 10.0;
            assert!((y(s) - (3.0 * s * s - 2.0 * s * s * s)).abs() < 1e-12);
        }
    }

    #[test]
    fn c_export_is_integer_only() {
        let t = parse(MINI).unwrap();
        let c = emit_c(&t);
        assert!(c.contains("#define ALLEN_COLORS_NIGHT 0x060812u"));
        assert!(c.contains("#define ALLEN_MOTION_LETTER_MS 35u"));
        assert!(c.contains("#define ALLEN_MOTION_EASE_SMOOTH_X1_Q16 21843"));
        assert!(
            !c.contains('.')
                || c.lines()
                    .all(|l| !l.starts_with("#define") || !l.contains('.'))
        );
    }
}
