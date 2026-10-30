//! allen-design-tokens: `generate` writes the exports from DESIGN.md; `check` fails if they are stale or a
//! contrast floor is missed.
//!
//!   cargo run -p allen-design-tokens -- generate [DESIGN.md] [OUT_DIR]
//!   cargo run -p allen-design-tokens -- check    [DESIGN.md] [OUT_DIR]

use std::path::PathBuf;
use std::process::ExitCode;

fn repo_root() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../..")
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let mode = args.first().map(String::as_str).unwrap_or("check");
    let design = args
        .get(1)
        .map(PathBuf::from)
        .unwrap_or_else(|| repo_root().join("docs/design/DESIGN.md"));
    let out_dir = args
        .get(2)
        .map(PathBuf::from)
        .unwrap_or_else(|| repo_root().join("docs/design/tokens"));
    if !matches!(mode, "generate" | "check") {
        eprintln!("usage: allen-design-tokens generate|check [DESIGN.md] [OUT_DIR]");
        return ExitCode::from(2);
    }
    let text = match std::fs::read_to_string(&design) {
        Ok(t) => t,
        Err(e) => {
            eprintln!("allen-design-tokens: cannot read {}: {e}", design.display());
            return ExitCode::FAILURE;
        }
    };
    let tokens = match allen_design_tokens::parse(&text) {
        Ok(t) => t,
        Err(e) => {
            eprintln!("allen-design-tokens: {}: {e}", design.display());
            return ExitCode::FAILURE;
        }
    };
    let results = match allen_design_tokens::check_contrast(&tokens) {
        Ok(r) => r,
        Err(e) => {
            eprintln!("allen-design-tokens: {e}");
            return ExitCode::FAILURE;
        }
    };
    print!("{}", allen_design_tokens::contrast_report(&results));
    let mut ok = true;
    for r in results.iter().filter(|r| r.gated && !r.passes()) {
        eprintln!(
            "allen-design-tokens: FAIL contrast {} on {}: {:.2}:1, floor {:.1}",
            r.fg, r.bg, r.ratio, r.floor
        );
        ok = false;
    }
    let outputs = match allen_design_tokens::outputs(&tokens) {
        Ok(o) => o,
        Err(e) => {
            eprintln!("allen-design-tokens: {e}");
            return ExitCode::FAILURE;
        }
    };
    for (name, body) in outputs {
        let path = out_dir.join(name);
        if mode == "generate" {
            if let Err(e) =
                std::fs::create_dir_all(&out_dir).and_then(|()| std::fs::write(&path, body))
            {
                eprintln!("allen-design-tokens: cannot write {}: {e}", path.display());
                return ExitCode::FAILURE;
            }
            println!("wrote {}", path.display());
        } else if std::fs::read_to_string(&path).ok().as_deref() != Some(body.as_str()) {
            eprintln!(
                "allen-design-tokens: {} is stale; run `cargo run -p allen-design-tokens -- generate`",
                path.display()
            );
            ok = false;
        }
    }
    if ok {
        ExitCode::SUCCESS
    } else {
        ExitCode::FAILURE
    }
}
